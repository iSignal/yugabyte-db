# Tserver-local DocDB copy of the PG catalog — design

Date: 2026-09-03, revised 2026-09-29. Scope: object locking and concurrent
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
of a round trip to master. Master stays the only writer. The copy is kept  
current by pulling the master system catalog tablet's WAL. Correctness contract:  
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
| poller                 | Per-tserver loop that pulls the master system catalog WAL through the change-stream RPC, applies the records to the local tablet, and publishes C.                                                                                                                                                                                                             |
| apply record           | WAL record written on master when a committed transaction's intents are applied; carries the commit HT. On the local tablet it turns staged intents into regular rows at that HT.                                                                                                                                                                              |
| poll safe time, S      | Hybrid time master returns with a poll response. Guarantee: every transaction that touched the system catalog and committed at or below S has been applied on master, and its apply record is in the WAL shipped up to and including this response. Master certifies it by resolving every running catalog transaction against its coordinator before capture. |
| C                      | The S of the last poll response whose records, apply records included, have all been applied to the local tablet. The local tablet's regular store is complete for every writer up to C.                                                                                                                                                                       |
| applied version, A[db] | Highest catalog version of database db whose transaction has been applied locally, as tracked by the poller.                                                                                                                                                                                                                                                   |
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
 catalog writes ─────────────► sys_catalog tablet: intents in WAL ─ ─ ─► poller: stage as external intents
 commit at HT_V ─────────────► apply record (HT_V) in WAL ─────────────► poller: apply at HT_V, regular store
                               poll response carries S ≥ HT_V ─────────► poller: all records applied → C := S
                               lock release (V) ────────────────────────► gate: wait A[db] ≥ V
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
- Poller: one per tserver, one change stream per tserver on the master system  
catalog tablet, raw WAL format. Applies write records as staged external  
intents, apply records as batch applies at the producer's commit HT, and  
schema-change records into the local tablet's metadata. Publishes C and  
A[db] after each fully applied response.
- Exposure gate: in the lock-release handler and in the heartbeat handler.
- Read redirect: in the tserver's request path for catalog reads, keyed on  
the read op's database and version.

## 4. Sync

### 4.1 Master poll service

The tserver's poller fetches the master system catalog tablet's WAL through
a request to master that carries the requester's identity and the WAL
position it has applied. Master answers with the WAL records after that
position, the position they end at, and, when it can certify one, a safe
time S. Master keeps a small in-memory record per requester (the last safe
time it computed and the WAL position that safe time is tied to); nothing
about requesters is persisted, and a new master leader starts each
requester afresh.

Master serves the request through the same routine xCluster's producer
uses for a transactional stream. The routine, as pseudo code (`state` is the
per-requester record):

```
handle_poll(from_position):
    T := current leader term; if the term changed since state was written, clear state
    leader_safe_time := this tablet's current safe time
        (every write with a hybrid time at or below it is in the WAL)

    if state.pending_safe_time is unset:
        may_compute := no catalog transaction is running on this tablet
                       or at least 250 ms passed since state.last_compute_time
                       or the caller asked to force it
        if may_compute:
            resolve_intents(leader_safe_time)          -- see below
            state.pending_safe_time     := leader_safe_time
            state.pending_safe_position := the highest WAL position that is
                                           majority-replicated right now
            state.last_compute_time     := now
        -- if not may_compute: no safe time will be certified by this poll

    records := read the WAL from from_position forward, up to a size cap
    end_position := position of the last record read
    convert each WAL entry into records:
        a write         -> one record per row written, tagged with the
                           transaction id when the write was transactional
        a transaction's -> an apply record carrying the commit hybrid time
          commit/apply     and the set of its aborted subtransactions
        a schema change -> a change-metadata record; the batch ends here
        a restore of the system catalog -> the batch is discarded and the
                           answer is "re-seed" (6.2)

    if state.pending_safe_time is set and
       (end_position >= state.pending_safe_position, or nothing was left to read):
        response.safe_time := state.pending_safe_time
        clear state.pending_safe_time and state.pending_safe_position
    return records, end_position, response.safe_time (if set)

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

What this gives the poller, and why:

- Every transaction that committed at or below S has its apply record in
  the WAL at or before `pending_safe_position`. `resolve_intents` ran
  before that position was captured: it waited for every transaction
  committed at or below S to be applied on master, and a transaction still
  running at that moment can no longer commit at or below S, because S is
  already below master's clock. So once the response whose end position
  reaches `pending_safe_position` has been shipped, the requester holds
  every record up to S.
- S is emitted only in that response. Earlier responses in the same cycle
  carry records and no safe time; the poller applies them and does not move
  C.
- Records arrive in WAL order, so a transaction's row records always
  precede its apply record.
- The 250 ms throttle: while catalog transactions are open on master, a
  new S is computed at most once per 250 ms per requester, because each
  computation costs a status round trip to the coordinators of every open
  transaction. With no catalog transaction open, every poll gets a fresh S
  and C tracks master's clock at the poll interval. The release path asks
  master to force the computation (4.4), since a safe time from before the
  DDL's commit is of no use to it.
- A batch cut short (size cap, a schema change, master's own limits) says
  so; the poller polls again at once instead of waiting out its interval.

### 4.2 Poller

The poller is one thread per tserver. It keeps the copy current and
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
way to ask a coordinator what became of a transaction. Instead the poller
stores a transaction's row records as external intents: a blob keyed by the
transaction id alone, in the intents store. No reverse mapping from a row
to its transaction exists, so a read that meets a row never finds these
writes and never asks any transaction's status. External intents exist for
one purpose: when the transaction's apply record arrives, the poller takes
the blob and writes every row in it into the regular store at the commit
time the apply record carries, then deletes the blob. A row record with no
transaction (master's own non-transactional writes) goes straight into the
regular store at its own hybrid time.

```
 master WAL                      poller                          copy
 ----------                      ------                          ----
 row r1 of txn T ─────────────►  stage under key T ────────────► intents store: T -> {r1}
 row r2 of txn T ─────────────►  stage under key T ────────────► intents store: T -> {r1, r2}
 apply T, commit time c ──────►  move T's rows to regular ────► regular store: r1@c, r2@c
                                                                 intents store: T removed
 row r3, no transaction ──────►  write at its own time ────────► regular store: r3@t
```

What C means. After the poller has applied every record of a response that
carried a safe time S, every transaction committed at or below S has had
its apply record applied here (4.1), so its rows are in the regular store.
The poller then publishes C := S. Reads are never served above C (5.3), so
no read ever depends on the intents store: any external intents present
belong to transactions that commit above C, and a read at or below C does
not look at them. That is what makes a read from the copy a plain read of
the regular store with no transaction status lookup.

What A[db] means. After publishing C, the poller reads the copy's own
catalog version table at C and records, per database, the version it
finds. A version row is written inside the DDL transaction that increments
it, in the same apply as the DDL's catalog rows, so A[db] ≥ V means the
copy holds every row of every transaction up to V for that database. A[db]
is what the freshness guarantee compares against (4.3).

The poller's loop:

1. Make sure the copy exists: open it from disk, or fetch it from master
   (Section 6). Take the requester position from the checkpoint recorded
   on disk, if any.
2. Poll master from that position (4.1). Master's answer may say "re-seed"
   or "the position is no longer retained"; both go to Section 6.
3. Apply the records in order: stage transactional rows as external
   intents; apply each apply record as one batch at its commit time, so a
   DDL's rows and its version-row update become visible together and a
   subtransaction the DDL rolled back is left out; write non-transactional
   rows at their own time; replay a schema change into the copy's metadata
   verbatim, since table ids and schema versions are the same as master's.
4. If the response carried S: read the version table at S, publish C := S
   and the new A[db] values together, wake anyone waiting on them. If it
   did not: keep C and A[db], advance only the position.
5. Record the new position on disk on a cadence (about once a second) and
   at shutdown.
6. Sleep until the next poll: a short idle interval (100 ms), cut short
   whenever master said more is waiting, or a release handler, a heartbeat
   or a read asks for an immediate poll because it is waiting for a version
   or a time the copy does not hold yet.

Rules the loop keeps:

- Apply everything before publishing. A response's S is published only
  after every one of its records is in the copy; publishing first would let
  a read at C miss rows master certified.
- Publish only forward. A safe time or a position not above the current
  one is ignored; a response with no safe time leaves C where it was. A new
  master leader may re-send an older position or withhold a safe time for a
  while; neither moves the copy backwards, and reads continue at the last
  C, which stays a complete point.
- Re-apply is harmless. The recorded position can lag what was applied, so
  a restart re-applies a suffix of the WAL. Staging the same rows under the
  same transaction key again writes the same values; an apply record whose
  blob is already gone applies nothing; a non-transactional row re-written
  at the same time is the same row. Re-delivery is a contiguous suffix in
  WAL order, so a row record is never re-applied without the apply record
  that follows it.
- The recorded position must never claim records the copy has not made
  durable ([todos T7](todos.md#27-t7-checkpoint-durable-before-the-wal)).
- A pushed batch (4.4) is applied by the same loop on the same thread, so a
  push and a poll never interleave.


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

Release path (every DDL that increments a version):

- Master reads the versions after the DDL committed and sends them in the
  lock release to every tserver with a live lease.
- The release handler does, in this order: wait until A[db] ≥ V for every
  database carried; publish the versions to shared memory; release the
  local locks; acknowledge. The DDL's client is told success only after
  every target has acknowledged.
- The wait completes by contract: the release carries the DDL's own change
  records (4.4), so applying them satisfies the check without a poll; when
  the records cannot be used, an immediate poll brings them.
- The DDL's own tserver is a target too: the synchronous release runs there
  before the commit returns to the DDL backend, and only then does that
  backend write V into its tserver's shared memory for the other backends
  on the node.

Heartbeat path (catch-up for a tserver a release did not reach, and for
versions that change outside a lock release):

- Master sends a version report only when some version changed since this
  tserver's last acknowledged report.
- The tserver publishes the report only when A[db] ≥ the carried version
  for every database in it. Otherwise it holds the report, requests an
  immediate poll, and publishes as soon as the copy has caught up; the
  versions arrive either with the release that is still in flight or with
  the poll. Single versions are never clamped, because the tserver's
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
   has a certified C from an applied poll, never the snapshot's own time
   (6.1 step 5).

### 4.4 Change records carried in the lock release

The stall this removes: without it, the release handler on every tserver waits  
for that tserver's next poll round trip before it can publish the DDL's  
version, so every DDL pays the slowest tserver's poll latency, and so does  
everything queued behind the locks the DDL holds.

- Tservers report the applied op id of their copy in each heartbeat.
- At release time master fetches one change batch from the lowest reported  
op id among the targets, certifies S once for the whole release, and slices  
the batch per target so that each tserver receives only records above its  
own reported op id. A report is stale by up to one heartbeat interval, so  
slices overlap what the tserver already holds; the tserver drops records  
at or below its applied op id.
- The tserver applies the pushed slice through the poller's own apply path,  
on the poller's thread, so push and pull never interleave. C becomes S and  
the gate passes without a poll. S is published only once the whole slice  
through its end op id has been applied.
- A push is refused, and replaced by an immediate pull, when its slice does  
not start at or below the copy's applied position, when a re-seed is in  
progress, or when the tserver has no copy. Master sends no records to a  
target whose report is missing or too old (new master leader before the  
first heartbeat), whose slice would exceed the payload cap, or which lags  
the freshest target by more than a bound; those tservers pull.
- Correctness never depends on the push: the gate is unchanged, and a  
lost, reordered or truncated push is repaired by the next pull from the  
tserver's own position. Two concurrent releases both start at or below the  
target's position; whichever applies second is filtered to nothing.

```
  master                         tserver (release handler)
  ------                         -------------------------
  DDL commits at HT_V
  read versions (V)
  from = min(reported op ids)
  one GetChanges(from) → batch, S
  per target: slice(batch, op id_t)
       ── release(V, records, S) ─────►  apply records (drop ≤ applied)
                                          C := S  (S ≥ HT_V), A[db] := V
                                          gate: A[db] ≥ V passes at once
       ◄──────── ack ─────────────────    publish V
```

The poller stays, at its own cadence, for catalog writes that happen outside  
PG DDL (master's own system catalog updates, catalog writes made outside DDL  
mode) and as the catch-up path. Cost items of the push are in implementation2.md.

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
between two scans of one snapshot as the poller applies records, and return  
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
2. Version check, a safety net: if A[db] < V, wait for the poller, at most
   one poll (bounded by a timeout, after which the read goes to master). In
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
  commit) has a commit time above C, wait until C passes it, at most one  
   poll, so that a session reads its own writes.
4. First local read of a fresh catalog snapshot: h is the clock time and is
  above C. Replace it with C on the session's read point. Every later read  
   under that snapshot finds h = that C.
5. Otherwise compare h with C. h ≤ C: serve locally at exactly h (C only
  moves forward, so any time set by step 4 stays at or below C). h > C: the  
   time came from master or from an explicit request; route this request to  
   master. Never serve at C in that case.
6. A local read below the copy's history retention is routed to master.
7. Never use the tserver clock as the local read time.

Consequences:

- Within one catalog snapshot all local reads see one state, even if the  
poller advances meanwhile; a DDL committing during the statement becomes  
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
backend's version unchanged; its rows arrive at C within a poll. The  
own-write check (rule 3) covers the writing session. Other sessions cannot  
name a temporary relation, so no cross-session read depends on it.

## 6. Bootstrap

### 6.1 Initial

1. Create the local tablet by snapshot transfer from the master system catalog
  tablet: the transfer copies the tablet's files and its metadata, including  
   every catalog table's schema. The result is a checkpoint at a committed  
   WAL position.
2. Rewrite the tablet's consensus metadata to a single-peer configuration
  (this tserver only) and open it.
3. A[db] and C are not initialized from the snapshot. The first poll
  response that carries a safe time fills A[db] for every database by  
   reading the local catalog version table at that time, and publishes it  
   together with C. Nothing about versions is persisted; a restart repeats  
   this step.
4. Start the poller at the snapshot's WAL position.
5. Enable local serving only once the poller has completed its first poll and
  applied it, so that C is a certified value, not the snapshot time.

A tserver restart with a copy on disk reopens it from local metadata without  
contacting master and resumes from the persisted checkpoint; serving is  
enabled after the first applied poll as above.

### 6.2 Re-seed

Same steps, replacing the existing local tablet (tombstone, then create).  
Triggers:

- The poll returns "checkpoint too old": master has garbage-collected WAL the  
poller had not consumed (retention is bounded on master; a tserver that lags  
beyond it cannot catch up incrementally).
- The tserver's YSQL lease lapsed and was regained (Section 7.1) and the poll  
cannot resume from its checkpoint.
- Master restored its system catalog from a snapshot (PITR). The restored  
rows are written on master outside the WAL, so no stream can carry them;  
master's change service stops at the restore marker and answers "re-seed",  
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
//R: A new-lease tserver does not re-seed in the implemented approach; the alternate method you describe is what is implemented: serving is held until a poll whose S passes the tserver's clock reading at the moment the new lease was observed, and that poll decides between incremental catch-up and re-seed ("checkpoint too old"). The race with a release that just completed does not exist: a release that did not target this tserver completed before the lease was granted, so its DDL committed before the grant, and the bound is at or above the grant; a release that computed its targets after the grant targets this tserver and runs the gate here. A restarted tserver needs no special case: the first lease of the new process is a new lease epoch and sets the same bound, and the copy reopened from disk resumes from its checkpoint. Carrying versions in the lease is not needed: the copy must poll for the rows anyway, and the time bound already forces that poll.

A tserver without a live YSQL lease is not a target of lock releases, so
master completes DDLs without this tserver running the freshness guarantee.
Every such DDL committed before master granted this tserver its next lease.
The rule:

- On loss, serving is disabled at once and a bound is set that C can never
  reach, so the copy stays out of the read path for as long as no new lease
  is granted. Lease loss also ends this tserver's PG sessions.
- On every new lease epoch, including the first lease of a freshly started
  process, the bound becomes the tserver's clock reading at the moment the
  lease is observed, which is at or above every DDL master could have
  completed without this tserver. An immediate poll is requested. Serving
  resumes on the first poll whose S passes the bound; C then covers every
  such DDL.
- The poll decides how the copy catches up: incrementally from its
  checkpoint in the normal case, or by re-seed when master answers
  "checkpoint too old" (6.2). A regain therefore does not imply a re-seed.
- A restarted tserver is the same case: its copy is reopened from disk and
  resumes from the persisted checkpoint; its first lease sets the bound;
  serving needs no master contact beyond that poll.

Why there is no race with a release that completed just before the grant:
that release did not target this tserver, so it completed, and its DDL
committed, before master granted the lease; the bound is at or above the
grant. A release that computed its targets after the grant includes this
tserver and runs the freshness guarantee here; if the copy is not serving
yet at that moment, the versions are published without waiting and the
reads that follow are covered by the version check of 5.3 rule 2 and by the
rule below.

Planned addition, closing the flip window (implementation2.md, "Login
preload in the not-serving-to-serving flip window"): a copy moves to serving
only when both hold: C is at or above the lease bound, and A[db] is at or
above every version published to shared memory while the copy was not
serving. With that, no read, including the startup preload that carries no
database, can be served from a copy that lacks a version the tserver has
already published.

Carrying the latest catalog versions in the lease response is not needed:
the copy has to poll for the rows in any case, and the time bound already
forces that poll and bounds it.

### 7.2 Poller failures

- Master leader change: the poller re-resolves the leader and continues from  
its checkpoint. Local serving stays enabled at the last C; the gate simply  
waits longer; heartbeat reports wait until the copy catches up.
- Tserver restart: the tablet and its checkpoint persist; the poller resumes;  
C is recomputed from the first applied response before serving is enabled.
- Repeated poll or apply failures: after N consecutive failures, or when C
  lags the tserver clock by more than a bound, the copy leaves the read
  path: reads go to master, the gate is skipped, heartbeat reports publish
  without waiting. Serving resumes under the rule of 7.1 on the next
  successful poll. Without this rule a stalled poller freezes C while the
  copy stays in the read path (11.3 item 1).

### 7.3 Master WAL retention

Retention on master is bounded by its ordinary log retention (15 minutes by  
default), which at the default poll cadence is ample headroom. A tserver  
whose checkpoint falls outside it re-seeds. Copies never anchor master's WAL.  
Automating a per-stream retention window is a follow-up item.

## 8. Observability

Counters and histograms on the tserver, all per tserver:

- poll latency histogram (request sent to response received);
- apply latency histogram (response received to C published);
- poll failures, apply failures, re-seeds, lease-triggered serving disables;
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
sampled lag and last-poll age, wait-time histograms and ASH wait codes, a  
per-reason breakdown of reads routed to master, a served-by-copy count in  
EXPLAIN, and debug pages on tserver and master listing the copy's state and  
master's view of every tserver's position.

## 9. Performance model

- DDL, push off (default): one extra poll round trip per tserver (release  
arrives, tserver polls), in parallel across tservers, plus master's status  
resolution when other catalog transactions are open. Push on: the records  
arrive with the release; the residue is the apply time.
- Readers: no master round trip, including connection startup. Visibility of  
a DDL to a backend lags its commit by at most one poll plus the release  
fan-out.
- Master: tservers × poll rate change-stream requests, each cheap when idle;  
status resolution round trips while catalog transactions are open, per  
stream per refresh window. Scaling of the latter with tserver count is a  
follow-up study (Section 12).
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
(checkpoint durability and ordering, poller wake-ups, read failure fallback,  
memstore flushing, compaction of staged intents, tablet-wide flags, explicit  
read times below the compacted history, release push cost, a restore inside  
the copied WAL tail) are in `implementation2.md`, "Implementation-level gaps".



### 11.1 PG side

The version check and the own-write check (5.3 rules 2 and 3) are driven by  
DDL commits only. A catalog write committed any other way is invisible to  
the next catalog read, of the writing session and of every other session, for  
up to one poll. Items 1 to 3 are the writers of that kind.

1. **Login profiles (High, decided, open; [todos T4](todos.md#24-t4-login-profile-catalogs)).** A failed login reads the role's
  failed-attempt counter, adds one and writes it back outside DDL. With the  
   copy, attempts spaced closer than one poll read the same counter, so the  
   lockout after N failures arrives late, and `ALTER ROLE ... ACCOUNT LOCK`  
   is seen late on other tservers. Plan: reads of `pg_yb_profile` and  
   `pg_yb_role_profile` always go to master. Rejected: a version bump per  
   failed login (a global bump, the catalog is shared).
2. **Large objects (High, open; [todos T5](todos.md#25-t5-large-objects)).** `lo_create`, `lowrite`, `lo_unlink`
  write `pg_largeobject` and `pg_largeobject_metadata` from the user's  
   transaction outside DDL. Another session reads them from the copy at C,  
   so an object written just before is missing or torn against the user row  
   that references it. Candidates: reads of the two catalogs always go to  
   master; or the own-write check extended to non-DDL catalog commits plus a  
   cross-session wait.
3. **Direct catalog updates under `yb_non_ddl_txn_for_sys_tables_allowed` ([todos T6](todos.md#26-t6-direct-catalog-updates-and-the-own-write-wait))
  (High, partial, TODO).** A request that carries the setting goes to  
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
  A poll response certifies its state: every record up to S is committed and  
   applied, and open transactions are staged in a form only their apply  
   record can materialize. Remote bootstrap certifies nothing: it copies the  
   tablet as it is, with (a) WAL entries master's leader appended but had not  
   committed, which the single-voter copy commits itself on open and which a  
   later master leader may truncate and replace, and (b) the row-keyed  
   intents of catalog transactions still open at that moment, whose write  
   records the poller never receives again and whose apply record then finds  
   nothing to apply; once master's coordinator forgets the transaction the  
   copy reads those rows as aborted. Either way the copy diverges from master  
   silently until the next re-seed. xCluster avoids (b) by aborting active  
   transactions on the source at bootstrap, which is not acceptable here: a  
   tserver can bootstrap at any time and that would abort in-flight DDLs  
   cluster-wide. Plan: for (a), resume from the copied log's committed  
   position and refuse to commit entries past it, and let master check the  
   term at the poller's position and answer "re-seed" on a mismatch; for  
   (b), rewrite the copied intents into the staged form before the poller  
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
   and are tolerated as one-poll lag covered by the version check of 5.3:  
   master's version read at release fails (the release carries no read time),  
   and the synchronous release exceeds its timeout (master completes it  
   asynchronously and the client is told success first).
3. **Safe time certified before a restarted master has loaded its ([todos T20](todos.md#218-t20-resolveintents-before-transaction-loading))
  transactions (Medium, open, existing issue).** After master opens its  
   system catalog tablet, the transaction participant loads existing  
   transactions in the background. A poll that arrives before that finishes  
   resolves only the loaded set, so a committed but not yet applied  
   transaction can be missed and S emitted before its apply record. This is  
   stock code shared by every transactional xCluster stream today. Plan: the  
   change service waits for loading before certifying S.
4. **Per-tserver load on master (Medium, open; [todos T21](todos.md#219-t21-master-load-per-tserver)).** Each tserver's poll reads
  its own window of master's WAL and rebuilds records, and certifies S by  
   resolving every open catalog transaction against its coordinator; both  
   scale with the number of tservers, so a catalog-heavy burst or a  
   long-open DDL costs master N × the work. Plan: compute the batch and S  
   once per cycle on master and serve every pull from that cache, sliced at  
   the caller's position. Optional further step: replace the polls with a  
   regular push from master, which is the release push of 4.4 run on a  
   timer.


### 11.3 Tserver side

1. **Statements that wait on an ad hoc poll (Medium, partial; [todos T11](todos.md#211-t11-serving-withdrawal-and-wait-fallbacks), [T36](todos.md#234-t36-wait-latency)).** In the
  common case no statement waits on the poller. A few narrow cases make a  
   statement wait until an immediate poll completes: the statement after a  
   `CREATE TEMP TABLE` (own-write check), a read that carries a version the  
   heartbeat announced before the copy had it (item 2), and a login in the  
   flip window (implementation2.md). If master is unreachable, that wait runs to  
   its timeout (5 s) and the read then goes to master, which fails as well;  
   the tserver is about to lose its lease in that state anyway. Fallback  
   needed: bound the wait once per statement, not once per request, and take  
   the copy out of the read path after repeated poll failures so that the  
   gate is skipped, heartbeat publication resumes and reads go to master  
   (7.2). Measured consequence without it: one tserver whose  
   poller is stuck holds every DDL in the cluster at its gate for the  
   release deadline.
2. **A heartbeat announces a version ahead of the DDL's release (Medium, [todos T1](todos.md#21-t1-freshness-guarantee-on-applied-versions);
   open).** A tserver that the release has not reached yet, or whose copy is
   behind, can learn a new version from the heartbeat first. The report is
   held until the copy's applied versions reach it and is published then
   (4.3); the wait ends when the release's records or the immediate poll
   arrive, so it costs at most one poll. It must not cost a heartbeat
   interval on top: a held report is published as soon as the copy catches
   up, not at the next heartbeat. Backends without a database are covered by
   the serving condition of 7.1.

## 12. Follow-up items

- Load study: a standalone poll client, independent of the tserver, running  
as many instances as a large cluster would have tservers against a real  
master, to measure change-stream and status-resolution load. First step  
with most of the saving: 11.2 item 4.
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
