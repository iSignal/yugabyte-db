# Tserver-local copy of the PG catalog: proposed GitHub issues

Date: 2026-10-28. Source: [local_catalog_docdb_design.md](local_catalog_docdb_design.md) (v2),
[simplified_with_object_locks.md](simplified_with_object_locks.md), [todos.md](todos.md).
Prototype: https://github.com/iSignal/yugabyte-db/tree/local_catalog_tablet_v2.

Priority: **High** = in the prototype already, or a fix without which the feature is not correct for
the common path; **Medium** = a less common scenario, or a cost rather than a correctness gap;
**Low** = a check or a cleanup.

Issues 1-3 are the initial piece and are reviewed in parallel. Everything else lands on top of
them, in roughly the order listed within each priority.

| #   | Summary                                                                                    | Priority | Body                                                                                         |
| --- | ------------------------------------------------------------------------------------------ | -------- | -------------------------------------------------------------------------------------------- |
| 1   | Master pushes its system catalog WAL to every tserver with a certified safe time           | High     | [1](#1-master-pushes-its-system-catalog-wal-to-every-tserver)                                |
| 2   | Each tserver keeps a private copy of the system catalog and applies master's pushes        | High     | [2](#2-each-tserver-keeps-a-private-copy-of-the-system-catalog)                              |
| 3   | Catalog cache misses outside DDL are served from the local copy                            | High     | [3](#3-catalog-cache-misses-outside-ddl-are-served-from-the-local-copy)                      |
| 4   | A DDL returns success only after every tserver's copy holds its changes                    | High     | [4](#4-a-ddl-returns-success-only-after-every-tservers-copy-holds-its-changes)               |
| 5   | A restarted or rejoining tserver accepts connections only once its copy is current         | High     | [5](#5-a-restarted-or-rejoining-tserver-accepts-connections-only-once-its-copy-is-current)   |
| 6   | A session sees its own DDL, temp tables and master reads in its next statement             | High     | [6](#6-a-session-sees-its-own-ddl-temp-tables-and-master-reads-in-its-next-statement)        |
| 7   | Every catalog read kind is routed correctly: the complete read-path rules                  | High     | [7](#7-every-catalog-read-kind-is-routed-correctly)                                          |
| 8   | A backend waiting for the copy gets its data within one push round trip, not at the next timer | High     | [8](#8-a-backend-waiting-for-the-copy-gets-its-data-within-one-push-round-trip)                               |
| 9  | A copy bootstrapped while a DDL is in flight stays correct (remote bootstrap fix-ups)      | High     | [9](#9-a-copy-bootstrapped-while-a-ddl-is-in-flight-stays-correct)                         |
| 10  | Optimize master performance of local copy pushes for 1K tservers                           | Medium   | [10](#10-optimize-master-performance-of-local-copy-pushes-for-1k-tservers)                   |
| 11  | Lease rejoin handling: a DDL completes on a rejoined or lagging tserver without blocking its release handler | High     | [11](#11-lease-rejoin-handling)             |
| 12  | Do not use heartbeat catalog version propagation when the local copy is enabled            | High     | [12](#12-do-not-use-heartbeat-catalog-version-propagation-when-the-local-copy-is-enabled)    |
| 13  | `\dt` and other catalog-only statements are served from the copy; mixed statements are not | Medium   | [13](#13-dt-and-other-catalog-only-statements-are-served-from-the-copy)                      |
| 14  | Features that write the catalog outside DDL are disabled while the copy is on              | Medium   | [14](#14-features-that-write-the-catalog-outside-ddl-are-disabled-while-the-copy-is-on)      |
| 15  | Checkpoint on schema change record apply                                                   | High     | [15](#15-checkpoint-on-schema-change-record-apply)                                           |
| 16  | Metrics, routing log and flag-on CI for the copy                                           | High     | [16](#16-metrics-routing-log-and-flag-on-ci-for-the-copy)                                    |
| 17  | Master load test harness: N simulated tservers, steady DDL, catch-up, `CREATE DATABASE`    | High     | [17](#17-master-load-test-harness)                                                           |
| 18  | A copy that is corrupt or cannot apply takes itself out of the read path and re-seeds      | Medium   | [18](#18-a-copy-that-is-corrupt-or-cannot-apply-takes-itself-out-of-the-read-path)           |
| 19  | PITR of a database is applied in place on every copy; connections after RESTORED see it    | Medium   | [19](#19-pitr-of-a-database-is-applied-in-place-on-every-copy)                               |
| 20  | Major version upgrade works with the copy serving throughout                               | Medium   | [20](#20-major-version-upgrade-works-with-the-copy-serving-throughout)                       |
| 21  | SERIALIZABLE transactions read the catalog from the copy                                   | Medium   | [21](#21-serializable-transactions-read-the-catalog-from-the-copy)                           |
| 22  | A DDL that times out on propagation returns an error, and its progress is visible          | Medium   | [22](#22-a-ddl-that-times-out-on-propagation-returns-an-error-and-its-progress-is-visible)   |
| 23  | The first statement after a DDL refetches only the tables the DDL changed                  | Medium   | [23](#23-the-first-statement-after-a-ddl-refetches-only-the-tables-the-ddl-changed)          |
| 24  | Login profiles, large objects and direct catalog updates work with the copy                | Medium   | [24](#24-login-profiles-large-objects-and-direct-catalog-updates-work-with-the-copy)         |
| 25  | A `NOTIFY` sent by a DDL transaction is delivered after the DDL is visible locally         | Medium   | [25](#25-a-notify-sent-by-a-ddl-transaction-is-delivered-after-the-ddl-is-visible-locally)   |
| 26  | Master's own metadata records are filtered out of the push and the bootstrap               | Medium   | [26](#26-masters-own-metadata-records-are-filtered-out-of-the-push-and-the-bootstrap)        |
| 27  | Catch-up of 1,000 tservers is not bound by one master thread                               | Medium   | [27](#27-catch-up-of-1000-tservers-is-not-bound-by-one-master-thread)                        |
| 28  | Preloading, the response cache and the full-refresh path are removed when the copy is on   | Medium   | [28](#28-preloading-the-response-cache-and-the-full-refresh-path-are-removed)                |
| 29  | The copy's memory is accounted and its tablet housekeeping matches other tablets           | Medium   | [29](#29-the-copys-memory-is-accounted-and-its-tablet-housekeeping-matches-other-tablets)    |
| 30  | Web pages and ASH wait events for the copy                                                 | Medium   | [30](#30-web-pages-and-ash-wait-events-for-the-copy)                                         |
| 31  | The copy works without object locking (backport to 2026.1)                                 | Low      | [31](#31-the-copy-works-without-object-locking)                                              |
| 32  | xCluster DDL queue handler reads are checked against the copy                              | Low      | [32](#32-xcluster-ddl-queue-handler-reads-are-checked-against-the-copy)                      |

---

## 1. Master pushes its system catalog WAL to every tserver

**Summary:** Master pushes its system catalog WAL to every tserver with a certified safe time.

**Scenario.** Master keeps every tserver's copy of the catalog current without any tserver
polling: after a DDL, and on a timer, master sends each lease holder the system catalog changes it
lacks, together with a safe time that certifies that every catalog transaction committed at or
before it is included. This is the transport the whole feature rests on; nothing user-visible yet
(flag off by default).

**Technical details.**

- Serve the system catalog tablet to a tserver over remote bootstrap (source side; the tserver's
  private-copy fix-up is issue 2).
- Push cycle (plan2 §4.1, design §5.5): `S := leader safe time`; resolve every catalog transaction
  with intents whose outcome is unknown against its coordinator (committed ≤ S → wait for its apply
  record; aborted → remove intents; pending with coordinator clock > S → nothing; pending with
  clock ≤ S → ask again); `position := majority-replicated index`; convert the WAL up to that
  position into xCluster change records (`PopulateXClusterRecords`); the last batch carries S.
  Reuses `TransactionParticipant::ResolveIntents` and the xCluster producer's record conversion.
- One push RPC per lease holder from the position it last answered with; the answer carries the
  copy's applied position and safe time. Trigger: a 1-minute timer (`local_catalog_push_interval_ms`),
  only when the WAL moved. No release integration (issue 4), no page cache or bounds (issue 10),
  no floor requests (issue 8).
- Flags: `enable_local_tserver_catalog` (master and tserver), off by default; master creates the
  pusher only when object locking and invalidation messages are on.
- Prototype: `src/yb/master/sys_catalog_pusher.{h,cc}`, `sys_catalog_change_service.cc`,
  the pusher half of `pg_local_catalog-test.cc`. Drop the pull path, the page cache and the
  in-flight scheduler for this issue.

**Tests.** A test receiver registered with master: after `CREATE TABLE` the next cycle's records
contain the DDL's rows and apply record and the safe time is above the commit; a cycle run while a
DDL transaction is open certifies below its commit and the cycle after commit includes it; a
master-only write (tablet metadata) arrives on the timer; a second receiver at an older position
gets the records from there.

## 2. Each tserver keeps a private copy of the system catalog

**Summary:** Each tserver keeps a private copy of the system catalog and applies master's pushes.

**Scenario.** Every tserver holds a readable copy of master's catalog tablet that tracks master
within one push. Still nothing user-visible: reads are hooked up in issue 3.

**Technical details.**

- Bootstrap by remote bootstrap from the master leader's system catalog tablet; after the transfer
  fix up the tablet metadata: reserved tablet id (`kLocalCatalogTabletId`), single-voter Raft
  config with this tserver, not in the tablet map, never reported to master (`OpenTabletPeer(...,
  private_copy = true)`, `ts_tablet_manager.cc`). Wait for `IsInitDbDone` before the first
  bootstrap.
- Applier (`local_catalog_applier.cc`): apply pushed records in WAL order through
  `XClusterWriteInterface`: transactional rows as external intents (`CombineExternalIntents`),
  apply records as `apply_external_transactions` at the commit time, non-transactional rows at
  their own time, schema-change records into the copy's metadata. Record the copy's safe time
  (the S of the last fully applied batch; `LocalCatalogReplica::safe_time()`).
- Push protocol: refuse a batch that does not start at the copy's position, answer with the
  position ("gap"); acknowledge a batch at or below the position without writing; answer every
  push with position and safe time. This is part of the protocol, not a later fix: without it a
  lost push corrupts the copy silently.
- History retention of the copy pinned to master's system catalog window
  (`LocalCatalogAllowedHistoryCutoff`, 4 h). Automatic compactions on.
- Checkpoint the applied position on disk about once a second and at shutdown, never ahead of
  what the copy has made durable (`PersistCheckpoint` waits for `WaitUntilAllFlushed`; the fsync
  half is todos T7); a restart reopens the copy from local metadata and resumes from the
  checkpoint; a stale checkpoint file that outlives its tablet is deleted (todos T22). Serving
  after a restart waits for the first page with a safe time (and, later, the lease bound of
  issue 5).
- Not in this issue: lease bound (issue 5), remote-bootstrap intents fix-up (issue 9), fault
  handling (issue 18).
- Prototype: `src/yb/tserver/local_catalog_replica.{h,cc}`, `local_catalog_applier.{h,cc}`,
  `local_catalog_read.cc` (the test hook only), the private-copy branch in `ts_tablet_manager.cc`,
  `kLocalCatalogTabletId` in `src/yb/common/constants.h`, the `tablet.cc` skip for
  `TEST_file_to_dump_docdb_writes`.

**Tests.** After a DDL and one push, a test-only read of the copy at its safe time returns the same
rows as master for `pg_class`, `pg_attribute`, `pg_yb_catalog_version`; a DDL open at push time is
staged and invisible, visible after its apply record; a push that skips a batch is refused and the
re-sent batch applies; the copy is absent from master's tablet listings and from the tserver's
reported tablets; `RestartReusesTheCopyWithoutAskingMaster`: a restart resumes from the
checkpoint with no transfer; a crash between apply and checkpoint re-applies a suffix without
duplicate-visible rows.

## 3. Catalog cache misses outside DDL are served from the local copy

**Summary:** Catalog cache misses outside DDL are served from the local copy.

**Scenario.** A cold connection or a cache miss after a DDL reads the catalog from the local
tserver instead of a round trip to master: in a region 60 ms from master a first query drops from
seconds to tens of milliseconds (design §10.2). Until issue 4 lands, what a backend reads may be up
to one push (a minute) behind a DDL it was not told about; usable in test clusters only.

**Technical details.**

- In `PgClientSession`: when a catalog read arrives on a fresh CatalogSnapshot (`is_catalog_snapshot`,
  new read-point serial, no time yet) and the backend is not in DDL mode, assign the copy's safe
  time as the snapshot's read time (`ReadHybridTime::SingleTime`) and execute the read on the copy
  (`ExecuteLocalCatalogReads` through `tablet->HandlePgsqlReadRequest` with empty transaction
  metadata, `is_explicit_request_read_time = true`); later reads under the same serial reuse the
  time. Everything else goes to master as today. Decline rules beyond "DDL mode" are issue 7.
- The PG side is unchanged: `is_catalog_snapshot` and `clamp_uncertainty_window` already come
  from pggate (`SetupReadTimeOptions`); the per-serial time lives on the tserver.
- Routing counters: reads served by the copy, reads sent to master with the reason; the
  `log_local_catalog_read_routing` line.
- Prototype: `TryServeCatalogReadsLocally`, `LocalCatalogReadTimeForFreshSnapshot`,
  `SetFreshCatalogSnapshotReadTime` in `pg_client_session.cc`; `local_catalog_read.cc`.

**Tests.** `pg_local_catalog-test` freshness group minus the gate cases: a cold backend's query
makes no master catalog read (counters); rows equal master's; a DDL-mode read goes to master; two
reads under one CatalogSnapshot see one state while a push lands between them.

## 4. A DDL returns success only after every tserver's copy holds its changes

**Summary:** A DDL returns success only after every tserver's copy holds its changes.

**Scenario.** After `CREATE TABLE` or `ALTER ROLE ... PASSWORD` returns, every connection on every
node sees it: a backend that takes a lock on the altered object, or that is told the new catalog
version, never reads a copy that lacks the change. This is the correctness contract of the feature
(design §5.6); with it the flag can be on in CI.

**Technical details.**

- Release path on master (`PrepareLocalCatalogPushes` in `UpdateAllTServers`, `PrepareRelease` in
  the pusher): run one push cycle (S is after the DDL's commit because the version read preceded
  it), and carry the records from each target's position inline in that target's release RPC
  (`ReleaseObjectLockRequestPB.local_catalog_changes`). Full payload, no size cap (the cap and the
  pending set are issues 10 and 11).
- Release handler on the tserver (`TSLocalLockManager::Impl::ReleaseObjectLocks`): apply the
  carried records; wait until the copy's safe time ≥ `catalog_versions_read_time`; then publish
  the versions and messages (`SetYsqlDBCatalogVersionsWithInvalMessages`); then unlock. A wait
  that exceeds the RPC deadline fails the release; the DDL's tserver resends (today's
  `ReleaseWithRetriesGlobalNow`). Answer carries position and safe time.
- While the copy is not serving (bootstrapping, re-seeding): publish at once, queue the records,
  and record the release's read time as a bound the copy must reach before it may serve
  (`F_serve`); the serving condition is "safe time ≥ every such bound" (`OnSafeTimePublished`).
- `YbCheckNewSharedCatalogVersionOptimization` becomes a no-op while the copy is on: the
  synchronous release has published the same versions on the DDL's own tserver.
- Prototype: `ts_local_lock_manager.cc:539-623`, `object_lock_info_manager.cc` release payloads,
  `local_catalog_replica.cc` serving state.

**Tests.** `CrossTserverDdlVisibleImmediately`, `GateHoldsDdlWhileApplierIsPaused`
(`TEST_local_catalog_pause_applier`), `ConcurrentDdlsFromTwoTserversConverge`, a login after
`ALTER ROLE ... NOLOGIN` returned fails on every node, `CREATE DATABASE` (11.6 MB payload)
completes.

## 5. A restarted or rejoining tserver accepts connections only once its copy is current

**Summary:** A restarted or rejoining tserver accepts connections only once its copy is current.

**Scenario.** A tserver that lost its YSQL lease (partition, long GC pause) or restarted does not
let a new connection read a catalog older than what the cluster has already acknowledged; PG on
that node starts once the copy has caught up, normally within a push round trip.

**Technical details.**

- On every new lease epoch, including the first of a new process (`ysql_lease_manager.cc`
  `ProcessLeaseUpdate` → `OnLeaseGained(MaxGlobalNow())`): bound := clock plus max skew; floor
  request to master (issue 8); hold PG's start until the copy's safe time ≥ bound; no timeout
  (`ShouldHoldPgStart`, the deferred PG restart thread).
- On lease loss: `OnLeaseLost` sets an unreachable bound (`kMax`); PG sessions are ended as today.
- A release that arrives while the copy is not serving raises the bound to its read time (issue 4);
  the heartbeat is not involved (issue 12).
- Catch-up after the lease: master pushes from the copy's checkpointed position when it still
  retains that WAL; otherwise it answers "re-seed" (position below its log retention, 15 minutes
  by default) and the tserver tombstones the copy and bootstraps again (`RequestReseed`,
  `reseed_required` on the push answer). During a re-seed the copy is not serving (issue 4's
  not-serving rules).
- Serving state machine: Bootstrapping → Serving (first safe time ≥ bound) → LeaseLost → Serving;
  Serving → Reseeding → Bootstrapping.
- Prototype: `OnLeaseGained`, `OnLeaseLost`, `OnSafeTimePublished`, the PG start hold in
  `ysql_lease_manager.cc` and `pg_wrapper.cc` (`StartAndMaybePause` / `InitPaused`).
  Remove `local_catalog_serve_wait_timeout_ms`.

**Tests.** `PgLocalCatalogRejoinTest`: partition a tserver until it loses its lease, run a DDL
elsewhere, heal; its first connection sees the DDL and PG started only after the copy's safe time
passed the bound. `ReseedWhenPositionTooOld` (a small master WAL retention): the rejoined copy is
replaced and serves afterwards.

## 6. A session sees its own DDL, temp tables and master reads in its next statement

**Summary:** A session sees its own DDL, temp tables and master reads in its next statement.

**Scenario.** `CREATE TEMP TABLE t; SELECT FROM t` works; a DDL block that rolled back does not
make the next statement read a catalog older than what the block saw on master; a session never
sees a change and then loses it.

**Technical details.**

- Session floor (design §5.7, §7.2): one time per backend (per `PgClientSession`). Set to the
  tserver's clock reading when a catalog read of this session was answered by master (DDL-mode
  reads included; `catalog_read_from_master_`, taken at the answer, not at the next read), and to
  the commit hybrid time when the session commits a transaction that wrote catalog rows
  (`HandleCommit`; use the commit time from the response rather than `clock()->Now()`; needs a
  request field for `has_catalog_write_ops_in_ddl_mode_`, `pg_client.proto`). Checked only when a
  fresh CatalogSnapshot takes its time: if the copy's safe time is below the floor, floor request
  (issue 8) and wait, bounded by the request deadline. Cleared once reached.
- Remove the version comparisons the prototype used instead (`WaitForAppliedVersion`,
  `applied_version_`, `backend_catalog_version` on the request, `own_catalog_write_floor_version_`).
- Routinely nothing waits: the DDL's own release (issue 4) and a temp relation's global
  `AccessExclusiveLock` release have already moved the copy past the commit.

**Tests.** `MasterSnapshotTimeIsFloorForNextLocalRead`, `TemporaryRelation`, a DDL block that reads
master then rolls back, `DISCARD ALL` with no temp objects makes no floor request.

## 7. Every catalog read kind is routed correctly

**Summary:** Every catalog read kind is routed correctly: the complete read-path rules.

**Scenario.** `ysql_dump --read-time`, logical replication's historical catalog reads, upgrade
migrations, serializable transactions, parallel workers and connection startup all get the right
answer: from the copy when it can serve the read at the requested time, from master otherwise.

**Technical details.** Decline rules in `TryServeCatalogReadsLocally` (design §5.7):

- to master: DDL mode or `ddl_use_regular_transaction_block`; `yb_non_ddl_txn_for_sys_tables_allowed`
  (upgrade migrations, initdb); SERIALIZABLE (until issue 21); non-MVCC and historic snapshots; a
  read time below the copy's retention; copy not serving (the session is then on master until the
  backend exits, design §7.2).
- an explicit read time above the copy's safe time (`yb_read_time`, `ysql_dump --read-time`, the
  walsender): never clamped down; floor request up to that time (issue 8) and serve at exactly
  that time.
- the legacy catalog session (startup reads, full refresh, parallel workers) is accepted: the
  chooser hands it the copy's safe time and `DoPerform` echoes it as `catalog_read_time` so the
  rest of the preload runs at one time; the version row is read in the same snapshot.
- the tserver response cache is bypassed while the copy serves (a hit precedes the copy path in
  `DoPerform` and would skip the floor).
- a distributed transaction's data read point is saved and restored around the catalog read
  (`ReadPointHistory`), so fixing the catalog serial's time does not move the data snapshot.

**Tests.** `RepeatableReadDataSnapshotSurvivesLocalCatalogRead`, explicit read time above and below
the copy's safe time, `ysql_dump --read-time`, parallel query served by the copy, a startup
preload with no master catalog read, each routing counter asserted.

## 8. A backend waiting for the copy gets its data within one push round trip

**Summary:** A backend waiting for the copy gets its data within one push round trip, not at the
next timer.

**Scenario.** Several paths make something wait until the copy has reached a given hybrid time:
PG start after a lease rejoin or restart (issue 5), the statement after a session's own DDL or
temp-table DDL or master read (issue 6), an explicit read time above the copy's safe time such as
`ysql_dump --read-time` or the walsender (issue 7). Without this issue those waits last until the
1-minute timer push, or forever if the WAL did not move. With it the tserver tells master the time
it needs and master runs a cycle and pushes at once, so each wait costs one push round trip.

**Technical details.** Floor request RPC (tserver → master): the copy's position and the hybrid
time waited for. Master runs cycles at most once per `local_catalog_push_min_cycle_interval_ms`
(50 ms) for all pending requests together until a certified safe time passes the floor, and pushes
the pages. A floor never moves master's clock (a floor can be `MaxGlobalNow`); a floor ahead of
master's clock costs up to the skew bound. One floor request in flight per tserver; repeated once
a second while a wait is open (reaches a new master leader). Prototype: `RequestPush` on both sides.

**Tests.** A waiting read is satisfied in one round trip; 100 concurrent floor requests cause one
cycle; a floor ahead of master's clock completes after the skew bound.

## 9. A copy bootstrapped while a DDL is in flight stays correct

**Summary:** A copy bootstrapped while a DDL is in flight stays correct (remote bootstrap fix-ups).

**Scenario.** A tserver that bootstraps its copy while another session is in the middle of a DDL
ends up with the DDL's rows once it commits, instead of silently losing them; a bootstrap during a
master leader change does not keep entries master never committed.

**Technical details** ([rbs-intents-plan.md](rbs-intents-plan.md), design §7.1):

- (a) Open transactions' intents: before the applier applies its first page, convert every copied
  transaction without a local commit time into the external-intent form the applier would have
  written (`GetIntentsBatchForCDC` → `ApplyRecords` with `subtransaction_id`), remove the regular
  intents (`IMMEDIATE_CLEANUP`), flush, and record `intents_converted` in the checkpoint. The
  copy's participant must never resolve a status (loader resolvers and the abort-check poller off
  for the private copy). Crash points per the plan's table.
- (b) Uncommitted log tail: resume from the copied log's Raft-committed index (bootstrap already
  computes it from each entry's `committed_op_id`) and drop the pending entries instead of letting
  the single-voter group commit them; master compares the term at the reported position and
  answers "re-seed" on a mismatch.
- Enable `DISABLED_DdlOpenAcrossReseedIsVisibleAfterCommit`.

**Tests.** The plan's seven cases: open DDL commits / rolls back after the bootstrap; a row written
before and after the bootstrap by one transaction; a rolled-back savepoint; conversion delayed
until master forgot the transaction; each crash point.

## 10. Optimize master performance of local copy pushes for 1K tservers

**Summary:** Optimize master performance of local copy pushes for 1K tservers: pages built once,
bounded pushes.

**Scenario.** Continuous DDL against 1,000 tservers costs master under 3 cores; a thousand copies
catching up at once are served in waves without master's memory or NIC spiking; a `CREATE
DATABASE` (12 MB) does not go out inline in 1,000 release RPCs at once.

**Technical details** (push-plan.md §2, design §5.5):

- Page cache: convert each WAL range once into a serialized page (~1 MB, `local_catalog_push_page_bytes`),
  sealed at the size, at a restore marker, and at the certified position (drop the inherited
  seal after a schema-change record, todos T46); 64 MB cache, oldest dropped first; pages below
  the cache built on demand (≤ 64 transient, shared by targets at one position).
- Scheduler: one thread; one push in flight per target; at most 64 RPCs / 64 MB in flight;
  16 slots reserved for targets a release is pending on; queue order: release-pending first, then
  furthest behind; per-target backoff on failure.
- Release: inline at most 4 MB (`local_catalog_release_inline_max_bytes`) of cached pages per
  target; otherwise the target is pushed first among the queue (issue 11 makes the release RPC
  follow once caught up).
- Positions from heartbeats (`local_catalog_applied_op_id` in `TSHeartbeatRequestPB`) as well as
  answers; cache and positions reset on a master term change; a new leader pushes a target with
  unknown position from the cache's start (or waits one heartbeat, issue 11 decides).
- Master-side memory tracker for the cache.

**Tests.** `pg_local_catalog_load-test` scenarios (issue 17): steady N=1000 under 3 cores above
baseline; catch-up K=1000 × 2000 DDLs within the in-flight bound; `CreateDatabaseFanout`.

## 11. Lease rejoin handling

**Summary:** Lease rejoin handling: a DDL completes on a rejoined or lagging tserver without
blocking its release handler.

**Scenario.** A tserver whose copy is far behind (just rejoined, or `CREATE DATABASE` in flight)
does not hold an RPC thread per waiting DDL and does not make every attempt start over; the DDL's
locks there are released as soon as the copy has caught up, with one version read and one bound
for the whole release.

**Technical details** (simplified_with_object_locks.md §4.1, §7 item 9):

- The handler never waits: copy's safe time ≥ read time → publish, unlock; otherwise answer
  "behind" with position and safe time, locks stay held.
- Master keeps the release (versions, messages, read time, cycle end position E) with the
  persisted in-progress request; targets without inline pages, or that answered "behind", form the
  pending set; the pusher pushes them first; when a push or release answer shows a target at
  position ≥ E, master sends it the release RPC (no pages); complete when every target has
  acknowledged or lost its lease. Driven from `OnPushDone` and `OnReleaseDone`.
- A failed version read fails the release so that it is resent, instead of going out without a
  map (`PopulateDbCatalogVersionCache` only warns today). Every request of a lease-expiry cleanup
  batch carries the map (today only the first).
- Optional, same area: the release bound as the newest commit time among the versions it carries
  (todos T44 option 2) so that nothing waits for commits after the DDL's.
- Fix the persisted request stripping the catalog fields (`ReleaseRequestToPersist`), so a new
  master leader continues the same release with the same bound.

**Tests.** Slow one tserver's apply (`TEST_local_catalog_pushed_batch_apply_delay_ms`) beyond one
RPC deadline: the DDL completes once the copy catches up, one version read per DDL; a lease-expiry
batch publishes versions on every request; master failover mid-release completes it.

## 12. Do not use heartbeat catalog version propagation when the local copy is enabled

**Summary:** Do not use heartbeat catalog version propagation when the local copy is enabled;
versions reach a tserver only through lock releases (and the restore message).

**Scenario.** The heartbeat can no longer tell a tserver about a version its copy does not hold
yet.

**Technical details.** The tserver does not publish heartbeat version reports (`heartbeater.cc`);
`held_version_report_` and `LocalCatalogCopyHoldsHeartbeatVersions` go away; the heartbeat still
carries the applied position to master and master may skip the version data for tservers with a
copy. The per-database message queue entry is created by the release's full map (`CREATE
DATABASE`'s release); `cutoff_catalog_version` is local GC and unaffected. Master-side writers
without a release are PITR (issue 19) and the major upgrade (issue 20).

**Tests.** A heartbeat with a newer version while the copy is paused publishes nothing; the
version arrives with the release; `CREATE DATABASE` from another tserver creates the queue entry.

## 13. `\dt` and other catalog-only statements are served from the copy

**Summary:** `\dt` and other catalog-only statements are served from the copy; mixed statements are
not.

**Scenario.** `\dt`, `\d`, `information_schema` queries and ORM queries on `pg_type` run locally
(4 ms instead of 300 ms warm in a remote region), while a statement that joins a catalog table with
a user table, or runs inside a transaction block, keeps PostgreSQL's single-snapshot semantics and
reads master at the transaction's read time.

**Technical details** (design §6, todos T8/T19): decide once at executor start whether the
statement is outside an explicit transaction block and references only catalog relations and no
function above `FirstNormalObjectId`; otherwise mark the scan "transaction snapshot" at
`ybc_heap_beginscan`; pggate leaves such a scan on the transaction's read point instead of the
catalog serial; the tserver sends it to master (step 1), later serves it from the copy when its
time is at or below the copy's safe time (step 2). GUC to serve every user catalog read from the
copy regardless. Precedents: `qpmReferencesCatalogRelation`, `contain_volatile_functions`,
`yb_plan_references_pg_rel` on the cached plan.

**Tests.** `UserCatalogReadMatchesMaster` extended: `\dt` served locally; `SELECT ... FROM pg_class,
t` goes to master; the same inside `BEGIN` goes to master; a user function over `pg_class` goes
to master; GUC on serves all from the copy.

## 14. Features that write the catalog outside DDL are disabled while the copy is on

**Summary:** Features that write the catalog outside DDL are disabled while the copy is on.

**Scenario.** Login profiles (failed-attempt counters), large objects (`lo_create` etc.) and
direct catalog updates under `yb_non_ddl_txn_for_sys_tables_allowed` outside upgrade and initdb
write catalog rows that no release announces; until issue 24 supports them, enabling the copy
refuses them with a clear error rather than serving stale rows.

**Technical details.** Guard at the three entry points (`ysql_enable_profile` with the copy on;
large-object functions; the GUC outside upgrade/initdb mode); todos T4, T5, T6 record the cases
and the candidate designs. Java tests `TestYbRoleProfile` and the large-object tests pin the copy
off.

**Tests.** Each feature errors with the copy on and works with it off.

## 15. Checkpoint on schema change record apply

**Summary:** Checkpoint on schema change record apply, so a restart from an older checkpoint
replays schema changes safely.

**Scenario.** A tserver that restarts after a catalog table was added, removed or altered on
master (initdb, upgrade, extension install) re-applies the WAL after its checkpoint without
crash-looping or keeping rows of a removed table.

**Technical details** (todos T38, T45, T46): persist the checkpoint just before and just after each
schema-change record, so no replay crosses one; skip a table-schema record whose table is absent
from the copy; clear the CDC retention fields of logical-replication stream records before
applying them (they set retention barriers nothing on the copy moves); drop the page seal after a
schema-change record on master (issue 10 lists it).

**Tests.** Restart from a checkpoint before each record kind (add table, remove table, alter
schema, stream creation): the copy opens and matches master; no retention barrier on the copy.

## 16. Metrics, routing log and flag-on CI for the copy

**Summary:** Metrics, routing log and flag-on CI for the copy.

**Scenario.** An operator can tell whether reads are served locally, how far the copy is behind,
and what a DDL waited on; CI runs the regress suites with the copy on.

**Technical details** (design §11): tserver metrics (serving state, safe time and its age sampled
continuously, position, pushes received/applied/refused, apply failures, floor requests, re-seeds,
gate wait histogram, read-side wait histograms, reads served / to master by reason); master
metrics (cycles and cycle time, push RPCs and bytes, pages built, in-flight maxima, floor requests,
inline releases and skipped ones, per-target position and last-answer age); the runtime routing
log; EXPLAIN's served-by-copy count. CI: the Java regress schedules and the C++ suites with
`enable_local_tserver_catalog=true`, periodic; triage the failures that count master round trips
(pin the flag off in those) from real ones.

**Tests.** Each counter asserted by the routing tests; a flag-on CI job.

## 17. Master load test harness

**Summary:** Master load test harness: N simulated tservers, steady DDL, catch-up, `CREATE DATABASE`.

**Scenario.** Master's CPU, memory and NIC under 1,000 tservers can be measured on one host before
and after each optimization.

**Technical details.** `pg_local_catalog_load-test` from the prototype: N push receivers in the test
process register with master (`TEST_local_catalog_push_extra_targets`), acknowledge and discard;
scenarios `PushSteady` (4 DDL sessions, N=200/1000), `PushJoinerWave` (K receivers behind by 50 or
2000 DDLs), `CreateDatabaseFanout`, `PushIdle` (N=8000), `RealTserversOnly`, `DdlWalSize`; per-run
master CPU from `/proc`, pidstat, vmstat, optional perf profile; results in
[loadtest-summary.md](loadtest-summary.md). Add: DDL latency with real lease holders (receivers
that hold a lease), full-inline `CREATE DATABASE` fan-out (issue 1's no-cap payload).

**Tests.** The harness is the test; results checked into the notes per run.

## 18. A copy that is corrupt or cannot apply takes itself out of the read path

**Summary:** A copy that is corrupt or cannot apply takes itself out of the read path and re-seeds.

**Scenario.** A disk error or a corrupted file on one tserver's copy degrades that node to master
reads instead of serving wrong rows or failing queries; the copy re-seeds and serves again.

**Technical details** (todos T11, T15, T9): after N consecutive apply failures, or a
corruption-class error on apply or read, move to a not-serving state (reads to master, releases
acknowledged at once with the bound raised), re-seed, resume under the serving condition. A read
that fails on the copy returns to master instead of erroring the query (today it errors with no
counter). A backend that read master in that window stays on master until it exits. A lagging copy
is not withdrawn.

**Tests.** `TEST_local_catalog_fail_push` / apply-failure injection: the copy stops serving, the
DDL completes, serving resumes after re-seed; a corrupted SST → re-seed; a failed local read falls
back.

## 19. PITR of a database is applied in place on every copy

**Summary:** PITR of a database is applied in place on every copy; connections after RESTORED see
it.

**Scenario.** Restoring one database to a point in time does not re-bootstrap every tserver's
copy, and a connection opened after `yb-admin` reports the restore complete reads the restored
catalog on every node (today 10 of 15 runs of `PgsqlSequencePartialCleanupAfterRestore` read the
pre-restore catalog).

**Technical details** ([pitr-handling.md](pitr-handling.md), design §7.7, todos T43): master sends a
restore message to every lease holder with the database oid, restore time, write time and new
version; the copy flushes, sets the same per-database hybrid-time filter on its SST files
(`RocksDBPatcher::SetHybridTimeFilter`), reopens, writes the version row, publishes after its safe
time passes the restore operation, acknowledges; `FinishRestoration` reports RESTORED only after
every lease holder acknowledged or lost its lease; the copy keeps master's catalog history bound
so the filter has the old versions to fall back to; re-seed when the history does not reach back
far enough or the apply fails; until this issue lands, a restore marker in the push makes every
copy re-seed (the prototype's behaviour, kept by issue 5's re-seed path). Master's own entries are
not restored on the copy (issue 26 makes that moot).

**Tests.** The PITR suites in release with the copy on, 20 runs each; restore older than the copy's
history → re-seed; a lease holder that does not answer → RESTORED only after its lease expires;
two databases, restore one, the other keeps serving.

## 20. Major version upgrade works with the copy serving throughout

**Summary:** Major version upgrade works with the copy serving throughout.

**Scenario.** A PG11 → PG15 upgrade (and later ones) runs with the copy on; each tserver's first
new-binary backend finds the new catalog in its copy; the rolling restart does not wait longer
than it has to.

**Technical details** (todos T25): remove the `ysql_major_version_upgrade_in_progress` guard from
`IsServing()` and the heartbeat field if nothing else reads it; initdb and pg_upgrade take no object
locks and bump no versions, so their writes reach the copies by push: run the timer cycle more
often while the upgrade is in progress, or push once from the upgrade handler after
`UpdateCatalogVersions` and before `MONITORING`, so copies are pre-loaded before the restarts;
verify table-removal records (finalize cleanup, rollback) apply on the copy; verify a new-binary
tserver cannot start PG before `MONITORING`.

**Tests.** `ysql_major_upgrade_*-test.cc` with the copy on, including rollback and
`ysql_major_upgrade_matview-test.cc`; `local_catalog_reads_served` grows during `MONITORING` and
after finalize, no re-seed.

## 21. SERIALIZABLE transactions read the catalog from the copy

**Summary:** SERIALIZABLE transactions read the catalog from the copy.

**Scenario.** A serializable transaction's catalog misses are local, like other isolation levels,
and its catalog reads see one consistent state instead of master's latest per read.

**Technical details** (design §7.2, worklog "Serializable isolation no longer serves catalog
reads"): give the catalog serial its own read point outside the transaction's (`SetupPlainSessionReadTime`
multiplexes one `ConsistentReadPoint` today), and let pggate send the catalog-snapshot flag and
clamp for serializable transactions (`SetupReadTimeOptions` returns early for them). PG takes no
predicate locks on catalogs, so nothing more is needed. Check whether serializable catalog reads
write read intents on master today.

**Tests.** A serializable transaction's catalog misses served by the copy; its data writes carry no
read time (`async_rpc.cc` check); the master-fallback floor no longer set for them.

## 22. A DDL that times out on propagation returns an error, and its progress is visible

**Summary:** A DDL that times out on propagation returns an error, and its progress is visible.

**Scenario.** When a DDL's release cannot complete on some tserver within 60 s, the client gets an
error saying the effect is unknown and propagation continues, instead of a success that is not yet
true everywhere; the master UI shows which tserver the DDL is waiting on.

**Technical details** (todos T48): the DDL's tserver returns an error after
`tserver_yb_client_default_timeout_ms` instead of "falling back to async" with success
(`ts_local_lock_manager.cc:985-991`); master lists in-progress releases with the DDL's transaction,
start time and per-lease-holder acknowledgement (metrics and the master web page); the DDL's own
session floor still covers it.

**Tests.** A release held at one tserver: the DDL returns the error after the timeout, the release
completes later, the UI shows the pending tserver meanwhile.

## 23. The first statement after a DDL refetches only the tables the DDL changed

**Summary:** The first statement after a DDL refetches only the tables the DDL changed.

**Scenario.** After an `ANALYZE` or `ALTER TABLE`, the first statement of every connection on a
tserver in a remote region currently refetches the DocDB schema and tablet locations of every table
it touches from master, two round trips each (3-4 s for a new connection, 0.25-0.5 s for a warm
backend, with or without the copy). With this change the post-DDL numbers equal the cold ones.

**Technical details** (todos T17): the release carries the DDL's changed-table list (master holds
it for DDL verification and already fills cache-update fields); the tserver's table cache evicts
only those entries instead of the whole database (`InvalidateDbTables`); check the second clear
path on `Get` with a higher version. Not a copy change, but the largest remaining post-DDL cost
and the main source of post-DDL master load.

**Tests.** r6 post-DDL benchmark: warm and new-connection numbers equal cold; `GetTableSchema` /
`GetTableLocations` counts per post-DDL query equal the DDL's changed tables.

## 24. Login profiles, large objects and direct catalog updates work with the copy

**Summary:** Login profiles, large objects and direct catalog updates work with the copy.

**Scenario.** Lifts the restrictions of issue 14.

**Technical details** (todos T4, T5, T6): reads of `pg_yb_profile` / `pg_yb_role_profile` always
from master (failed-login counters are written outside DDL on every failed login); large-object
catalogs from master, or the commit floor extended to non-DDL catalog commits plus a cross-session
wait; direct updates: keep the session on master from its first such write until the copy passes
it, and decide whether the manual version increment stays required.

**Tests.** `TestYbRoleProfile`, large-object tests and the direct-update tests with the copy on.

## 25. A `NOTIFY` sent by a DDL transaction is delivered after the DDL is visible locally

**Summary:** A `NOTIFY` sent by a DDL transaction is delivered after the DDL is visible locally.

**Scenario.** An application that creates a table and notifies listeners in the same transaction,
whose listeners then query the table, no longer sees "relation does not exist" on a listener
whose tserver has not received the release yet.

**Technical details** (design §7.5): the notifications poller receives a transaction's
notifications from its change stream and writes the batch to PG's queue on the commit record;
hold the batch until the local copy's safe time ≥ the commit time (floor request), then write it.
Cost at most one push round trip per batch, routinely nothing. Signals through user tables the
application polls itself are out of scope (accepted fan-out window).

**Tests.** `CREATE TABLE` + `NOTIFY` in one transaction with a listener on another tserver whose
applier is paused: the notification arrives after the copy holds the table.

## 26. Master's own metadata records are filtered out of the push and the bootstrap

**Summary:** Master's own metadata records are filtered out of the push and the bootstrap.

**Scenario.** Leader-election storms and load-balancer moves no longer generate catalog push
traffic to every tserver, and the copy stops carrying records it cannot read.

**Technical details** (todos T35): measure bytes pushed per minute under a leader-election storm
and a rebalance with no DDL; skip master's own tables in the page builder and in the bootstrap
snapshot (or delete them from the copy after the transfer); keep what the copy needs, if anything
(today the table cache fetches DocDB schemas from master, issue 23). Removes the question of
master's own entries in issue 19.

**Tests.** A leader-election storm produces no pushes; the copy holds no master-only tables; PITR
and upgrade suites still pass.

## 27. Catch-up of 1,000 tservers is not bound by one master thread

**Summary:** Catch-up of 1,000 tservers is not bound by one master thread.

**Scenario.** 1,000 tservers catching up 2,000 DDLs (25.8 GB) complete faster than 14 s and cost
master less than 21 CPU-s; a `CREATE DATABASE` to 1,000 tservers (11.85 GB) is bound by the NIC.

**Technical details** (loadtest-summary.md "Catch-up CPU, profiled"): the pusher thread copies each
page into the request and again into the RPC buffer and checksums it, per target (41% of master
CPU), and runs at 100% of one core (1.8 GB/s). Serialize and checksum each page once and send it
as a shared buffer (RPC sidecar); coalesce the ~6 KB per-DDL pages into ~1 MB pages for catch-up
(the WAL-built 140 KB pages were 19% faster with 9% less CPU); or spread pushes over several
threads. Later: a one-level tree through tservers when the NIC is the limit.

**Tests.** `PushJoinerWave` K=1000 × 2000 DDLs before and after; perf profile shows the per-target
copy and CRC gone.

## 28. Preloading, the response cache and the full-refresh path are removed

**Summary:** Preloading, the response cache and the full-refresh path are removed when the copy is
on.

**Scenario.** Connections no longer pay memory per backend for preloaded catalogs
(`ysql_catalog_preload_additional_tables`), and the code paths that only existed to avoid master
round trips (response cache, mid-statement full refresh, custom pg_inherits cache and the other
per-catalog hacks) go away.

**Technical details** (todos T18, design §5.8): with the copy on, init follows standard PG (lazy
catcache/relcache loads; the relcache init file stays); the full cache refresh resets the caches
to empty and reloads lazily; invalidation messages may later be read from the copy instead of the
tserver's message cache; remove the preload options and the custom caches once the copy is the
default.

**Tests.** Connection memory with the copy on; regress suites without preloading.

## 29. The copy's memory is accounted and its tablet housekeeping matches other tablets

**Summary:** The copy's memory is accounted and its tablet housekeeping matches other tablets.

**Technical details** (todos T42, T23, T29, T32): a `local_catalog` memory tracker under the tserver
root for the applier's pending pushes, parsed pages and external-intent batches, read buffers and
the block-cache share; the copy's memtables under the global memstore flusher; external intents of
a DDL open longer than `external_intent_cleanup_secs` (a day) deleted on the copy (document or
extend); tablet-wide flags and assertions that reach the copy checked.

**Tests.** The tracker reports the copy's usage; a copy under memstore pressure flushes.

## 30. Web pages and ASH wait events for the copy

**Summary:** Web pages and ASH wait events for the copy.

**Technical details** (design §11.2, §11.5): a tserver page (serving state, safe time and age,
position, last push, recent transitions) and a master page (per-tserver position, last-answer age,
pending pushes, in-progress releases from issue 22); ASH wait events for the gate wait and the
read-side waits; a debug-mode served-by-copy count in EXPLAIN and pg_stat_statements.

## 31. The copy works without object locking

**Summary:** The copy works without object locking (backport to 2026.1).

**Scenario.** Releases without object locking get the same local catalog reads, with the legacy
version and schema checks and their retries kept.

**Technical details** ([support_object_locking_off.md](support_object_locking_off.md), design
Appendix B): the DDL commit sends the release RPC with no locks; the release handler runs only the
catalog part outside the lock manager; the applier publishes the copy's versions after every apply
(repairs a release whose DDL tserver died, since master holds no lock records); the version read
on the retry paths waits for the copy to reach the tserver's clock plus the skew bound; the legacy
one-read-time-per-backend maps onto the copy's rules; prerequisite: lease and invalidation
messages on. Behaviour changes are performance only.

## 32. xCluster DDL queue handler reads are checked against the copy

**Summary:** xCluster DDL queue handler reads are checked against the copy.

**Technical details** (todos T47): the handler replays replicated DDLs through a PG connection with
a consistency GUC; document which catalog reads it issues and whether any must bypass the copy.
`CREATE EXTENSION` itself needs nothing: it is an ordinary DDL whose release reaches every lease
holder before it returns.
