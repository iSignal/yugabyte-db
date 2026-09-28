# Tserver-local DocDB copy of the PG catalog — design

Date: 2026-09-03. Supersedes `plan.md` for the read model and the gate; keeps
its bootstrap and sync decisions. Scope: object locking and concurrent
(transactional) DDL enabled, per-database catalog versions, invalidation
messages on.

## 1. Goal

Serve catalog reads issued by DML statements from a copy of the PG catalog that
lives on the tserver, so that a catalog cache miss costs a local read instead
of a round trip to master. Master stays the only writer. The copy is kept
current by pulling the master system catalog tablet's WAL. Correctness contract:
a backend that has been told catalog version V reads a copy that contains every
catalog change up to V, and every read under one catalog snapshot sees one
consistent state.

## 2. Reference

| Term | Meaning |
|---|---|
| catalog version, V | Per-database counter incremented inside every DDL transaction. Version order equals commit order within a database. |
| local version | The version a backend believes it is at. Advances when it applies invalidation messages or commits its own DDL. |
| shared version | Latest version the tserver has published to backends through shared memory. |
| commit HT | Hybrid time at which a DDL transaction committed. |
| local tablet | Single-replica DocDB tablet on the tserver holding a copy of the master system catalog tablet: same table ids, same schemas, same row keys. |
| poller | Per-tserver loop that pulls the master system catalog WAL through the change-stream RPC, applies the records to the local tablet, and publishes C. |
| apply record | WAL record written on master when a committed transaction's intents are applied; carries the commit HT. On the local tablet it turns staged intents into regular rows at that HT. |
| poll safe time, S | Hybrid time master returns with a poll response. Guarantee: every transaction that touched the system catalog and committed at or below S has been applied on master, and its apply record is in the WAL shipped up to and including this response. Master certifies it by resolving every running catalog transaction against its coordinator before capture. |
| C | The S of the last poll response whose records, apply records included, have all been applied to the local tablet. The local tablet's regular store is complete for every writer up to C. |
| applied version, A[db] | Highest catalog version of database db whose transaction has been applied locally, as tracked by the poller. |
| exposure gate | The tserver publishes version V for database db to shared memory only after C ≥ commit HT of V. |
| cache miss reads | Catalog scans that fill the catcache or build a relcache entry. |
| internal catalog reads | Catalog scans issued by PG internals during DML on catalogs without a catcache, and direct heap scans. |
| user catalog reads | Executor scans of catalog tables from user SQL. |
| in DDL | The backend is executing a DDL statement, or is in a transaction block in which a DDL already ran. |
| catalog snapshot | PG's per-backend catalog snapshot. Created on first catalog read, dropped on invalidation (statement start in `READ COMMITTED`, accepted invalidation messages otherwise). Its read point is the DocDB read time for all catalog reads under it. |

## 3. Architecture

```
 DDL backend (tserver T1)      master                                tserver T2
 ------------------------      ------                                ----------
 catalog writes ─────────────► sys_catalog tablet: intents in WAL ─ ─ ─► poller: stage as external intents
 commit at HT_V ─────────────► apply record (HT_V) in WAL ─────────────► poller: apply at HT_V, regular store
                               poll response carries S ≥ HT_V ─────────► poller: all records applied → C := S
                               lock release (V, HT_V) ─────────────────► gate: wait C ≥ HT_V
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
- Read redirect: in the tserver's perform path for catalog reads, keyed on
  the read op's database and version.

## 4. Sync

### 4.1 What a poll response guarantees

- Records are shipped in master WAL order. Write records of a transaction
  arrive before its apply record.
- S is reported only when every record up to the WAL position captured with S
  is included in the response or an earlier one. Before capturing S, master
  waits until its own tablet has applied every transaction committed at or
  below S; running transactions are resolved against their coordinators so
  that none can still commit at or below S.
- Therefore, after the poller has applied every record of the response, the
  local regular store holds every row committed at or below S. Reads at or
  below S need no intent resolution and cannot observe a row appearing later.

### 4.2 Poller obligations

- Apply all records of a response before publishing its S as C. Publishing
  first would let a read at C miss rows that master certified.
- Apply records of one transaction as one batch at the producer's commit HT
  (existing external-transaction apply), so a DDL's rows and its version-row
  update become visible together.
- Replay schema-change records into the local tablet's metadata verbatim
  (add table, alter schema, add multiple tables, remove table). Table ids and
  schema versions are identical to master's, so no remapping.
- Maintain A[db]: after applying a response, read the local catalog version
  table for every database whose version row was written in that response
  and record the value with the commit HT of the transaction that wrote it.
- Persist the poll checkpoint (WAL position) locally with the tablet on a
  cadence and at shutdown, so a tserver restart resumes from it. Because the
  checkpoint may lag the last applied record, a restart re-applies a bounded
  suffix; every local write must therefore be idempotent: re-staging an
  intent writes the same key, value and time; re-applying a transaction whose
  intents are already converted does nothing; re-delivery is a contiguous
  suffix in WAL order, so a write record is never re-applied without the
  apply record that follows it.
- Publish monotonically: a response that carries no safe time (a new master
  leader has not yet completed its first certification) applies its records
  and advances the applied versions and the position, but leaves C unchanged;
  a safe time or a checkpoint that is not above the current value is ignored.
  Reads continue at the last C, which stays a complete point for the copy.
- Poll cadence: a short idle interval, and an immediate poll when a lock
  release or heartbeat announces a version the local copy does not yet have.

### 4.3 Version gate

- Lock release: master already sends the new versions and invalidation
  messages with the release request. Master adds the hybrid time it read the
  versions at (at or after the DDL's commit HT). The tserver waits until
  C ≥ that time, then publishes the versions to shared memory and
  acknowledges. The DDL's client sees success only after every tserver has
  acknowledged, so a backend that acquires a lock after the release finds its
  local copy already containing the DDL.
- Heartbeat: the heartbeat response carries the same version-read time. While
  C is below it, the tserver skips the response's version data entirely and
  triggers an immediate poll; the next heartbeat re-sends the full report, so
  nothing is lost. Clamping individual versions is not an option: the
  tserver's version table treats a version below the published one as a
  stale master report and aborts the process after a grace period, and
  omitting a database from a report deletes it from the table.
- The DDL's own tserver is covered by the same gate: the release handler runs
  there too, synchronously, before the commit returns to the client, so by
  the time the DDL session issues its next read the local copy already holds
  V. No separate wait is needed for it.

## 5. Reads

### 5.1 What is served locally

All three read kinds, when the backend is not in DDL:

| Read kind | Served from | Why C is the right time |
|---|---|---|
| Cache miss reads | Local tablet at the catalog snapshot's time, which the first local read fixes to C | The catcache needs a state at or beyond the backend's version and consistent across catalogs for the life of the catalog snapshot. C ≥ HT_V by the gate, and C is a complete point for every writer, so all misses under one snapshot see one state. |
| Internal catalog reads | Local tablet at the same time as cache miss reads of the same snapshot | They run under the same catalog snapshot; serving them from the same copy at the same time is what keeps them consistent with the cached entries. Catalogs without a catcache included; the read time is not refreshed per scan for them. |
| User catalog reads | Today: they run under the catalog snapshot, so exactly as cache miss reads. If they move to the transaction's read time h: local tablet at h when h ≤ C, master otherwise | `REPEATABLE READ` and `SERIALIZABLE`: h was fixed at transaction start and is normally ≤ C, so the read is exact at h. `READ COMMITTED`: h is the statement's read time and normally > C; such a read is not served from the copy at all rather than served at an older time under a snapshot that expects h. |

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
- Scans with non-MVCC snapshots (self, dirty, any) and historic snapshots.
  All are DDL-side or logical-decoding paths.
- While the tserver is bootstrapping or re-seeding the local tablet, or the
  poller is stopped (Section 7). Reads go to master in that state only.

### 5.3 Picking the local read time

Background: the PG catalog snapshot holds no hybrid time, only a read-point
serial number. The tserver session keeps the hybrid time per serial, and on a
fresh catalog snapshot it assigns a time from its own clock before any read is
issued. The redirect works on that per-serial time.

Rule, evaluated on the tserver for each catalog read op:

1. The op carries the backend's database, version V and its read-point
   serial; the session's read point for that serial holds a time h (a clock
   time on the first read of a fresh catalog snapshot, or the time fixed by
   an earlier read).
2. Version check first, for ops that carry a version. Cache miss reads and
   internal catalog reads carry none (PG does not stamp the version on
   internal scans of system relations); their freshness is guaranteed by the
   exposure gate alone. User catalog reads carry V: if A[db] < V, wait for
   the poller, at most one poll. This wait matters in two residual paths: a
   version delivered by heartbeat while the copy is behind, and a release
   acknowledged while the copy was not serving. Never fix a read time before
   this check.
3. First local read of a fresh catalog snapshot: h is the clock time and is
   above C. Replace it with C on the session's read point. Every later read
   under that snapshot finds h = that C.
4. Otherwise compare h with C. h ≤ C: serve locally at exactly h (C only
   moves forward, so any time set by step 3 stays at or below C). h > C: the
   time came from master or from an explicit request; route this op to
   master. Never serve at C in that case.
5. A local read below the copy's history retention is routed to master.
6. Never use the tserver clock as the local read time.

Consequences:

- Within one catalog snapshot all local reads see one state, even if the
  poller advances meanwhile; a DDL committing during the statement becomes
  visible at the next invalidation, as in PG.
- A snapshot that started on master (tserver not serving, backend in DDL)
  stays on master until it is dropped; no mixing of sources within a
  snapshot.

Transactions with an attached distributed transaction (2026-09-03): the
implementation initially refused requests whose plain session carried a
distributed transaction, on the ground that the session's read point is then
the transaction's own and writing C there would move the transaction's data
snapshot. That is not how the session behaves: the catalog snapshot has its
own read-point serial, and on every switch to it the tserver saves the
current read point (the transaction's) into history under the transaction
serial, then either restores the catalog serial's time into the same object
or resets it and writes the clock time there. When the transaction serial
returns, the transaction's time is restored from history. The existing master
path therefore already reuses the transaction's read point object for the
catalog excursion; writing C into it is the same write the clamp performs
and is undone the same way. The exclusion is being removed, verified by a
repeatable-read test that a locally served catalog miss after a write does
not change the transaction's data snapshot. Until that test passes, catalog
misses after a write in a transaction block go to master.

Why the request-level version wait is required even though cache miss reads
carry no per-op version: while the copy is not serving, the gate publishes
versions without waiting. When the first applied response then flips the copy
to serving, its safe time can be below the time master read those versions
at, so a backend already at V could read a copy that lacks V's rows. The
backend's version travels on the request (not on the op) for this reason.
- The DDL's own tserver needs no special handling: its release handler
  completed before the commit returned, so the copy already holds V.
- A DDL that does not increment the version (temporary relations) leaves the
  backend's version unchanged; its rows arrive at C within a poll. The
  writing backend records the commit HT of its own catalog-writing
  transaction and requires C ≥ that HT before its next local read, so it reads
  its own writes.

## 6. Bootstrap

### 6.1 Initial

1. Create the local tablet by snapshot transfer from the master system catalog
   tablet: the transfer copies the tablet's files and its metadata, including
   every catalog table's schema. The result is a checkpoint at a committed
   WAL position.
2. Rewrite the tablet's consensus metadata to a single-peer configuration
   (this tserver only) and open it.
3. Initialize A[db] for every database by reading the local catalog version
   table; initialize C as the snapshot's safe time.
4. Start the poller at the snapshot's WAL position.
5. Enable local serving only once the poller has completed its first poll and
   applied it, so that C is a certified value, not the snapshot time.

### 6.2 Re-seed

Same steps, replacing the existing local tablet (tombstone, then create).
Triggers:

- The poll returns "checkpoint too old": master has garbage-collected WAL the
  poller had not consumed (retention is bounded on master; a tserver that lags
  beyond it cannot catch up incrementally).
- The tserver's YSQL lease lapsed and was regained (Section 7.1) and the poll
  cannot resume from its checkpoint.
- Local tablet failure (corruption, disk loss, metadata mismatch).

During re-seed local serving is disabled; catalog reads go to master, and the
version gate publishes versions without waiting (the copy is not consulted).

## 7. Failure handling

### 7.1 Lease loss and regain

While the tserver's YSQL lease is lost, master may complete DDLs without this
tserver acknowledging lock releases, so versions advance without the gate
having run here. On regain:

- Local serving is disabled immediately on loss and stays disabled until the
  poller has applied a response whose S is at or after the time master
  granted the new lease, so that every version published during the gap is
  present locally.
- The poll must block publishing versions from the lease refresh or heartbeat
  until that condition holds; otherwise backends would learn versions the
  copy lacks.
- If the poll fails with "checkpoint too old", re-seed (6.2).

Detailed handling of lease loss is a follow-up item; the design requirement is
the two rules above.

### 7.2 Poller failures

- Master leader change: the poller re-resolves the leader and continues from
  its checkpoint. Local serving stays enabled at the last C; the gate simply
  waits longer; versions arriving by heartbeat are clamped.
- Repeated poll or apply failures: exponential backoff, counters incremented,
  local serving disabled after a configurable number of consecutive failures.
- Tserver restart: the tablet and its checkpoint persist; the poller resumes;
  C is recomputed from the first applied response before serving is enabled.

### 7.3 Master WAL retention

Retention on master is bounded by the change-stream retention policy. A
tserver whose checkpoint falls outside it re-seeds. Automating the retention
window and the re-seed trigger is a follow-up item.

## 8. Observability

Counters and histograms on the tserver, all per tserver:

- poll latency histogram (request sent to response received);
- apply latency histogram (response received to C published);
- poll failures, apply failures, re-seeds, lease-triggered serving disables;
- lag histogram: C behind the tserver clock at each publish;
- gate wait histogram: time the lock-release handler waited for C;
- local catalog reads served, local reads that waited for A[db], reads routed
  to master and the reason (in DDL, not serving, snapshot kind).

## 9. Performance model

- DDL: one extra poll round trip per tserver (release arrives, tserver polls),
  in parallel across tservers, plus master's status resolution when other
  catalog transactions are open.
- Readers: no master round trip. Visibility of a DDL to a backend lags its
  commit by at most one poll plus the release fan-out.
- Master: tservers × poll rate change-stream requests, each cheap when idle;
  status resolution round trips while catalog transactions are open, per
  stream per refresh window. Scaling of the latter with tserver count is a
  follow-up study (Section 10).

## 10. Modes in which the local tablet is not used

- initdb: no redirect. Initdb mode forces the legacy catalog-ops path, which
  never marks reads for redirect; the local tablet is bootstrapped only after
  master reports initdb complete.
- Sys-table prefetching (catalog preload at backend start and after a full
  cache refresh) and parallel workers also run on the legacy path and read
  master. Preload traffic is bounded by the response cache as today, not by
  this design. Measured consequence: over five cold-backend runs of one query,
  master saw 2 catalog reads with the copy serving against 40 with it out of
  the read path; the 2 are connection startup (authentication and preload) on
  the legacy session. Zero is reachable only by moving preload onto the copy,
  which is separate work.
- YSQL upgrade migrations: upgrade-mode sessions read master; ordinary
  sessions are unaffected.
- Major version upgrade: postponed. While an upgrade is in progress the
  tserver disables serving and routes all catalog reads to master; two
  catalog version tables exist in that window and some DDLs still run, so the
  single-version gate does not describe the cluster. Support is a follow-up.

## 11. Follow-up items

- Lease loss and regain: full handling per 7.1.
- Master WAL retention and automatic re-seed.
- Load study: a standalone poll client, independent of the tserver, running
  as many instances as a large cluster would have tservers against a real
  master, to measure change-stream and status-resolution load.
- Sharing master's status resolution across streams on the same source tablet
  if the study shows it dominates.
- Send a DDL's changed DocDB table list in the lock release, so a tserver can
  evict those table cache entries instead of emptying the whole database's.

  **The problem.** On a tserver a region away from master, the first query on a
  new connection after any DDL takes about 9 s where it otherwise takes 0.5 s.
  Measured on the three-region cluster with 100 ms one way to node 3: `ALTER
  TABLE t ADD COLUMN` on node 1, then `SELECT * FROM t01_r1` on a fresh backend
  on node 3, 8.9 s against 0.46 s in steady state. `t01_r1` is not the table that
  was altered.

  **What it is not.** It is not the local catalog copy, and it is not catalog
  reads. The per-request routing log (`--log_local_catalog_read_routing`)
  shows every catalog request in that window answered by the copy and none sent
  to master; master's catalog read count moves by 2; ASH records one
  `CatalogRead` sample out of about a hundred. Turning the copy off makes the
  same query 20.1 s instead of 10.1 s, so the copy is already removing half of
  this.

  **The root cause.** Master's RPC counters across one slow query show
  `MasterDdl::GetTableSchema` 52 times, about 200 ms each, which is the whole of
  the 9 s. Those are misses in the tserver's `PgTableCache`, which holds the
  DocDB schema of each table. A DDL empties it: master's new catalog version for
  the database reaches the tserver, `SetYsqlDBCatalogVersionsUnlocked` puts the
  database in `db_oids_updated` (`src/yb/tserver/tablet_server.cc:1696`), that
  calls `InvalidatePgTableCache` (`:1884`), and
  `PgTableCache::Impl::InvalidateDbTables` clears the whole per-database map
  (`src/yb/tserver/pg_table_cache.cc:243-266`, whose own log line reads
  "Invalidating entire table cache of database"). The only thing the
  invalidation is keyed on is the database's catalog version, which says that
  something in the database changed and nothing about what, so every table's
  entry is dropped and every table the next backend touches is re-fetched from
  master one round trip at a time. An existing connection does not feel this
  because commit `ad458d744db` made the PG-side cache evict selectively; a new
  connection starts with an empty PG cache and falls through to the tserver's.
  That commit says so itself: it "only optimizes PG table cache, not tserver
  table cache ... at tserver side, it does not understand the invalidation
  messages yet so the entire cache is still cleared", and new connections
  "continue to behave as currently".

  **The proposed fix.** Master already knows which tables a DDL transaction
  changed, because that set is what its own DDL verification runs on. Put that
  set in the lock release and have each tserver evict exactly those entries
  instead of the database's whole map.

  1. The set is `CatalogManager::YsqlDdlTransactionState`, held per transaction
     in `ysql_ddl_txn_verfication_state_map_`
     (`src/yb/master/catalog_manager.h:3417-3436`, `:3456`). It carries `tables`,
     "the table info objects of the tables affected by this transaction", and
     `nochange_tables`, "set of tables whose DocDB schema do not change".
     `tables` minus `nochange_tables` is precisely the set whose DocDB schema
     moved, which is precisely what a tserver's table cache has to drop.
  2. The set is authoritative rather than advisory. It is the same state the
     verifier acts on, built on master as it processes the DDL's own
     `CreateTable`, `AlterTableWithBatchTracker` and `DeleteTable` requests
     (`src/yb/master/catalog_manager.cc:4925`, `:8449`, `:7301`). If it were
     incomplete, DDL verification itself would be wrong, so completeness is
     already a requirement the system enforces elsewhere and this change does
     not add a new one.
  3. The release is issued after verification has finished and is synchronous,
     so at that moment the set is final and every tserver is reached:
     `DdlAtomicityFinishTransaction` calls `WaitForDdlVerificationToFinish` and
     only then `ReleaseObjectLocksIfNecessary(..., kSync)`
     (`src/yb/tserver/pg_client_session.cc:3271-3296`).
  4. Master already fills cache-update fields into this very request, in
     `ObjectLockInfoManager::Impl::PopulateDbCatalogVersionCache`
     (`src/yb/master/object_lock_info_manager.cc:1178-1210`), which is where the
     new list goes. `ReleaseObjectLockRequestPB` already carries
     `db_catalog_version_data` for the same purpose; field 7's comment says it is
     there "to optimize the case where a DDL release causes the catalog-version
     to increase -- where the release request can update the TServers instead of
     waiting for the next heartbeat" (`src/yb/tserver/tserver.proto:539-551`).
     The tserver consumes it in `ts_local_lock_manager.cc:538-606`, which calls
     `SetYsqlDBCatalogVersions`, which is the call that empties the cache. The
     new list lands in the same message and is applied at the same point.

  Not PG's `ddl_transaction_state.altered_table_ids`
  (`src/postgres/src/backend/utils/misc/pg_yb_utils.c:2714-2728`), which was the
  first idea and is the wrong source. It is per backend and never sent to master,
  it is populated at only two call sites, both on the ALTER path
  (`src/postgres/src/backend/commands/yb_cmds.c:1778`, `:1919`), and master builds
  its verification state independently of it, so a gap in that list would not be
  caught by verification. It exists to evict the backend's own cache entries in
  `YbInvalidateTableCacheForAlteredTables`
  (`src/postgres/src/backend/utils/misc/pg_yb_utils.c:4700-4730`), which is the
  local counterpart of what this item does for every other tserver.

  Things to settle before writing it:
  - Keep the present whole-database clear as the fallback for any release that
    does not carry the list, including retries, since field 7's comment already
    warns that a retried release may carry stale information, and for a DDL that
    master did not track verification state for.
  - A global impact DDL already skips per-database granularity and clears
    everything (`src/yb/tserver/tablet_server.cc:1881`); that stays as it is.
  - `PopulateDbCatalogVersionCache` carries its own TODO that it sends every
    database's catalog version "because the cache invalidation logic on the
    tserver side expects a full report"; the two coarsenesses are in the same
    function and could be narrowed together.
  - `db_catalog_inval_messages_data` is already field 10 of the same request, so
    a tserver that learned to read invalidation messages could reach a similar
    result without a new field. Master's verification set is preferable: it is
    exact rather than inferred, and it is the set the system already has to get
    right.

- Master-driven fan-out on the DDL path, slow poll otherwise. Plan:
  1. Tservers report the applied op id of their copy in heartbeats.
  2. The lock-release message for a DDL carries the change records from that
     tserver's reported op id to the DDL's commit, and the S certified once
     for the whole release. Master resolves running catalog transactions once
     per release, not once per tserver per poll. The tserver applies, its C
     reaches S, and the gate passes without a poll round trip.
  3. The poller stays, at a low cadence (order of a minute), for catalog
     writes that happen outside PG DDL (master's own system catalog updates)
     and as the catch-up path: a tserver that receives a push starting above
     its applied op id (missed push, reordered RPC) refuses it and pulls from
     its own position.
  Issues to settle:
  - Pushing only at lock release is not enough on its own: non-DDL catalog
    writes accumulate between DDLs, so a release after a long DDL-free period
    would carry a large batch; the slow poll bounds that backlog.
  - After a master leader change the new leader does not know any tserver's
    op id until heartbeats arrive; it must not push until it has a fresh
    report from the target tserver, and tservers pull meanwhile. The same
    holds for a tserver whose report is stale beyond a bound.
  - A tserver far behind (beyond a lag cap) is not served by push; it pulls
    or re-seeds, and WAL retention is bound to the capped minimum op id.
  - S certified for a batch is valid for a tserver only once it has applied
    through the batch end; a size-truncated push must say so, and the
    tserver finishes by pull before publishing S.
  - Own-writes floor for DDL without a version increment (temporary
    relations) keeps working because the wait for it triggers an immediate
    pull; the slow cadence does not lengthen it.
  Cheapest first step with most of the CPU saving: keep pulls, compute the
  batch and S once per cycle on master, serve every pull from that cache
  sliced at the caller's op id.

## 12. Change records carried in the lock release (planned)

### 12.1 The stall this removes

Today a DDL's commit is followed by a lock-release fan-out to every tserver
with a live lease. Each release handler holds the new catalog versions back
until the local copy's safe time C reaches the time master read those
versions, and C only moves when the copy's poller happens to run. The DDL,
and everything queued behind the locks it holds, therefore waits for the
slowest tserver's next poll round trip to master, even though most
tservers' backends have no interest in the changed objects. Requests that
carry a version the copy has not applied wait the same way.

### 12.2 Design

- The release request carries the change records themselves plus the safe
  time S master certified after the DDL's commit. The tserver applies the
  records, sets C to S, and the gate wait passes without a poll.
- Master learns each tserver's applied op id from the heartbeat request. At
  release time it fetches one change batch from the lowest applied op id
  among the release's targets, resolves the running catalog transactions once
  to certify S, and slices the batch per target so that each tserver receives
  only records above its own reported op id. The reported op id is stale by up
  to a heartbeat interval, so slices overlap what the tserver already holds;
  the tserver drops records at or below its applied op id.
- The pushed batch is applied through the same path the poller uses, on the
  poller's thread (queued and woken), so push and pull never interleave. A
  push whose first record is not the successor of the copy's applied op id
  is refused and replaced by an immediate pull. The poller stays as the
  catch-up path and continues at its own cadence for catalog writes that
  happen outside DDL.
- S is published only when the whole batch through its end op id has been
  applied. Master caps the push payload; when a target's slice would exceed
  the cap, master sends no records to that target and the tserver pulls as
  today.
- Targets without a fresh op id report (new master leader before the first
  heartbeat, a tserver whose report is older than a bound) receive no
  records and pull. The version read time in the release is unchanged and
  the gate is unchanged, so correctness never depends on the push.

```
  master                         tserver (release handler)
  ------                         -------------------------
  DDL commits at HT_V
  read versions at R ≥ HT_V
  from = min(reported op ids)
  one GetChanges(from) → batch, S
  per target: slice(batch, op id_t)
       ── release(V, R, records, S) ──►  apply records (drop ≤ applied)
                                          C := S  (S ≥ HT_V ≥ ... )
                                          gate: C ≥ R passes at once
       ◄──────── ack ─────────────────    publish V
```

### 12.3 Why min over reported op ids and not a fixed lookback

A fixed lookback ("the last minute of op ids") sends the same records to
every target, cannot tell which targets it fails to cover, and still needs
each target to detect the gap. The reported op id gives master the exact
start per target and lets it decide, before sending, which targets will
pull instead. The lookback survives only as the payload cap: a target more
than the cap behind is not served by push.

### 12.4 Failure cases

- Push lost or reordered: the next push or poll from the tserver's own op id
  repairs it; S from a lost push is never published.
- Master leader change: no op id reports yet, so releases carry no records
  until heartbeats arrive; tservers pull.
- Two DDLs release concurrently: both pushes start at or below the target's
  applied op id; whichever applies second is filtered to nothing.
- Lease-less tserver: not a release target, unchanged.

