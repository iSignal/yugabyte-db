# Tserver-local DocDB copy of the PG catalog — design

Date: 2026-09-03, revised 2026-09-29; sync rewritten for master push 2026-10-07 ([push-plan.md](push-plan.md)). Scope: object locking and concurrent
(transactional) DDL enabled, per-database catalog versions, invalidation
messages on. This document is the design and records no implementation
status. The current code is described in [implementation2.md](implementation2.md);
every open item, with severity and code pointers, is in [todos.md](todos.md);
the chronological record is [worklog2.md](worklog2.md); the alternatives
weighed before this design are in [comparison.md](comparison.md); background
reading is under [learnings/](learnings/).

## 1. Goal

Serve catalog reads issued by DML statements from a copy of the PG catalog that  
lives on the tserver, so that a catalog cache miss costs a local read instead  
of a round trip to master. Master stays the only writer. Master keeps the copy  
current by pushing the master system catalog tablet's WAL to it. Correctness contract:  
a backend that has been told catalog version V reads a copy that contains every  
catalog change up to V, and every read under one catalog snapshot sees one  
consistent state.

## 2. Reference

| Term                   | Meaning                                                                                                                                                                                                                                                                                                                                                        |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| catalog version, V     | Per-database counter incremented inside every DDL transaction. Version order equals commit order within a database.                                                                                                                                                                                                                                            |
| local version          | The version a backend believes it is at. Advances when it applies invalidation messages or commits its own DDL.                                                                                                                                                                                                                                                |
| shared version         | Latest version the tserver has published to backends through shared memory.                                                                                                                                                                                                                                                                                    |
| commit HT              | Hybrid time at which a DDL transaction committed.                                                                                                                                                                                                                                                                                                              |
| local tablet, the copy | Single-replica DocDB tablet on the tserver holding a copy of the master system catalog tablet: same table ids, same schemas, same row keys.                                                                                                                                                                                                                    |
| applier                | Per-tserver thread that applies the pages master pushes to the local tablet and publishes C.                                                                                                                                                                                                                                                                  |
| page                   | The change records of a contiguous range of master's system catalog WAL operations, serialized once on master and sent unchanged to every tserver below that range.                                                                                                                                                                                           |
| push cycle             | One certification on master: resolve the open catalog transactions at the tablet's leader safe time S, capture the majority-replicated index I_S, build the pages up to I_S.                                                                                                                                                                                  |
| floor request          | A tserver's request that master push its copy up to a hybrid time some caller on it waits for.                                                                                                                                                                                                                                                              |
| apply record           | WAL record written on master when a committed transaction's intents are applied; carries the commit HT. On the local tablet it turns staged intents into regular rows at that HT.                                                                                                                                                                              |
| safe time, S           | Hybrid time a push cycle certifies and attaches to the page that ends at I_S. Guarantee: every transaction that touched the system catalog and committed at or below S has been applied on master, and its apply record is at or before I_S. Master certifies it by resolving every running catalog transaction against its coordinator before capturing I_S. |
| C                      | The S of the last page carrying one that the local tablet has applied, every earlier page included. The local tablet's regular store is complete for every writer up to C.                                                                                                                                                                                      |
| applied version, A[db] | Highest catalog version of database db whose transaction has been applied locally, read from the copy's version table at C.                                                                                                                                                                                                                                    |
| exposure gate          | The tserver publishes version V for database db to shared memory only once A[db] ≥ V, on both the release and the heartbeat path (4.3). |
| serving                | The copy is in the read path. Reads are routed to the copy only while it is serving; otherwise they go to master and the gate is skipped.                                                                                                                                                                                                                      |
| cache miss reads       | Catalog scans that fill the catcache or build a relcache entry.                                                                                                                                                                                                                                                                                                |
| internal catalog reads | Catalog scans issued by PG internals during DML on catalogs without a catcache, and direct heap scans.                                                                                                                                                                                                                                                         |
| user catalog reads     | Executor scans of catalog tables from user SQL.                                                                                                                                                                                                                                                                                                                |
| in DDL                 | The backend is executing a DDL statement, or is in a transaction block in which a DDL already ran.                                                                                                                                                                                                                                                             |
| catalog snapshot       | PG's per-backend catalog snapshot. Created on first catalog read, dropped on invalidation (statement start in `READ COMMITTED`, accepted invalidation messages otherwise). Its read point is the DocDB read time for all catalog reads under it.                                                                                                               |

## 3. Architecture

```
 DDL backend (tserver T1)      master                                tserver T2
 ------------------------      ------                                ----------
 catalog writes ─────────────► sys_catalog tablet: intents in WAL
 commit at HT_V ─────────────► apply record (HT_V) in WAL
                               push cycle: resolve intents, S ≥ HT_V,
                               pages up to I_S (built once)
                               lock release (V, pages) ─────────────────► applier: stage intents, apply at HT_V,
                                                                          all pages applied → C := S
                                                                          gate: wait A[db] ≥ V
                                                                          shared version := V
                               ◄──────────────────────── ack ────────────
 "success" to client
                                                                          backend: local version := V,
                                                                          catalog snapshot dropped
                                                                          cache miss read (V, h) ──► tserver:
                                                                            A[db] ≥ V ? read local tablet at min(h, C)
```

Components:

- Local tablet: created by snapshot transfer from the master system catalog  
tablet (Section 6). Regular RocksDB plus intents RocksDB. No transaction  
participant of its own; no status tablet; not a member of any multi-node  
Raft group.
- Master pusher: one per master, on the system catalog tablet's leader. Keeps
  a cache of pages and each tserver's applied position, runs push cycles,
  and pushes each tserver the pages it lacks, a bounded number of RPCs and
  bytes at a time.
- Applier: one per tserver. Applies write records as staged external
  intents, apply records as batch applies at the producer's commit HT, and
  schema-change records into the local tablet's metadata. Publishes C and
  A[db] after each page that carries S.
- Exposure gate: in the lock-release handler and in the heartbeat handler.
- Read redirect: in the tserver's request path for catalog reads, keyed on  
the read op's database and version.

## 4. Sync

### 4.1 Master push service

Master pushes the system catalog tablet's WAL to the copies; no tserver pulls.
One component on master does it: it keeps a cache of pages, keeps each
tserver's applied position, certifies safe times in push cycles, and sends
pages under fixed in-flight bounds.

A page is the change records of a contiguous range of WAL operations,
`(from, end]`. Master converts the range once, with the record conversion
xCluster's producer uses, and keeps the serialized bytes. Every tserver
below the range receives the same bytes; no record is built per tserver.
A page covers every operation in its range, including operations that
produce no record (a transaction status record that is not an apply, an
election's no-op), so a copy at or above `from` holds everything up to `end`
once it has applied the page.

A push cycle certifies one safe time for the whole cluster:

```
cycle():
    S   := the tablet's leader safe time
           (every write with a hybrid time at or below S is in the WAL)
    resolve_intents(S)                         -- once, for every copy
    I_S := the majority-replicated WAL index now
    build pages for (end of cache, I_S]; the page that ends at I_S carries S
    (no new WAL: the last page takes the new S, its end is I_S already)

resolve_intents(t):
    wait until this server's clock is past t
    loop:
        for every transaction on this tablet that has intents here and whose
        commit is not yet known locally, ask its coordinator for its status
        - committed at or below t and not yet applied here: wait for the
          apply to land (re-check until it has)
        - aborted: remove its intents
        - still running with a start at or below t: re-check
        stop when nothing is left to re-check
```

What this gives the copy, and why:

- Every transaction that committed at or below S has its apply record at
  or before I_S. `resolve_intents` ran before I_S was captured: it waited
  for every transaction committed at or below S to be applied on master, and
  a transaction still running at that moment can no longer commit at or
  below S, because S is already below master's clock. So once a copy has
  applied every page through the one that ends at I_S, it holds every
  record up to S.
- Writes after I_S have hybrid times above S, so a copy that has applied
  more than I_S may still publish S.
- A page sealed at a size cap carries no S; the copy applies it and does
  not move C.
- Records arrive in WAL order, so a transaction's row records always
  precede its apply record.

When master runs a cycle:

| Trigger | Cycle |
|---|---|
| A lock release that publishes catalog versions | at once, on the release's thread, so that S is after the DDL's commit |
| A floor request (a tserver waits for its copy to reach a hybrid time h) | at most one cycle per `local_catalog_push_min_cycle_interval_ms` (50 ms) for all requests together, repeated until S ≥ h. Master's clock is not moved to h: h can be a tserver's clock plus the maximum skew (the lease bound), and moving master's clock there would let successive floors ratchet the cluster's hybrid time ahead of physical time until a server crashes on the skew check. A floor ahead of master's clock therefore costs up to that skew (500 ms) |
| Timer, every `local_catalog_push_interval_ms` (5 min) | only if the WAL moved past the cache; otherwise the tick costs nothing |

Concurrent requests share cycles: a caller waits for the first cycle that
started after it asked. Status round trips to coordinators are therefore at
most one round per cycle for the whole cluster, not one per tserver.

Which tserver gets which pages:

- Master keeps each tserver's applied position, from the tserver's own
  answers (push and release responses, floor requests) and from its
  heartbeat. Nothing is persisted; a new master leader starts empty and
  learns positions within one heartbeat interval or from the first answer.
- A tserver with a live YSQL lease whose position is below the cache's end
  is pushed the next pages from its position. One push RPC is in flight per
  tserver, so pages reach a copy in order.
- A tserver below the cache's first page gets pages built from the WAL on
  demand (from the log cache, or from disk), shared by tservers at the same
  position. A position below the retained WAL gets "re-seed" (6.2).
- At most `local_catalog_push_max_in_flight_rpcs` (64) RPCs and
  `local_catalog_push_max_in_flight_bytes` (64 MB) are outstanding at once;
  tservers a release or a floor request waits on go first, and 16 slots are
  reserved for them.
- A restore of the system catalog is a page of its own with no records; a
  copy below it is told to re-seed.
- A term change of the system catalog tablet empties the cache and every
  position, because entries past a deposed leader's committed index may
  have been replaced.

```
 master                                      tserver A (at p1)      tserver B (at p0 < p1)
 ------                                      -----------------      ----------------------
 cycle: S, I_S; pages P1=(p0,p1], P2=(p1,I_S]
 push P2 ──────────────────────────────────► apply, C := S
 push P1 ─────────────────────────────────────────────────────────► apply
 ◄── ack (p1 → I_S) ─────────────────────────
 ◄── ack (p0 → p1) ────────────────────────────────────────────────
 push P2 (same bytes) ────────────────────────────────────────────► apply, C := S
```

### 4.2 Applier

The applier is one thread per tserver. It applies the pages master pushes and
publishes the two values reads depend on: C, the time up to which the copy
is complete, and A[db], the catalog version of each database that the copy
has applied.

How records land in the copy. A DocDB tablet keeps two stores: the regular
store, which reads consult, and the intents store, which holds the
provisional writes of transactions that have not committed. On master, a
catalog transaction's rows sit in the intents store, keyed by row and
pointing at the transaction, until the transaction commits and master
applies it, which moves the rows into the regular store at the commit time.

The copy does not replay that. It has no transaction participant and no
way to ask a coordinator what became of a transaction. Instead the applier
stores a transaction's row records as external intents: a blob keyed by the
transaction id alone, in the intents store. No reverse mapping from a row
to its transaction exists, so a read that meets a row never finds these
writes and never asks any transaction's status. External intents exist for
one purpose: when the transaction's apply record arrives, the applier takes
the blob and writes every row in it into the regular store at the commit
time the apply record carries, then deletes the blob. A row record with no
transaction (master's own non-transactional writes) goes straight into the
regular store at its own hybrid time.

```
 master WAL                      applier                         copy
 ----------                      -------                         ----
 row r1 of txn T ─────────────►  stage under key T ────────────► intents store: T -> {r1}
 row r2 of txn T ─────────────►  stage under key T ────────────► intents store: T -> {r1, r2}
 apply T, commit time c ──────►  move T's rows to regular ────► regular store: r1@c, r2@c
                                                                 intents store: T removed
 row r3, no transaction ──────►  write at its own time ────────► regular store: r3@t
```

What C means. After the applier has applied every page through one that
carried a safe time S, every transaction committed at or below S has had
its apply record applied here (4.1), so its rows are in the regular store.
The applier then publishes C := S. Reads are never served above C (5.3), so
no read ever depends on the intents store: any external intents present
belong to transactions that commit above C, and a read at or below C does
not look at them. That is what makes a read from the copy a plain read of
the regular store with no transaction status lookup.

What A[db] means. After publishing C, the applier reads the copy's own
catalog version table at C and records, per database, the version it
finds. A version row is written inside the DDL transaction that increments
it, in the same apply as the DDL's catalog rows, so A[db] ≥ V means the
copy holds every row of every transaction up to V for that database. A[db]
is what the freshness guarantee compares against (4.3).

The applier's work:

1. Make sure the copy exists: open it from disk, or fetch it from master
   (Section 6). Take the applied position from the checkpoint recorded on
   disk, if any. Ask master for a push up to the tserver's clock, so that
   master learns the position and the copy gets its first S.
2. Apply pages as they arrive, from a push RPC or inside a lock release,
   lowest starting position first: a page that starts above the applied
   position is answered "gap" with the position and not applied, and
   master re-sends from there; a page that ends at or below the position is
   acknowledged without a write; otherwise the records above the position
   are applied: transactional rows as external intents, each apply record
   as one batch at its commit time (a DDL's rows and its version-row update
   become visible together, a rolled-back subtransaction is left out),
   non-transactional rows at their own time, a schema change into the
   copy's metadata verbatim.
3. When the last applied page carried S: read the version table at S,
   publish C := S and the new A[db] values together, wake anyone waiting on
   them. Otherwise keep C and A[db] and advance only the position.
4. Answer the push with the applied position and C, so that master knows
   which page to send next.
5. A caller that waits for C or A[db] to reach a value asks master for a
   push (a floor request); while the wait is open the request is repeated
   once a second, which repairs a lost request and reaches a new master
   leader.
6. Record the position on disk on a cadence (about once a second) and at
   shutdown.

Rules the applier keeps:

- Apply everything before publishing. A page's S is published only after
  every one of its records, and every earlier page, is in the copy;
  publishing first would let a read at C miss rows master certified.
- Publish only forward. A safe time or a position not above the current
  one is ignored; a page with no safe time leaves C where it was. Pages
  from a master term below the highest one applied are refused.
- Re-apply is avoided: records at or below the applied position are dropped
  from a page that straddles it. A restart that resumes from an older
  recorded position re-applies a contiguous suffix of the WAL, which is
  harmless: staging the same rows under the same transaction key writes the
  same values, an apply record whose blob is already gone applies nothing,
  and a non-transactional row re-written at the same time is the same row.
- The recorded position must never claim records the copy has not made
  durable ([todos T7](todos.md#27-t7-checkpoint-durable-before-the-wal)).
- Pages from a release and pages from a separate push go through one queue
  ordered by their starting position, on one thread, so they never
  interleave.


### 4.3 Freshness guarantee

The guarantee: a backend that has been told version V never reads a copy
that lacks V's catalog rows. It is enforced where versions are published to
the tserver's shared memory, by one rule on both paths: a version report is
published only when, for every database in it, the copy's applied version
A[db] is at or above the carried version. A[db] is read from the copy's own
version table at C (4.2), so A[db] ≥ V means the transaction that wrote V
committed at or below C and its catalog rows were applied in the same batch
as its version row: the copy holds everything V describes. The floor is the
DDL's own apply on the copy; no time from master is involved.

Comparing C with the time R at which master read the report's versions also
gives the guarantee: every version in the report committed at or below R,
so C ≥ R implies A[db] ≥ V. That rule is stricter than needed, because it
also waits for commits between the newest version and R. The version
comparison is chosen for its lower wait, not for correctness.

Release path (every DDL that increments a version):

- Master reads the versions after the DDL committed and sends them in the
  lock release to every tserver with a live lease.
- The release handler does, in this order: wait until A[db] ≥ V for every
  database carried; publish the versions to shared memory; release the
  local locks; acknowledge. The DDL's client is told success only after
  every target has acknowledged.
- The wait completes by contract: the release carries the DDL's own pages
  (4.4), so applying them satisfies the check without another round trip;
  when they cannot be used or did not fit, master pushes them separately
  and the waiting tserver's floor request asks for them.
- The DDL's own tserver is a target too: the synchronous release runs there
  before the commit returns to the DDL backend, and only then does that
  backend write V into its tserver's shared memory for the other backends
  on the node.

Heartbeat path (catch-up for a tserver a release did not reach, and for
versions that change outside a lock release):

- Master sends a version report only when some version changed since this
  tserver's last acknowledged report.
- The tserver publishes the report only when A[db] ≥ the carried version
  for every database in it. Otherwise it holds the report, asks master for
  a push, and publishes as soon as the copy has caught up; the versions
  arrive either with the release that is still in flight or with the push. Single versions are never clamped, because the tserver's
  version table treats a version below the published one as a stale master
  report.

What both paths give a reader: versions are published only once the copy
holds them, and a fresh catalog snapshot is assigned C, so every version a
backend can have been told is at or below the state it reads.

Cases outside both paths: a DDL whose synchronous release timed out and was
completed asynchronously, and a DDL whose version read failed on master
(11.2 item 2), are caught by the version check of 5.3 rule 2; a tserver that
held no lease while a DDL completed is covered by Section 7.1.

Where the copy's safe time C is used, given that versions carry the
freshness guarantee:

1. The read time of a fresh catalog snapshot. A backend's catalog snapshot
   is assigned C on its first local read and keeps it, so all reads under
   one snapshot see one complete state (5.3 rule 4). This is C's main use.
2. Historical and explicit read times. A read that asks for a time h of its
   own (`yb_read_time`, `ysql_dump --read-time`, a transaction's read time
   under Section 14) is served locally only when h ≤ C and h is within the
   copy's retention; otherwise it goes to master (5.3 rule 5).
3. A session's own catalog writes that increment no version. A temporary
   relation's DDL leaves every version unchanged, so the version check
   cannot see it; the session records the commit time of that transaction
   and its next local read waits until C passes it (5.3 rule 3). The same
   session-level wait covers a session that read master at a time t1 and
   must not fall back below it (11.1 item 6).
4. The lease bound. After a lease is granted, serving resumes only when C is
   at or above the tserver's clock reading at the grant, which proves the
   copy holds every DDL master completed while this tserver had no lease
   (7.1).
5. Enabling serving. After bootstrap or re-seed the copy serves only once it
   has a certified C from an applied page, never the snapshot's own time
   (6.1 step 5).

### 4.4 Pages carried in the lock release

The stall this removes: without it, the release handler on every tserver
waits for a separate push before it can publish the DDL's version, so every
DDL pays an extra round trip to the slowest tserver, and so does everything
queued behind the locks the DDL holds.

- At release time master runs a push cycle (4.1), which certifies an S
  after the DDL's commit and builds the DDL's pages into the cache once.
- For each target, master takes the pages from the target's position to
  the cache's end. When they are all in the cache and total at most
  `local_catalog_release_inline_max_bytes` (4 MB), the release RPC carries
  them; each target costs one copy of each page's bytes.
- A target whose pages are not in the cache (it is behind) or do not fit
  (`CREATE DATABASE` is 11.6 MB) gets none inline; master pushes it the pages
  in the background, first among the queued targets, and its release
  handler waits at the gate meanwhile.
- The tserver applies the carried pages through the applier (4.2) before the
  gate. C becomes S and the gate passes without another round trip. A copy
  that is not serving queues the pages and acknowledges without waiting
  (7.1).
- The release's answer carries the copy's applied position and C, so master
  knows what that copy needs next without waiting for a heartbeat.
- Correctness never depends on the carried pages: the gate is unchanged. A
  lost, refused or gapped page leaves the gate waiting; the waiting tserver
  sends a floor request with its position, and master pushes from there.
  Two releases racing to one copy go through the applier's one queue; the
  second is filtered to nothing.

```
  master                         tserver (release handler)
  ------                         -------------------------
  DDL commits at HT_V
  read versions (V)
  push cycle: S ≥ HT_V, pages up to I_S (built once)
  per target: pages from its position
       ── release(V, pages) ─────────►  apply pages (drop ≤ applied)
                                         C := S, A[db] := V
                                         gate: A[db] ≥ V passes at once
       ◄──────── ack (position, C) ──    publish V
```

Catalog writes outside PG DDL (master's own system catalog updates, catalog
writes made outside DDL mode) reach the copies on the next cycle any trigger
starts: a later release, a floor request from a tserver that waits for them
(a temporary relation's own-write wait, 5.3 rule 3), or the timer.

## 5. Reads

### 5.1 What is served locally

All three read kinds, when the backend is not in DDL:

| Read kind                                             | Served from                                                                                                                                                                          | Why C is the right time                                                                                                                                                                                                                                                                                        |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Cache miss reads                                      | Local tablet at the catalog snapshot's time, which the first local read fixes to C                                                                                                   | The catcache needs a state at or beyond the backend's version and consistent across catalogs for the life of the catalog snapshot. C ≥ HT_V by the gate, and C is a complete point for every writer, so all misses under one snapshot see one state.                                                           |
| Internal catalog reads                                | Local tablet at the same time as cache miss reads of the same snapshot                                                                                                               | They run under the same catalog snapshot; serving them from the same copy at the same time is what keeps them consistent with the cached entries. Catalogs without a catcache included; the read time is not refreshed per scan for them.                                                                      |
| User catalog reads                                    | Local tablet at C when the statement is outside an explicit transaction block and, after planning, references only catalog tables and calls no user-defined function; otherwise master at the transaction's read time (Section 14) | A standalone statement has no transaction snapshot that a catalog read at C could disagree with; only the release fan-out window remains, as for cache miss reads. Inside a transaction block the executor's catalog scan and its user-table reads would run at different times (Section 14).                 |
| Startup preload, full cache refresh, parallel workers | Local tablet at C, like cache miss reads                                                                                                                                             | These run on the legacy catalog path. Since 2026-09-26 that path is accepted by the redirect, so a fresh connection makes no master catalog round trip. The catalog version row is part of the same preload, so the version a backend adopts is read from the copy under the same snapshot as the rows (13.2). |

Why a read is never served at a time later than C: the local tablet is  
complete only up to C. A read at a later time could observe rows appearing  
between two scans of one snapshot as the applier applies records, and return  
pg_class and pg_attribute from different states. Why a read is never clamped  
down to C either: a snapshot whose time is above C got that time elsewhere  
(master, or an explicit request) and may already have read at it; serving it  
at C would return an older state under the same snapshot. Such reads go to  
master.

### 5.2 Not served locally

- In DDL. The transaction's own uncommitted rows exist only on master, so  
every catalog read of a DDL statement, and of every later statement in the  
same transaction block, goes to master.
- A request whose session has `yb_non_ddl_txn_for_sys_tables_allowed` set at  
that moment (upgrade migrations, direct catalog updates): the session may  
hold uncommitted catalog rows only master has. The exclusion does not  
outlast the setting; the gap that leaves is in 11.1 item 3.
- A `SERIALIZABLE` transaction: its catalog reads carry no read time and take  
the master tablet's own safe time per operation, which the copy cannot  
reproduce.
- Scans with non-MVCC snapshots (self, dirty, any) and historic snapshots.  
All are DDL-side or logical-decoding paths.
- While the tserver is bootstrapping or re-seeding the local tablet, the  
lease is lost, or a major version upgrade is in progress (Sections 7 and  
10). Reads go to master in that state only.

### 5.3 Picking the local read time

Background: the PG catalog snapshot holds no hybrid time, only a read-point  
serial number. The tserver session keeps the hybrid time per serial, and on a  
fresh catalog snapshot it assigns a time from its own clock before any read is  
issued. The redirect works on that per-serial time.

Rule, evaluated on the tserver for each catalog read request:

1. The request carries the backend's database, version V and its read-point
  serial; the session's read point for that serial holds a time h (a clock  
   time on the first read of a fresh catalog snapshot, or the time fixed by  
   an earlier read).
2. Version check, a safety net: if A[db] < V, ask master for a push up to
   the tserver's clock and wait for it (bounded by
   `local_catalog_serve_wait_timeout_ms`, after which the read goes to
   master). In
   steady state it always passes, because every version a backend can hold
   was either read from the copy or published through the gate (4.3), and a
   copy enters serving only once it holds every version published while it
   was not serving (7.1). It catches the asynchronous fallback of a
   timed-out release (11.2 item 2). The
   backend's version travels on the request, not on the individual
   operations, because PG does not stamp a version on internal scans of
   system relations. A request from a backend that has not attached to a
   database yet carries no database, so this check does not apply to it.
3. Own-write check: if the session's last catalog-writing transaction (a DDL
  commit) has a commit time above C, ask master for a push up to it and wait
   until C passes it, so that a session reads its own writes.
4. First local read of a fresh catalog snapshot: h is the clock time and is
  above C. Replace it with C on the session's read point. Every later read  
   under that snapshot finds h = that C.
5. Otherwise compare h with C. h ≤ C: serve locally at exactly h (C only
  moves forward, so any time set by step 4 stays at or below C). h > C: the  
   time came from master or from an explicit request, and is at or below the
   clock, so master can certify it: ask master for a push up to h, wait (the
   same bound as rule 2), and serve at exactly h; on timeout route this
   request to master. Never serve at C in that case.
6. A local read below the copy's history retention is routed to master.
7. Never use the tserver clock as the local read time.

Consequences:

- Within one catalog snapshot all local reads see one state, even if the  
copy advances meanwhile; a DDL committing during the statement becomes  
visible at the next invalidation, as in PG.
- A snapshot that started on master (tserver not serving, backend in DDL)  
stays on master until it is dropped; no mixing of sources within a  
snapshot.
- Transactions with an attached distributed transaction are served. The  
catalog snapshot has its own read-point serial; the tserver saves the  
transaction's read point before the catalog excursion and restores it  
afterwards, so fixing the catalog serial's time to C does not move the  
transaction's data snapshot. Verified by a repeatable-read test: a locally  
served catalog miss after a write leaves the transaction's row count  
unchanged until commit.

Why the request-level version wait is kept even though the gate exists:
while the copy is not serving, versions are published without the gate. When
the copy then starts serving, A[db] can still be below a version a backend
already holds, so that backend could read a copy that lacks the version's
rows. The version wait catches that (7.1 closes the window at its source).
One more residual path: a release acknowledged after the synchronous
timeout (11.2 item 2).

A DDL that does not increment the version (temporary relations) leaves the  
backend's version unchanged; its rows reach the copy on the next push cycle,
which the session's own-write check (rule 3) requests. The own-write check
covers the writing session. Other sessions cannot  
name a temporary relation, so no cross-session read depends on it.

## 6. Bootstrap

### 6.1 Initial

1. Create the local tablet by snapshot transfer from the master system catalog
  tablet: the transfer copies the tablet's files and its metadata, including  
   every catalog table's schema. The result is a checkpoint at a committed  
   WAL position.
2. Rewrite the tablet's consensus metadata to a single-peer configuration
  (this tserver only) and open it.
3. A[db] and C are not initialized from the snapshot. The first pushed page
  that carries a safe time fills A[db] for every database by  
   reading the local catalog version table at that time, and publishes it  
   together with C. Nothing about versions is persisted; a restart repeats  
   this step.
4. Ask master for a push from the snapshot's WAL position up to the
  tserver's clock.
5. Enable local serving only once a page carrying a safe time has been
  applied, so that C is a certified value, not the snapshot time.

A tserver restart with a copy on disk reopens it from local metadata without  
contacting master and resumes from the persisted checkpoint; serving is  
enabled after the first applied page with a safe time as above.

### 6.2 Re-seed

Same steps, replacing the existing local tablet (tombstone, then create).  
Triggers:

- Master pushes "re-seed": it has garbage-collected WAL the copy had not
  applied (retention is bounded on master; a tserver that lags beyond it
  cannot catch up incrementally).
- The tserver's YSQL lease lapsed and was regained (Section 7.1) and master
  cannot push from its checkpoint.
- Master restored its system catalog from a snapshot (PITR). The restored  
rows are written on master outside the WAL, so no page can carry them;  
the restore operation is a page of its own that tells a copy below it to re-seed,  
and no safe time can pass the restore before that answer is seen. A  
restore of one database re-seeds every tserver's whole copy.
- Local tablet failure, at open or detected while running (corruption, disk
  loss, metadata mismatch, a corruption-class error while applying or
  reading).

During re-seed local serving is disabled; catalog reads go to master, and the  
version gate publishes versions without waiting (the copy is not consulted).


## 7. Failure handling

### 7.1 Lease loss, lease regain and tserver restart

//C: What about a restarted tserver or a new lease joining tserver? A new lease joining tserver is always RBS-ing in our approach, which is reasonable but an alternate method is that it holds up local reads until a poll at now completes (is there a race here with a release that just completed). The poll decides if incremental catch up works or if a new RBS is needed. A restarted tserver - do we need a special case or does it also know if it lost a lease and is regaining it?
//C: Alternately, we can have lease carry the latest catalog version but that is some complexity to the light lease rpc.
//R: A new-lease tserver does not re-seed. The copy is held out of the read path until it holds everything up to the tserver's clock reading at the moment the new lease was observed (the bound B), and with push (2026-10-07) PG itself is not started until then: the tserver asks master for a push up to B, master certifies an S at or above B and pushes the missing pages, and only then is the postmaster started. Master decides between incremental pages and "re-seed" (WAL no longer retained). The race with a release that just completed does not exist: a release that did not target this tserver completed before the lease was granted, so its DDL committed before the grant, and B is at or above the grant; a release that computed its targets after the grant targets this tserver. A restarted tserver needs no special case: its first lease is a new epoch with the same bound, and the copy reopened from disk resumes from its checkpoint. Carrying versions in the lease is not needed: the copy needs the rows anyway, and the time bound already forces the push that brings them.

A tserver without a live YSQL lease is not a target of lock releases, so
master completes DDLs without this tserver running the freshness guarantee.
Every such DDL committed before master granted this tserver its next lease.
The rule:

- On loss, serving is disabled at once and a bound is set that C can never
  reach, so the copy stays out of the read path for as long as no new lease
  is granted. Lease loss also ends this tserver's PG sessions.
- On every new lease epoch, including the first lease of a freshly started
  process, the bound B becomes the tserver's clock reading at the moment the
  lease is observed, which is at or above every DDL master could have
  completed without this tserver. A copy that is serving with C below B
  leaves serving (an epoch can arrive before the tserver noticed the old
  lease run out). The tserver asks master for a push up to B.
- PG's start is held until the copy serves at B, for at most
  `local_catalog_serve_wait_timeout_ms` (5 min; 0 starts PG at once, -1 holds
  until the copy serves). With PG not running there are no backends that
  could read a stale copy; after the timeout PG starts and reads master
  while the copy catches up, which is the behavior without the hold. The
  wait runs off the lease refresher thread, which must keep refreshing the
  lease.
- Master decides how the copy catches up: pages from its checkpoint in the
  normal case, or "re-seed" when the WAL after the checkpoint is no longer
  retained (6.2). A regain does not imply a re-seed.
- A restarted tserver is the same case: its copy is reopened from disk and
  resumes from the persisted checkpoint; its first lease sets the bound.

```
lease refresher (new epoch e)          applier                         master
  B := MaxGlobalNow()
  OnLeaseGained(B): not serving,
    floor request (position, B) ─────────────────────────────────────► cycle with S ≥ B
  hold PG's start (other thread)                                        pages from position
                                       ◄─────────────────────────────── push ...  page with S
                                       C := S ≥ B, A[db] read at C
                                       serving if A[db] ≥ every version
                                       published while not serving
  copy serves (or timeout): start PG
```

Why there is no race with a release that completed just before the grant:
that release did not target this tserver, so it completed, and its DDL
committed, before master granted the lease; the bound is at or above the
grant. A release that computed its targets after the grant includes this
tserver; if the copy is not serving yet at that moment, the tserver queues
the release's pages, publishes the versions without waiting, and
acknowledges.

Entering serving requires both: C at or above the lease bound, and A[db] at
or above every version published to shared memory while the copy was not
serving (each report is the full version map, so a database missing from a
later report was dropped and its record goes too). With that, no read,
including the startup preload that carries no database, can be served from
a copy that lacks a version the tserver has already published.

What a new backend's catalog version rests on. Without the copy, lease loss
kills the tserver's PG backends, and a backend started after the next grant
reads the latest catalog version from master. With the copy, a new backend
reads the version row from the copy at C, in the same preload as its other
catalog rows (13). The guarantee that this backend does not miss a DDL that
master completed while this tserver had no lease therefore rests on the
serving condition above, not on a master read:

- PG does not start until the copy serves, so no backend exists before C is
  at or above B, and every such DDL committed at or below B.
- When the hold times out, PG starts while the copy is not serving, every
  catalog read goes to master, and the backend reads its version from master
  as it does without the copy.

Lease loss keeps killing the tserver's backends, as it does without the
copy. Backends that survived a lease loss and learned the latest version by
propagation instead would need nothing further from the copy: their reads go
to master until the copy serves at B. This design does not change the kill
behavior.

A joiner wave (many tservers regaining their leases at once, each needing
minutes of WAL) does not hold DDLs back: a copy that is not serving has no
backends to protect, so its release handler acknowledges without waiting,
and master's in-flight bounds keep the catch-up pushes from crowding out the
releases' own pages. Delaying the lease grant until the copy is current was
rejected: the lease decides when a tserver's locks are released and its
sessions killed, and a grant that waits on a WAL transfer couples
availability to catch-up time.

### 7.2 Push and apply failures

- Master leader change: the new leader starts with an empty page cache and
  no positions. Positions arrive with heartbeats within a second and with
  the first floor request or push answer; a copy whose position is unknown
  is pushed the start of the new leader's cache and drops what it holds.
  Local serving stays enabled at the last C; a waiting tserver repeats its
  floor request, which reaches the new leader.
- A push that fails (RPC error, timeout, apply error) is retried by master
  with backoff (100 ms doubling to 5 s). A push the copy cannot use because
  it starts above the copy's position is answered "gap" with the position,
  and master re-sends from there at once.
- Tserver restart: the tablet and its checkpoint persist; the copy asks
  master for a push from its checkpoint; C is recomputed from the first
  applied page with S before serving is enabled.
- Repeated apply failures: after N consecutive failures, or when C lags the
  tserver clock by more than a bound, the copy should leave the read path
  (reads go to master, the gate is skipped, heartbeat reports publish
  without waiting) and resume under the rule of 7.1. Not implemented
  ([todos T11](todos.md#211-t11-serving-withdrawal-and-wait-fallbacks)).

### 7.3 Master WAL retention

Retention on master is bounded by its ordinary log retention (15 minutes by
default). Master builds pages for a copy below its page cache from the WAL;
a copy whose position falls below the retained WAL is told to re-seed.
Copies never anchor master's WAL. Automating a per-copy retention window is
a follow-up item.

## 8. Observability

Counters and histograms on the tserver, all per tserver:

- apply latency histogram (pages received to C published);
- apply failures, re-seeds, lease-triggered serving disables, floor requests
  sent;
- lag histogram: C behind the tserver clock at each publish;
- gate wait histogram: time the lock-release handler waited for C;
- local catalog reads served, local reads that waited for A[db], reads routed  
to master and the reason (in DDL, not serving, serializable, snapshot  
kind); pushes received, applied, refused;
- serving-state gauge and current C gauge;
- per-request routing log, switchable at runtime: one line per catalog request  
saying which source answered and why.

Known holes: a stalled copy looks healthy (failures do not change the serving  
gauge, lag is sampled only when C moves); a failed local read leaves no  
counter and no fallback; EXPLAIN and pg_stat_statements count copy reads as  
master reads; the waits of 5.3 rules 2 and 3 are invisible to ASH. Planned  
additions: applied op id and applied version per database, continuously  
sampled lag and last-push age, wait-time histograms and ASH wait codes, a  
per-reason breakdown of reads routed to master, a served-by-copy count in  
EXPLAIN, and debug pages on tserver and master listing the copy's state and  
master's view of every tserver's position. Master: push cycles and cycle
time, push RPCs and bytes, pages built (cached and transient), in-flight
maxima, floor requests received, pages carried inline by releases and
releases that could not carry them.

## 9. Performance model

- DDL: one push cycle on the release path (status resolution when other
  catalog transactions are open, then building the DDL's pages once); the
  pages arrive with the release, so the residue on each tserver is the
  apply time. A DDL whose pages exceed the inline cap (`CREATE DATABASE`)
  waits for separate pushes, bounded by master's in-flight limits.
- Readers: no master round trip, including connection startup. Visibility of
  a DDL to a backend lags its commit by the release fan-out.
- Master: nothing when nothing changes. Per DDL: one cycle and one copy of
  each page's bytes per tserver. Status resolution: at most one round per
  cycle for the cluster, cycles at most every 50 ms when requested by
  tservers. Measurements: [loadtest-summary.md](loadtest-summary.md) (pull,
  before) and the push scenarios of the load harness (push-plan §9.7).
- Post-DDL cost that is not the copy's: a DDL empties the tserver's whole
  per-database table cache, and a new connection in a far region then pays
  one master round trip per table it touches (9 s for a first query at
  100 ms one way). The release will carry the DDL's changed table list so
  that only those entries are evicted (Section 12).

## 10. Modes in which the local tablet is not used

- initdb: no redirect. Initdb mode forces the legacy catalog-ops path with a  
session type that never reaches the redirect; the local tablet is  
bootstrapped only after master reports initdb complete.
- YSQL upgrade migrations: upgrade-mode sessions write catalog rows outside  
DDL with `yb_non_ddl_txn_for_sys_tables_allowed` set and read master;  
ordinary sessions are unaffected.
- Major version upgrade: postponed. While an upgrade is in progress the  
tserver disables serving and routes all catalog reads to master; two  
catalog version tables exist in that window and some DDLs still run, so the  
single-version gate does not describe the cluster. Support is a follow-up.


## 11. Edge cases and known gaps

Ranked High or Medium only. Status: **open** (nothing in the code),  
**partial** (part of the mechanism exists), **addressed** (in the code and  
tested), **accepted** (a behavior we keep). Implementation-level gaps  
(checkpoint durability and ordering, waiter wake-ups, read failure fallback,  
memstore flushing, compaction of staged intents, tablet-wide flags, explicit  
read times below the compacted history, release push cost, a restore inside  
the copied WAL tail) are in `implementation2.md`, "Implementation-level gaps".



### 11.1 PG side

The version check and the own-write check (5.3 rules 2 and 3) are driven by  
DDL commits only. A catalog write committed any other way is invisible to  
the next catalog read, of the writing session and of every other session, for  
until the next push. Items 1 to 3 are the writers of that kind.

Decision 2026-10-09: the features of items 1 to 3 are disabled while the copy
is enabled; support is a follow-up and the plans below are its candidates.

1. **Login profiles (Medium, decided: disabled, open; [todos T4](todos.md#24-t4-login-profile-catalogs)).** A failed login reads the role's
  failed-attempt counter, adds one and writes it back outside DDL. With the  
   copy, attempts spaced closer than one push read the same counter, so the  
   lockout after N failures arrives late, and `ALTER ROLE ... ACCOUNT LOCK`  
   is seen late on other tservers. Plan: reads of `pg_yb_profile` and  
   `pg_yb_role_profile` always go to master. Rejected: a version bump per  
   failed login (a global bump, the catalog is shared).
2. **Large objects (Medium, decided: disabled, open; [todos T5](todos.md#25-t5-large-objects)).** `lo_create`, `lowrite`, `lo_unlink`
  write `pg_largeobject` and `pg_largeobject_metadata` from the user's  
   transaction outside DDL. Another session reads them from the copy at C,  
   so an object written just before is missing or torn against the user row  
   that references it. Candidates: reads of the two catalogs always go to  
   master; or the own-write check extended to non-DDL catalog commits plus a  
   cross-session wait.
3. **Direct catalog updates under `yb_non_ddl_txn_for_sys_tables_allowed` ([todos T6](todos.md#26-t6-direct-catalog-updates-and-the-own-write-wait))
  (Medium, decided: disabled outside upgrade and initdb, open).** A request that carries the setting goes to  
   master, but the exclusion ends with the statement, so the session's next  
   read is served from the copy below its own write. A manual increment of  
   the version row covers other sessions (the gate publishes the new version  
   only after the copy holds it) but not the writer: a raw update of the  
   version row does not advance the writer's own version. Open: whether to  
   keep requiring the manual increment. Plan for the writer: the own-write  
   check extended to any catalog-writing commit, or keeping the session on  
   master from its first such write until C passes it.
4. **User catalog reads (High, open; [todos T8](todos.md#28-t8-user-catalog-reads-guard)).** An executor scan of a catalog table
   (`\dt`, `SELECT ... FROM pg_class`) served from the copy can show a state
   that disagrees with the statement's or transaction's user-table reads, or
   with what another tserver shows at the same moment. Proposal: serve such
   reads from the copy only for a statement that is not inside an explicit
   transaction block and that, after parsing and planning, references only
   catalog tables and calls no user-defined function; everything else goes
   to master at the transaction's read time. Section 14 has the mechanism,
   the guard, the optional flag and the long-term cluster-wide lock.

5. **Login after a role or database change (Medium, addressed; the flip window is [todos T2](todos.md#22-t2-serving-condition-on-entering-serving)).** `CREATE
   ROLE`, `ALTER ROLE` and `CREATE DATABASE` are not complete until every
   active tserver has run the gate, so a login attempted after the DDL
   returned sees the change on every tserver, whichever database it connects
   to. The auth preload carries no database and so has no version check; it
   relies on the gate alone. One narrow implementation hole, the
   not-serving-to-serving flip, is in implementation2.md.

6. **A session sees a change and then loses it (Medium, open; [todos T13](todos.md#213-t13-session-wait-after-a-master-fallback)).** A catalog
  snapshot that fell back to master reads at a clock time t1; the next  
   fresh snapshot gets C, which can be below t1. A change committed between  
   C and t1 shows in one statement and not in the next. Causes: a wait  
   timeout, a brief not-serving state, the first read after a DDL block.  
   Plan: when a session reads master at t1, its next local read waits until  
   C ≥ t1, the same session-level wait as the temporary-table case.


### 11.2 Master side

1. **Remote bootstrap copies state the copy must not trust (High, open; [todos T3](todos.md#23-t3-remote-bootstrap-copies-untrusted-state)).**
  A page with a safe time certifies its state: every record up to S is committed and  
   applied, and open transactions are staged in a form only their apply  
   record can materialize. Remote bootstrap certifies nothing: it copies the  
   tablet as it is, with (a) WAL entries master's leader appended but had not  
   committed, which the single-voter copy commits itself on open and which a  
   later master leader may truncate and replace, and (b) the row-keyed  
   intents of catalog transactions still open at that moment, whose write  
   records the copy never receives again and whose apply record then finds  
   nothing to apply; once master's coordinator forgets the transaction the  
   copy reads those rows as aborted. Either way the copy diverges from master  
   silently until the next re-seed. xCluster avoids (b) by aborting active  
   transactions on the source at bootstrap, which is not acceptable here: a  
   tserver can bootstrap at any time and that would abort in-flight DDLs  
   cluster-wide. Plan: for (a), resume from the copied log's committed  
   position and refuse to commit entries past it, and let master check the  
   term at the copy's position and answer "re-seed" on a mismatch; for  
   (b), rewrite the copied intents into the staged form before the applier  
   starts, or apply such transactions through the copy's own participant at  
   the apply record's commit time. Detail in implementation2.md.
2. **A DDL that fails or dies after commit (Medium, addressed; the two skips are [todos A2](todos.md#237-a2-releases-that-skip-the-gate)).** A DDL that
  increments the version holds an exclusive lock on the catalog version  
   table, so it always has a release to send, and the release carries the  
   versions and their read time, so the gate runs on every tserver. If the  
   backend dies after commit, its tserver's session cleanup sends the  
   release. Master persists a release once received and retries it until  
   every tserver with a live lease has acknowledged. If the tserver itself  
   dies, master releases that lease epoch's locks on expiry without a version  
   read time; the version then reaches the other tservers through the  
   heartbeat, where the gate also runs. Two paths skip the gate on purpose  
   and are tolerated as a short lag covered by the version check of 5.3:  
   master's version read at release fails (the release carries no read time),  
   and the synchronous release exceeds its timeout (master completes it  
   asynchronously and the client is told success first).
3. **Safe time certified before a restarted master has loaded its ([todos T20](todos.md#218-t20-resolveintents-before-transaction-loading))
  transactions (Medium, open, existing issue).** After master opens its  
   system catalog tablet, the transaction participant loads existing  
   transactions in the background. A push cycle that runs before that finishes  
   resolves only the loaded set, so a committed but not yet applied  
   transaction can be missed and S emitted before its apply record. This is  
   stock code shared by every transactional xCluster stream today. Plan: the  
   pusher waits for loading before certifying S.
4. **Per-tserver load on master (resolved by push, 2026-10-07; [todos T21](todos.md#219-t21-master-load-per-tserver)).** Each
   tserver's poll used to read its own window of master's WAL, rebuild the
   records and certify S by resolving every open catalog transaction, so a
   catalog-heavy burst cost master N × the work. Master now builds each page
   once and certifies S once per cycle for every copy (4.1), and bounds the
   pushes in flight. What remains is N copies of the bytes on master's NIC
   ([post-perf-design.md](post-perf-design.md) §4).


### 11.3 Tserver side

1. **Statements that wait on a push (Medium, partial; [todos T11](todos.md#211-t11-serving-withdrawal-and-wait-fallbacks), [T36](todos.md#234-t36-wait-latency)).** In the
  common case no statement waits on the copy. A few narrow cases make a  
   statement wait until a requested push arrives: the statement after a  
   `CREATE TEMP TABLE` (own-write check), a read that carries a version the  
   heartbeat announced before the copy had it (item 2), and a login in the  
   flip window (implementation2.md). If master is unreachable, that wait runs to  
   its timeout and the read then goes to master, which fails as well;  
   the tserver is about to lose its lease in that state anyway. Fallback  
   needed: bound the wait once per statement, not once per request, and take  
   the copy out of the read path after repeated apply failures so that the  
   gate is skipped, heartbeat publication resumes and reads go to master  
   (7.2). Measured consequence without it: one tserver whose  
   applier is stuck holds every DDL in the cluster at its gate for the  
   release deadline.
2. **A heartbeat announces a version ahead of the DDL's release (Medium, [todos T1](todos.md#21-t1-publish-gate-on-applied-versions);
   open).** A tserver that the release has not reached yet, or whose copy is
   behind, can learn a new version from the heartbeat first. The report is
   held until the copy's applied versions reach it and is published then
   (4.3); the wait ends when the release's pages or the requested push
   arrive. It must not cost a heartbeat
   interval on top: a held report is published as soon as the copy catches
   up, not at the next heartbeat. Backends without a database are covered by
   the serving condition of 7.1.

## 12. Follow-up items

- Load study of push: N push receivers in the test process standing in for N
  tservers (`pg_local_catalog_load-test`, push scenarios); results to be
  added to loadtest-summary.md and post-perf-design.md §4.
- Send a DDL's changed table list in the lock release, so a tserver evicts  
only those entries from its table cache instead of the whole database's.  
Master already holds that set for DDL verification and already fills  
cache-update fields into the release request; the tserver applies the list  
where it applies the versions. Removes the post-DDL first-query cost  
of Section 9. Details in implementation2.md.
- Master WAL retention policy per stream and automatic re-seed (7.3).
- Lease: a rejoin test (a tserver partitioned long enough to lose its lease
  and miss a DDL, then reconnecting and withholding reads until its copy
  proves it holds what it missed).
- Major version upgrade support (Section 10).
- Filter non-PG rows of the system catalog out of the stream and the snapshot  
if space matters.
- Observability additions listed in Section 8.



## 13. How the PG backend flow changes

Two places where a backend loads catalog state in bulk today:

| Case                                                                                                | Today                                                                                                                                                                                                                                                                                | New approach                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| --------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Backend init                                                                                        | Default sys-table prefetching: the shared catalogs before authentication, then the database's catalogs, fetched from master (or from the tserver response cache) in a few batched reads; the relcache init file when it is valid; the catalog version row taken from the same batch. | Everything reads the copy at C. The relcache init file stays: rebuilding the relcache from it is faster than one tserver round trip per relation. The rest of init follows standard PG: catcache and relcache entries load lazily, each miss a local read. The preloading option is removed entirely. The backend's version is the version row read from the copy under the same snapshot as its other rows, so version and rows always match and the version is never ahead of the copy. |
| Mid-statement full cache refresh (rare: a version arrives whose invalidation messages are unusable) | Re-runs the preload against master and re-adopts the version row from it.                                                                                                                                                                                                            | Removed. The caches are reset to empty and reload lazily from the copy, as at init.                                                                                                                                                                                                                                                                                                                                                                                                       |
| Invalidation messages | Carried by every heartbeat and every release, and read by backends from tserver shared memory. | Not carried. The invalidation messages catalog is in the copy, and the gate guarantees that when a backend learns version V the copy holds V's messages, so the backend reads them locally. Heartbeats and releases still carry the versions and their read time, for the gate. |




## 14. User catalog reads

This section covers executor scans of catalog tables issued by user SQL:
`\dt` and the other psql meta-commands, `SELECT ... FROM pg_class`, the
`information_schema` views, schema browsers and migration tools that check
`pg_class` before acting. Catcache and relcache loads are not covered here;
they keep the rules of Section 5.

### 14.1 Why they need their own rule

PostgreSQL runs two snapshots in one backend:

- The catalog snapshot. Catcache and relcache loads use it. PostgreSQL
  refreshes it whenever an invalidation arrives, so it is always at least as
  new as the last DDL the backend was told about.
- The transaction snapshot. Executor scans use it, including executor scans
  of catalog tables. Under `REPEATABLE READ` it is fixed for the whole
  transaction; under `READ COMMITTED` it is taken per statement.

In PostgreSQL an executor scan of `pg_class` therefore sees the transaction
snapshot's state, like any other table. In YugabyteDB today, every read of a
catalog table is placed on the catalog snapshot's read time by pggate, based
on the table alone, whether the read is an internal load or an executor
scan. With the copy, that read time is C. Two things can then go wrong:

- Linearizability across tservers. While a DDL's release is still fanning
  out, tserver 1 has published it and tserver 2 has not. A client that runs
  `\dt` on tserver 1 and then on tserver 2 sees the new table and then does
  not. The window is the release fan-out. Catcache reads have the same
  window today, and it is judged acceptable for them.
- MVCC inside one transaction or one statement. A catalog read at C and a
  user-table read at the transaction's or statement's read time h, which is
  normally above C, can disagree:
  - Inside a `REPEATABLE READ` transaction, `SELECT ... FROM pg_class`
    does not show a table that the same transaction created earlier, while
    the transaction's own data reads see the transaction's writes.
  - Inside one statement, `SELECT ... FROM pg_class, tenants` run just
    after another session committed `CREATE TABLE t2; INSERT INTO tenants
    VALUES (42); COMMIT` can return the tenant row and not the table: the
    user rows are read at h, above the commit, and the catalog at C, below
    it. One statement, two states.
  - A user-defined function called from a catalog-only statement, for
    example `SELECT f(relname) FROM pg_class`, produces the same skew. The
    function's own queries run through SPI at execution time and are not
    part of the outer statement's plan. A stable function runs them under
    the caller's snapshot; a volatile function runs them under the
    transaction's snapshot under `REPEATABLE READ`, and under a new
    per-query snapshot under `READ COMMITTED`, which PostgreSQL permits. So
    in every case except volatile-under-`READ COMMITTED` the inner
    user-table read is at the outer statement's h > C.

### 14.2 Proposal

User catalog reads are served from the copy by default when both conditions
hold:

1. The statement is not inside an explicit transaction block.
2. After parsing and planning, the statement references only catalog
   tables and calls no user-defined function. A user-defined function is
   one whose oid is above the built-in range; extension functions therefore
   count as user-defined, which is conservative.

Both conditions are decided once, at executor start, when the planned
statement's range table is final and views have been expanded. A statement
that runs inside a function is its own executor invocation and is decided
on its own.

What the conditions admit and what they reject:

- Admitted: `\dt` and the other psql meta-commands, `SELECT ... FROM
  pg_class`, the `information_schema` views (they expand into catalog
  relations at rewrite and call only built-in functions), and schema
  browser queries of the same shape. Built-in functions read catalogs only,
  through the catalog snapshot, so they do not reintroduce the skew.
- Rejected: any statement inside `BEGIN ... COMMIT`; `SELECT ... FROM
  pg_class, user_table`; `SELECT f(relname) FROM pg_class` for a user
  function `f`.

What happens to a rejected statement:

- Step 1: its executor catalog scans keep the transaction's read time and go
  to master. That is PostgreSQL's semantics, and it is what the reads cost
  today.
- Step 2, later: such a scan is served from the copy when the transaction's
  read time is at or below C, which is the normal case under `REPEATABLE
  READ` because that time was fixed at transaction start; otherwise it goes
  to master.

With both conditions met, only the linearizability window of 14.1 remains,
the same one catcache reads have today.

Optional flag: serve user catalog reads from the copy in all cases, which
is today's behaviour, for a deployment that accepts the MVCC skew.

### 14.3 Why this is implementable

- Telling the two read kinds apart. PostgreSQL hands the snapshot to every
  scan when the scan starts: an internal load carries the catalog snapshot,
  an executor scan carries the transaction snapshot. So the kind is known
  at scan start. pggate today ignores it and decides by table type. The
  change is one flag per scan, set by PostgreSQL from the snapshot kind and
  the statement's decision, and honoured by pggate, which then leaves the
  scan on the transaction's read time instead of switching it to the
  catalog snapshot's.
- Deciding the conditions. Condition 1 is a standard PostgreSQL check.
  Condition 2 is one walk over the planned statement's range table and one
  walk over its expression trees. Both walks have precedents in the code:
  the query performance monitor already walks the range table for catalog
  relations, and PostgreSQL's own volatile-function check is the shape of
  the expression walk. The plan cache already carries a per-plan YugabyteDB
  flag computed at plan time and read at execution, which is where the
  decision can be cached.
- The tserver side. A scan left on the transaction's read time reaches the
  tserver as an ordinary read at that time; today it goes to master because
  it is not on a catalog snapshot (step 1). Step 2 is the same rule the
  copy already applies to an explicit read time: serve at exactly that time
  when it is at or below C and within the copy's retention.

Code pointers, the SPI read-point details and the precedents are in
implementation2.md, "User catalog reads: guard and cluster-wide lock".

### 14.4 Long-term: a cluster-wide catalog lock

Credit: Minghui Yang ("Options for local catalog.md", "Optional: strict
user catalog reads"). The idea: a DDL commit holds a cluster-wide lock in
exclusive mode across its release fan-out, and a user catalog read takes the
same lock in a shared mode before it reads. A reader that holds the lock
knows that no DDL is mid-fan-out, so every DDL is either visible nowhere or
visible everywhere. This closes the linearizability window that 14.2 leaves
open.

What exists already:

- Every DDL that increments a catalog version takes an exclusive object lock
  on the catalog version table before it writes the increment. That lock is
  a global object lock: it is fanned out to every tserver.
- Each tserver releases it only after its copy has applied the DDL and the
  versions are published (4.3). So on any tserver, once the exclusive
  lock is gone, that tserver's copy and shared memory already hold the DDL.

What is missing, and its cost:

- The reader side is one lock acquisition: a `ROW SHARE` object lock on the
  catalog version table before a user catalog scan. `ROW SHARE` is the
  weakest mode that conflicts with `EXCLUSIVE`; `ACCESS SHARE` does not
  conflict with it and would not do.
- `ROW SHARE` is a fast-path lock: it is taken in the tserver's shared
  memory with no RPC unless a DDL currently holds the exclusive lock on this
  tserver, in which case the reader waits for this tserver's release to
  finish. A DDL that starts later waits for the reader on every tserver.
- Cost: a DDL commit waits for in-flight user catalog scans. The lock lives
  until the transaction ends, so inside a transaction block it would hold
  every DDL in the cluster until that transaction ends. It is therefore
  taken only for statements that meet condition 1 of 14.2, where its
  lifetime is the statement. Statements in a block go to master, which is
  linearizable without a lock.
- Not verified: a transaction block that reads `pg_class` and then runs a
  DDL upgrades its own `ROW SHARE` to `EXCLUSIVE`; this is the same shape as
  DML followed by DDL on one table and is expected to work.
