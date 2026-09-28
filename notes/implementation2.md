# Implementation plan for plan2.md

Repository: `/net/dev-server-sanketh-3/share/worktrees/repo5`, branch
`approach3-local-catalog`, HEAD `3a9b891d86d` (the PG shadow work squashed into
one commit on top of master `65d6873a4c2`). Line numbers were taken against
that HEAD on 2026-09-03 and drift; the symbol names are the stable pointers. Build and run tests with
`./yb_build.sh fastdebug --clang21 --cxx-test <test>`.

The core plan (phases 0 to 6) is one deliverable: the feature must work end
to end behind a flag, with the tests of phase 7 passing, before any TODO is
picked up. Do not trade a phase for a fallback.

## Phase 0. Flags, metrics, feature skeleton

- gflag `enable_local_tserver_catalog` (tserver, non-runtime, default false).
  Location pattern: `src/yb/tserver/tablet_server.cc` flag block.
- Metrics (server-level, defined next to the xCluster consumer counters at
  `src/yb/tserver/xcluster_consumer.cc:58-70` for the pattern):
  - counters: `local_catalog_poll_failures`, `local_catalog_apply_failures`,
    `local_catalog_reseeds`, `local_catalog_serving_disabled_lease`,
    `local_catalog_reads_served`, `local_catalog_reads_waited_for_version`,
    `local_catalog_reads_to_master_in_ddl`,
    `local_catalog_reads_to_master_not_serving`;
  - histograms: `local_catalog_poll_latency_us`,
    `local_catalog_apply_latency_us`, `local_catalog_lag_us` (tserver clock
    minus C at publish), `local_catalog_gate_wait_us`.
  - Increment `local_catalog_poll_failures` on any failed change-stream RPC
    and `local_catalog_apply_failures` on any failed local write.
- New component class `LocalCatalogReplica` owned by `TabletServer`
  (`src/yb/tserver/tablet_server.h`, alongside `ysql_lease_manager_` at
  `:750`), holding: tablet peer, poller, `C` (atomic HybridTime),
  `applied_version_[db_oid]` map, serving state enum
  {kBootstrapping, kServing, kDisabledLease, kReseeding}.

Phase test (independent): tserver starts with the flag on and off; with it on
the component exists in state kBootstrapping, all counters are registered at
zero and visible on the metrics endpoint; with it off nothing is created.
A unit test on `LocalCatalogReplica` state transitions with a fake poller.

## Phase 1. Master: transactional raw change stream on the system catalog

- Lift the guard `src/yb/cdc/cdc_service.cc:1720-1723` ("Polling sys catalog
  tablet is only supported for CDC") for streams whose source type is
  XCLUSTER; keep it for others.
- Stream creation: one XCLUSTER-source-type, transactional, WAL-format stream
  on `kSysCatalogTabletId` (`src/yb/master/sys_catalog_constants.h:39`),
  created by the tserver on first bootstrap through the existing stream
  creation path (`src/yb/master/xrepl_catalog_manager.cc:1042
  CatalogManager::CreateCDCStream`). Stream id is stored with the local
  tablet metadata. The producer path is
  `src/yb/cdc/xcluster_producer.cc GetChangesForXCluster` (leader safe time
  at `:301`, intent resolution at `:338`, safe-time capture at `:347-359`,
  safe-time emission at `:440-452`).
- Ensure the aborted-subtransaction set is included in apply records. It is
  gated by `xcluster_enable_subtxn_abort_propagation`
  (`src/yb/cdc/xcluster_producer.cc:52`), an AUTO flag whose value depends on
  the cluster's promotion state, checked at `:154` and `:195`. The stream for
  the local catalog must not depend on that promotion: the producer's two
  checks get an override for the system catalog change stream so the set is
  always emitted for it (implemented in repo6 as an
  `IsSysCatalogChangeStream()` condition). Without it a rolled-back
  subtransaction's catalog rows would become visible in the copy on clusters
  where the auto flag is still off.
- Version read time for the gate: `SysCatalogTable::ReadWithRestarts`
  (`src/yb/master/sys_catalog.cc:962`) chooses the read time used by
  `ReadYsqlAllDBCatalogVersions` (`:1007-1021`). Thread that hybrid time out
  to `ObjectLockInfoManager::Impl::PopulateDbCatalogVersionCache`
  (`src/yb/master/object_lock_info_manager.cc:1087`, called at `:1225`) and
  put it in a new field `catalog_versions_read_time` on
  `ReleaseObjectLockRequestPB` (`src/yb/tserver/tserver.proto`, next to
  `db_catalog_version_data = 7` at `:538`). Do not reuse
  `apply_after_hybrid_time` (`:525`); it is a clock wait with different
  semantics.
- Heartbeat: add the same read time to the heartbeat response next to
  `db_catalog_version_data` (`src/yb/master/master_heartbeat.proto`, filled in
  `src/yb/master/master_heartbeat_service.cc FillHeartbeatResponse`).

Phase test (independent of any tserver code): a cxx test that creates the
stream on the master system catalog tablet and drives the change-stream RPC
from the test itself (client pattern in
`src/yb/integration-tests/cdc_service-int-test.cc`). Assert: (a) after a
`CREATE TABLE`, the response contains the transaction's write records and an
apply record whose commit HT is set and whose aborted-subtransaction set is
present; (b) after `BEGIN; CREATE TABLE a; SAVEPOINT s; CREATE TABLE b;
ROLLBACK TO s; COMMIT;`, the aborted set names b's subtransaction; (c) the
reported safe time is at or after the commit HT of every DDL whose apply
record was returned, and is not held back while another session sits idle in
a transaction block after a DDL (assert it advances within one second);
(d) a `CREATE DATABASE` produces a schema-change record before that
database's row records; (e) the lock-release request received by a tserver
(intercept in a test hook) carries the new read-time field at or after the
DDL commit HT.

## Phase 2. Tserver: local tablet creation and bootstrap

- Snapshot transfer: `RemoteSnapshotTransferClient::Start`
  (`src/yb/tserver/remote_snapshot_transfer_client.cc:62`) and `Finish`
  (`:139`); driven today by `TSTabletManager::StartRemoteSnapshotTransfer`
  (`src/yb/tserver/ts_tablet_manager.cc:1916`). Source side needs no Raft
  membership (`RemoteBootstrapServiceImpl::CreateRemoteSession`,
  `src/yb/tserver/remote_bootstrap_service.cc:453`;
  `RemoteBootstrapSession::InitSnapshotTransferSession`,
  `src/yb/tserver/remote_bootstrap_session.cc:129`). Target: a new tablet id
  reserved for the local copy (not `kSysCatalogTabletId`, to avoid
  meta-cache confusion), with the master tablet as source.
- After transfer: rewrite consensus metadata to a single-peer config
  (precedent `SysCatalogTable::SetupConfig` in `src/yb/master/sys_catalog.cc`,
  and `ConsensusMetadata::set_committed_config`), open the tablet through the
  tablet manager. Superblock carries all cotable schemas.
- Record the transfer's committed WAL position as the poller's starting
  checkpoint; persist it in the local tablet's metadata (extend
  `RaftGroupMetadata` with a small local-catalog section).
- Initialize `applied_version_[db]` by reading the local copy of
  `pg_yb_catalog_version` with the master read primitive made portable:
  `SysCatalogTable::ReadYsqlDBCatalogVersionImplWithReadTime`
  (`src/yb/master/sys_catalog.cc:1030`) works on any tablet peer once lifted
  out of the master class.
- Re-seed = tombstone the local tablet, repeat the above. Trigger on
  `CHECKPOINT_TOO_OLD` from the poll, on lease regain when the poll cannot
  resume, and on local open failure. Increment `local_catalog_reseeds`.
- Serving state stays kBootstrapping until the first poll response is applied
  and C is published.

Phase test (independent of the poller and redirect): after cluster start,
each tserver has the local tablet open as a single-peer tablet whose metadata
lists every catalog cotable of every database with the same table ids and
schema versions as master's tablet (compare superblocks). Read
`pg_yb_catalog_version` from the local tablet directly with a tablet-level
read and assert it equals master's for every database. Restart a tserver and
assert the tablet reopens without a transfer (`local_catalog_reseeds`
unchanged). Tombstone the tablet through a test hook and assert a re-seed
recreates it.

## Phase 3. Tserver: poller and apply

Model on `XClusterPoller` (`src/yb/tserver/xcluster_poller.h:55`):
`SchedulePoll` (`src/yb/tserver/xcluster_poller.cc:303`, backoff on
`poll_failures_` at `:317-319`), `DoPoll` (`:325`), `HandleGetChangesResponse`
(`:484`), `ApplyChanges` (`:699`), `ApplyChangesCallback` (`:566`), safe time
published only after apply (`producer_safe_time_` update at `:286-291`,
`GetSafeTime` at `:268`). Write a new class rather than instantiating
`XClusterPoller`, which is tied to replication-group metadata.

- Request: change-stream `GetChanges` on the master CDC service (master runs
  `CDCServiceImpl`; tablet id `kSysCatalogTabletId`; stream id from phase 1;
  from-checkpoint = persisted position).
- Apply write records: reuse the local-tablet path
  `XClusterOutputClient::ProcessRecordForLocalTablet`
  (`src/yb/tserver/xcluster_output_client.cc:512`) and the write-batch
  builders in `src/yb/tserver/xcluster_write_implementations.cc`
  (`CombineExternalIntents` `:106`, apply-transaction entries `:194-197`
  including `aborted_subtransactions`). The write lands through the
  non-transactional batch writer (`src/yb/docdb/rocksdb_writer.cc:1141
  NonTransactionalBatchWriter`, external intents apply at `:1099` and
  `:1178`). Send the batch to the local tablet through the local tablet
  service `Write` path with `external_hybrid_time` set
  (`src/yb/tablet/write_query.cc:144, 526`).
- Apply schema-change records: do not use
  `XClusterOutputClient::ProcessChangeMetadataOp` (`:521`, it remaps and
  drops add-multiple/remove). Build a `ChangeMetadataOperation` from the
  record's request and apply it to the local tablet
  (`src/yb/tablet/operations/change_metadata_operation.cc:127 Apply`;
  branches for alter schema `:242`, remove table `:253`, add multiple tables
  `:265`).
- Publish after apply: when every record of a response is applied, set
  `C := response.safe_hybrid_time`, then update `applied_version_[db]` for
  each database whose version row was in the response (parse writes to the
  `pg_yb_catalog_version` cotable, or re-read it locally at C), persist the
  checkpoint, record `local_catalog_apply_latency_us` and
  `local_catalog_lag_us`.
- Poll cadence: idle interval flag (start at 100 ms); immediate poll when the
  lock-release handler or heartbeat handler observes a version above
  `applied_version_[db]`.
- Failure handling: on RPC failure increment `local_catalog_poll_failures`,
  back off, retry; on `CHECKPOINT_TOO_OLD` trigger re-seed; on apply failure
  increment `local_catalog_apply_failures`, do not advance C, retry the
  response.

Phase test (independent of the gate and redirect; verify the copy with direct
tablet reads, no PG redirect): (a) `CREATE TABLE t` on master; poll until C ≥
its commit HT; read the local pg_class cotable at C and assert t's row is
present and identical to master's, and that a read at C from the local tablet
returns nothing from the intents store (read with intents disabled); (b) the
savepoint-rollback DDL from the phase 1 test: b's rows must be absent locally
after apply; (c) `CREATE DATABASE` then a table in it: the new cotables exist
in local metadata and hold the row; (d) `ALTER TABLE ADD COLUMN` on a catalog
is out of scope, but `CREATE INDEX` must produce the in-place `relhasindex`
update in the local pg_class row; (e) A[db] after each DDL equals master's
version for that database; (f) inject an RPC failure with a test flag and
assert the failure counter increments and C does not move; (g) a transaction
block left idle after a DDL in one session does not stop C from advancing
past a DDL committed by another session (assert within one second).

## Phase 4. Tserver: version gate

- Lock release: `TSLocalLockManager::Impl::ReleaseObjectLocks`
  (`src/yb/tserver/ts_local_lock_manager.cc:513`). After
  `WaitToApplyIfNecessary` (`:526`) and before
  `SetYsqlDBCatalogVersionsWithInvalMessages` / `SetYsqlDBCatalogVersions`
  (`:535`, `:541`): if the feature is on and serving, wait until
  `C >= req.catalog_versions_read_time()` (trigger an immediate poll first).
  Record `local_catalog_gate_wait_us`. On timeout return non-OK so master
  retries. The RPC handler is synchronous
  (`src/yb/tserver/tablet_service.cc:3982 TabletServiceImpl::ReleaseObjectLocks`,
  call at `:4005`), so the wait delays the ack as intended.
- Heartbeat: do not clamp. `TabletServer::SetYsqlDBCatalogVersionsUnlocked`
  (`src/yb/tserver/tablet_server.cc:1660-1692`) treats a reported version
  below the one already published as a stale master report and, after
  `ysql_stale_catalog_version_min_seconds`, `LOG(FATAL)`s; a database with no
  version row in the copy would clamp to 0 and trip exactly that. Omitting a
  database from the report is also unsafe: the version data is a full report
  and a missing database is removed from the tserver's map. Rule instead: in
  the heartbeat handler (`src/yb/tserver/heartbeater.cc:532-544`), while
  serving and `C < db_catalog_versions_read_time` of the response, skip the
  response's version data entirely and trigger an immediate poll; the next
  heartbeat re-sends the full report, so nothing is lost. (Corrected
  2026-09-03; the clamp was the earlier plan.)
- Not serving (bootstrap, re-seed, lease-disabled): publish without waiting.

Phase test (independent of the redirect): with a test flag that pauses the
poller, run a DDL in session A and assert its commit does not return until
the poller is resumed (release acknowledgement held), and that
`local_catalog_gate_wait_us` recorded the pause. With the poller paused and a
version arriving by heartbeat only (disable the release-path version push
with the existing flag that turns it off), assert the shared-memory version
read by a backend does not advance until the poller resumes, that no database
disappears from the tserver's version map meanwhile, and that no stale-version
warning is logged. With the poller running, assert a DDL's release wait is
below a few poll intervals.

## Phase 5. Tserver: read redirect and read-time rule

### 5.1 How the catalog read time is carried today (non-legacy mode)

The PG snapshot never holds a hybrid time. It holds a read-point serial
number; the hybrid time lives in the tserver's session, keyed by that serial.

```
 PG backend                          pggate                                tserver PgClientSession
 ----------                          ------                                -----------------------
 GetCatalogSnapshot →
   GetSnapshotData: snapshot.yb_read_point_handle
   := YbResetTransactionReadPoint(catalog)  ──► serial_no_.IncMaxReadTime()
                                                  (new serial N, not yet current)
 catalog scan → Perform ────────────► UpdateReadPointForCatalogOps:
                                        RestoreReadPoint(N)  (serial N current)
                                        SetupReadTimeOptions:
                                          read_time_serial_no = N
                                          read_time_serial_no_history_min = oldest live serial
                                          clamp_uncertainty_window = true
                                          no read_time                      ──► SetupSession(kPlain):
                                                                                SetupPlainSessionReadTime:
                                                                                  history.Cleanup(min)
                                                                                  serial changed → Save(old read point, old serial)
                                                                                                    Restore(N) if in history
                                                                                UpdateReadTime:
                                                                                  N ≠ current and nothing restored → ResetReadPoint
                                                                                  clamp set and read point empty →
                                                                                    SetCurrentReadTime(clamped)  (tserver clock)
                                                                                op to storage at that HT
 next catalog scan under the same snapshot → same serial N ─────────────────────► "Keep read time" (same HT)
 InvalidateCatalogSnapshot → next GetCatalogSnapshot allocates serial N+1 ────► Save(HT under N), Restore(N+1) fails → reset → new HT
```

Facts, with pointers:

- Serial allocation: `PgTxnManager::SerialNo::IncMaxReadTime`
  (`src/yb/yql/pggate/pg_txn_manager.cc:215`), called through
  `ResetTransactionReadPoint(is_catalog_snapshot=true)` (`:501`) from PG's
  `GetSnapshotData` (`src/postgres/src/backend/storage/ipc/procarray.c`, the
  block that assigns `snapshot->yb_read_point_handle`). The handle stored in
  `SnapshotData` is the serial (`src/postgres/src/backend/utils/time/snapmgr.c:213,
  :310`).
- Switching to the catalog serial for catalog ops:
  `PgSession::UpdateReadPointForCatalogOps` (`src/yb/yql/pggate/pg_session.cc:1181-1192`)
  restores serial N before catalog ops and the caller restores the previous
  serial afterwards (`RunHelper` block around `pg_session.cc:1141-1195`). All
  three read kinds go through this today, including user catalog reads,
  because the check is on the first table being a catalog table.
- Options sent: `SetupReadTimeOptions` (`pg_txn_manager.cc:804-868`) sets
  `read_time_serial_no`, `read_time_serial_no_history_min` and, for catalog
  snapshots, `ClampCatalogReadTime` (`:780`), which sets
  `clamp_uncertainty_window` and leaves `read_time` unset.
- Tserver history: `SetupPlainSessionReadTime`
  (`src/yb/tserver/pg_client_session.cc:4079-4096`) saves the current kPlain
  read point under the outgoing serial and restores the incoming serial from
  `ReadPointHistory` (`src/yb/tserver/pg_client_session_util.h:120`, `Save`
  `:140`, `Restore` `:124`, `Cleanup` `:169`). `UpdateReadTime` (`:3674`)
  resets the read point when the serial changed and nothing was restored
  (`:3740-3748`), keeps it otherwise.
- Who picks the hybrid time: the tserver, from its own clock. In
  `UpdateReadTime`, when `clamp_uncertainty_window` is set and the kPlain
  read point has no time, the session calls
  `read_point()->SetCurrentReadTime(ClampUncertaintyWindow::kTrue)`
  (`:3793`); the same happens on the restart path (`:3690`). So a catalog
  snapshot owns a tserver-clock time from its first read, before any op
  reaches storage; the storage-echo path (`MakeUsedReadTimeApplier`, `:4425`,
  applied by `CheckPlainSessionPendingUsedReadTime`, `:4100`) does not run for
  catalog reads because the read point already has a time. The time is saved
  into history on the next serial switch. (Corrected 2026-09-03; an earlier
  revision said storage picks it.)
- What flows back to PG: nothing about the hybrid time in non-legacy mode.
  `resp.catalog_read_time` is filled only for the legacy catalog session
  (`:3389-3393`), and pggate's `TrySetCatalogReadPoint`
  (`src/yb/yql/pggate/pg_perform_future.cc:75`, `pg_session.cc:1087`) is a
  no-op when it is unset. PG keeps only the serial. Paging continuation
  carries the read time inside `paging_state.read_time` (`:463-464`,
  `:805-808`) so later pages of one scan stay at the same time.
- History retention: PG publishes the oldest live snapshot serial
  (`YbNoteReadPointAdded/Removed`, `snapmgr.c:2815-2828`) which pggate sends
  as `read_time_serial_no_history_min`; the tserver drops history below it.
  So a read point stays recoverable as long as the PG snapshot that owns the
  serial is alive.

### 5.2 Where the local read time is set

The local read is a different storage target under the same session and
serial machinery; nothing changes on the PG side and the serial keeps meaning
"one catalog snapshot".

- Chokepoint: `DoPerform` after `SetupSession` (`:3377-3380`) and before the
  response-cache and storage dispatch (`options.has_caching_info()` at
  `:3362`). Condition: flag on, serving, op is a catalog read
  (`is_ysql_catalog_table`), serial is a catalog snapshot serial (options
  carry `clamp_uncertainty_window` from `ClampCatalogReadTime`; add an
  explicit `is_catalog_snapshot` boolean to `ReadTimeOptionsPB` rather than
  inferring), backend not in DDL (`options.ddl_mode()` false and, for
  transaction blocks, `ddl_use_regular_transaction_block()` false; both set
  in `SetupPerformOptions`, `pg_txn_manager.cc:930-932`).
- Version check, only for ops that carry one. Cache miss reads and internal
  catalog reads carry no version: `YbSetCatalogCacheVersion` is skipped for
  internal scans of system relations
  (`src/postgres/src/backend/access/yb_scan/yb_scan_core.c:3701`, the
  `!(is_internal_scan && IsSystemRelation(table))` guard), so
  `ysql_db_catalog_version` is unset on them and a wait keyed on it is a
  no-op. Their correctness rests entirely on the exposure gate: a backend
  learns V only from shared memory, and neither the release path nor the
  heartbeat path publishes V before C has passed the time master read V at.
  User catalog reads are not internal scans and do carry the version
  (`src/yb/common/pgsql_protocol.proto:553, 557`); for them, wait until
  `applied_version_[op.ysql_db_oid] >= op.ysql_db_catalog_version`, trigger
  an immediate poll, count `local_catalog_reads_waited_for_version`. The two
  paths where this wait is the real guard: a version delivered by heartbeat
  while the copy is behind, and a release acknowledged while the copy was not
  serving. The DDL's own tserver needs no wait: the release handler runs
  there too, synchronously, before the commit returns to the client, so
  A[db] is already at V when that session issues its next read (confirmed by
  the test run: served grows, waited stays at zero). Temporary-relation DDL
  (no version increment) is covered by neither and is why the own-writes
  floor exists. (Corrected 2026-09-03.)
- Read time, using the kPlain read point that `SetupSession` just prepared.
  By the time the redirect runs, `UpdateReadTime` has already given a fresh
  catalog snapshot a clamped tserver-clock time (`:3793`), so the redirect
  never sees an empty read point; it sees either that clock time or a time
  restored from history. One comparison decides everything, because C only
  moves forward: a locally chosen time is always at or below the current C,
  and a clock-derived time is always above it.
  1. First redirected read of a fresh catalog snapshot: the read point holds
     the clock time just set by `UpdateReadTime`, which is above C. Replace it
     with `ReadHybridTime::SingleTime(C)` (`session->SetReadPoint`, as
     `UpdateReadTime` does for an explicit read time at `:3738`). The next
     serial switch saves it into history under this serial, so every later
     read under the same catalog snapshot restores the same C. As
     implemented: `UpdateReadTime` records
     `catalog_read_time_picked_this_request_` when it takes the catalog
     snapshot's time from the clock, and the redirect replaces the time in
     that case and in the empty case, keeping it otherwise. The clock pick is
     left in place rather than suppressed: it is what keeps a catalog read
     that does go to master from being answered below the session's own
     just-committed DDL (reasoning at `src/yb/yql/pggate/pg_session.cc:930-940`).
  2. Later reads: read point time h ≤ C means it was set by this rule (or
     by any earlier local read): serve locally at exactly h. h > C means the
     snapshot's time came from somewhere else (its first read went to master
     because the tserver was not serving or the backend was in DDL, or an
     explicit read time was requested): route that op to master. Never read
     locally at C for such a snapshot; it would return a state older than an
     earlier read under the same snapshot. No source tagging is needed.
  3. `SingleTime` makes read = local limit = global limit, so read restarts
     are disabled, matching the catalog read semantics on master; see the
     same construction in `UpdateReadPointForXClusterConsistentReads`
     (`:3540`).
  4. A local read that fails with SnapshotTooOld (h below the local tablet's
     history retention) is routed to master, not failed.
  5. Never take the tserver clock as the local read time; the redirect's job
     is to replace the clock time the session already chose.
- Paging: fill `paging_state.read_time` for local reads exactly as the
  storage path does (`:463-464`), so continuation pages use the fixed time.
- What flows back to PG: unchanged, nothing. The catalog snapshot still owns
  only the serial; the hybrid time C stays in the tserver history under that
  serial. Do not set `resp.catalog_read_time` (legacy-only field).
- Routing to master with a counter: in DDL, not serving, non-MVCC snapshot
  kinds and historic snapshots (walsender), read point time above C, and
  SnapshotTooOld from the local copy.
- User catalog reads: today they share the catalog snapshot serial (through
  `UpdateReadPointForCatalogOps`), so the rule above already applies to them
  with h being the catalog snapshot's time. When YB moves them to the
  transaction's read point, they will arrive under the transaction serial
  with `read_time` set; the rule becomes: read at h if h ≤ C, else route to
  master for that op (do not silently read at C under a transaction
  snapshot that expects h).
- Own-writes floor for no-increment DDLs: after a catalog-writing
  transaction commits without a version increment, pggate records the
  commit HT on the session and sends it in `ReadTimeOptionsPB` as a minimum
  required C; the tserver waits for `C >= floor` before serving.
- Explicit read times (`yb_read_time`, dumps at a time): the request carries
  `read_time`, `UpdateReadTime` sets it on the read point (`:3738`), and the
  rule above handles it with no special case: at or below C and within local
  retention, served locally at exactly that time; above C, master; below
  retention, master via the SnapshotTooOld route.

Phase test (independent of phase 4; run with the gate disabled by flag):
(a) a backend's first catalog miss under a fresh catalog snapshot is served
locally and the kPlain read point afterwards equals C at that moment
(expose through a test-only RPC or log); (b) a second miss under the same
snapshot uses the same time although C advanced (compare read times in the
verbose log or paging state); (c) after the snapshot is invalidated the next
miss uses a newer time; (d) pause the poller, run a DDL, resume: the
backend's read tagged with the new version waits and is served at a C at or
after the DDL commit; (e) time above C: with serving disabled by flag, run a
statement whose catalog snapshot reads master, re-enable serving
mid-transaction with a repeatable-read catalog snapshot still alive, and
assert the next miss under it goes to master, not to the local tablet at an
older C; (f) walsender and in-DDL requests are counted as routed to master;
(g) `SET yb_read_time` to a time below the local retention and read a
catalog table: served by master, no error; to a time at or below C: served
locally at that time.

## Phase 6. Lease hooks (skeleton; details are a TODO)

- Lease state comes through `YsqlLeaseClient` with a
  `YsqlLeaderClientListener` callback (`src/yb/tserver/ysql_lease_poller.h:29-42`),
  managed by `YSQLLeaseManager` (`src/yb/tserver/ysql_lease_manager.h`,
  started from `TabletServer::StartYSQLLeaseRefresher`,
  `src/yb/tserver/tablet_server.cc:2594`).
- On lease loss: set serving state kDisabledLease, count
  `local_catalog_serving_disabled_lease`.
- On regain: hold version publication from the lease refresh and heartbeat
  until the poller has applied a response with `S >=` the lease grant time;
  on `CHECKPOINT_TOO_OLD` re-seed. Full semantics: TODO.

Phase test (skeleton level): force lease loss through the existing lease test
hooks; assert serving state becomes kDisabledLease, the counter increments,
and catalog reads are routed to master (`reads_to_master_not_serving`) while
correct. On regain with the poller caught up, assert serving resumes and the
version published equals master's.

## Phase 7. Tests

Location: new `src/yb/yql/pgwrapper/pg_local_catalog-test.cc` (register in
`src/yb/yql/pgwrapper/CMakeLists.txt`), modeled on
`pg_catalog_perf-test.cc` (RPC-count assertions) and
`pg_shadow_catalog_cache-test.cc` (cross-session DDL flows, log waiters).
Use a 3-tserver mini cluster with object locking, transactional DDL and the
feature flag on. Assertions use the phase 0 metrics
(`local_catalog_reads_served` etc.) read through the tserver metrics endpoint,
and master read counts from `pg_catalog_perf-test.cc`'s helpers.

Cache miss reads:

1. Cold backend, warm copy: connect, query a user table; assert the pg_class,
   pg_attribute and pg_index lookups were served locally (served counter
   increments, master catalog read count unchanged).
2. Cross-tserver DDL: session A on T1 creates a table; session B on T2 queries
   it immediately after A returns; assert B's misses were served locally and
   correct (rows returned), and that no read went to master.
3. Own-tserver DDL: session A creates a table and immediately queries it;
   assert the read was served locally and that `waited_for_version` stayed at
   zero (the release handler ran on this tserver before the commit returned).
   Separately assert the two paths where the wait is the real guard: a
   version delivered by heartbeat while the poller is paused, and a release
   acknowledged while serving was disabled, each followed by a user catalog
   read tagged with V that must wait.
4. Equivalence under concurrent DDL: session A runs a statement whose misses
   span pg_class and pg_attribute while session B commits `ALTER TABLE ADD
   COLUMN` between the two. Run the same sequence twice, once served from the
   copy and once with `TEST_local_catalog_disable_serving` so master answers,
   and assert identical results. (The earlier "both old or both new" premise
   does not hold: accepting invalidation messages mid-statement drops the
   catalog snapshot on master too; the property the copy owes is equivalence
   with master.)
5. In DDL: `BEGIN; CREATE TABLE t; INSERT INTO t ...; SELECT ...; COMMIT;`
   in one session; assert every catalog read after the CREATE went to master
   (`reads_to_master_in_ddl` increments) and the block succeeds.
6. Temporary relation: `CREATE TEMP TABLE`, force a relcache rebuild, query;
   assert the own-writes floor made the read wait and succeed locally.

Internal catalog reads:

7. A DML statement that reads `pg_depend` and `pg_description` (for example
   a `COMMENT`-bearing view expansion, or `pg_get_expr` on a constraint):
   assert those scans were served locally under the same read time as the
   cache miss reads of the statement (read time returned equal across ops).
8. Direct heap scan at connection time (database listing) is not redirected;
   assert master count.

User catalog reads:

9. `REPEATABLE READ` equivalence: `BEGIN; SELECT count(*) FROM pg_class;`
   then another session creates a table; `SELECT count(*) FROM pg_class`
   again in the first session. A repeatable-read transaction does not hold
   one catalog snapshot across statements: accepting invalidation messages
   drops it, so the new table (and its primary-key index) is visible to the
   second count on master as well (measured 391 then 393). Run the sequence
   once from the copy and once with `TEST_local_catalog_disable_serving`;
   assert the deltas match and that the copy served the reads.
10. `READ COMMITTED`: same flow; assert the second read sees the new table
    once the first session's version has advanced, and that the read time
    used was C (returned read time ≤ tserver clock, ≥ HT of the DDL).
11. User catalog read in DDL block goes to master.

Bootstrap and failures:

12. Fresh cluster: local tablet exists on every tserver after start, serving
    state reaches kServing, first poll applied (poll latency histogram
    non-empty).
13. Tserver restart: poller resumes from checkpoint; reads served locally
    after restart without re-seed (`reseeds` unchanged).
14. Forced re-seed: shrink master WAL retention flags, stop the poller with a
    test flag long enough to fall behind, resume; assert `CHECKPOINT_TOO_OLD`
    leads to `reseeds` increment and serving resumes.
15. Poll failure counter: inject RPC failures with a test flag; assert
    `local_catalog_poll_failures` increments and serving continues at the
    last C.
16. Gate wait: measure `local_catalog_gate_wait_us` on a DDL; assert bounded
    (below a few poll intervals).

Concurrency (added 2026-09-03 after the review):

17. Open DDL transaction block does not block another DDL or its readers.
    Session X on T1: `BEGIN; CREATE TABLE a (...);` and stay idle (X now
    holds intents in the master system catalog tablet). Session Y on T2:
    `CREATE TABLE b (...)` autocommit. Assert: Y returns within a bounded time
    (a few poll intervals, well under X's idle period); a cold backend on T3
    reads b served locally with the correct rows and `reads_served` grows; C
    on every tserver advanced past b's commit while X was still open. Then X
    commits; assert a cold backend on T3 reads a served locally. Repeat with X
    and Y on the same tserver, and with X holding two DDLs before Y runs.
    This is the property that the master-side intent resolution certifies the
    safe time without waiting for open transactions; a regression here shows
    as Y's commit blocking for X's lifetime, or as the gate wait histogram
    recording X's idle time.
18. Two DDLs racing to commit on different tables from different tservers,
    ten iterations: both return, both tables readable locally on a third
    tserver, applied versions on every tserver end equal to master's, and no
    gate wait exceeds a few poll intervals.
19. Explicit read time: `SET yb_read_time` to a time at or below C and read a
    catalog table: served locally at that time; set it below the copy's
    history retention: served by master with no error; set it above C: master.

Regression: run `yb.orig.schema`, `yb.orig.select`, `yb.orig.guc`,
`yb.orig.query_consistent_snapshot` and `TestPgRegressPgTable` with the flag
on.

### Gaps after implementation (2026-09-03), excluding the plan's original follow-ups

- Catalog preload (backend start, full cache refresh) and parallel workers
  read master: both run on the legacy catalog-ops path. Not in the original
  plan's read-kind table; recorded as a limitation, accounts for the residual
  master reads per fresh connection.
  FIXED 2026-09-16: the copy now serves the legacy catalog session too, via
  `IsCopyServableCatalogRequest` in `src/yb/tserver/pg_client_session.cc`. The
  cost this hid is one master round trip per fresh connection, for the shared
  catalog preload (`pg_authid`, `pg_database`, `pg_yb_catalog_version`,
  `pg_yb_logical_client_version`), which is 200 ms on a region 100 ms away.
  Measured by `PgLocalCatalogTest.FreshBackendDoesNotPayMasterCatalogLatency`;
  details in `notes/worklog2.md`.
- Major version upgrade postponed (guard only).
- Serving is not disabled after repeated poll failures (plan 7.2); the lease
  lapse covers a master outage, but a poll-only failure keeps serving at the
  last C indefinitely. TODO: after N consecutive poll failures (flag), move to
  a not-serving state so catalog reads go to master and versions publish
  without the gate; resume on the next successful poll. Test: block the
  change RPC on master with a test flag, run a DDL elsewhere, assert the
  copy stops serving and reads succeed via master; unblock and assert serving
  resumes.
- Runtime corruption of the local tablet does not trigger re-seed; only an
  unopenable tablet at start or a checkpoint-too-old response does.
- Lease grant time is the local clock's global maximum at epoch observation,
  not master's grant time (accepted).
- History cutoff of the copy used the generic user-tablet provider (namespace
  backfill via master client, xCluster safe time, 15 min clock policy); the
  namespace lookup can pin the cutoff at the minimum and stop all history GC.
  Landed 2026-09-03: a provider selected on the tablet id that applies
  `timestamp_syscatalog_history_retention_interval_sec` (4 h, the same bound
  master's sys_catalog uses) in both cutoff fields and skips the namespace and
  xCluster lookups. Correction: the namespace lookup did not fail for the copy
  (master's metadata carries the system namespace id and remote bootstrap copies
  it), so the only gap was the 15 min versus 4 h mismatch. The copy's
  committed cutoff is seeded from the SST frontiers copied from master, so it
  starts at master's cutoff rather than empty.
- Master deleting the copy as an orphan: verified not possible (2026-09-04).
  Tablet reports, incremental and full, are built only from the tablet map
  (dirty set filtered through it; full report from GetTabletPeersUnlocked),
  the copy is excluded from the map at startup and never registered by the
  open path, and its bootstrap does not use the remote-bootstrap tracking set
  that the report also includes. Master deletes an unknown reported tablet
  only when the id is in its list of tablets deleted in the past
  (`master_enable_deletion_check_for_orphaned_tablets`, default true), and a
  master-generated id is a version 4 UUID whose version nibble is 4, so it can
  never equal the copy's id (all zeros then 1). Tests requested: the copy id is
  absent from a full tablet report; with the orphan check disabled and a
  forced full report, the copy survives. No tablet id reservation or
  heartbeat marker is needed.
- Automatic RocksDB compactions run on the copy; scheduled full compactions and
  admin-triggered compactions do not, because both enumerate the tablet map
  the copy is kept out of. Accepted.
- Review annotations: user's disposition (2026-09-05) was strip the `//C:` and
  `//R:` lines from the four source files, commit locally, do not update
  D57801. Done; feature commit amended (39/39, lint clean); D57801 still shows
  the 27-test state deliberately. Phase 8 started the same day.
- Startup reuse (2026-09-05): an existing copy is reopened from local metadata
  before any master contact; the initdb check runs only on the fresh-copy
  path; the first setup attempt runs at once (the scheduler asked for the
  interval before the first poll, so the 1 s delay applied before any
  attempt). Measured 0.227 s from restart to serving. Test
  `RestartReusesTheCopyWithoutAskingMaster` (retry delay set to 60 s so a
  regression cannot pass quietly; asserts master's IsInitDbDone handler count
  unchanged, a cluster-wide counter that weakens rather than fails if another
  caller appears).
- Copy's own Raft group, three defects fixed 2026-09-05: it waited a
  failure-detection interval before it could apply; electing it at open put
  the election no-op's position into the resume checkpoint (a position in the
  copy's log, not master's WAL), which froze C, skipped one master op, and
  broke CREATE DATABASE on that tserver, fixed by index-based checkpoint
  comparison; reads required a leader lease and were refused for
  `leader_lease_duration_ms` after every restart (1.30 s first cold query vs
  0.05 s from master). Suite 38/38.
- Tests 17 to 19 written and passing (suite 35/35 at the time). Constraints learned in
  test 19: "above C" only holds while the poller is paused
  (`TEST_local_catalog_pause_poller`); a read time far enough back to fall
  below a 4 h cutoff predates the cluster's catalog and PG fails in relcache
  build before the redirect runs, so retention is tested by shortening the
  intervals and forcing a flush with `memstore_size_mb=1` under DDL churn,
  never by reaching further back; `TEST_committed_history_cutoff_initial_value_usec`
  sets only the primary cutoff while reads are bounded by the cotables cutoff
  when valid, so it no longer pins catalog reads once the provider sets both.
- Also fixed: checkpoint persistence waits for the log to flush before
  recording a position (master ships only what follows the checkpoint, so a
  lost log tail would never be re-sent); a read-before-inject race in the
  poll-failure test.

## Cross-check against the PG shadow test suite

The PG shadow implementation (`src/yb/yql/pgwrapper/pg_shadow_catalog_cache-test.cc`
on branch `approach3-local-catalog`, worktree `repo5`) accumulated tests that
encode catalog-consistency scenarios independent of the storage substrate.
Run the applicable ones, ported to the new flag and metrics, at the end of the
core plan. Substrate-specific tests (tablespace, shadow schema bootstrap,
change-log population) do not apply.

| Existing test | What it encodes | Applies to the local tablet | Port notes |
|---|---|---|---|
| `PicksUpDdlAcrossSessionsAfterHeartbeat` | Cross-session DDL: session B's first pg_class miss after A's CREATE is served locally and correct, second query has no miss | Yes | Replace the log-marker assertion with the `local_catalog_reads_served` counter. Same as test 2 in phase 7. |
| `PicksUpDdlAfterClusterRestart` | Cross-session flow, full cluster restart, flow again on a different table | Yes | Verifies poller resume from checkpoint and no re-seed; assert `local_catalog_reseeds` unchanged. |
| `DdlSessionNotBlocked` | CREATE then ALTER in one session; parse-time catcache rows must be usable by the ALTER's catalog update | Yes | With the local tablet, rows come from DocDB so the row identity is native; the test still guards that in-DDL reads go to master. |
| `PicksUpPgProcWithMultilineBody` | pg_proc body with newlines round-trips | Yes | Trivial for a byte-identical DocDB copy; keep as a smoke test. |
| `PicksUpCheckConstraint` | pg_constraint CHECK enforcement after cross-session DDL | Yes | Internal catalog read (`pg_constraint` scan in relcache build) served locally. |
| `PicksUpSharedAuthid` | CREATE ROLE in A, privilege check in B served from the local copy of a shared catalog | Yes | Shared catalogs are ordinary cotables in the local tablet; also covers the global-DDL version bump reaching every database. |
| `PicksUpTypeNameLookup` | Custom enum type resolved at parse time from the local copy | Yes | |
| `PicksUpAttrMissingValAcrossSessions` (disabled in PG shadow) | ADD COLUMN with a wide default; `attmissingval` read correctly across sessions | Yes | Was blocked by the shadow's freshness race; must pass here. Also exercises large values in the local tablet. |
| `PicksUpViewDefinedAcrossSessions` | CREATE VIEW in A, view usable in B; pg_rewrite rules resolved | Yes | pg_rewrite is a normal cotable here; rule order comes from the same index. |
| `PicksUpNewIndexAcrossSessions` (disabled in PG shadow) | CREATE INDEX in B, planner in A uses it on the next statement, served locally | Yes | Was unprovable in the shadow because the statement that applied the invalidation also triggered the writer; here the version gate makes the first post-invalidation read local. Key test for the gate. |
| `SameSessionCreateIndexThenExplain` | Own-session CREATE INDEX, next EXPLAIN uses it | Yes | Exercises the own-tserver per-op version wait (test 3 in phase 7). |
| `AlterTableSetSchemaPreservesIndexes` (and its `_NoShadow` control) | CREATE INDEX then ALTER TABLE SET SCHEMA; index still listed and used | Yes | In the shadow this caught a missed in-place pg_class update. Here it guards that in-place updates in the WAL are applied. Keep the control variant with the flag off. |
| `DmlAfterDdlInSameTransactionBlock` | `BEGIN; CREATE TABLE; INSERT; SELECT; COMMIT` reads master after the DDL | Yes | Same as test 5 in phase 7; assert `reads_to_master_in_ddl`. |
| `SubtransactionRollbackDoesNotReachShadow` | SAVEPOINT; CREATE TABLE; ROLLBACK TO; the rolled-back relation must not be visible locally | Yes | Here it verifies the apply record's aborted-subtransaction set is honored (producer flag on). Name lookup must fail locally as it does on master. |
| `TempRelationDdlReachesShadow` | CREATE TEMP TABLE, relcache rebuild, query must succeed | Yes | Here it verifies the own-writes floor for a DDL without a version increment (test 6 in phase 7). |
| `GlobalDdlDoesNotLeakPerDatabaseRows` | CREATE TABLE plus CREATE ROLE in one transaction; other databases must not see the table | Yes, as a negative control | Cannot fail structurally in the local tablet (rows live in their own cotable), but cheap to keep. |
| `PhaseAB_DdlPopulatesChangeLog` | change_log column populated | No | Change log does not exist in this design. |
| `Phase1_*`, `Phase2_*` | tablespace and shadow schema bootstrap | No | Replace with test 12 in phase 7 (local tablet exists and serves). |

### Existing master tests that surfaced defects during the PG shadow work

These are pre-existing tests, not written for the shadow, that failed during
the shadow bring-up and each pointed at a real defect. Run all of them with
the local-tablet flag on, in fastdebug and in release, at the end of the core
plan. The "watch for" column translates the shadow defect into the failure the
same test would expose in this design.

| Test | Mode it failed in | Defect it exposed in the PG shadow | Watch for in the local tablet |
|---|---|---|---|
| `PgCatalogPerfTest.AfterCacheRefreshRPCCountOnInsertMinPreload` (`src/yb/yql/pgwrapper/pg_catalog_perf-test.cc`) | fastdebug | The writer published a version it had not read together with the data: its pg_class scan lacked a table committed at a version it then published; a same-session `ALTER TABLE` got "relation does not exist". Never fully fixed in the shadow. | The exposure gate must publish V only after C ≥ the version-read time master sent, and the own-tserver per-op wait must hold. The test also counts master catalog RPCs after a refresh; with the local copy serving, that count should drop to zero. |
| `TestPgRegressMisc#testPgRegressMiscSerial`, entries `yb.orig.schema`, `yb.orig.select`, `yb.orig.guc`, `yb.orig.query_consistent_snapshot` | fastdebug only; passed in release | A whole catalog write path was not captured: `index_create` sets `pg_class.relhasindex` through the non-transactional in-place update, which bypassed the capture hooks. Plans showed Seq Scan instead of Index Scan and `\d` lost its Indexes section. The mode dependence came from the "in DDL" gate being evaluated differently under transactional DDL. | In-place updates are ordinary writes in the master WAL, so they arrive by construction; the test guards that the poller applies them and that the read time rule does not let a DDL read a pre-update pg_class row. Run in both build modes: default flags differ (`yb_ddl_transaction_block_enabled`, `enable_object_locking_for_table_locks` are true in release, false in fastdebug), and the design assumes both on. |
| `yb.orig.compound_key` (same schedule) | fastdebug | Same-session `CREATE INDEX` followed by `EXPLAIN` did not use the new index: the freshness gate served a stale shadow to the statement right after the DDL. | Own-tserver per-op version wait (phase 5) after the DDL backend's local version advanced; reproduced by `SameSessionCreateIndexThenExplain`. |
| `TestPgRegressPgTable` (3 tests) | fastdebug | Regression check after the writer changed from truncate to delete-and-insert. | General catalog correctness under the redirect. |
| Java `TestPgReplicationSlot#testAddDropPrimaryKey` | any | Walsender crashed with SIGABRT: the reader took a fresh MVCC snapshot while logical decoding had a historic snapshot active. | Logical decoding backends must never be redirected; the tserver-side redirect must see the historic-snapshot case and route to master (phase 5 routing rule). |
| Plain `psql`: `create table t; insert into t` in one session | fastdebug | The writer's truncate emitted about 200 storage invalidation messages per pass and overflowed the shared invalidation queue of an idle backend, which then crashed in a critical section. | Not a test; keep as a smoke scenario. The local tablet emits no PG invalidations, so a regression here would mean PG-side changes leaked into the DML path. |
| `PgShadowCatalogCacheTest.PicksUpAttrMissingValAcrossSessions`, `PicksUpNewIndexAcrossSessions` | fastdebug | Disabled in the shadow because the version stamped as "fresh" could be newer than the shadow's content when a heartbeat bumped the version between the init-file load and the freshness mark. | Direct analog of the exposure gate; both must pass here (listed in the table above). |
| `CreateAbortTest.TestAbortIndexCreation`, `PgAutoAnalyzeTest.AutoAnalyzeRetryAnalyze` | any | Reactor join hang at tserver shutdown, traced to an unrelated shutdown-ordering bug; the shadow's trigger RPC was made shutdown-aware as a guard. | The poller and the snapshot-transfer client must check the tserver shutdown flag before scheduling work, or they will hold the reactor past its join deadline. If these two tests hang, get a stack before attributing it to this work. |

Operational lessons from the same runs that apply here:

- A stale `initial_sys_catalog_snapshot` under `build/` after any catalog
  header change made master load an old schema and produced confusing DDL
  failures; run `./yb_build.sh reinitdb` after touching catalog headers or
  protos that change the system catalog.
- Rows in `pg_yb_invalidation_messages` are deleted after their expiration on
  every DDL commit; a long DDL (a 25 s index build) outlived them. The local
  tablet does not depend on that table for data, but the master WAL retention
  for the per-tserver streams plays the same role; the re-seed path must be
  exercised by test 14 with retention deliberately shortened.
- Failures in the "Seq Scan instead of Index Scan" family always traced to one
  missing catalog row in the local copy. First check the applied-version map
  and C against the version the backend carried, then check the poller's
  apply log for the transaction that wrote the row.

## Addendum: initdb, YSQL upgrade, major version upgrade, explicit read times

Decision: the local tablet is not used during initdb; it is used during a
major version upgrade for ordinary sessions, with the conditions below.

### initdb

- Global initdb runs in master's embedded PG and writes the system catalog
  tablet directly; no tserver has a local tablet yet, and there is nothing to
  transfer until initdb finishes. Local initdb on each tserver
  (`PgWrapper::InitDbLocalOnlyIfNeeded`, `src/yb/yql/pgwrapper/pg_wrapper.cc`)
  builds only the local PG data directory.
- Rule: no pggate change is needed. `YBCIsLegacyModeForCatalogOps()`
  (`src/yb/yql/pggate/ybc_pggate.cc:2062-2074`) returns true when
  `YBCIsInitDbModeEnvVarSet()` is set, and only the non-legacy path marks a
  read as a catalog snapshot read (`IsCatalogSnapshot` in
  `PgSession::Perform`), so initdb reads never carry the redirect marker and
  go to master. The bootstrap of the local tablet (phase 2) starts only after
  master reports initdb complete (the initdb-done state the tserver already
  waits on before accepting PG connections).
- The same predicate excludes three more paths from the feature, and the
  plan relies on that: sys-table prefetching
  (`YBCIsSysTablePrefetchingStarted()`), so the catalog preload path at
  backend start and after a full cache refresh keeps reading master through
  the response cache; parallel workers (`IsParallelWorker()`); and any
  cluster with object locking or concurrent DDL turned off. Preload traffic
  is therefore not reduced by this design; it is bounded by the response
  cache as today.
- Regenerating `initial_sys_catalog_snapshot` is unaffected: it is produced by
  the global initdb path.

### YSQL upgrade (minor; migrations)

- Migrations run with `IsYsqlUpgrade` and `yb_non_ddl_txn_for_sys_tables_allowed`
  and write catalog rows through SQL; they increment the version through the
  SQL functions. Their writes reach the local tablet through the WAL like any
  other; A[db] follows the version row.
- Rule: sessions in upgrade mode (`IsYsqlUpgrade`) are treated as in DDL and
  read master. Ordinary sessions keep reading the local tablet; the version
  gate and the heartbeat clamp cover the increments.

### Major version upgrade: postponed (TODO), not supported by the core plan

Decision (user, 2026-09-03, superseding the earlier rule set): the local
tablet is not used while a major version upgrade is in progress. Reason:
during that window two catalog version tables exist (one per PG version), and
some DDLs still run (`REFRESH MATERIALIZED VIEW CONCURRENTLY` was the example),
so the single-version gate and applied-version map of this plan do not
describe the cluster's state.

Core-plan rule: while the cluster is in a major version upgrade (the state the
tserver already learns from master for upgrade handling), the tserver keeps
serving disabled and routes every catalog read to master; the poller may keep
running so the copy is current when the upgrade completes. No support for
upgrade-time redirect is to be attempted in the core plan.

TODO for later: define which version table gates which sessions, how new
cotables created by the upgrade (arriving through schema-change records)
become eligible, and how rollback (removal records) is handled. Notes kept
from the earlier draft: upgrade-process sessions (major-upgrade initdb mode,
`IsYsqlUpgrade`) would always read master; a read for a cotable absent from
local metadata would route to master without waiting.

### Explicit read times (`yb_read_time`, `ysql_dump --read-time`)

- A catalog read that carries an explicit read time h (options
  `read_time_options.read_time` set, `SetReadTimeIfPresent` in
  `pg_session.cc`) is served locally only if h ≤ C and h is within the local
  tablet's history retention; otherwise it is routed to master. Never clamp
  such a read to C: the caller asked for h.

### Phase test additions

- initdb: a fresh cluster initializes with the flag on; assert no local read
  is attempted before initdb completes (served counter zero until then) and
  that the local tablet bootstrap starts afterwards.
- Minor upgrade: run the YSQL upgrade on a cluster with the flag on; assert
  the upgrade sessions' reads went to master, ordinary sessions kept being
  served locally, and A[db] equals master's version after each migration.
- Major upgrade (core plan): run the major-upgrade test harness (see
  `src/yb/integration-tests` upgrade tests) with the flag on; assert that no
  catalog read is served locally while the upgrade is in progress
  (`reads_to_master_not_serving` increments, served counter flat), that the
  cluster upgrades and rolls back correctly, and that serving resumes after
  the upgrade completes.

### Deviations recorded from the repo6 implementation (2026-09-03)

The implementing session reported three departures from the pointers above;
they stand, and later readers should follow the implementation, not the
original pointer:

- Sync channel: master ships the WAL through a new `SysCatalogChangeService`
  rather than `CDCService::GetChanges`. Reason: `GetChanges` needs a stream
  entry in the master catalog and writes `cdc_state` on every call, at the
  poll rate, per tserver. Phase 1's stream creation and guard-lifting items
  are therefore replaced by the new service; the safe-time semantics of 4.1
  in `plan2.md` must be reproduced by it (leader safe time after intent
  resolution, reported only once the WAL up to it has been shipped).
- Bootstrap: the copy is fetched by remote bootstrap rather than the
  snapshot-transfer client, because the snapshot transfer ships neither the
  WAL nor a committed position to start polling from.
- Tablet registration: the copy's tablet peer is kept out of
  `TSTabletManager::tablet_map_`, because everything in that map is offered
  to master in the heartbeat tablet report and master would delete the
  unknown tablet.

Facts about the new service that the plan relies on (confirmed by the
implementer, 2026-09-03):

- `SysCatalogChangeServiceImpl` does not compute a safe time itself. It builds
  an `XClusterGetChangesContext` and calls `cdc::GetChangesForXCluster`
  (`src/yb/cdc/xcluster_producer.cc:289`) unchanged. So the safe-time
  semantics of `plan2.md` 4.1 hold by construction: leader safe time, then
  `TransactionParticipant::ResolveIntents` against every running transaction's
  coordinator, then the time is recorded with the majority-replicated WAL
  index at that moment and emitted only in the response whose checkpoint
  reaches that index.
- Throttling is likewise unchanged: a new safe time is computed when the
  participant has no running transactions, or when
  `xcluster_consistent_wal_safe_time_frequency_ms` (250 ms) has elapsed since
  the last one. With catalog transactions in flight, master performs at most
  one intent-resolution round per 250 ms per requesting tserver; with an idle
  catalog, every poll gets a fresh time and C tracks master's clock at the
  poll interval.
- The per-requestor state (`last_apply_safe_time_`,
  `apply_safe_time_checkpoint_op_id_`) lives in a
  `StreamMetadata::StreamTabletMetadata` (`src/yb/cdc/xrepl_stream_metadata.h:40`)
  held in master memory, one per tserver uuid, created on first request and
  never persisted. Consequence: after a master leader change the first poll
  from each tserver starts a fresh computation; no safe time can be emitted
  from stale state, and a poll may briefly return records without a safe
  time until the new computation completes. The poller must treat a response
  without a safe time as "records only, C unchanged".
- Monotonic publication (confirmed): a response without a safe time applies
  its records and advances A[db] and the WAL position, but leaves C where it
  was; `PublishAfterApply` stores an incoming safe time only if it is valid
  and strictly greater than the current C, and takes a response checkpoint
  only if it is above the current one. So neither an absent safe time nor an
  older one re-sent by a new master leader can move C or the checkpoint
  backwards. Reads keep being served at the last C, which remains a complete
  point for the copy.
- Checkpoint persistence and idempotent re-apply (confirmed): the WAL
  position is persisted on a cadence (`local_catalog_checkpoint_flush_interval_ms`,
  default 1 s) and once at shutdown, not after every response. A restart
  therefore re-applies a bounded suffix of records already applied. This is
  safe because the copy's writes are idempotent by construction: a re-applied
  write record re-stages the same external intent with the same key, value
  and external hybrid time; a re-applied apply record for a transaction whose
  staged intents were already converted finds none and applies nothing; and
  when both are re-applied in order, the conversion writes the same rows at
  the same commit HT. Re-delivery is always a contiguous suffix in WAL order,
  so a write record is never re-applied without the apply record that
  follows it. A[db] is recomputed from the version rows and C is guarded as
  above, so neither regresses.
- Bring-up fixes recorded from the repo6 worklog (each one blocked the
  tserver or the copy until found): `RaftConfigPB::committed_op_index` must
  be set on the copy's committed single-peer config or `VerifyRaftConfig`
  rejects it; the copy's tablet peer needs a live mark-dirty callback because
  consensus invokes it at start (an empty callback crashes every tserver);
  `bootstrap_retryable_requests` must be off for the copy or tablet bootstrap
  replays master's entire initdb WAL instead of starting at the flushed op
  id; and the persisted WAL position must not live in the tablet metadata
  directory, because `FsManager::ListTabletIds` treats every file there as a
  tablet id and the tserver refuses to start.
- Test status (2026-09-03): all 20 tests in `pg_local_catalog-test` pass on
  a three-tserver external cluster.
- Major-version-upgrade guard: `TSHeartbeatResponsePB` gained
  `ysql_major_version_upgrade_in_progress` (field 35), filled from
  `YsqlManagerIf::IsMajorUpgradeInProgress()`; `LocalCatalogReplica::IsServing()`
  returns false while it is set, so every catalog read goes to master while
  the poller keeps running.

## Review of the implementation (2026-09-03, commit `5eea0006ec8` on `docdb_local_catalog`)

Reviewed against `plan2.md` and this document; worklog in
`worklog2.md`. No deviation requires a design change. Items sent to the
implementing session, in order of importance:

1. Coverage: the redirect refuses any request whose plain session carries a
   distributed transaction, which is every catalog miss after a write in a
   transaction block. Review asked to remove the check; WITHDRAWN after the
   implementer showed the check is load-bearing: `YBSession::read_point()`
   returns the transaction's read point when one is attached
   (`src/yb/client/session.cc:305-307`), so the redirect would overwrite the
   transaction's data snapshot on a first read and serve the catalog at the
   transaction's start on later reads. The coverage gap is real and is now a
   design follow-up. REOPENED the same day: the user pointed out that catalog
   reads run under the catalog snapshot's own serial, and the code confirms
   that on every switch to that serial the tserver saves the transaction's
   read point into history under the transaction serial and restores it when
   that serial returns; the clamp already writes the clock time into the
   transaction's read point object during the excursion. Writing C there is
   the same write, undone the same way. Asked the implementer to remove the
   exclusion and prove it with a repeatable-read test (data snapshot fixed,
   write, locally served catalog miss, count unchanged). RESOLVED 2026-09-03:
   exclusion removed; `RepeatableReadDataSnapshotSurvivesLocalCatalogRead`
   shows the count fixed across the locally served miss and changed only after
   COMMIT; `CatalogMissInTransactionBlockAfterAWrite` now asserts local
   serving under both isolation levels.
2. The uncommitted `ReadWaitsForAnUnappliedVersion` test pauses one tserver's
   poller and expects a DDL elsewhere to return; the gate on that tserver
   blocks the DDL instead. Use serving-disabled during the DDL, then
   re-enable with the poller still paused.
3. Commit the request-level version (`backend_catalog_version`) plumbing: it
   is what makes the bootstrapping-to-serving transition safe for cache-miss
   reads, which carry no per-op version. Done. Justification recorded in
   `plan2.md` 5.3: versions published ungated while not serving, followed by a
   first applied response whose safe time is below their read time.
4. Own-writes floor was set after every DDL, costing the DDL session one poll
   on its next catalog read. Done without a PG change: the tserver records the
   `backend_catalog_version` each catalog request carries, snapshots it at
   commit, and skips the floor when the version has moved past that snapshot
   (a version-incrementing DDL); when no prior version was seen it keeps the
   floor. Tests `TemporaryRelation` (strengthened, asserts the new counter
   `local_catalog_reads_waited_for_own_writes`) and
   `VersionIncrementingDdlDoesNotPayTheOwnWritesFloor`.
5. `TemporaryRelation` test does not force a catalog re-read (`DISCARD PLANS`
   drops plans, not relcache); add a second no-increment DDL and a
   `pg_attribute` read.
6. Version and safe-time wait timeouts fail the request; route to master.
7. Served path skips the history-retention pin refresh at the end of
   `DoPerform`.
8. Master's per-tserver stream map is never pruned.
9. Confirmed: a garbage-collected position surfaces as `CHECKPOINT_TOO_OLD`
   through the xCluster log-cache read, so the injected re-seed path equals
   the real one.
10. Java regress with the flag on not yet run (still open). The other half of
    item 10 produced the number the suite lacked: phase 7 test 1 asks for
    the served counter to grow AND master's catalog read count to stay flat,
    and only the first half had been asserted. New test
    `MasterCatalogReadsDropWhenServing` counts master's tablet-service read
    handler total around five cold-backend iterations of one query, with the
    copy serving and with it disabled: measured 2 reads versus 40. An exact
    zero is not achievable and the test does not claim it: a fresh
    connection's authentication and catalog preload run on the legacy catalog
    session, which the legacy-mode predicate keeps off the copy. Those 2
    reads are the preload traffic the addendum already places out of scope;
    bringing the count to zero is preload work, not redirect work.
11. Per-op mutex on `applied_version` (known rough edge).

Status after the review round (2026-09-03): 26 of 26 tests pass on the
three-tserver external cluster with all fixes in (27 with the new master-read
test, re-run in progress); lint clean after registering the new
`YbcPgLocalCatalogVersion` typedef in `src/postgres/src/tools/pgindent/yb_typedefs.list`
(the `missing_yb_typedef` check requires it for any new `Ybc*` typedef);
D57801 updated with the full diff and Jenkins re-triggered.

Verified as matching the plan: publish-after-apply, monotonic C and
checkpoint, single-time reads, heartbeat skip, lease floor, first-read
replacement of the clock time, aborted-subtransaction override, upgrade
guard. Also noted: the commit flips `ysql_yb_ddl_transaction_block_enabled`,
`enable_object_locking_for_table_locks` and `ysql_enable_concurrent_ddl` to
default on in every build flavor, on the user's instruction; this changes the
mode existing debug and fastdebug tests run in.

## Status of the default-on CI runs (2026-09-14)

Details of every test, its failure message and its explanation are in `notes/worklog2.md`; this
section carries only the counts and what has to be done.

| Launch | Diff | Base | Unique failed items |
|---|---|---|---|
| 169341 | D58090 / 315855 | `748104ff13` | 201 |
| 169670 | D58090 / 315957 | `34f97f9f63f6` | 23 |

The two launches do not share a base commit, so the drop from 201 to 23 is not attributable to the
fix commits alone and the part of it that is base drift has not been separated.

Launch 169670's 23, by cause:

| Count | Cause | State |
|---:|---|---|
| 15 | The version token does not represent every catalog write (pattern 1 below) | understood, two TODOs below |
| 2 | A test's hardcoded catalog-version propagation deadline (pattern 3 below) | understood, TODO below |
| 1 | A tablet-level test flag reaches the copy's tablet (pattern 2 below) | understood, TODO below |
| 2 | Not ours: they fail identically with `enable_local_tserver_catalog=false` | closed |
| 3 | Open | see below |

The three patterns, each of which produced more than one failure and none of which is a one-off:

1. **The catalog version a backend reports is the only ordering token between a catalog writer and
   a catalog reader, and it does not represent every catalog write.** Both guards in
   `LocalCatalogCompleteTime` read that token: the version wait
   (`src/yb/tserver/pg_client_session.cc:3563-3587`) and the own-write floor
   (`:3598-3617`, recorded only under `is_ddl_mode` at `:4594-4604`), as does the exposure gate
   (`src/yb/tserver/heartbeater.cc:560-573`). A write escapes the token either by incrementing no
   version, or by raising the version row with a raw `UPDATE pg_yb_catalog_version`, which writes
   the row but advances no backend's `yb_catalog_cache_version` -- that happens only on a DDL commit
   (`src/postgres/src/backend/utils/misc/pg_yb_utils.c:3044`, `:3443`) or on a read of shared
   memory. Underneath both, the copy's complete time C is a step function that moves only when a
   poll lands, so the exposure is a window of one `local_catalog_poll_interval_ms` (default 100 ms);
   83 ms of it was measured directly.
2. **The copy is a tablet on every tserver, so anything written for "every tablet" now also applies
   to it.** Two instances so far: `conflict_resolve_keys_verification-itest` in launch 169341, where
   the copy's writes landed in the golden `TEST_file_to_dump_docdb_writes` file and which was fixed
   at `src/yb/tablet/tablet.cc:2238`, and `PgLibPqTest.ReplayDeletedTableInColocatedDBPostUpgrade`
   in launch 169670.
3. **The exposure gate lengthens catalog version propagation by about one poll interval plus one
   heartbeat interval, roughly 1.1 s,** so any test that bounds propagation with a fixed sleep is at
   risk. Two instances so far.

The production surface of pattern 1 is small and was audited. Catalog write sites outside a DDL
commit: `src/postgres/src/backend/catalog/yb_catalog/yb_catalog_version.c:209`, `:344`, `:759` and
`:866`, which are the version mechanism itself; `YBCExecuteUpdateLoginAttempts`
(`src/postgres/src/backend/executor/ybModifyTable.c:1310`); `yb_reset_analyze_statistics`
(`src/postgres/src/backend/catalog/yb_system_functions.sql:20-92`); and
`ybInsertPendingNotifiesToTable` (`src/postgres/src/backend/commands/async.c:2979`), which writes
the notifications table in the system database rather than a catalog the copy holds. Launch 169670
hit two of those. The rest of pattern 1's failures come through the
`yb_non_ddl_txn_for_sys_tables_allowed` escape hatch, which is a test-only surface.

Still open, all three narrowed but not closed:

- `TestPgSequencesWithServerCacheFlag#testCacheFlagValueHigherThanCacheOption` and
  `#testLowerThanDefaultCacheFlagValue`. The whole class runs clean locally with the copy on, 53
  tests and 0 failures in 657 s, and the launch needed 5 and 6 attempts, so these are timing
  failures under CI load. The window is not identified and no copy-off control has been run.
- `BackupUpgradeTest.TestRestoreBackupAfterRollback`. A 631 s harness timeout inside
  `yb::ExternalYbController::RunBackupCommand()`; the SIGSEGV in the log is `run-with-timeout`
  killing the child, not a crash of ours. Not run locally, because it needs yb-controller.

## TODOs after the core plan

- Lease loss and regain: complete phase 6 semantics, including master-side
  lease grant time in the refresh response and the poll-blocking rule.
- Master WAL retention: retention policy for the per-tserver streams, the
  4 h staleness eviction interplay, automatic re-seed on eviction.
- Load study: standalone poll client. Write a small binary that opens N
  change streams on the master system catalog tablet and polls each at the
  production cadence, independent of any tserver, and run it with N = 1000
  against a real cluster while DDLs run. Pointers for the client:
  `GetChanges` usage in `src/yb/integration-tests/cdc_service-int-test.cc`
  and `src/yb/integration-tests/cdcsdk_ysql_test_base.cc`; stream creation
  through `CatalogManager::CreateCDCStream`. Measure master CPU, change-stream
  RPC latency, and status-resolution RPC counts to coordinators; decide
  whether to share resolution per source tablet (producer keeps
  `last_apply_safe_time_` per stream today, `src/yb/cdc/xcluster_producer.cc:347-359`).
- Filter non-PG rows of the system catalog out of the stream and the snapshot
  if space matters.
- TODO (2026-09-14): make the copy refuse a catalog snapshot whose session raised the version row
  by hand. A raw `UPDATE pg_yb_catalog_version` writes the row but advances no backend's
  `yb_catalog_cache_version`, so the version wait
  (`src/yb/tserver/pg_client_session.cc:3563-3587`) is satisfied at the old number and the read is
  served below the write. The same session's own statement and any backend that connects afterwards
  are both exposed. Covering it through the existing GUC refusal is probably enough, because every
  site that raises the row by hand also sets `yb_non_ddl_txn_for_sys_tables_allowed`
  (`src/postgres/src/backend/catalog/yb_system_functions.sql:92`,
  `src/postgres/src/test/regress/sql/yb.orig.planner_base_scans_cost_model.sql:24`), but that has
  not been checked against every caller. Launch 169670 items covered: `TestPgRegressPlanner`,
  `TestPgRegressResetAnalyze`, `TestPgCostModelSeekNextEstimation` x2.
- TODO (2026-09-14): keep tablet-level test flags and tablet-level global assertions off the copy's
  tablet. `TEST_invalidate_last_change_metadata_op` is set on every tserver by
  `src/yb/yql/pgwrapper/pg_libpq-test.cc:1840` to simulate an upgrade from a release that never
  recorded the last change metadata op id; the copy's metadata takes that branch
  (`src/yb/tablet/tablet_metadata.cc:1187-1191`), its apply marker becomes `-1.-1`, the
  `kChangeMetadata` that adds the system catalog tables raises the flush marker to `1.581`, the
  check at `src/yb/tablet/tablet_metadata.cc:1355-1359` fails, and
  `src/yb/tablet/operations/operation_driver.cc:425` turns that into a glog FATAL that kills the
  tserver. Reproduces deterministically. This is the second instance of the pattern; the first was
  fixed at `src/yb/tablet/tablet.cc:2238`. Worth a general sweep rather than a second point fix.
- TODO (2026-09-14): fix the two tests whose catalog version propagation deadline the exposure gate
  now exceeds. `WaitForCatalogVersionToPropagate`
  (`src/yb/yql/pgwrapper/libpq_test_base.cc:417-424`) is a flat 2 s sleep whose own comment already
  says it may return too early; make it wait on the condition.
  `PgCatalogVersionTest.IncrementAllDBCatalogVersions` and `PgBackendsTest.MultipleWaiters` are the
  two, and the second passes locally in one run, so it is load sensitivity rather than a fixed cost.
- TODO (2026-09-14, decided by the user): send every read of the role profile catalogs to master
  and never to the copy. The place to do it is `AllOpsAreReadsOfLocalCatalogTables`
  (`src/yb/tserver/local_catalog_read.cc:35-56`), which already rejects a request per table id by
  looking each op's `table_id` up in the copy's metadata; add `pg_yb_role_profile` and
  `pg_yb_profile` to that rejection. The cost is one extra master read per authentication, on a path
  that already makes several. The alternative considered and not taken was to have the counter write
  increment the catalog version: that would make both existing guards fire and needs no new
  mechanism, but `pg_yb_role_profile` is `BKI_SHARED_RELATION`
  (`src/postgres/src/include/catalog/pg_yb_role_profile.h:28`), so the bump would have to be global
  across every database, and it would fire on every failed login, turning a password-guessing burst
  into a burst of global bumps that invalidate every backend's catalog cache on every tserver;
  `YbResetFailedAttemptsIfAllowed` would add bumps on successful logins whenever the counter was
  non-zero. Launch 169670 items covered: `TestYbRoleProfile` x4 and
  `YsqlRoleProfileDuringMajorUpgradeTest.RoleProfileWritesDisabled`, plus
  `TestYbRoleProfile#testAdminCanLockAndUnlock[0]`, which the launch did not list but which fails
  locally. For the record, the counter write increments nothing today: there is no version-increment
  call in `src/postgres/src/backend/commands/yb_profile.c`, in `YBCExecuteUpdateLoginAttempts`
  (`src/postgres/src/backend/executor/ybModifyTable.c:1298-1367`) or in
  `src/postgres/src/backend/libpq/auth.c`; `pg_yb_role_profile` has no syscache entry, so PG's
  invalidation machinery, which is what drives a version bump, never fires for it; and the write is
  a `YB_SINGLE_SHARD_TRANSACTION` issued outside DDL mode.
- TODO (2026-09-14): take the copy out of the read path for a session that has
  `yb_non_ddl_txn_for_sys_tables_allowed` set, and keep it out until that
  session's writes are provably in the copy. A DML run under that GUC writes
  system catalog rows without a DDL commit and without a catalog version
  increment, so neither guard in `LocalCatalogCompleteTime` fires: the backend's
  `backend_catalog_version` does not move
  (`src/yb/tserver/pg_client_session.cc:3563-3587`), and
  `own_catalog_write_floor_` is stored only under `is_ddl_mode`
  (`src/yb/tserver/pg_client_session.cc:4594-4604`). The session's next catalog
  read is then answered from the copy at C, below its own write, and returns the
  pre-write value. `TryServeCatalogReadsLocally` already refuses to serve a
  request that itself carries the GUC
  (`src/yb/tserver/pg_client_session.cc:3704-3711`); what is missing is that the
  refusal does not persist past the statement that set it. Confirmed by
  `TestYsqlUpgrade#updatePgNodeTreeType`, which writes `pg_proc.proargdefaults`
  and reads back the value initdb wrote. Launch 169670 items covered:
  `TestYsqlUpgrade` x5, `TestPgUpdatePrimaryKey#basicSystemTables`,
  `TestPgCostModelSeekNextEstimation` x2.
- Eager apply: carry the DDL's own WAL records in the lock-release message to
  skip the poll round trip on the DDL path.
- Change records in the lock release: promoted to Phase 8 below (plan2.md
  section 12).
- TODO (2026-09-05): a lease-loss test worth having. The test that existed,
  `LeaseLossTakesTheCopyOutOfTheReadPath`, was removed rather than repaired. It
  disabled lease refresh on one tserver and asserted that the copy left the read
  path, but lease expiry kills that tserver's PG sessions
  (`YSQLLeaseManager::Impl::CheckLeaseStatusInner`, which calls `OnLeaseLost()`
  and then `KillPg()`, `src/yb/tserver/ysql_lease_manager.cc:296-302`), so with
  no session left to read from the copy the assertion barely observed anything a
  client could see. It was also the suite's only flaky test: on a 4 CPU host
  running several clusters it timed out waiting 120 s for the transition, with
  identical results whether or not Phase 8's push was enabled, so the delay is
  in the detection of the lapse rather than in anything the copy does.
  What is worth testing instead is the rejoin: a tserver partitioned from master
  long enough to lose its lease and to miss a DDL committed while it was away,
  then reconnecting, and its copy withholding reads until it can prove it holds
  what it missed -- reads going to master in the meantime, and the copy
  rejoining the read path only once the poller has applied a response at or
  after the new lease grant. Two things that test needs and the removed one did
  not have: a way to keep the tserver from master without also killing its
  ability to reconnect afterwards, and an assertion on where reads went during
  the window rather than on a state counter.
  Removing it also let the fixture's short lease settings go
  (`master_ysql_operation_lease_ttl_ms=5000`,
  `ysql_lease_refresher_interval_ms=500`), which existed only for that test and
  made every other test in the file more sensitive to load.

## Phase 8. Change records carried in the lock release (plan2.md section 12)

Pointers are into repo6 at the working tree of 2026-09-05.

0. Wire: `CDCRecordPB` carried no WAL position (fields 1-15), which the
   per-target slice and the tserver's gap check both need. Added
   `optional OpIdPB op_id = 16`, stamped in the xCluster change loop only when
   the stream is the sys catalog change stream, so xCluster's wire size is
   unchanged. All records of one message share its position, so slicing and
   the drop rule operate per message; the batch end for publishing S is the
   response checkpoint, not the last record's op id (they differ when the
   tail of the range has no row records). Decided 2026-09-05 over the
   alternative of one change-service call per distinct position, which
   degenerates to one call per tserver per DDL.
1. Heartbeat: add the copy's applied op id (`OpIdPB local_catalog_applied_op_id`)
   to `TSHeartbeatRequestPB` (`src/yb/master/master_heartbeat.proto`; the
   request's next free number was 25, used 25; 34/35 are response fields).
   The poller's `checkpoint_` is poll-thread state, so the poller publishes it
   into `LocalCatalogReplica` under the replica's mutex whenever it moves and
   on load, and the heartbeater reads it there (unset when there is no copy).
   `TSDescriptor::UpdateFromHeartbeat` stores it with the heartbeat timestamp;
   `LocalCatalogAppliedOpId()` returns both so master can tell a stale report.
2. Master, release fan-out (`src/yb/master/object_lock_info_manager.cc`):
   targets are `GetAllTSDescriptorsWithALiveLease()` (:1685), the read time
   is set in `PopulateDbCatalogVersionCache` (:1089-1110). After the read
   time is set: take the minimum reported op id over targets with a fresh
   report; call the change service in-process
   (`src/yb/cdc/sys_catalog_change_service.cc`, the same entry the poll RPC
   uses) once from that op id; keep the response's records, end checkpoint
   and `safe_hybrid_time` (S). Per target, when the report is fresh and the
   slice above its op id fits the payload cap (new flag
   `local_catalog_release_push_max_bytes`), attach the slice, end op id and
   S to that target's `ReleaseObjectLockRequestPB` (new fields after 15,
   `src/yb/tserver/tserver.proto:522-570`); otherwise attach nothing. The
   per-target request already exists because the loop at :1703 sends one RPC
   per descriptor.
3. Tserver, release handler (`src/yb/tserver/ts_local_lock_manager.cc:533-557`):
   before the gate wait, if the request carries records, hand them to the
   poller (`LocalCatalogPoller::ApplyPushedBatch`, new: enqueue on the
   poller thread and wake it, then wait for completion with the release
   deadline). The apply must reuse `ApplyRecords` and the checkpoint and C
   publication of `FetchAndApplyOnce` (`local_catalog_poller.cc`, the block
   around the `ApplyRecords` call and `safe_hybrid_time` handling). Refuse
   when the first record's index is not `checkpoint_.index + 1`; compare
   indexes only, never terms: the checkpoint is a position in master's WAL,
   and after the copy's own election its log carries a term that is not
   master's, which is why the poller's checkpoint comparison is index-based
   (defect fixed 2026-09-05). On refusal or any error,
   `RequestImmediatePoll()` and fall through to the existing gate wait.
   Publish S only after the full batch is applied.
4. Counters: pushes received, pushes applied, pushes refused (gap), pushes
   skipped by master (no report, over cap), bytes pushed; gate wait
   histogram already exists (`local_catalog_gate_wait_us`).
5. Flags: `enable_local_catalog_release_push` (default off until tests pass),
   payload cap, report staleness bound.

Tests:
- Push path: DDL on T1; assert the gate wait on T2 and T3 is near zero and
  `pushes_applied` grew on each; a cold backend on T3 reads the new table
  locally right after the DDL returns; poll counter unchanged during the DDL.
- Gap: pause T2's poller, run two DDLs, drop the first push with a test flag;
  the second push is refused on T2, `pushes_refused` grows, an immediate pull
  follows, and the release still completes.
- Over cap: set the cap to a few bytes; master attaches nothing, the counter
  for skipped pushes grows, behavior equals today's.
- Stale report: stop T3's heartbeats with a test flag beyond the staleness
  bound; master attaches nothing for T3, T3 pulls, DDL completes.
- New master leader: step down master leader, run a DDL at once; the release
  carries no records for targets that have not heartbeated yet, the DDL still
  completes, and the next DDL after heartbeats carries records again.
- Concurrent DDLs: ten pairs from different tservers; every copy converges,
  no refusal storms (`pushes_refused` stays small).
- Equivalence: the whole existing suite with the push flag on.

### Phase 8 review (2026-09-05, commit 652ce82b0fb)

Design matches plan2 section 12 except item 3. Sent to the implementer:

1. Use-after-free in the push handoff: the release handler's waiter times out
   and withdraws the stack-allocated pending push while the poll thread is
   still applying it and will write its result into it. Fix: shared ownership
   of the pending push; the poll thread empties the slot when it takes the
   batch. Plus a busy spin once the deadline has passed.
2. Contiguity check too strict: it requires the first retained record's index
   to be applied + 1, but record-less master WAL ops (history cutoff, no-op,
   transaction status heartbeats) sit between records, so legitimate slices
   are refused and fall back to the poll; empty slices whose S could be
   published are refused by the tserver and never sent by master. Fix: master
   sends the position it sliced from; the tserver requires only that this
   position is at or below its applied index.
3. Deviation: no lag cap on the lowest reported position; one straggling
   copy with a fresh heartbeat makes the shared batch truncate without a safe
   time and master then pushes nothing to anyone. Fix per plan2 12.2: cap by
   distance from the highest reported index.
4. A push accepted just before a re-seed waits until the release deadline.
5. Add a master histogram for the in-process fetch duration.
6. Stale-report test's skipped-counter assertion cannot fail; count
   "no fresh report" as skipped.
Verified correct: transient field excluded from persistence and comparison;
index-only comparisons; S published only after the whole retained batch;
forced safe-time recompute only on master's own stream; op id stamping
guarded to the sys catalog stream; heartbeat request field 25; push applied
before the pause check and before any pull. Six tests present; 45/45 off,
44/45 on (the one failure fails identically with the push off).

Fixes verified 2026-09-05 in 88ad50da853 (amended): items 1-6 closed. Item 1
was worse than reported: the pending push also pointed at the release
handler's stack batch, so the apply itself read freed memory. Nine Phase 8
tests; suite 47/48 with the push off and on (the one failure is the
lease-loss timing test, identical both ways). ASAN run of the lifetime test
requested.

Finding from the lag-cap test: a copy that is still serving but whose poller
is stalled blocks every lock release at its gate for the full release
deadline (35 s per DDL observed, master retrying), because C never moves and
serving is never withdrawn. This is the plan 7.2 gap with a measured
cluster-wide consequence: one stalled poller stalls all DDLs. Decision for the
user: implement 7.2 (withdraw serving after N consecutive poll failures or
when C lags the tserver clock by more than a bound; reads go to master, the
gate is skipped, heartbeat publication resumes; serving resumes on the next
successful poll), or bound the gate wait and rely on the request-level
version wait for reads.

