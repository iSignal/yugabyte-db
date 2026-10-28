# TODOs — tserver-local DocDB copy of the PG catalog

Every open and closed item for the feature, in one place. The design is
[plan2.md](plan2.md); the current code is described in
[implementation2.md](implementation2.md); the chronological record is
[worklog2.md](worklog2.md). Code pointers are against branch
`docdb_local_catalog` at `8cd70bea5b4` (worktree `repo6`) and drift; symbol
names are the stable pointers.

Since 2026-10-07 master pushes the system catalog WAL to the copies
([push-plan.md](push-plan.md)); the tserver no longer polls master. The tserver
class `LocalCatalogPoller` (`local_catalog_poller.cc`) became
`LocalCatalogApplier` (`local_catalog_applier.cc`), and its thread applies
pushed pages and sends floor requests. Closed items, history paragraphs and
observations dated before 2026-10-07 still say "poll" and "poller" and describe
the pull path as it was.

Severity: **High** = silent wrong results, corruption, outage or a hang in a
plausible scenario; **Medium** = bounded staleness, latency or availability
cost, or cost at scale; **Low** = minor. Status: **open**, **decided** (the
approach is fixed, not coded), **closed** (in the code and tested, or dropped).

## 1. Summary

### 1.1 Open, High

| Id | Item | plan2 | Details |
|---|---|---|---|
| T3 | Remote bootstrap copies state the copy must not trust: uncommitted WAL tail and open transactions' intents | [§11.2 item 1](plan2.md#112-master-side) | [2.3](#23-t3-remote-bootstrap-copies-untrusted-state) |
| T7 | Checkpoint file durable before the WAL it covers | [§4.2](plan2.md#42-applier) | [2.7](#27-t7-checkpoint-durable-before-the-wal) |
| T8 | User catalog reads: the standalone-statement guard (step 1) | [§14.2](plan2.md#142-proposal) | [2.8](#28-t8-user-catalog-reads-guard) |
| T9 | A failed read from the copy becomes a query error, no fallback, no trace | [§8](plan2.md#8-observability) | [2.9](#29-t9-failed-local-read-fallback) |
| T38 | Schema-change records: re-apply is safe except a table-schema record for a removed table (T45 skips it); decided 2026-10-18: persist the checkpoint just before and just after each schema-change record, so no replay crosses a schema change | [§4.2](plan2.md#42-applier) | [2.38](#238-t38-schema-change-records-re-apply-and-ordering) |
| T43 | PITR: a new connection reads the catalog from before the restore until the copy gets the restore page (fails 10 of 15 local runs of `PgsqlSequencePartialCleanupAfterRestore`); every restore re-seeds every copy. Plan: apply the restore in place on the copy, keep master's catalog history on the copy, master waits for the copies before RESTORED (2026-10-10) | [§6.2](plan2.md#62-re-seed) | [2.43](#243-t43-pitr-restore-reaches-the-copies-in-place) |

### 1.2 Open, Medium

| Id | Item | plan2 | Details |
|---|---|---|---|
| T1 | Performance: the release gate and the heartbeat hold compare the copy's safe time with master's version read time; compare applied versions instead, so that a report is not held for commits it does not depend on (correct today; reclassified from High 2026-10-10) | [§4.3](plan2.md#43-freshness-guarantee) | [2.1](#21-t1-publish-gate-on-applied-versions) |
| T4 | Login profiles disabled while the copy is enabled (decided 2026-10-09) | [§11.1 item 1](plan2.md#111-pg-side) | [2.4](#24-t4-login-profile-catalogs) |
| T5 | Large objects disabled while the copy is enabled (decided 2026-10-09) | [§11.1 item 2](plan2.md#111-pg-side) | [2.5](#25-t5-large-objects) |
| T6 | Direct catalog writes under `yb_non_ddl_txn_for_sys_tables_allowed` disabled while the copy is enabled, except upgrade and initdb (decided 2026-10-09) | [§11.1 item 3](plan2.md#111-pg-side), [§5.3 rule 3](plan2.md#53-picking-the-local-read-time) | [2.6](#26-t6-direct-catalog-updates-and-the-own-write-wait) |
| T39 | Re-seed storms: no bound on concurrent copy fetches from master; seeding from a peer tserver's copy (design review 2026-10, Hari, Kannan) | [§6.2](plan2.md#62-re-seed) | [2.39](#239-t39-re-seed-storms-and-peer-seeding) |
| T40 | Database clone may re-seed every tserver's copy (design review 2026-10, Hari) | [§6.2](plan2.md#62-re-seed) | [2.40](#240-t40-database-clone-and-re-seed) |
| T11 | Serving withdrawal after repeated push or apply failures; wait computed once per statement; timed-out read falls back to master | [§7.2](plan2.md#72-push-and-apply-failures), [§11.3 item 1](plan2.md#113-tserver-side) | [2.11](#211-t11-serving-withdrawal-and-wait-fallbacks) |
| T12 | Own-write check skipped on a stale version record | [§5.3 rule 3](plan2.md#53-picking-the-local-read-time) | [2.12](#212-t12-own-write-check-version-record) |
| T13 | A session sees a change and then loses it after a master fallback (implemented 2026-10-07, see 2.13; `MasterSnapshotTimeIsFloorForNextLocalRead` passes in release and fails when the session wait is disabled (checked 2026-10-09); not yet run in CI) | [§11.1 item 6](plan2.md#111-pg-side) | [2.13](#213-t13-session-wait-after-a-master-fallback) |
| T14 | Heartbeat report held and published on catch-up instead of skipped and re-sent (part of T1); held report implemented 2026-10-07, published at the next heartbeat after catch-up, not at once | [§11.3 item 2](plan2.md#113-tserver-side) | [2.1](#21-t1-publish-gate-on-applied-versions) |
| T15 | Re-seed on corruption detected while running | [§6.2](plan2.md#62-re-seed) | [2.14](#214-t15-re-seed-on-runtime-corruption) |
| T17 | Changed-table list in the lock release for the tserver table cache | [§12](plan2.md#12-follow-up-items), [§9](plan2.md#9-performance-model) | [2.16](#216-t17-changed-table-list-in-the-release) |
| T18 | PG backend flow: remove preloading, cache reset instead of full refresh, invalidation messages from the copy | [§13](plan2.md#13-how-the-pg-backend-flow-changes) | [2.17](#217-t18-pg-backend-flow) |
| T19 | User catalog reads step 2: serve a flagged scan from the copy when its read time is at or below C | [§14.2](plan2.md#142-proposal) | [2.8](#28-t8-user-catalog-reads-guard) |
| T20 | Safe time certified before a restarted master has loaded its transactions (existing xCluster issue) | [§11.2 item 3](plan2.md#112-master-side) | [2.18](#218-t20-resolveintents-before-transaction-loading) |
| T22 | A stale checkpoint file outlives its tablet | [§6.2](plan2.md#62-re-seed) | [2.20](#220-t22-stale-checkpoint-file) |
| T23 | The copy's memtables are outside the global memstore flusher | — | [2.21](#221-t23-memstore-flusher) |
| T24 | Master WAL retention policy per stream and automatic re-seed | [§7.3](plan2.md#73-master-wal-retention) | [2.22](#222-t24-master-wal-retention) |
| T25 | Major version upgrade: decided 2026-10-17, no special case; the copy keeps serving and the guard is removed; open: table removal records, a faster periodic push while the upgrade runs, PG start before `MONITORING` | [§10](plan2.md#10-modes-in-which-the-local-tablet-is-not-used) | [2.23](#223-t25-major-version-upgrade) |
| T26 | Observability holes and additions | [§8](plan2.md#8-observability) | [2.24](#224-t26-observability) |
| T27 | Java regress schedules with the feature on; two CI items not yet explained | — | [2.25](#225-t27-test-coverage-and-open-ci-items) |
| T36 | Wait latency: the own-write wait applies to every read of the session; wait timeouts counted as "not serving" (items 1-2 closed by floor requests) | [§4.1](plan2.md#41-master-push-service) | [2.34](#234-t36-wait-latency) |
| T35 | Filter master's own records (tablet metadata, leader changes, OID allocation) out of the push stream and the remote-bootstrap snapshot: the copy cannot read them and they are pushed to every tserver on every change (design review 2026-10-24, Mark, Nita); raised from Low | [§12](plan2.md#12-follow-up-items) | [2.33](#233-t35-non-pg-rows) |
| T47 | xCluster DDL queue handler: check its catalog reads (the consistency GUC and the read path it uses when applying replicated DDLs) against the copy; nothing is special about `CREATE EXTENSION` itself (design review 2026-10-24, Mark) | — | [2.47](#247-t47-xcluster-ddl-queue-handler) |
| T48 | Release timeout: the DDL's tserver today responds success when the synchronous release times out (`ts_local_lock_manager.cc:985-991`); the design returns an error (effect unknown) and exposes each in-progress release per lease holder in master's UI and metrics so the user can follow it to completion (design review 2026-10-24) | [design §7.4](local_catalog_docdb_design.md) | [2.48](#248-t48-release-timeout) |
| T44 | The release's floor moves on every retry that master processes again, so a tserver slower than one attempt never completes the release; fix: a floor that does not change between attempts, preferably the newest commit time among the versions in the release (2026-10-17) | [simplified §4.1](simplified_with_object_locks.md#41-lock-release) | [2.44](#244-t44-the-release-floor-moves-on-every-retry) |
| T45 | The applier applies the CDC retention fields of logical-replication stream records to the copy, setting retention barriers that nothing on the copy moves; fix: clear those fields, skip records left with nothing to apply (2026-10-18) | [§4.2](plan2.md#42-applier) | [2.45](#245-t45-cdc-retention-fields-in-schema-change-records) |
| T46 | Drop the page seal after a schema-change record: inherited from the xCluster producer's `ends_batch` (`xcluster_producer.cc:289-291`, honoured at `sys_catalog_pusher.cc:407-409`); the applier applies a schema-change record in place and nothing reads `ends_with_metadata`. Keep the size, restore and certified-position seals (2026-10-18) | [simplified §4.1](simplified_with_object_locks.md#41-lock-release) | — |

### 1.3 Open, Low

| Id | Item | plan2 | Details |
|---|---|---|---|
| T28 | Cluster-wide catalog lock, reader half | [§14.4](plan2.md#144-long-term-a-cluster-wide-catalog-lock) | [2.26](#226-t28-cluster-wide-catalog-lock) |
| T29 | External intents of a DDL open longer than a day are deleted on the copy | — | [2.27](#227-t29-external-intent-cleanup) |
| T30 | Explicit read times below master's compacted history; decided 2026-10-18: a serving cutoff at or after the seed time minus the system catalog retention interval | [§4.3](plan2.md#43-freshness-guarantee) | [2.28](#228-t30-explicit-read-time-below-the-cutoff) |
| T31 | A restore inside the WAL tail copied by remote bootstrap | [§6.2](plan2.md#62-re-seed) | [2.29](#229-t31-restore-inside-the-copied-wal-tail) |
| T32 | Tablet-wide flags and assertions reach the copy | — | [2.30](#230-t32-tablet-wide-flags) |
| T33 | Hot-path formatting; applied-version mutex on the read path (the poll and stream items went with the pull path) | — | [2.31](#231-t33-small-poller-and-service-items) |
| T42 | Memory accounting of the copy on the tserver (design review 2026-10, Mark) | — | [2.42](#242-t42-memory-accounting-of-the-copy) |

### 1.4 Accepted (no change planned)

| Id | Item | plan2 | Details |
|---|---|---|---|
| A1 | A DDL transaction's user-table writes are visible before its catalog change on other tservers, for the length of the release fan-out; `NOTIFY` from a DDL transaction leaks the same way | [§14.1](plan2.md#141-why-they-need-their-own-rule) | [2.36](#236-a1-ddl-and-dml-in-one-transaction) |
| A2 | The gate is skipped when master's version read at release fails, or when the synchronous release times out and completes asynchronously; tolerated as one-poll lag caught by the version check | [§11.2 item 2](plan2.md#112-master-side) | [2.37](#237-a2-releases-that-skip-the-gate) |

### 1.5 Closed

| Id | Item | Where |
|---|---|---|
| C1 | Transaction exclusion (requests with an attached distributed transaction went to master) | removed; `RepeatableReadDataSnapshotSurvivesLocalCatalogRead`; worklog2 "The transaction exclusion is gone" |
| C2 | Lease loss set no floor, so the copy re-entered the read path on the next poll | `OnLeaseLost` sets an unreachable bound; worklog2 "a real gap in the lease-loss rule" |
| C3 | Copy's own Raft group: waited a failure-detection interval; election at open corrupted the resume position; reads required a leader lease | worklog2 "The copy's own raft group: three defects" |
| C4 | Restart re-fetched the copy from master; now reopened from disk without master contact | `RestartReusesTheCopyWithoutAskingMaster`; worklog2 "A restart with a copy on disk" |
| C5 | PITR restore never reached the copy and the version wait held queries for the request deadline | `reseed_required` on `RESTORE_SYS_CATALOG`; worklog2 "master tells a poller that its batch crossed a restore" |
| C6 | Catalog snapshot read time chosen on the transaction serial; SERIALIZABLE now routed to master | commit 146f355944c; worklog2 "The catalog snapshot's read time lives on the plain session's read point" |
| C7 | Startup preload and parallel workers read master (legacy catalog session refused) | commit c43b6e2c0ad; worklog2 "A fresh backend paid a master round trip" |
| C8 | History cutoff of the copy used the user-tablet rule (15 min) instead of master's 4 h | `LocalCatalogAllowedHistoryCutoff`; worklog2 "The copy's history retention follows master's" |
| C9 | Master could delete the copy as an orphan | verified impossible; `LocalCatalogCopyIsNotInTheReportableTabletSet` |
| C10 | Push handoff use-after-free; contiguity check too strict; no lag cap; push during re-seed waited out the deadline | Phase 8 review items 1-6, commit 88ad50da853 |
| C11 | Pre-election poll cycle counted as an apply failure | commit be4463f8400 |
| C12 | Test flag `TEST_file_to_dump_docdb_writes` captured the copy's writes | `tablet.cc` skip for `kLocalCatalogTabletId` |
| C13 | Copy served with object locking off | commit 9735b896ed9, `LocalCatalogPrerequisitesMet` |
| C14 | Master's per-tserver stream map never pruned | idle pruning in `sys_catalog_change_service.cc` |
| C15 | Carrying catalog versions in the lease response; the "first observed" read time per database; heartbeat skip-and-resend as the design | dropped from the design 2026-09-28 (plan2 §7.1, §4.3) |
| C16 | Checkpoint persisted before the log append queue drained | `PersistCheckpoint` waits for `WaitUntilAllFlushed` (T7 covers the fsync half) |
| C17 | Java `TestYbRoleProfile`, `TestYsqlUpgrade`, cost-model and reset-analyze CI failures | root-caused to T4-T6; worklog2 "Fourth default-on CI run" |
| C18 | plan2 and implementation2 said the startup preload and parallel workers read master (review L9) | documents corrected 2026-09-28 |
| C19 | Master does not update its clock from the release request's propagated hybrid time, so the release's version read time reaching the commit time depended on the clock skew bound | moot once T1 replaces the read time by the version comparison |
| C20 | Coexistence with xCluster and logical replication | checked, no interaction: no `cdc_state` row, no WAL retention hook, the copy is outside `tablet_map_` so the tserver's CDC service, xCluster consumer and virtual WAL cannot reach it |
| C22 | T2: serving condition on entering serving (lease bound re-applied while serving; versions published while not serving) | `NotePublishedVersions`, `OnSafeTimePublished`, `OnLeaseGained`; push-plan §3.3; `ReleaseDuringCatchupGatesTheFlip` |
| C23 | T10: every waiter made the poller poll back-to-back | no polls; waiters raise one pending floor, one floor request in flight per tserver (push-plan §3.2); `notify_all` only on change |
| C24 | T16: release push cost items and default | replaced by pages built once per cycle and carried inline up to 4 MB (push-plan §2.5); always on |
| C25 | T21: per-tserver load on master | page cache, one S per cycle, bounded in-flight pushes (push-plan §2); load measurements pending (push-plan §10) |
| C26 | T34: lease rejoin test | `PgLocalCatalogRejoinTest` (push-plan §9.4) |
| C27 | T41: lease-loss guarantee when a new backend takes its version from the copy; propagate calls instead of killing backends | plan2 §7.1 states the dependency (2026-10-10); lease loss keeps killing backends, as without the copy; see 2.41 |
| C21 | The notes under `repo6/notes/` had diverged from this directory (T37) | repo6 notes commit of 2026-09-30 copies `plan2.md`, `implementation2.md`, `todos.md` and `worklog2.md` from here |

## 2. Details

### 2.1 T1. Publish gate on applied versions

Reclassified 2026-10-10 from High (correctness) to Medium (performance).
The time comparison in the code is correct. Master reads the versions of a
report at time R, so every version in the report committed at or below R.
When the copy's safe time C is at or above R, the copy holds every one of
those commits, and with them the rows of every version in the report. The
time comparison is stricter than the version comparison: it also waits for
commits between the newest version's commit and R, which the report does not
depend on. On the heartbeat path R is a periodic cache refresh time, up to
`heartbeat_interval_ms/2` above the newest commit, so a held report waits for
a push that covers R instead of for the version it carries. The remaining
work is that latency reduction.

Version comparisons already in the code (checked 2026-10-10):

- Read path: `LocalCatalogCompleteTime` (`pg_client_session.cc:3584-3605`)
  waits, before a local read, until the copy's applied version for the
  backend's database is at or above the version the backend sends with the
  request (`backend_catalog_version`, from `YbGetLocalCatalogVersion` in
  `pg_yb_utils.c`). This does not replace the publish gate: a backend updates
  its own version only after its cache refresh completes
  (`postgres.c:4829`, `relcache.c:3185`), so the reads of that refresh carry
  the previous version.
- Entry into serving: `OnSafeTimePublished` (`local_catalog_replica.cc:534-551`)
  requires the applied version of every database to be at or above each
  version published while the copy was not serving (C22).

Status 2026-10-07 (master push, [push-plan.md](push-plan.md) §3.2): the
heartbeat starvation described below is answered: a heartbeat report the
gate blocks is held (`held_version_report_` in `heartbeater.cc`) and sends a
floor request at its read time; master runs cycles until its own clock passes
that time (a floor never moves master's clock), and the copy reaches it in
one push round trip; the held report is published at the next heartbeat
after the copy holds its read time (T14). The
release and heartbeat paths still compare times, not versions; the version
comparison remains open and is independent of the push.

Design: [plan2 §4.3](plan2.md#43-freshness-guarantee). Both paths publish a
version report only when, for every database in it, the copy's applied
version A[db] is at or above the carried version. The heartbeat path holds
the report and publishes when the wait ends rather than skipping it.

Today's code compares times, not versions:

- Release handler: `ts_local_lock_manager.cc:577-590` waits
  `local_catalog->WaitForSafeTime(req.catalog_versions_read_time(), deadline)`
  before `SetYsqlDBCatalogVersions*` (`:598-606`) and before the local unlock
  (`:618-623`).
- Heartbeat: `heartbeater.cc:137-145` (`LocalCatalogCopyHoldsHeartbeatVersions`
  → `HoldsCatalogStateAt(db_catalog_versions_read_time)`), used at `:560-573`
  to hold the report and send a floor request.
- Master fills the read time at `master_heartbeat_service.cc:358-360` from
  `GetYsqlAllDBCatalogVersions(use_cache=true)` (`catalog_manager.cc:11140-11167`,
  cache read time at `:14389`), and for the release at
  `object_lock_info_manager.cc` `PopulateDbCatalogVersionCache`.

Why the change: the heartbeat read time is the periodic cache refresh time
(every `heartbeat_interval_ms/2`, `ysql_manager.cc:56`, and on DDL commit,
`ysql_ddl_handler.cc:384-386`), which is above the newest commit by up to that
interval and moves on every re-read, so a lagging copy chases a moving
target. A[db] is exact: it is read from the copy's own version table at C
(`local_catalog_applier.cc` `ReadLocalCatalogVersions`), so A[db] ≥ V proves
the copy holds V's rows.

Change:

1. Release handler: replace the `WaitForSafeTime` with
   `WaitForAppliedVersion(db, version)` for every entry of
   `req.db_catalog_version_data()`.
2. Heartbeat: replace `HoldsCatalogStateAt` by the same per-database check;
   when it fails, keep the report, send a floor request, and publish it
   from the applier's publish path (or a short heartbeat-thread wait) as soon
   as the copy catches up, so that the cost is one push round trip and not
   one heartbeat interval on top (T14).
3. Master: `db_catalog_versions_read_time` on both protos becomes unused;
   remove after the tserver side has landed.

Test: pause the applier with `TEST_local_catalog_pause_applier`, run a DDL
elsewhere, assert the heartbeat report is held and no version below the DDL's
is published, resume, assert publication within one push round trip;
`GateHoldsDdlWhileApplierIsPaused` covers the release side.

Observed 2026-09-29, with the pull path: the moving target starves the heartbeat path
indefinitely, not for one poll. `PgBackendsTest.CachedVersion` with
`--local_catalog_poll_interval_ms=1500` fails like CI launch 173734 (version 2
not in shared memory after 10 s). The gate skipped all 10 heartbeats. Each
immediate poll moved C to just after the skipped heartbeat, and the next
heartbeat carried a read time about 0.87 s above that C (read time
02:57:41.508678 against C 02:57:40.639983, and so on). With defaults the test
passes locally (1.0-1.15 s per bump); in CI it fails under load (6 of 7
attempts in launch 169670). Details: worklog2 "Fifth default-on CI run",
row H.

### 2.2 T2. Serving condition on entering serving

Status 2026-10-07: CLOSED (C22). Implemented as described under "Change",
with the PG start held after a new lease until the copy serves
(push-plan §3.3).

Design: [plan2 §7.1](plan2.md#71-lease-loss-lease-regain-and-tserver-restart).
A copy enters serving only when C ≥ the lease bound and A[db] ≥ every version
published to shared memory while the copy was not serving.

Two holes today:

- Flip window (review M1). While not serving, the gate publishes versions
  without waiting (`ts_local_lock_manager.cc:577` skipped when
  `!IsServing()`; `heartbeater.cc:137-145`). When the first applied response
  flips the copy to serving (`local_catalog_replica.cc:442-465`), its C can be
  below the read time of those versions. A backend that has not attached to a
  database preloads `pg_authid`, `pg_database` and the profile tables with an
  invalid db oid, so `SetBackendCatalogVersion` records nothing
  (`pg_session.cc:929-937`) and the version wait is skipped
  (`pg_client_session.cc:3582`). A login in that window reads roles from
  before an acknowledged `ALTER ROLE`/`CREATE ROLE`/`CREATE DATABASE`; with
  `ysql_enable_read_request_cache_for_connection_auth` the stale preload is
  cached under the new version for up to
  `pg_cache_response_trust_auth_lifetime_limit_ms` (unverified).
- Lease bound not re-checked while already serving. `OnLeaseGained` stores
  the bound (`local_catalog_replica.cc:381-393`); `OnSafeTimePublished`
  consults it only in `kDisabledLease`, `kBootstrapping` and `kReseeding`
  (`:442-465`). At process start the lease manager calls `OnLeaseGained` on
  the first epoch (`ysql_lease_manager.cc:145-172`, `!lease_is_live_` →
  `restart_pg`); if the copy's first poll published S before that lease
  arrived, the state is already `kServing` and the bound is never applied.

Change: keep, per database, the highest version published while not
serving (in `TabletServer::SetYsqlDBCatalogVersions*` when
`!local_catalog->IsServing()`); in `OnSafeTimePublished`, enter serving only
when C ≥ `lease_gap_floor_` and `applied_version_[db]` ≥ that map for every
db; in `OnLeaseGained`, if the state is `kServing` and `safe_time() < bound`,
move to `kDisabledLease` so the bound is re-applied.

Test: `ReadWaitsForAVersionPublishedWhileNotServing` covers the read-side
wait; add a login test: disable serving, `ALTER ROLE u NOLOGIN` from another
tserver, re-enable, connect as `u` immediately, expect failure.

### 2.3 T3. Remote bootstrap copies untrusted state

Design: [plan2 §11.2 item 1](plan2.md#112-master-side). Two parts.

(a) Uncommitted WAL tail (review S2, INFERRED). `remote_bootstrap_session.cc:311-315`
includes the active segment, so entries the master leader appended but did
not commit are copied. `tablet_bootstrap.cc:1721-1728` hands entries past
the committed index to consensus as pending; the copy is the only voter of
its group, so its election no-op commits them. Master's log cache reads from
the poller's position by index only (`consensus_queue.cc:768-772`,
`log_cache.cc:409-410`); `local_catalog_poller.cc:451` compares positions by
index. A later master leader that truncates and replaces that index leaves
the copy with a write master never committed and without the replacement.
Fix candidates: resume from the copied log's last committed op id and do not
let the private copy commit orphaned replicates; have master check the term
at `from_op_id` and answer with a re-seed on mismatch.

(b) Open transactions' intents (review S3, INFERRED; the coordinator's
ABORTED answer for unknown transactions is verified at
`transaction_coordinator.cc:1378-1381`). Remote bootstrap copies the intents
DB as it is. A catalog transaction T open at that moment arrives as regular
intents (one per row, keyed by the row) plus T's metadata record. The poller
starts after the last copied WAL entry (`local_catalog_poller.cc:193-195`),
so T's write records are never shipped again; only its APPLY record arrives,
and the external apply (`NonTransactionalBatchWriter::PrepareApplyExternalIntents`,
`rocksdb_writer.cc:1296-1330`) finds no external-intent blob and writes
nothing. A read that meets T's regular intents asks the copy's participant,
which asks T's coordinator; after master applies T the coordinator forgets it
and answers ABORTED. T's rows are missing from the copy until the next
re-seed.

```
master: T writes catalog intents (regular, keyed by row)
tserver: remote bootstrap copies intents DB + WAL tail   (T's intents included)
tserver: poller starts after the last copied entry
master: T commits at c, master applies T
coordinator: forgets T (master was its only known participant)
poller: T's APPLY record -> external apply finds no blob -> writes nothing
copy read at h >= c: row intent of T -> ask coordinator -> ABORTED
```

How xCluster avoids it: bootstrap aborts every active transaction on the
source when it records the start position
(`TabletPeer::GetOpIdAndSafeTimeForXReplBootstrap`, `tablet_peer.cc:1204-1226`,
`abort_active_txns_during_xrepl_bootstrap`), snapshots only after resolving
intents (`backup_service.cc:104-108`), restores without the intents DB
(`tablet_snapshots.cc:734`). Not acceptable here: a tserver can bootstrap at
any time and that would abort in-flight DDLs cluster-wide. Starting the
poller at the oldest open transaction's first write does not work either:
there can always be an open DDL, and master may have garbage-collected that
WAL.

Fix candidates:

1. After the copy opens and replays its WAL, before the poller starts and
   before serving, rewrite every transaction the copy's participant still
   tracks as unapplied: read its intents through the reverse index, write one
   external-intent blob in the `CombineExternalIntents` format
   (`xcluster_write_implementations.cc:106`), delete the regular intents and
   the metadata record, drop it from the participant. The APPLY record then
   applies the blob; an aborted transaction's blob is removed by the
   external-intent cleanup. The copy then never holds regular intents.
2. When an APPLY record arrives for a transaction the copy's participant
   tracks, apply it through the participant at the record's commit time
   instead of the external apply, and stop the participant from cleaning
   those transactions up on an ABORTED answer.

Test (write first, expect failure today): session X on T1 runs
`BEGIN; CREATE TABLE a (...);` and stays idle; force a re-seed of T2's copy
(`TEST_local_catalog_poll_checkpoint_too_old`); X commits; wait for the
coordinator to forget X's transaction; a cold backend on T2 queries `a` with
the copy serving and again with `TEST_local_catalog_disable_serving`; the
results must match.

Reproduced 2026-09-29, so (b) is no longer inferred. The test above is
`PgLocalCatalogTest.DdlOpenAcrossReseedIsVisibleAfterCommit` (repo6, not
committed). It fails on the first run: after the re-seed the copy's
participant reports "loaded 1 transactions", the DDL's APPLY applies only the
two provisional records the poller shipped (the `pg_yb_catalog_version` bump
and the invalidation messages), and the query on the re-seeded tserver fails
with `relation "open_across_reseed" does not exist` while the same query read
from master returns the row. The same mechanism is behind the 8
`TestPgSequences*` failures of CI launch 173734: a tserver spawned by the test
bootstrapped its copy while `CREATE SEQUENCE` was open. There the early writes
arrived through replay of the copied WAL tail, not the copied intents DB.
Details: worklog2 "Fifth default-on CI run", row D.

Plan for (b), 2026-10-09: [rbs-intents-plan.md](rbs-intents-plan.md). After open and WAL
replay, before the first pushed page, rewrite every locally uncommitted transaction's regular
intents into the applier's external-intent blobs and remove the regular intents; the copy's
participant does no status resolution.

### 2.4 T4. Login profile catalogs

Decision 2026-10-09 (design review): login profiles are disabled while the
copy is enabled. The profile feature is gated by `ysql_enable_profile`
(non-runtime, default `false`, `ybc_gflags.cc:42`). Change: the copy refuses
to start when that flag is set (`LocalCatalogPrerequisitesMet`, with a
warning), or the tserver refuses the flag combination at start. Support is a
follow-up; the earlier plan below (profile reads always to master) is the
candidate for it.

Design: [plan2 §11.1 item 1](plan2.md#111-pg-side). Earlier plan: every read of
`pg_yb_profile` and `pg_yb_role_profile` goes to master.

Mechanism (review S5, VERIFIED by CI): `YbMaybeIncFailedAttemptsAndDisableProfile`
(`yb_profile.c:726-772`) reads the counter through
`yb_get_role_profile_tuple_by_role_oid` (a `table_beginscan_catalog`,
`yb_profile.c:432`), so under the catalog snapshot served from the copy at C,
adds one and writes it with `YBCExecuteUpdateLoginAttempts`
(`ybModifyTable.c:1298-1367`), a `YB_SINGLE_SHARD_TRANSACTION` outside DDL:
no version bump, no own-write record. Attempts closer than one poll read the
same count; `ALTER ROLE ... ACCOUNT LOCK` writes a catalog with no syscache,
so it is seen late on other tservers too. Measured on `testLogin[0]`:
reader's snapshot pinned 28 ms before the write (worklog2 "Fourth default-on
CI run").

Where: `AllOpsAreReadsOfLocalCatalogTables` (`local_catalog_read.cc:35-56`)
already rejects a request per table id when the copy's metadata lacks it;
add the two profile table ids to that rejection. Cost: one master read per
authentication on a path that already makes several. Rejected alternative:
bumping the version on every failed login (`pg_yb_role_profile` is
`BKI_SHARED_RELATION`, so the bump would be global, on every password
guess; `YbResetFailedAttemptsIfAllowed` would add bumps on successful logins).

Covers CI items: `TestYbRoleProfile` ×5 and
`YsqlRoleProfileDuringMajorUpgradeTest.RoleProfileWritesDisabled`.

### 2.5 T5. Large objects

Decision 2026-10-09 (design review): large objects are disabled while the copy
is enabled. Change: the large-object functions (`lo_create`, `lo_import`,
`lo_from_bytea`, `lowrite`, `lo_put`, `lo_unlink`, `lo_open` for write)
raise "not supported with the tserver-local catalog". Refuse the functions
themselves; do not rely on the `yb_non_ddl_txn_for_sys_tables_allowed` check
alone, because the object-locking path may let these catalog writes through
without the setting (to be rechecked against 2b14b044f36). Support is a
follow-up; the fix candidates below remain the options.

Design: [plan2 §11.1 item 2](plan2.md#111-pg-side).

Mechanism (review S4, INFERRED): `lo_create`, `lowrite`, `lo_unlink`,
`lo_from_bytea` write `pg_largeobject` and `pg_largeobject_metadata` from the
user's transaction with `yb_non_ddl_txn_for_sys_tables_allowed`
(`pg_session.cc:208-221`, commit 2b14b044f36 requires the setting), not in
DDL mode: no version bump, no own-write record (`pg_client_session.cc:4693`
stores it only on `is_ddl_mode`). A reader without the setting reads the two
catalogs under its catalog snapshot at C, served from the copy
(`pg_session.cc:1257-1263`). Master today has no problem: the data read
advances the reader's clock past the commit and the catalog snapshot reads
master at that clock time.

```
writer: BEGIN; UPDATE docs SET rev = 2 WHERE id = 1;
        SELECT lowrite(lo_open(o, 131072), 'new'); COMMIT;
reader: SELECT rev FROM docs WHERE id = 1;   -> 2        (user tablet, fresh)
reader: SELECT lo_get(o);                    -> old bytes (copy at C < commit)
```

Also (review L3): `BEGIN; SET yb_non_ddl_txn_for_sys_tables_allowed = 1;
SELECT lo_create(42); SET ... = 0; SELECT lo_get(42);` returns "does not
exist": the copy reads without transaction metadata
(`ExecuteLocalCatalogReads`, `local_catalog_read.cc`) and the exclusion looks
only at the setting's current value (`pg_client_session.cc:3654`).

Fix candidates: add the two large-object table ids to the same rejection as
T4; or T6's own-write wait for the writer plus a cross-session wait keyed on
a time carried with the user data.

### 2.6 T6. Direct catalog updates and the own-write wait

Decision 2026-10-09 (design review): direct catalog writes are disabled while
the copy is enabled. Change: setting `yb_non_ddl_txn_for_sys_tables_allowed`
fails outside YSQL upgrade mode and initdb. Upgrade migrations keep the
setting and keep reading master (plan2 §10). Open sub-items:

- `yb_reset_analyze_statistics` raises the version row by hand
  (`yb_system_functions.sql:84`) and needs a DDL-mode path or must be
  refused too.
- Tests that use `BasePgSQLTest.executeSystemTableDml` and
  `yb.orig.planner_base_scans_cost_model.sql` run with the copy off.
- The own-write wait for non-DDL catalog commits (below) is no longer needed
  for this case; it stays a candidate for T5 support later.

Design: [plan2 §11.1 item 3](plan2.md#111-pg-side) and
[§5.3 rule 3](plan2.md#53-picking-the-local-read-time).

Mechanism: `TryServeCatalogReadsLocally` refuses a request that itself
carries `yb_non_ddl_txn_for_sys_tables_allowed` (`pg_client_session.cc:3761-3771`),
but the refusal ends with the statement; the session's next read is served
from the copy at C below its own write. A manual `UPDATE pg_yb_catalog_version`
writes the row but advances no backend's `yb_catalog_cache_version`, which
moves only on a DDL commit (`pg_yb_utils.c:3044`, `:3443`) or a shared-memory
read; so the writer's version check passes at once. Other sessions are
covered by the gate once the increment is published.

Sites that write catalogs outside a DDL commit (audited 2026-09-14):
`yb_catalog_version.c:209, :344, :759, :866` (the version mechanism itself),
`YBCExecuteUpdateLoginAttempts` (T4), `yb_reset_analyze_statistics`
(`yb_system_functions.sql:20-92`, raises the row by hand at `:84`),
`ybInsertPendingNotifiesToTable` (`async.c:2979`, writes the notifications
table, not a catalog the copy serves). Tests reach it through
`BasePgSQLTest.executeSystemTableDml` (`BasePgSQLTest.java:1524`) and
`yb.orig.planner_base_scans_cost_model.sql:24-30`.

Change: in `HandleCommit` (`pg_client_session.cc:4688-4700`) record the
own-write time for any transaction that wrote a catalog table, not only under
`is_ddl_mode` (pggate knows: `has_catalog_write_ops_in_ddl_mode_` at
`pg_session.cc:1295-1297` is the DDL-mode variant; a non-DDL counterpart is
needed), or keep the session on master from its first write under the
setting until C passes that write's commit. Open question in plan2: whether
the manual increment stays required.

Covers CI items: `TestYsqlUpgrade` ×5, `TestPgUpdatePrimaryKey#basicSystemTables`,
`TestPgCostModelSeekNextEstimation` ×2, `TestPgRegressPlanner`,
`TestPgRegressResetAnalyze`.

### 2.7 T7. Checkpoint durable before the WAL

Design: [plan2 §4.2](plan2.md#42-applier) ("the checkpoint must
never claim a position whose records are not durable on the copy").

Mechanism (review S1, VERIFIED): `LocalCatalogApplier::PersistCheckpoint`
(`local_catalog_applier.cc`) waits `log()->WaitUntilAllFlushed()`
(`log.cc:1668-1674`), which drains the append queue but does not fsync, then
writes the file with `pb_util::SYNC`. With the defaults
(`durable_wal_write=false`, `interval_durable_wal_write_ms=1000`,
`bytes_durable_wal_write_mb=1`, `log_util.cc:86-98`) the copy's WAL is
fsynced in the background. A power loss between the two leaves a checkpoint
that claims records the copy no longer has; master ships only what follows
it. Fix: `Log::Sync()` before writing the file, or store the position with
the data (flushed frontier or superblock).

### 2.8 T8. User catalog reads guard

Design: [plan2 §14](plan2.md#14-user-catalog-reads). Step 1 (T8): user
catalog reads use the copy only when the statement is not inside an explicit
transaction block and, after parsing and planning, references only catalog
tables and calls no user-defined function; otherwise the executor's catalog
scans keep the transaction's read time and go to master. Step 2 (T19): serve
such a scan from the copy when its read time is at or below C.

How PG and pggate treat an executor scan of a catalog table today:

- Internal reads: `systable_beginscan` takes the catalog snapshot
  (`genam.c:447`, `:728`) and enters the YB catalog scan (`genam.c:422`,
  `ybc_systable_beginscan`). Only `CatalogSnapshotData` has
  `yb_is_catalog_snapshot = true` (`snapmgr.c:556`).
- Executor scans: `ybc_heap_beginscan(relation, snapshot, ...)`
  (`yb_scan_core.c:4074-4083`) receives the active snapshot, the transaction
  snapshot for `SELECT ... FROM pg_class` or `\dt`.
- pggate ignores the snapshot: `PgSession::DoRunAsync` switches the session
  read point to the catalog snapshot's serial for any op group whose first
  table `is_ysql_catalog_table()` (`pg_session.cc:1257-1263`,
  `UpdateReadPointForCatalogOps` at `:1214`), and the request flag
  `is_catalog_snapshot` derives from `has_catalog_ops` (`:971-972`,
  `:1290-1292`).
- On the tserver, `IsCopyServableCatalogRequest` accepts a plain-session
  request only with `is_catalog_snapshot` (`pg_client_session.cc:3654-3660`).

Change:

1. PG: decide once per statement at executor start (`standard_ExecutorStart`,
   where `plannedstmt->rtable` is final and views are expanded):
   `mixed = IsTransactionBlock() || (an RTE_RELATION whose relid is not a
   catalog relation) || (a FuncExpr/OpExpr/Aggref/WindowFunc whose function
   oid >= FirstNormalObjectId)`. Store it in the estate or on the cached plan
   the way `yb_plan_references_pg_rel` is (`plancache.c:454`, consumed at
   `spi.c:2607`). Precedents: `qpmReferencesCatalogRelation`
   (`yb_qpm.c:790-808`) walks the range table for `IsCatalogRelationOid`;
   `contain_volatile_functions` (`clauses.c`, used at `yb_cmds.c:1436`,
   `yb_uniqkeys.c:93`) is the expression-walker shape. Then at
   `ybc_heap_beginscan` (and the index-scan equivalent) of a catalog relation,
   when `!snapshot->yb_is_catalog_snapshot && mixed`, mark the select handle
   "transaction snapshot".
2. pggate: for a marked op group skip `UpdateReadPointForCatalogOps` and do
   not count it in `has_catalog_ops`; the request carries the transaction's
   read point serial.
3. tserver, step 1: nothing; such a request has no `is_catalog_snapshot` and
   goes to master at the transaction's read time.
4. tserver, step 2 (T19): accept a marked request when its read time h ≤ C
   and within retention (the same branch as an explicit read time,
   `pg_client_session.cc:3833-3838`), serve at exactly h.

Why the function check: a user-defined function called from a catalog-only
statement runs its own statements through SPI at execution time; their range
tables are not in the outer plan. SPI's snapshot (`spi.c:1796-1801`,
`:2734-2738`): a stable/immutable function uses `GetActiveSnapshot()`, the
caller's; a volatile function uses `GetTransactionSnapshot()`, which under
`READ COMMITTED` is a new snapshot and a new read point (`procarray.c:2682` →
`YbResetTransactionReadPoint` → `pg_txn_manager.cc:485-497`) and under
`REPEATABLE READ` the transaction's one snapshot (same read point). So except
for volatile-under-RC an inner user-table read runs at the outer statement's
h > C while the outer catalog scan ran at C. Built-in functions do not read
user tables; extension functions have oids ≥ `FirstNormalObjectId` and are
excluded conservatively.

Existing test to extend: `UserCatalogReadMatchesMaster`.

### 2.9 T9. Failed local read fallback

Mechanism (review S7, VERIFIED): when `ExecuteLocalCatalogReads` fails
(`pg_client_session.cc:3860-3865`), for example because a re-seed closed the
tablet between the serving check and the read, the status goes into the
response and is sent to PG; the read is not retried on master;
`routing_reason` is set only on success or on a decline, so the routing log
writes nothing and no counter is incremented. Direction from the user: a
local read must not fail; a re-seed or shutdown notifies the read path so it
falls back to master; add a `local_catalog_read_failures` counter and a
throttled warning.

### 2.10 T10. Waiter-driven polls

Status 2026-10-07: CLOSED (C23). There are no polls; a waiter raises the
pending floor and the apply thread sends at most one floor request at a
time, repeated once a second while the floor is not reached.

Mechanism (review S6, mechanism VERIFIED, rates INFERRED). Three code paths
block until the copy reaches a target: the release handler's gate wait
(`ts_local_lock_manager.cc:577`, `WaitForSafeTime`), a read's version check
(`pg_client_session.cc:3594`, `WaitForAppliedVersion`) and a read's own-write
check (`:3625`, `WaitForSafeTime`). Both wait functions are loops
(`local_catalog_replica.cc:314-356`): check the target, call
`poller_->RequestImmediatePoll()`, sleep on `cond_` until notified or the
deadline. `RequestImmediatePoll` sets the scheduler's `poll_asap_`
(`local_catalog_poller.cc:125`, `master_leader_poller.cc:100`), which makes
the poll thread skip the rest of its interval and any backoff
(`master_leader_poller.cc:126`). After every poll `PublishAfterApply` ends
with `cond_.notify_all()` (`local_catalog_replica.cc:287`) whether or not C
or an applied version moved. Loop while one target is unmet: poll completes
→ every sleeper wakes → each re-checks, fails, sets `poll_asap_` again,
sleeps → the next poll starts at once. While a catalog transaction is open
on master, master computes a new S at most once per 250 ms per stream
(`xcluster_consistent_wal_safe_time_frequency_ms`), so most of those polls
carry no S and the loop runs at the RPC round-trip rate (about 1000 polls/s
per tserver at 1 ms) on master's high-priority pool. In the common case
nothing waits; the cost is master load whenever anything does.

Fix: `notify_all` only when C or an applied version changed; set
`poll_asap_` once per target rather than on every wake-up.

### 2.11 T11. Serving withdrawal and wait fallbacks

Design: [plan2 §7.2](plan2.md#72-push-and-apply-failures) and
[§11.3 item 1](plan2.md#113-tserver-side).

Mechanism (review M3 and worklog2 "How a stalled poller becomes a
cluster-wide DDL problem"; with push the same holds for refused or failed
pushes): nothing moves the state out of `kServing` on push
or apply failures (`local_catalog_replica.cc`: only `OnLeaseLost`, a re-seed
request and the major upgrade flag do). While C is frozen and the copy stays
serving: the heartbeat gate withholds every version report (a new database
never reaches shared memory, "Failed to find suitable shared memory index
for db"); every lock release waits at that tserver's gate for the release
deadline, so every DDL in the cluster stalls (35 s per DDL measured); each
catalog read on that node waits `local_catalog_serve_wait_timeout_ms` (5 s),
twice on a snapshot's first request (`SetFreshCatalogSnapshotReadTime` at
`pg_client_session.cc:4168` calls `LocalCatalogCompleteTime`, and
`TryServeCatalogReadsLocally` at `:3810` calls it again; each call computes a
fresh deadline at `:236-241`), before going to master; nothing remembers
that a wait timed out.

Change: after N consecutive apply failures or when C lags the tserver clock
by more than a bound, move to a not-serving state (reads to master, gate
skipped, heartbeat reports published), resume under T2's condition on the
next successful apply; compute the wait once per request; skip the wait after
a recent timeout; route a timed-out read to master rather than failing it.
Test: fail every push with `TEST_local_catalog_fail_push`, run a DDL
elsewhere, assert the copy stops serving and the DDL completes; unblock and
assert serving resumes.

### 2.12 T12. Own-write check version record

Mechanism (review M2, logic VERIFIED): `backend_catalog_version_` is stored
only by a request that reaches `LocalCatalogCompleteTime` while the copy is
serving (`pg_client_session.cc:3584`); DDL-mode reads, response-cache hits,
SERIALIZABLE sessions and not-serving reads never update it. At a DDL commit
the floor version is copied from that field (`:4698-4699`), and the check is
skipped when `backend_version > floor_version` (`:3619`). After a DDL whose
reads all went to master the recorded value is stale or 0, so a following
`CREATE TEMP TABLE` (no version bump) is not waited for and the next
statement can miss the temp table ("relation does not exist", or a permanent
table of the same name on the search path). Normally the synchronous release
has moved the copy past the commit anyway; exposed after the asynchronous
release fallback (`ts_local_lock_manager.cc:987`) or T2's flip window. The
comment at `:3612` is also wrong: a concurrent DDL's version can reach the
session after the floor version was recorded. Fix: record the floor version
from the version the DDL itself committed; keep the check when nothing was
recorded.

### 2.13 T13. Session wait after a master fallback

Status 2026-10-07: implemented with the push change. When a catalog read that
the copy could answer in principle goes to master instead (not serving, a
wait ran out, SERIALIZABLE, a read time above C), the session sets
`catalog_read_from_master_`; its next read from the copy waits, through a
floor request, until C reaches the tserver's clock reading taken at that
read. The master response has moved the tserver's clock past master's read
time, so the copy then holds everything the session saw. Reads in DDL mode
do not set it: the DDL's own commit is covered by the version gate and the
own-write check. Test: `MasterSnapshotTimeIsFloorForNextLocalRead`.

Design: [plan2 §11.1 item 6](plan2.md#111-pg-side). Mechanism (review L1,
INFERRED): a catalog snapshot that fell back to master takes its time t1
from the clock (`SetFreshCatalogSnapshotReadTime`, `pg_client_session.cc:3692`);
the next fresh snapshot gets C (`:3685`), which can be below t1. A change
committed between C and t1 shows in one statement and not in the next.
Causes of the fallback: a wait timeout, a brief not-serving state, the first
read after a DDL block (`:3812-3825`, `:3700`). Fix: when a session reads
master at t1, record t1 on the session and make its next local read wait
until C ≥ t1, the same session-level wait as the temporary-table case.

### 2.14 T15. Re-seed on runtime corruption

Design: [plan2 §6.2](plan2.md#62-re-seed) lists "local tablet failure, at
open or detected while running" as a trigger. Today only an unopenable
tablet at start (`OpenOrCreateLocalCatalogTablet`), `CHECKPOINT_TOO_OLD`
(`local_catalog_poller.cc:358`) and `reseed_required` (`:368`) re-seed. A
RocksDB error while applying fails every poll forever (`IncApplyFailures`,
`:404`) while the copy stays serving (T11). Fix: treat a corruption-class
apply or read error as `RequestReseed()`.

### 2.15 T16. Release push cost and default

Status 2026-10-07: CLOSED (C24). The sliced release push is gone; pages are
built once per cycle and carried inline (push-plan §2.5).

Design: [plan2 §4.4](plan2.md#44-change-records-carried-in-the-lock-release).
The push is behind `enable_local_catalog_release_push`, default `false`
(`object_lock_info_manager.cc:93`). Cost items (review M7, VERIFIED) in
`PrepareLocalCatalogPushes` (`:1772-1900`): each target's slice is built and
serialized in full (up to one 4 MB batch) before the 1 MB cap
(`local_catalog_release_push_max_bytes`) is checked (`:1872-1898`); accepted
payloads stay in `local_catalog_pushes_` until every tserver has
acknowledged and are copied on each RPC attempt (`:2122-2124`), N × the DDL's
catalog bytes per DDL in flight; every release uses the shared stream
`master-release-push` with `force_apply_safe_time`
(`sys_catalog_change_service.cc:93-100`), so `ResolveIntents` runs
synchronously on the release caller's thread and concurrent releases queue
on that stream's mutex. Also (review L10): `force_apply_safe_time` applies
only when no S is pending (`xcluster_producer.cc:331-333`), so after a
truncated push the next push can carry an older S or none; clear the pending
S before a forced fetch. Change before default-on: slice against the cap,
share one serialized batch across targets, resolve once per cycle, then flip
the default.

### 2.16 T17. Changed-table list in the release

Design: [plan2 §12](plan2.md#12-follow-up-items), cost in
[§9](plan2.md#9-performance-model). Measured: `ALTER TABLE t ADD COLUMN` on
node 1, then `SELECT * FROM t01_r1` on a fresh backend on node 3 at 100 ms one
way takes 8.9 s against 0.46 s in steady state (`t01_r1` is not the altered
table). The copy answered all 66 catalog requests; the time is 52
`MasterDdl::GetTableSchema` calls, misses in the tserver's `PgTableCache`,
which a DDL empties for the whole database: the new version reaches the
tserver, `SetYsqlDBCatalogVersionsUnlocked` adds the database to
`db_oids_updated` (`tablet_server.cc:1696`), `InvalidatePgTableCache`
follows (`:1884`), and `PgTableCache::Impl::InvalidateDbTables` clears the
per-database map (`pg_table_cache.cc:243-266`). Commit `ad458d744db` (D45266)
made the PG-side cache evict selectively and left the tserver cache out.

Fix: master already holds the set of tables each DDL transaction changed,
`CatalogManager::YsqlDdlTransactionState::tables` minus `nochange_tables`
(`catalog_manager.h:3417-3436`, `:3456`), built as master processes the DDL's
own `CreateTable`, `AlterTableWithBatchTracker`, `DeleteTable`
(`catalog_manager.cc:4925`, `:8449`, `:7301`). The release is issued after
`WaitForDdlVerificationToFinish` and is synchronous
(`pg_client_session.cc:3271-3296`); master already fills cache-update fields
into it in `PopulateDbCatalogVersionCache` (`object_lock_info_manager.cc:1178-1210`);
`ReleaseObjectLockRequestPB` already carries `db_catalog_version_data`
(`tserver.proto:539-551`), consumed at `ts_local_lock_manager.cc:598-606`.
Carry the table list there and evict only those entries. Not PG's
`ddl_transaction_state.altered_table_ids`: per backend, never sent to
master, ALTER only. `PopulateDbCatalogVersionCache` carries its own TODO
that it sends every database's version "because the cache invalidation
logic on the tserver side expects a full report"; the two coarsenesses could
be narrowed together.

### 2.17 T18. PG backend flow

Design: [plan2 §13](plan2.md#13-how-the-pg-backend-flow-changes). Three
changes, none started:

- Remove sys-table preloading. Today the preload runs on the legacy catalog
  session (`YBCIsLegacyModeForCatalogOps` true while
  `YBCIsSysTablePrefetchingStarted`, `ybc_pggate.cc:2084-2086`) and is
  served from the copy since `IsCopyServableCatalogRequest` accepts that
  session kind (`pg_client_session.cc:3654-3667`). The relcache init file
  stays. Startup then follows standard PG lazy loading against the copy.
- Replace the mid-statement full cache refresh (`pg_yb_utils.c:283`,
  `YbGetMasterCatalogVersion` on the refresh path; `relcache.c:3185`) by a
  cache reset that reloads lazily.
- Read invalidation messages from the copy (`pg_yb_invalidation_messages` is
  a catalog table the copy holds) instead of carrying them in heartbeats
  (`master_heartbeat_service.cc:365-380`) and releases
  (`db_catalog_inval_messages_data`, field 10 of `ReleaseObjectLockRequestPB`).
  The gate guarantees the copy holds V's messages when V is published.

### 2.18 T20. ResolveIntents before transaction loading

Mechanism (review M9, INFERRED; stock xCluster code): right after master
opens its system catalog tablet the participant loads existing transactions
in the background (`transaction_participant.cc:1211`). `ResolveIntents`
(`:1342-1409`) walks `transactions_` only, with no `loader_.WaitAllLoaded()`.
A poll that reaches the new leader before loading finishes resolves only the
loaded set, so a committed but not yet applied transaction can be missed and
S emitted before its APPLY record. Not checked whether anything blocks
GetChanges on a new leader until loading completes. Fix:
`RETURN_NOT_OK(loader_.WaitAllLoaded())` at the start of `ResolveIntents`, or
in `SysCatalogChangeServiceImpl` before asking for S.

### 2.19 T21. Master load per tserver

Status 2026-10-07: CLOSED (C25) in code. Master builds each page once and
certifies one S per cycle for every copy (push-plan §2). The load study is
the push scenarios of `pg_local_catalog_load-test` (push-plan §9.7); the
numbers are pending (push-plan §10).

Design: [plan2 §11.2 item 4](plan2.md#112-master-side). Review M8, M10.
Each tserver has its own stream (`sys_catalog_change_service.cc:74-83`), and
each call re-reads up to `consensus_max_batch_size_bytes` (4 MB) of WAL and
rebuilds records (`:113-159`, `xcluster_producer.cc`); response memory is
charged to the memory tracker only when the read went to disk. S is
certified per stream by resolving open catalog transactions against their
coordinators: N tservers × 4 rounds per second × the number of status
tablets; a committed but unapplied transaction makes the handler sleep in a
10 ms loop up to the 30 s deadline. Fix: compute the batch and S once per
cycle on master and serve every pull from that cache, sliced at the caller's
position; optionally replace polls with a timer-driven push (the release
push of T16 on a timer). Load study: a standalone poll client with N = 1000
streams against a real master while DDLs run (pointers: `GetChanges` usage
in `cdc_service-int-test.cc`, `cdcsdk_ysql_test_base.cc`); measure master
CPU, RPC latency and status-resolution RPC counts.

### 2.20 T22. Stale checkpoint file

Mechanism (review M5, INFERRED): the re-seed deletes the tablet and then the
file (`local_catalog_poller.cc:274-278`), and `DeleteFile` is only
`WARN_NOT_OK`. A crash or a failed delete between the two leaves a fresh copy
loading the old position (`LoadCheckpoint`, `:188-209`). After
`CHECKPOINT_TOO_OLD` the old position is rejected again (liveness only);
after a restore, pre-restore records between the old position and the
restore op are re-applied onto the post-restore copy and their safe times
move it to serving until the batch crossing the restore triggers another
re-seed. Fix: delete the file first and fsync the directory, or tie the file
to the tablet's identity.

### 2.21 T23. Memstore flusher

Mechanism (review M6, VERIFIED): the flusher's candidates come from
`GetTabletPeers()` (`ts_tablet_manager.cc:641-646`), which excludes the copy,
but the copy shares `tablet_options_` and counts toward the global memstore
limit (`tablet_memory_manager.cc:365-400`). Up to `memstore_size_mb` (128 MB)
of its WAL stays anchored and is replayed on restart; on a 4 GB node about
38 % of the memstore budget cannot be reclaimed; on a node whose limit is
below 128 MB, `TabletToFlush` can return null while the limit is exceeded and
the `while (Exceeded())` loop spins. Fix: include the copy in the flusher's
candidates.

### 2.22 T24. Master WAL retention

Design: [plan2 §7.3](plan2.md#73-master-wal-retention). Nothing added: the
copy relies on master's ordinary log retention (`log_min_seconds_to_retain`,
900 s) and re-seeds on `CHECKPOINT_TOO_OLD` (`CheckpointTooOldReseeds`).
Copies do not anchor master's WAL (no `cdc_state` row, no retention hook).
Follow-up: a retention policy per stream, the 4 h staleness interplay,
automatic re-seed on eviction.

### 2.23 T25. Major version upgrade

Design: [plan2 §10](plan2.md#10-modes-in-which-the-local-tablet-is-not-used).

Decision 2026-10-17: no special case. The copy keeps serving through a YSQL
major version upgrade, and the guard that takes it out of the read path is
removed. Code pointers below are against `5886f97e64e`.

Today's guard: `TSHeartbeatResponsePB.ysql_major_version_upgrade_in_progress`
(field 35, from `YsqlManagerIf::IsMajorUpgradeInProgress()`), consumed by
`SetMajorVersionUpgradeInProgress` (`heartbeater.cc:590-591`,
`local_catalog_replica.cc:220`), makes `IsServing()` false while set
(`local_catalog_replica.cc:216-228`). It was not added because of a test
failure. It went in with the first commit of the feature, 25f011a795f, on the
user's instruction, for two reasons: the upgrade window has two catalog
version tables in play, and DDLs such as `REFRESH MATERIALIZED VIEW
CONCURRENTLY` run inside it (worklog2 "Addendum handling: initdb, YSQL
upgrade, major version upgrade, explicit read times"; "What is not done").
No upgrade behavior was designed or tested. The only later change it caused:
`FirstQueryLatencyAfterRestart` showed that holding PG's start on
`IsServing()` would hold PG until the timeout during an upgrade, so the hold
checks `state()` instead.

What a major upgrade does (`master/ysql/ysql_initdb_major_upgrade_handler.cc`):

| Phase | What happens | Catalog writes allowed |
|---|---|---|
| Masters on the new binary | The upgrade is in progress while master's persisted catalog version differs from its build's version (`:156`), until finalize. | forced updates only (`IsWriteToCatalogTableAllowed`, `:295-320`) |
| `PERFORMING_INIT_DB` | Master aborts the open transactions on its system catalog tablet (`:431`). The new PG's initdb creates the new-version catalog tables in the same system catalog tablet; their table ids carry a version byte (`common/entity_ids.cc:126-140`). | same |
| `PERFORMING_PG_UPGRADE` | pg_upgrade fills the new tables. `UpdateCatalogVersions` copies the version rows from the old-version version table into the new one and deletes the new-version invalidation messages (`:441-457`). | same |
| `MONITORING` | Tservers restart one by one on the new binary. New PG reads the new-version tables; old PG reads the old-version tables. | writes to either version table are rejected (`:300-308`) |
| Finalize (`DONE`) | Requires every tserver on the new version (`:234-236`). Master starts reading the new-version version table. A cleanup deletes the old-version tables (`:760-762`, `DeleteYsqlDBTables` with `kMajorUpgradeCleanup`). | normal |
| Rollback | Requires every tserver back on the old version (`:245-247`). The new-version tables are deleted. | — |

During the upgrade, DDLs increment no catalog version by default
(`pg_yb_utils.c:3646-3655`).

Why the copy needs no special case:

1. Both catalogs are in master's system catalog tablet, so the copy holds both.
   Every upgrade write reaches the copy through master's WAL as pushed pages:
   PG's writes go through the normal write path, and `CopyPgsqlTables` and
   `DeleteAllYsqlCatalogTableRows` write through `SysCatalogWriter`
   (`sys_catalog.cc:2016`, `:2059`).
2. Every catalog that a running backend reads is frozen during the upgrade.
   - The old-version tables accept only forced updates from the start.
   - The new-version tables are written only while the catalog is built,
     before any backend reads them.
   - Writes to both version tables are rejected during `MONITORING`.
3. Building the new catalog (`PERFORMING_INIT_DB`, `PERFORMING_PG_UPGRADE`)
   changes only the new-version tables, which no running backend reads yet.
   The copy keeps serving the old-version tables to old-binary backends.
4. A backend switches catalogs only when its tserver restarts on the new
   binary, which happens during `MONITORING`, after the new catalog was
   written. The restart rule holds PG's start until C ≥ B, where B is the
   clock reading at the new process's first lease. So when PG starts, the copy
   holds the whole new catalog and the copied version rows.
   - For the PG11 to PG15 upgrade, the old binary has no copy, so this restart
     is a first bootstrap, and PG is held until the copy is seeded.
   - T3(b) (open catalog transactions copied by remote bootstrap) cannot occur
     here: master aborted them when the upgrade started, and catalog writes
     stay blocked afterwards.
5. The two version tables hold the same values for the whole window:
   `UpdateCatalogVersions` copied them, and no version write is allowed
   afterwards. At finalize, master switches the table it reads versions from,
   and no tserver sees a version change. With a gate that compares hybrid
   times (C against the bound a release or report carries), which table master
   reads the versions from does not matter.
6. Rollback restarts the tservers on the old binary, so the restart rule
   applies again. The old-version tables were never changed.
7. `REFRESH MATERIALIZED VIEW`: only the in-place refresh is allowed during
   the upgrade (user, 2026-10-17). It turns on forced catalog modification only
   for its temp-table operations (`matview.c:317-332`). Those rows belong to
   the refreshing session, which runs the refresh in DDL mode and reads master.
   No other session can see them.
8. Forced catalog updates (`yb_force_catalog_update_on_next_ddl`) change rows
   without a version increment. Cached entries are not invalidated, the same
   as without the copy. A new catalog cache miss sees the change after the
   next push. This is an operator-only tool.

Change: remove `major_version_upgrade_in_progress_` from `IsServing()` and
the `SetMajorVersionUpgradeInProgress` call in the heartbeater; remove field
35 from `TSHeartbeatResponsePB` if nothing else reads it. Review item O4 (the
serving gauge disagrees with `IsServing()`) then applies only to
`TEST_local_catalog_disable_serving`.

Lock releases and push during the upgrade (checked 2026-10-17): no release RPC
is sent per DDL or at the end, so nothing moves C on any tserver during the
upgrade except the timer and floor requests.

| Phase | Writer | Object locks | Release RPC |
|---|---|---|---|
| initdb of the new catalog, pg_upgrade | PG processes started by master, connected to master's own `PgClientServiceImpl` (`master.cc:366`) | none: `ShouldEnableTableLocks()` is false under `YBCIsInitDbModeEnvVarSet()` and `YBCIsBinaryUpgrade()` (`pggate.cc:507-510`), so `PgSession::AcquireObjectLock` returns at once (`pg_session.cc:1394-1403`); no version-table lock because `IsBinaryUpgrade` gives `YB_DDL_MODE_NO_ALTERING` (`pg_yb_utils.c:3682`) | none: `MasterTabletServer::ts_local_lock_manager()` is `nullptr` (`master_tserver.cc:133-135`), so `IsObjectLockingEnabled()` is false (`pg_client_session.cc:4864-4868`) and `ReleaseObjectLocksIfNecessary` returns before building a request (`:4774-4776`) |
| `UpdateCatalogVersions` | master, `SysCatalogWriter` (`:441-457`) | — | none |
| finalize cleanup, rollback deletions | master, `DeleteYsqlDBTables` | — | none |

What makes a new-binary tserver see the new tables is the restart hold, not a
release: the restarted process's first lease refresh runs `OnLeaseGained(MaxGlobalNow)`
(`ysql_lease_manager.cc:160-186`); B is a clock reading taken after every upgrade
write committed (the restart happens in `MONITORING`); master pushes the pages
from the copy's position, or answers "re-seed" when the upgrade's volume exceeded
its WAL retention (15 min), and the RBS copies everything; PG starts only when
C ≥ B. The cost lands on the rolling restart: each tserver's PG start waits for
the whole upgrade volume at that moment. Optional: a push cycle triggered by the
upgrade handler after `UpdateCatalogVersions`, before `MONITORING`, pre-loads
every copy while the old binaries keep serving.

Open:

- Table removal records. The finalize cleanup and the rollback delete catalog
  tables on master. On the copy those deletions arrive as table removal
  records. Not checked: that the applier removes the tables from the copy's
  metadata. Worst case after a rollback and a later retry: the create records
  for the same table ids fail on the copy, N apply failures, re-seed.
- Faster periodic push during the upgrade: the new-version catalog (initdb plus
  pg_upgrade of every database) reaches the copies only through the timer and
  floor requests, so the rolling restart pays the whole volume at each PG start.
  Run the timer cycle more often while the upgrade is in progress, or trigger a
  push cycle from the upgrade handler after `UpdateCatalogVersions`, before
  `MONITORING`, so every copy is pre-loaded while the old binaries still serve.
- Not checked: whether a tserver on the new binary can start PG before the
  upgrade reaches `MONITORING`. If it can, master's own new-version tables are
  incomplete too: an upgrade-orchestration property, not a copy one.

Test: the `ysql_major_upgrade_*-test.cc` suites under
`src/yb/integration-tests/upgrade-tests/` with the copy on, including
rollback (`ysql_major_upgrade_matview-test.cc` covers the refresh case); add
a check that catalog reads are served by the copy during `MONITORING` and
after finalize (`local_catalog_reads_served` grows, no re-seed).

### 2.24 T26. Observability

Design: [plan2 §8](plan2.md#8-observability). From the 2026-09-28 review:

- O1 (High): a stalled copy looks healthy; poll and apply failures never
  change the serving gauge; `local_catalog_lag_us` is sampled only when C
  is published; no histogram times the read-side waits.
- O2 (High): EXPLAIN "Catalog Read Requests"/"Catalog Read Execution Time"
  and pg_stat_statements `catalog_wait_time` count copy reads as master
  reads (`pg_doc_op.cc:124-175`, `pg_doc_metrics.cc:43-47`); `explain-analyze.md:58`
  defines the count as reads from YB-Master; `PgPerformResponsePB` has no
  field saying which source answered.
- O3 (High): T9.
- O4 (Medium): serving gauge vs `IsServing()` during a major upgrade or
  `TEST_local_catalog_disable_serving` (`local_catalog_replica.cc:216-228`).
- O5 (Medium): `reads_to_master_in_ddl` counts every DDL-mode Perform
  including writes (check at `pg_client_session.cc:3770` runs before the
  read check); `reads_to_master_not_serving` merges about eight causes and
  counts a snapshot's first request twice (`:3576` via `:4168`, then
  `:3789`); `pushes_refused` includes the TryAgain for "another push in
  progress" (`local_catalog_poller.cc:484`).
- O6 (Medium): traces of shared-memory requests served locally are dropped
  (dumped only in the `FlushAsync` callback, `pg_client_session.cc:3440-3466`).
- O7 (Medium): ASH samples the copy's waits as `OnCpu_Active`
  (`pg_client_service.cc:838`); only the tablet safe-time wait shows as
  `MVCC_WaitForSafeTime`.
- O8 (Medium): `gate_wait_us` leaves out `ApplyPushedBatch`; no counter for
  gate failures (`ts_local_lock_manager.cc:563-590`).
- O9 (Low): poll warnings blame master for local failures
  (`master_leader_poller.cc:137` logs "Failed to poll <master leader>" for
  apply failures, checkpoint write failures, bootstrap errors and the
  harmless TryAgain while waiting for initdb or the copy's own election);
  `poll_failures` does not count bootstrap failures.
- O10 (Low): the routing log says "see the wait lines above", but those are
  VLOG(1) and absent when only `--log_local_catalog_read_routing` is set
  (`pg_client_session.cc:3812`).
- O11 (Low): per-copy state is not visible from master;
  `SysCatalogChangeServiceImpl::streams_` is logged only on create and drop;
  `pushes_skipped` merges four reasons, all VLOG(1).
- O12 (Low): copy fetches look like ordinary master remote bootstraps of the
  system catalog tablet (sessions gauge, INFO logs); the copy's disk use is
  absent from tablet reports.
- O13 (Low): master's `TabletServerService_Read` count no longer
  approximates cluster catalog load or invalidation storms; tests that
  count it were pinned off by 01317fa1d69.
- O14 (Low): the copy's tablet metrics are registered in the tserver's
  shared registry (`ts_tablet_manager.cc:2397`), so `/metrics` and
  `/prometheus-metrics` show tablet `00000000000000000000000000000001` of
  master's `sys.catalog` table on every tserver; dashboards that aggregate
  by table would show a system catalog table on tservers (not checked on a
  live cluster).

Additions agreed: gauges `local_catalog_applied_op_index`,
`local_catalog_safe_time_lag_ms` sampled continuously,
`local_catalog_last_poll_age_ms`, applied version per database; counters
split by reason, once per request; a read-side wait histogram and ASH wait
codes; master `local_catalog_change_polls`, `local_catalog_resolve_intents_us`,
`local_catalog_requester_streams`; EXPLAIN served-by-copy count; debug pages
`tserver:9000/local-catalog` and `master:7000/local-catalog-copies`
(following `tserver-path-handlers.cc:473-493`; `TSDescriptor::LocalCatalogAppliedOpId`
exists, `ts_descriptor.cc:232`).

Also: `pg_cache_response_trust_auth` and the "own-writes floor latency"
items from the 2026-09-26 discussion: tserver-triggered polls are not
forced, so a wait pays up to 250 ms whenever another catalog transaction runs
on master; the poll RPC carries no hybrid time, so a wait also pays for
master's clock being behind; one change covers both: the poll request carries
the wait target, master updates its clock from it and forces a new S for that
request.

### 2.25 T27. Test coverage and open CI items

- Java regress schedules with the feature on: `yb.orig.schema`,
  `yb.orig.select`, `yb.orig.guc`, `yb.orig.query_consistent_snapshot`,
  `TestPgRegressPgTable`. Not run.
- Two tests with a flat 2 s propagation deadline
  (`WaitForCatalogVersionToPropagate`, `libpq_test_base.cc:417-424`):
  `PgCatalogVersionTest.IncrementAllDBCatalogVersions`,
  `PgBackendsTest.MultipleWaiters`; make them wait on the condition. T1
  reduces the delay they see.
- Not explained: `TestPgSequencesWithServerCacheFlag#testCacheFlagValueHigherThanCacheOption`
  and `#testLowerThanDefaultCacheFlagValue` (clean locally, 5-6 CI attempts;
  no copy-off control run); `BackupUpgradeTest.TestRestoreBackupAfterRollback`
  (631 s harness timeout in `RunBackupCommand`, needs yb-controller).
- The `pg_vector_index-test` cluster of launch 169341 was a CI-experiment
  artifact (worklog2 "Third default-on CI run").

### 2.26 T28. Cluster-wide catalog lock

Design: [plan2 §14.4](plan2.md#144-long-term-a-cluster-wide-catalog-lock).

- Exclusive half, exists: every version-incrementing path takes
  `LockRelationOid(YBCatalogVersionRelationId, ExclusiveLock)` in non-legacy
  mode (`yb_catalog_version.c:499` via `YbIncrementMasterCatalogVersionTableEntry`
  from `pg_yb_utils.c:3372`; `:749`, `:855` for the database rows). With
  object locking every `LockAcquire` becomes `YBCAcquireObjectLock`
  (`lock.c:999`); `ExclusiveLock` is not a fast-path mode, so it goes by RPC
  to master and is fanned out (`pg_session.cc:1394-1415`,
  `pg_client.cc:1195-1212`). Each tserver's release handler gates, publishes,
  then unlocks (`ts_local_lock_manager.cc:577-623`).
- Reader half, missing: `LockRelationOid(YBCatalogVersionRelationId,
  RowShareLock)` before an executor scan of a catalog relation. `ROW_SHARE`
  maps to `[kWeakObjectLock] {kStrongRead}` and `EXCLUSIVE` to
  `[kWeakObjectLock] {kStrongRead, kWeakWrite}`, which conflict
  (`lock_util.cc:145-155`, `:165-167`, `:195-199`); `ACCESS_SHARE` maps to
  `{kWeakRead}` and does not conflict. `kRowShare` is a fast-path type
  (`object_lock_shared_fwd.h:21`, `object_lock_shared_state.cc:201-208`);
  `PgSession::AcquireObjectLock` tries shared memory first
  (`pg_session.cc:1405-1408`) and falls back to the RPC when a conflicting
  write intent is active locally; release is local with the transaction
  (`pg_txn_manager.cc:560`). Lifetime is transaction-scoped, so it is taken
  only for statements outside a transaction block; a session lock released
  at scan end would avoid that but is excluded from the fast path
  (`pg_session.cc:1406`).
- Not verified: same-owner upgrade `ROW SHARE` → `EXCLUSIVE` in a block that
  reads `pg_class` and then runs a DDL; same shape as DML-then-DDL on one
  table.

### 2.27 T29. External intent cleanup

Mechanism (review L11): the compaction filter removes staged external
intents older than `external_intent_cleanup_secs` (24 h,
`docdb_compaction_filter_intents.cc:224-240`); a DDL transaction that commits
after that loses its rows on the copy (the APPLY finds nothing). Fix: exempt
the copy, or re-seed when an APPLY record finds no staged intents.

### 2.28 T30. Explicit read time below the cutoff

Mechanism (review L4, INFERRED): a freshly seeded copy inherits master's
compacted files but its own history cutoff starts low
(`tablet_retention_policy.cc:84-128`, `LocalCatalogAllowedHistoryCutoff`,
`ts_tablet_manager.cc:4004`). A `yb_read_time`, `ysql_dump --read-time` or
walsender read below master's cutoff can return the state as of the cutoff
with no error where master would return SnapshotTooOld. Not checked whether
the superblock carries a cutoff; implementation2 §4 says the committed cutoff
is seeded from the SST frontiers copied from master.

Decided 2026-10-18: give the copy a serving cutoff that can be later than
master's. Any cutoff at or above every cutoff master used when it compacted the
copied files is correct; a later one only sends more old explicit-time reads to
master, which serves them or fails with SnapshotTooOld as today.

- Value at seed (and at every re-seed): the maximum of the seed time minus
  `timestamp_syscatalog_history_retention_interval_sec` and the value seeded
  today. Master's system catalog cutoff is now minus that interval, or earlier
  when a snapshot schedule or CDC needs more history, so every compaction
  cutoff master used before the seed is at or below the seed time minus the
  interval. This assumes the interval was never lowered and master and tserver
  use the same value.
- Reads below the serving cutoff go to master (plan2 §5.3 rule 6).
- The serving cutoff decides only which reads the copy serves. What compaction
  on the copy keeps is separate: PITR in place (pitr-handling.md §3.2) needs the
  copy to keep history back to master's schedule bound.

### 2.29 T31. Restore inside the copied WAL tail

Mechanism (review L12): a re-seed during a PITR restore can copy the
`RESTORE_SYS_CATALOG` entry in the WAL tail; the poller starts after that
tail and never sees the marker, and what the copy's own bootstrap replay of
that entry does is unverified. Fix: a restore counter on master compared by
the poller, or a bootstrap check that the copied tail holds no restore
entry.

### 2.30 T32. Tablet-wide flags

The copy is an ordinary tablet on the tserver, so every tablet-level flag,
test flag and global assertion applies to it. Two instances:
`TEST_file_to_dump_docdb_writes` (fixed, `tablet.cc:2238`) and
`TEST_invalidate_last_change_metadata_op`, set by `pg_libpq-test.cc:1840`,
which makes the copy's metadata take the upgrade branch
(`tablet_metadata.cc:1187-1191`), its apply marker `-1.-1`, and the
`kChangeMetadata` that adds the system catalog tables fails the check at
`:1355-1359`, turned into a glog FATAL by `operation_driver.cc:425`
(`PgLibPqTest.ReplayDeletedTableInColocatedDBPostUpgrade`, reproduces
deterministically). Rule: flags meant for user tablets exclude
`kLocalCatalogTabletId`; new tablet-wide behavior is reviewed against the
copy. A general sweep is preferable to a second point fix.

### 2.31 T33. Small poller and service items

Status 2026-10-07: L5, L6 and L8 went with the pull path. L7 and the
read-path mutex remain.

- L5: a memory-limit Busy with no messages comes back as "more to send" with
  no records, and the poller polls again at 0 ms (`consensus_queue.cc:802-808`,
  `local_catalog_poller.cc:155-157`).
- L6: the idle-pruning loop at `sys_catalog_change_service.cc:64` runs over
  all streams on every call, about 10M iterations/s at 1000 tservers × 10
  polls/s.
- L7: `routing_reason = Format(...)` runs for every Perform whether or not
  logging is on, and the served path makes two `localtime_r` calls
  (`pg_client_session.cc:3774`, `:3866-3868`); `ApplyRecords` decodes a key
  and builds a table-name string per record only for statistics.
- L8: lag and `OnSafeTimePublished` are recorded twice per poll, and also
  when C did not move (`local_catalog_poller.cc:403-404`, `:469-470`).
- Known rough edges: `applied_version` and the two wait functions take one
  mutex per call on every backend's read path; three tservers bootstrapping
  at cluster start serialize on master's RocksDB checkpoint lock, a second or
  two per tserver.

### 2.32 T34. Lease rejoin test

Status 2026-10-07: CLOSED (C26). `PgLocalCatalogRejoinTest` keeps a tserver
from refreshing its lease (`TEST_tserver_enable_ysql_lease_refresh=false`,
lease TTL 5 s), runs DDLs, re-enables the refresh and asserts that PG
starts only after the copy serves and that the first query reads the copy.

The test that existed, `LeaseLossTakesTheCopyOutOfTheReadPath`, was removed
(commit 668c499641f): lease expiry kills that tserver's PG sessions
(`YSQLLeaseManager::Impl::CheckLeaseStatusInner` → `OnLeaseLost()` →
`KillPg()`, `ysql_lease_manager.cc:296-302`), so nothing a client could see
was observed, and it was the suite's only flaky test. What is worth testing:
a tserver partitioned from master long enough to lose its lease and miss a
DDL, then reconnecting, its copy withholding reads until the poller has
applied a response at or after the new lease grant, reads going to master
meanwhile. Needs a way to keep the tserver from master without killing its
ability to reconnect, and an assertion on where reads went.

### 2.33 T35. Non-PG rows

Raised to Medium 2026-10-24 (design review: Mark, Nita). The system catalog
tablet also holds master's own records: tablet and table metadata, Raft
configuration and leader changes, OID allocation, tablespace and placement.
They are written by master directly, have no MVCC read-as-of semantics, and
nothing on the copy reads them, yet every change is pushed to every tserver
(and copied by every remote bootstrap). Under a leader-election storm or
load-balancer moves this is continuous traffic unrelated to PG.

Work:

1. Measure the volume: bytes pushed per minute during a leader-election storm
   and during a load-balancer rebalance, with no DDL running.
2. Filter in the page builder (skip records of master's own tables) and in the
   remote-bootstrap snapshot (or delete those tables from the copy after the
   transfer).
3. Keep what the copy does need, if anything: today the tserver's table cache
   fetches DocDB schemas from master (todos T17), not from the copy.
4. Interaction with PITR (T43): the restore's master-entries step is already not
   repeated on the copy; filtering removes the question of what later writes to
   those entries do on the copy.

### 2.34 T36. Wait latency

Status 2026-10-07: items 1 and 2 CLOSED by floor requests: the request
carries the waited-for time, master moves its clock to it and runs a cycle
for it. Items 3 and 4 remain.

From the 2026-09-26 discussion. The own-write wait (5.3 rule 3) and the
version wait trigger their own immediate poll, so a slow idle cadence does
not lengthen them, but:

1. A tserver-triggered poll does not force a new S, so while another catalog
   transaction runs on master the wait pays up to the 250 ms throttle
   ([plan2 §4.1](plan2.md#41-master-push-service)); the release path already
   forces (`force_apply_safe_time`, `sys_catalog_change_service.cc:93-100`).
2. The poll RPC carries no hybrid time (`GetSysCatalogChangesRequestPB`), so
   a wait also pays for master's clock being behind this tserver's clock.
3. The own-write wait applies to every catalog read of the session, not only
   reads of the temporary relation.
4. Wait timeouts are counted as "not serving" (T26, O5).

One change covers 1 and 2: the poll request carries the wait target (or the
tserver's clock), master updates its clock from it and forces a new S for
that request.

### 2.35 T37. repo6 notes copies

`repo6/notes/plan2.md` and `repo6/notes/implementation2.md` (committed on
`docdb_local_catalog`) diverged from this directory before 2026-09-28:
repo6's implementation2 holds the 2026-09-14 CI status and TODOs (now in
T4-T6, T27, T32); this directory's copies are the current ones. Replace the
repo6 copies with this directory's `plan2.md`, `implementation2.md`,
`todos.md` and `worklog2.md` in the next notes commit.

Done 2026-09-30: the repo6 notes commit of that day carries this directory's
four files. Closed as C21.

### 2.36 A1. DDL and DML in one transaction

Review M4 (INFERRED). The gate delays only the acknowledgement to the DDL's
own client. Rows written to user tables in the same transaction are visible
to everyone from the commit time; on each tserver the copy shows the catalog
change only once its release handler has finished. A worker that sees the
new data row and then asks the catalog for the new table can get "does not
exist" for one poll. DML against the new table is safe: it blocks on the
object lock, released only after the gate. `NOTIFY` sent from the DDL
transaction (`ybInsertPendingNotifiesToTable` at pre-commit, `async.c:1179`;
PostgREST's `ddl_command_end` trigger is the known user) leaks the same way;
unknown whether notification delivery latency already exceeds the copy's
lag. Master today has the same anomaly only through a stale negative cache
entry. Accepted as the release fan-out window; T8 removes the in-statement
variant for executor catalog scans.

### 2.37 A2. Releases that skip the gate

Review L2 (INFERRED). `PopulateDbCatalogVersionCache` only logs a warning
when master's version read fails (`object_lock_info_manager.cc:1192`), so the
release carries no versions and no tserver runs the gate; the inline-release
timeout fallback (`ts_local_lock_manager.cc:983-988`) acknowledges the
client without the gate. Master today loses the equivalent version push but
its executor catalog reads stay fresh; with the copy a read after such a DDL
can be served below the DDL for one poll, caught by the version check of
[plan2 §5.3 rule 2](plan2.md#53-picking-the-local-read-time) once the
version arrives by heartbeat. Accepted.


### 2.38 T38. Schema-change records: re-apply and ordering

Source: design review 2026-10 (Hari, Mark); reviewed against the code 2026-10-18 (`5886f97e64e`).
Design: [plan2 §4.2](plan2.md#42-applier). Related: T45 (CDC retention fields in the same kind of
record).

**What the records are.** A schema-change record (`CHANGE_METADATA_OP`) is a Raft operation on a
tablet that changes the tablet's metadata (its superblock): the DocDB tables it contains, their
schemas and schema versions, and tablet-wide settings such as WAL retention and CDC retention
barriers. Master's system catalog tablet is colocated: it holds master's own `sys.catalog` table
and every PG catalog relation of every database as a separate DocDB table. On it, the records come
from:

| Source | Record | Code |
|---|---|---|
| `CREATE DATABASE` | one record adding the template's catalog tables (about 90) when `batch_ysql_system_tables_metadata` is on | `catalog_manager.cc:4224-4290` |
| A YSQL migration that creates a catalog table or index (e.g. V68 creates `pg_yb_invalidation_messages`), initdb, a major upgrade's new-version catalog | one record per table added | `CreateYsqlSysTable`, `catalog_manager.cc:4007-4070` |
| `DROP DATABASE`, `DeleteYsqlSystemTable`, major-upgrade cleanup and rollback | one record per table removed | `catalog_manager.cc:7495-7516`, `sys_catalog.cc:2088-2093`, `DeleteYsqlDBTables` |
| Logical replication stream creation | one record per table re-sending the current schema unchanged, with CDC retention fields (T45) | `xrepl_catalog_manager.cc:1868-1900` |

Ordinary DDL changes catalog rows, not this tablet's metadata. No migration changes an existing
catalog table's schema: no migration file contains `ALTER TABLE`.

What exists:

- Master seals a page after every schema-change record, the producer's rule that such a record is
  the last of a batch (`xcluster_producer.cc:286-289`, push-plan §2.2). Each page therefore holds
  at most one schema-change record, at its end.
- The applier writes out the row batch it has accumulated, then submits the schema-change record
  as a `ChangeMetadataOperation` with the tablet id rewritten to the copy's
  (`local_catalog_applier.cc:671-743`, `:789-812`).

Items:

1. **Re-apply after a restart** (resolved by code reading). The checkpoint file is written about
   once a second, so after a restart master pushes again the records the copy applied after it.
   `ChangeMetadataOperation::Apply` is the same code tablet bootstrap uses to replay these records
   (`tablet_bootstrap.cc:1853-1871`):

   | Record | Second apply | Code |
   |---|---|---|
   | Add table(s) | the existing entry is kept, after a check that the colocation id is unchanged | `tablet_metadata.cc:1693-1724` |
   | Remove table | does nothing when the table is absent | `tablet_metadata.cc:1733-1748` |
   | Table schema, table present | skipped when the table's schema version is already at or above the record's ("Already running schema version") | `change_metadata_operation.cc:166-190`, `:233-239` |
   | Table schema, table already removed | fails: the schema-version lookup returns an error (`:174-180`), and a failed apply is a glog FATAL (`operation_driver.cc:425-427`), so the copy crash-loops | — |

   The last case is closed by T45 item 3: the applier skips a table-schema record whose table is
   absent from the copy's metadata, as tablet bootstrap did before D19063
   (`tablet_bootstrap.cc:1820-1826`).
2. **Durability ordering** (decided). The superblock flush on apply is normal durability: it
   records `last_flushed_change_metadata_op_id`, bootstrap replays only the schema-change records
   after it (`tablet_bootstrap.cc:1171-1175`), and a WAL segment holding a metadata change can be
   deleted only after the flush (`:849-853`). Rule for the copy, around each schema-change record:
   1. apply the page's row records and write them out;
   2. persist the checkpoint at the position just before the schema-change record (records carry
      op ids);
   3. apply the schema-change record;
   4. persist the checkpoint at the record's position.

   A crash between steps 2 and 4 replays only the schema-change record, which item 1 shows is
   safe. A crash after step 4 replays nothing from before the change. No replay crosses a schema
   change, so:
   - rows of a removed table are never replayed after the table left the copy's metadata;
   - rows packed under a schema version older than the change are never replayed. The copy runs
     old-packing cleanup after compaction (`enable_schema_packing_gc`, `OldSchemaGC`,
     `tablet.cc:651-666`) without xCluster's minimum-version floor
     (`FillMinXClusterSchemaVersion`). Without this rule, a compaction that repacked a table's rows
     could let that cleanup drop a packing that replayed rows still need.

   Both persists happen only after the copy's own WAL is durable (T7). Cost: two synchronous
   checkpoint writes per schema change; schema changes come only with `CREATE DATABASE`,
   `DROP DATABASE`, migrations and upgrades.
3. **The xCluster rule** (resolved). xCluster forces the stream checkpoint to be stored after a
   batch that ended with a schema change (`xcluster_poller.cc:403-408`, `cdc_service.cc:2115`), so
   a restarted poller replays at most one schema version back. Its reason: replayed rows are packed
   under the source's schema version and need a producer-to-consumer mapping for it ("Schema
   version mapping for $0 not found", `xcluster_write_implementations.cc:91`) and the matching old
   packing on the target. The page-seal rule plus item 2 gives the copy a stronger guarantee: no
   replay crosses a schema change.
4. **Catalog version bumps must not be skipped** (Hari). Holds by construction where versions are
   read from the copy (legacy mode, support_object_locking_off.md §3 item 4): versions are read at
   C and published only forward, so several bumps inside one page are safe. Under object locking
   no version is read from the copy (simplified_with_object_locks.md).

Test: restart a tserver with its checkpoint older than the copy's metadata (`TEST_` flag that skips
`PersistCheckpoint`) in the middle of `CREATE DATABASE` (about 90 new tables,
`NewDatabaseCotablesReachTheCopy` shape), in the middle of `DROP DATABASE`, and after a
logical replication stream creation followed by a `DROP DATABASE` of the stream's database; expect
the copy to serve and to match master. Inject a crash between steps 2 and 4 of item 2.

### 2.39 T39. Re-seed storms and peer seeding

Source: design review 2026-10 (Hari, Kannan); post-perf-design §3 item 5;
push-plan §4 "Optional later". Design: [plan2 §6.2](plan2.md#62-re-seed).

Cases where many tservers fetch the copy from master at once:

- An AZ that returns after master's WAL retention (15 min default, plan2
  §7.3): each tserver is told to re-seed.
- A PITR restore of the system catalog: every tserver re-seeds its whole
  copy (plan2 §6.2).
- Cluster start: three tservers already serialize on master's RocksDB
  checkpoint lock, one to two seconds each (T33).

Master bounds the WAL pushes (64 RPCs, 64 MB in flight) but not the snapshot
transfers. Change:

1. Bound concurrent copy fetches on master; queue the rest (Kannan: serve,
   for example, 10 at a time).
2. Let a tserver seed its copy from a peer tserver's copy in the same region.
   A peer's copy holds only certified state (regular rows up to its C, open
   transactions as external intents, no uncommitted WAL tail), so this path
   also avoids T3(a) and T3(b). Master names the peer and the position the
   seed starts from.

### 2.40 T40. Database clone and re-seed

Source: design review 2026-10 (Hari: clone can produce a very large
payload). Design: [plan2 §6.2](plan2.md#62-re-seed).

Clone finishes with a snapshot restore of the target namespace
(`CloneStateManager`, `clone_state_manager.cc:870`,
`snapshot_coordinator().Restore`). The snapshot coordinator restores the
system catalog only in the initial phase of a snapshot that belongs to a
schedule (`master_snapshot_coordinator.cc:2199`). Not checked: whether the
clone's target snapshot carries a schedule id, so whether a clone writes a
`RESTORE_SYS_CATALOG` operation. If it does, every clone sends every tserver
a restore page and re-seeds every copy (T39 at its worst). Then either give
clone a restore record limited to the cloned database's rows, or carry the
restored rows as pages.

Test: clone a database with the copy on; count re-seeds
(`local_catalog_reseeds`) on every tserver; query the clone from a tserver
that did not run the clone.

### 2.41 T41. Lease-loss guarantee and the backend's version source

Closed 2026-10-10 (C27).

- Item 1: plan2 §7.1 now states the dependency in the paragraph "What a new
  backend's catalog version rests on".
- Item 2: no change. Lease loss keeps killing the tserver's PG backends, as
  it does without the copy. The copy adds no reason to replace that with
  propagate calls. The notes do not record the problem Mark raised with the
  kill behavior beyond the line in this item.

Source: design review 2026-10 (Mark). Design:
[plan2 §7.1](plan2.md#71-lease-loss-lease-regain-and-tserver-restart),
[§13](plan2.md#13-how-the-pg-backend-flow-changes).

Today a tserver that loses its lease kills its PG backends, and a new
backend reads the latest catalog version from master. With the copy, a new
backend reads the version row from the copy at C (plan2 §13,
implementation2 §9). That is safe only because PG's start is held until the
copy serves at the lease bound B, and because the copy enters serving only
when A[db] covers every version published while it was not serving (plan2
§7.1). Change:

1. State this dependency in plan2 §7.1: the lease guarantee now rests on the
   serving condition, not on master reads, and the hold timeout
   (`local_catalog_serve_wait_timeout_ms`) falls back to master reads, which
   keeps the old guarantee.
2. Mark to evaluate delivering the latest version to running backends
   through propagate calls instead of killing them, and whether that needs
   anything from the copy beyond the gate.

### 2.42 T42. Memory accounting of the copy

Source: design review 2026-10 (Mark: the copy must count toward the memory
tracker). Master's page cache is charged to the `SysCatalogPusher` tracker
(implementation2 §2); T23 covers only the memstore flusher on the tserver.
List each tserver-side consumer and the tracker it is charged to, and add a
`local_catalog` tracker under the tserver root where none is:

- the copy's memtables (shared `tablet_options_`, counted in the global
  memstore limit, T23);
- the applier's pending pushes (`pending_pushes_`, release pages up to 4 MB
  each plus separate pushes);
- parsed pages during apply and the external-intent batches;
- read-path buffers of locally served reads;
- the copy's share of the block cache.

### 2.43 T43. PITR restore reaches the copies in place

Plan: [pitr-handling.md](pitr-handling.md). Found in the CI triage of diff
320164 (2026-10-10).

Mechanism: master marks a restoration complete without waiting for the
copies. The restore raises the database's catalog version on master only;
the tserver learns the new version from its next heartbeat. A connection
opened in between starts at the old version, the version wait passes, and
it reads the copy at a C below the restore. Without the copy the same
connection reads master and sees the restored rows. Separately, the restore
page makes every copy re-seed by remote bootstrap, for a restore of one
database.

Change (pitr-handling.md §3):

1. The restore page carries the database oid, restore time, write time and
   new version; the copy sets the same hybrid-time filter on its own files
   and writes the version row, with no re-seed.
2. Master sends its catalog history bound to the copies; each copy keeps
   history back to it.
3. `FinishRestoration` reports the restore complete only after every lease
   holder's copy has acknowledged the restore page.
4. A restore time older than the copy's history, or a failed in-place apply,
   falls back to a re-seed.

Open items: pitr-handling.md §5.

### 2.47 T47. xCluster DDL queue handler

Source: design review 2026-10-24 (Mark). `CREATE EXTENSION` and the DDLs the
xCluster DDL queue handler replays on the target are ordinary DDLs: their
releases reach every lease holder before the DDL returns, and a backend that
starts after a lease loss waits for the copy. Nothing about the extension needs
a master read. What has to be checked is the handler's own catalog read path:
it applies replicated DDLs through a PG connection with a consistency GUC and
may depend on reading the catalog at a particular time or at master's latest;
document which reads it issues and whether any of them must bypass the copy.

### 2.48 T48. Release timeout

Source: design review 2026-10-24. Today, when the synchronous release does not
complete within `tserver_yb_client_default_timeout_ms` (60 s), the DDL's tserver
ignores the error and responds success (`ts_local_lock_manager.cc:985-991`,
"falling back to async release"). The design (local_catalog_docdb_design.md
§5.6, §7.4) returns an error instead: the DDL may have committed, but its effect
is not in force on every tserver, the same situation as a lost commit response.
Master keeps completing the release in the background.

The user then has no way to know when the DDL is in force everywhere, and
running it again does not help (`CREATE ROLE` fails with "already exists").
Change:

1. Return an error from the DDL after the timeout; wording that says the effect
   is unknown and propagation continues.
2. Master UI and metrics: list in-progress releases with the DDL's transaction,
   start time, and per lease holder whether it has acknowledged (design §11.1
   item 4), so a DDL that timed out can be followed to completion.

### 2.44 T44. The release floor moves on every retry

Source: discussion 2026-10-17. Design: [simplified_with_object_locks.md §4.1](simplified_with_object_locks.md#41-lock-release).
Code pointers are against `5886f97e64e`.

The floor a tserver waits for before it publishes a release's versions and releases the DDL's
locks is today `catalog_versions_read_time`. Master sets it when it reads the version map for the
release (`PopulateDbCatalogVersionCache`, `object_lock_info_manager.cc:1133-1160`, with
`use_cache=false`). That read starts at the system catalog tablet's current safe time, about
master's "now" (`SysCatalogTable::ReadWithRestarts`, `sys_catalog.cc:978`), and restarts at any
write it meets inside its uncertainty window (`:980-994`). The read time is therefore at or above
the commit of every version in the map, by the clock skew bound (C19). It is not a commit time:
each new read gives a later time.

Which retries read again:

| Retry | Same floor? |
|---|---|
| Master resends to one tserver after an RPC transport failure (`UpdateTServer::SendRequest`, `:1902-1908`) | yes: the request was filled once before the fan-out (`:1275`) |
| A tserver answers with an error, for example its wait for the floor timed out (`ts_local_lock_manager.cc:577-595`, `tablet_service.cc:4013-4014`) | master does not retry this attempt: the task fails (`HandleResponse`, `:1920-1945`) and master returns the error to the DDL's tserver |
| The DDL's tserver resends the global release after any failure, at once, without backoff (`ReleaseWithRetriesGlobalNow`, `ts_local_lock_manager.cc:111-122`) | **no**: master reads the versions again, runs a new push cycle and sends to every lease holder again |
| Master leader failover (`RelaunchInProgressRequests`, `:1490-1507`) | **no**, same as above |
| Lost-message re-release after the transaction's deadline (`ScheduleReleaseForLostMessages`, `ts_local_lock_manager.cc:321-334`) | **no**, same as above |

Effect:

- A healthy tserver passes each attempt after one apply: the attempt carries pages up to its own
  push cycle's S, which is at or above its own read time. A tserver that already handled an
  earlier attempt waits again before it acknowledges.
- A tserver that is slow but catching up chases a target that moves with every attempt. If it
  needs longer than one attempt's deadline to reach "now", every attempt fails on it and the
  release never completes there. With a floor that stays fixed, it would pass as soon as it
  caught up.
- Every attempt costs master a version read, a push cycle and a fan-out to every lease holder.

Fix candidates:

1. **Master keeps the floor in memory.** On the first processing of a release, master keeps the
   version map and its floor in memory, keyed by the release, until the release completes. A
   resend from the DDL's tserver and a lost-message re-release reuse them. A master failover
   computes them once more on the new leader. Reusing an older map later is safe because a
   release sets `ignore_catalog_version_staleness_check` (`object_lock_info_manager.cc:1158`).
   Not checked: whether master recognizes the DDL tserver's resent request as the release it
   already has in progress.
2. **The floor is the newest commit time among the versions in the map** (preferred). Each
   version was written by its DDL's transaction, so the hybrid time at which its row was written
   is that DDL's commit time. The maximum over the versions in the map is the highest commit time
   the tserver needs, and nothing more. It does not change between attempts while the map is
   unchanged, and it needs nothing kept or recomputed.
   - Source: the version's row in `pg_yb_invalidation_messages`. Each PG-side increment runs
     `yb_increment_db_catalog_version_with_inval_messages` or
     `yb_increment_all_db_catalog_versions_with_inval_messages` (`pg_proc.dat:8145-8155`):
     one statement that deletes rows older than the expiration (only this database's rows, or
     every database's rows for the all-database variant), updates the version row, and inserts one
     messages row per new version, with NULL messages when there are none. That row is inserted
     once, inside the DDL's transaction, and never updated, so its write time is exactly the DDL's
     commit time. The row of a database's current version is never deleted by that cleanup: a
     row is deleted only by a later increment of the same database (all databases for the
     all-database variant), which inserts a newer current row in the same statement.
   - `pg_yb_catalog_version` cannot be the source: its row is updated in place by every
     increment, and with packed rows its last update time cannot be read exactly.
   - Cases where the current version of a database has no messages row:
     1. The database's initial version. `YbCreateMasterDBCatalogVersionTableEntry`
        (`yb_catalog_version.c:743`, called from `yb_cmds.c:161` for `CREATE DATABASE`) inserts
        the version row as a single-shard write with no messages row; initdb's databases are the
        same. This lasts until the first DDL in that database.
     2. A PITR restore. Master writes the restored database's version as current + 1
        (`PgCatalogRestorePatch::Finish`, `restore_sys_catalog_state.cc:196-210`) and writes no
        messages row (pitr-handling.md §2).
     3. A major version upgrade. `UpdateCatalogVersions` deletes every row of the new-version
        messages table after copying the version rows
        (`ysql_initdb_major_upgrade_handler.cc:441-457`). This lasts until the next DDL in each
        database.
     4. Clone (T40): not checked.
     5. Out of scope while the copy is on: increments when invalidation messages are disabled or
        the messages function or table does not exist yet (the update-only path,
        `yb_catalog_version.c:563-600`), and manual writes to the version row (T6, disabled).

     A dropped database's rows are deleted (`YbDeleteMasterDBInvalidationMessagesTableEntries`,
     `yb_catalog_version.c:798`), but the database also leaves the version map, so it needs no
     floor.
   - Fix for the missing rows: make "every current version has a messages row" an invariant by
     inserting a row with NULL messages at the three writers above: database creation (in the
     same write as the version row), the PITR restore (in the restore's own write), and
     `UpdateCatalogVersions` (one row per database's current version, instead of an empty
     table). PG already inserts NULL-message rows when it has no message list
     (`GetInvalidationMessages`, `yb_catalog_version.c:133-141`). Not checked: how the tserver's
     message queue and the backends treat a NULL list after a restore or an upgrade; a full
     catalog refresh is what a backend needs there anyway (pitr-handling.md §5 item 2).
   - Fallback when a row is still missing: use master's read time for that database. It is a
     valid upper bound, but it moves on every attempt.
   - A release whose transaction changed no version gets the commit time of the newest version
     already published, which the copy normally holds already, so tservers pass at once. This
     also removes the push cycle such releases cost today (simplified_with_object_locks.md §7
     item 5).
   - If another DDL committed between two attempts, the map of the later attempt includes its
     version and the floor moves to that DDL's commit time. The tserver needs that DDL's rows
     anyway before it can publish its version.
   - A row of a committed transaction is applied at the transaction's commit hybrid time, so its
     write time is the commit time. Not checked: which time a read reports for a value that is
     still an intent of a committed transaction not yet applied on master.
   - Today's read path does not return write times for these tables. `DocRowwiseIterator` fills
     `QLTableColumn::write_time` only outside flat document mode (`doc_rowwise_iterator.cc:900-917`),
     and master reads its catalog tables with that iterator (`sys_catalog.cc:1055-1078`). This
     needs a change for the messages table.
   - The heartbeat can carry the same floor, for the cases where the copy does not serve
     (simplified_with_object_locks.md §4.3).

Test: slow one tserver's apply (`TEST_local_catalog_pushed_batch_apply_delay_ms`) to longer than
one attempt's deadline, run a DDL elsewhere, and assert that the release completes on that tserver
once its copy catches up. Count master's version reads and push cycles per DDL before and after.

### 2.45 T45. CDC retention fields in schema-change records

Source: discussion 2026-10-18. Code pointers are against `5886f97e64e`.

When a logical replication (CDCSDK) stream is created, master writes one schema-change record
(`CHANGE_METADATA_OP`) on its system catalog tablet for each of `pg_class`, `pg_publication_rel`
and `pg_replication_origin` (`SetAllInitialCDCSDKRetentionBarriersOnCatalogTable`,
`xrepl_catalog_manager.cc:1868-1900`). The record re-sends the table's current schema at its
current version, so it changes no schema. It also carries `retention_requester_id` (the stream id),
`cdc_sdk_require_history_cutoff` and, when set on the table, `wal_retention_secs`.

The copy's applier submits every schema-change record unchanged, with only the tablet id rewritten
(`LocalCatalogApplier::ApplyChangeMetadataRecord`, `local_catalog_applier.cc:789-812`).
`ChangeMetadataOperation::Apply` acts on the retention fields before it looks at the schema
(`change_metadata_operation.cc:141-155`), so on every tserver the copy gets:

- a WAL barrier at the record's position (`log->set_cdc_min_replicated_index`,
  `change_metadata_operation.cc:300-301`). WAL cleanup resets a barrier that has not been
  refreshed within the stale threshold (`TabletPeer::RunLogGC` →
  `reset_cdc_min_replicated_index_if_stale`, `tablet_peer.cc:899-905`, `:1314-1322`;
  `cdc_min_replicated_index_considered_stale_secs`, 1800 s, or 4 h for the system catalog tablet),
  so the copy keeps up to that much extra WAL;
- an intent-cleanup barrier and, with `cdc_sdk_require_history_cutoff`, a history retention
  barrier (`SetAllInitialCDCSDKRetentionBarriers`, `change_metadata_operation.cc:320-321`). A
  staleness check exists for the history barrier (`is_cdc_sdk_safe_time_stale`,
  `tablet_peer.cc:1293`). Not checked: whether the copy's retention path
  (`LocalCatalogAllowedHistoryCutoff`) honors these barriers, and when they are reset;
- the stream's WAL retention period (`change_metadata_operation.cc:146-153`).

On an ordinary tablet, the tserver's CDC service moves these barriers forward from the stream
checkpoints in `cdc_state`. On master's system catalog tablet, master does it, and the history
barrier follows `timestamp_syscatalog_history_retention_interval_sec` (TODO #26427 in
`xrepl_catalog_manager.cc:1896-1898`). Nothing moves them on the copy: it has no `cdc_state` row
and is outside `tablet_map_` (C20). The copy needs none of them, because no CDC stream reads the
copy.

These records are also the only table-schema records on the system catalog today: no migration
changes an existing catalog table's schema. A table-schema record replayed after its table was
removed fails: the schema-version lookup for the table returns an error
(`change_metadata_operation.cc:174-180`), and a failed apply is a glog FATAL
(`operation_driver.cc:425-427`). The copy then crash-loops, because a restart pushes the same
record again from the checkpoint. The sequence is: a stream is created, its database is dropped,
and a copy restarts from a checkpoint older than both (T38).

Change:

1. Before submitting a schema-change record, the applier clears `retention_requester_id`,
   `cdc_sdk_require_history_cutoff` and `wal_retention_secs`.
2. It skips the record when nothing is left to apply: no table added or removed, and no schema
   newer than the copy's.
3. It skips a table-schema record whose table is absent from the copy's metadata. This is what
   tablet bootstrap did before D19063 ("If table id isn't in metadata, ignore the replay as the
   table might've been dropped", `tablet_bootstrap.cc:1820-1826`). It also covers a future
   migration that adds a column to a per-database catalog table whose database is later dropped.

Test: create a logical replication slot with the copy on. Assert that the copy's WAL barrier and
WAL retention period are unchanged, and that the copy's WAL is cleaned up as before. Then drop the
stream's database, restart a tserver with a checkpoint older than the stream creation
(`TEST_` flag that skips `PersistCheckpoint`), and assert that the copy serves.
