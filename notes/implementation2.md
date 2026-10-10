# Implementation of the tserver-local catalog copy

What is in the code today, by component, with pointers. Branch
`docdb_local_catalog` at `cc0b13766e6` plus the uncommitted master-push change
of 2026-10-07 ([push-plan.md](push-plan.md)), worktree
`/net/dev-server-sanketh-3/share/worktrees/repo6`; line numbers drift, symbol
names are the stable pointers. The design is [plan2.md](plan2.md); every gap
between this code and that design is in [todos.md](todos.md); the
chronological record of how the code got here is [worklog2.md](worklog2.md).
Build and run tests with `./yb_build.sh fastdebug --clang21 --cxx-test <test>`.

## 1. Scope and switches

- `enable_local_tserver_catalog` (tserver, non-runtime,
  `local_catalog_replica.cc:44`): `true` under `NDEBUG`, `false` otherwise,
  the same condition as `enable_object_locking_for_table_locks` and
  `ysql_enable_concurrent_ddl`, which the copy requires.
  `LocalCatalogPrerequisitesMet()` refuses to create the copy without both
  (`tablet_server.cc`, warning logged), and master creates its pusher and
  registers its change service under the same condition (`master.cc`).
- The copy exists on every tserver as tablet `kLocalCatalogTabletId`
  (`00000000000000000000000000000001`, `common/constants.h:48`), a private
  single-voter copy of master's `kSysCatalogTabletId`.
- New files: `src/yb/master/sys_catalog_pusher.{h,cc}` (pusher and change
  service), `src/yb/master/sys_catalog_pusher-test.cc`,
  `src/yb/tserver/local_catalog_{replica,applier,read}.{h,cc}`,
  `src/yb/yql/pgwrapper/pg_local_catalog-test.cc`,
  `src/yb/yql/pgwrapper/pg_local_catalog_load-test.cc`. The pull service
  (`cdc/sys_catalog_change_service.{h,cc}`, `GetSysCatalogChanges`) and the
  tserver poller (`local_catalog_poller.{h,cc}`) are gone.

Component map:

| Component | Files | plan2 |
|---|---|---|
| Master pusher | `master/sys_catalog_pusher.{h,cc}`, `cdc/cdc_service.proto` (`RequestSysCatalogPush`), `cdc/xcluster_producer.cc` (`PopulateXClusterRecords`), `cdc/cdc_producer.h`, `cdc/xrepl_stream_metadata.{h,cc}`, `consensus/{consensus_queue,raft_consensus}.{h,cc}` (`ReadCommittedMessages`), `master/master.{h,cc}` | [§4.1](plan2.md#41-master-push-service) |
| Replica state and waits | `tserver/local_catalog_replica.{h,cc}` | [§4.2](plan2.md#42-applier), [§7](plan2.md#7-failure-handling) |
| Bootstrap and tablet | `tserver/ts_tablet_manager.{h,cc}`, `tserver/remote_bootstrap_client.{h,cc}`, `tablet/tablet.cc` | [§6](plan2.md#6-bootstrap) |
| Applier | `tserver/local_catalog_applier.{h,cc}`, `tserver/tserver.proto` (`LocalCatalogCheckpointPB`, `LocalCatalogPagePB`, `PushSysCatalogChanges*PB`), `tserver/tserver_service.proto`, `tserver/tablet_service.cc` | [§4.2](plan2.md#42-applier) |
| Freshness gate | `tserver/ts_local_lock_manager.cc`, `tserver/heartbeater.cc`, `master/master_heartbeat.proto`, `master/master_heartbeat_service.cc`, `master/catalog_manager.cc`, `master/sys_catalog.cc`, `tserver/tserver.proto` | [§4.3](plan2.md#43-freshness-guarantee) |
| Pages in the release | `master/object_lock_info_manager.cc`, `master/sys_catalog_pusher.cc`, `master/ts_descriptor.{h,cc}`, `tserver/ts_local_lock_manager.cc`, `tserver/tablet_service.cc` | [§4.4](plan2.md#44-pages-carried-in-the-lock-release) |
| Read routing | `tserver/pg_client_session.{h,cc}`, `tserver/local_catalog_read.{h,cc}`, `tserver/pg_client.proto`, `yql/pggate/pg_session.{h,cc}`, `yql/pggate/pg_txn_manager.cc`, `yql/pggate/ybc_pg_typedefs.h`, `postgres/.../pg_yb_utils.c` | [§5](plan2.md#5-reads) |
| Lease and upgrade hooks | `tserver/ysql_lease_manager.cc`, `tserver/heartbeater.cc`, `tserver/tablet_server.cc` (`NotePublishedVersions`) | [§7.1](plan2.md#71-lease-loss-lease-regain-and-tserver-restart), [§10](plan2.md#10-modes-in-which-the-local-tablet-is-not-used) |

## 2. Master pusher

`SysCatalogPusher` (`master/sys_catalog_pusher.{h,cc}`), owned by `Master`
(`sys_catalog_pusher_`, created in `RegisterServices`, started after the RPC
server, shut down first in `Master::Shutdown`). `SysCatalogChangeService`
(same file) is the RPC service with one method, `RequestSysCatalogPush`
(`cdc_service.proto`), registered on master's normal pool.

- Pages: `Page {from, end, records, num_records, safe_time, ends_at_restore,
  ends_with_metadata}`; `records` is a shared serialized
  `cdc::GetChangesResponsePB` holding only its records. `BuildPages`
  (`:277`) reads committed operations with
  `RaftConsensus::ReadCommittedMessages(after, to_index, page_bytes,
  deadline)` (new; log cache first, disk below it, `CHECKPOINT_TOO_OLD` when
  not retained), converts each with `cdc::PopulateXClusterRecords` (the
  per-operation body of `GetChangesForXCluster`, factored out so the two
  produce identical records, op ids stamped), appends the serialized records
  to the current page, and seals a page at `local_catalog_push_page_bytes`
  (1 MB), after a `CHANGE_METADATA` record, and around a
  `RESTORE_SYS_CATALOG` operation (a page of its own with no records).
  Writes with an external hybrid time are covered without a record, as the
  producer drops them.
- Cache: `pages_` (deque, contiguous), bounded by
  `local_catalog_push_cache_mb` (64), oldest dropped first, last page always
  kept; bytes charged to the `SysCatalogPusher` mem tracker. A term change
  (`ResetForTermUnlocked`) drops pages and every target's position. A new
  leader seeds the cache at `I_S - local_catalog_push_unknown_position_lookback_ops`
  (1000).
- Cycle: `RunCycle(floor, deadline)` (`:475`) coalesces callers (a caller
  waits for the first cycle that started after it asked);
  `DoCycle` (`:510`): `LeaderSafeTime` (a floor never moves master's clock; the scheduler
  repeats cycles until S passes it),
  `ResolveIntents`, `GetCommittedAndMajorityReplicatedIndex().second` as
  I_S, `BuildPages` up to I_S waiting for the commit index to reach it; the
  page ending at I_S gets S, or, with no new WAL, the last page is replaced
  by a copy carrying the new S.
- Targets: `targets_` keyed by uuid: `acked` (applied position), `floor`,
  `last_safe_time`, `in_flight`, `releases_in_flight`, `release_priority`,
  backoff. Positions come from push answers (`OnPushDone`), release answers
  (`OnReleaseDone`), floor requests (`RequestPush`) and heartbeats
  (`MergeHeartbeatUnlocked`, from `TSDescriptor::LocalCatalogAppliedOpId`,
  merged once per report). Live targets are
  `ObjectLockInfoManager::GetAllTSDescriptorsWithALiveLease()` plus, in
  tests, `TEST_local_catalog_push_extra_targets`.
- Scheduler thread (`SchedulerLoop`, `:1188`): runs a cycle when a floor is
  above the last S (at most once per
  `local_catalog_push_min_cycle_interval_ms`, 50) or on the timer
  (`local_catalog_push_interval_ms`, 300 000) when the WAL moved past the
  cache; then `Drain` (`:1047`). `Drain` rebuilds the waiting queue by a
  full scan of the targets when pages were added, on a request, and once a
  second; a completed push only re-queues its own target, so an
  acknowledgement costs the same at any cluster size. Order: targets a
  release or a floor waits on, then the furthest behind. Bounds:
  `local_catalog_push_max_in_flight_rpcs` (64),
  `local_catalog_push_max_in_flight_bytes` (64 MB),
  `local_catalog_push_release_reserved_rpcs` (16). Next pages per target
  (`NextPagesUnlocked`): from the cache up to one page size of pages; below
  the cache a transient page built from the WAL (`BuildTransientPage`,
  shared by targets at the same position, at most 64 kept); WAL not
  retained → a restore page, which tells the copy to re-seed. The RPC is
  `TabletServerService.PushSysCatalogChanges`, sent with the descriptor's
  proxy after `mutex_` is released (`PrepareSend`/`Send`), timeout
  `local_catalog_push_rpc_timeout_ms` (30 s); failures back off 100 ms
  doubling to 5 s.
- Metrics (master): `local_catalog_push_cycles`, `_push_rpcs_sent`,
  `_push_bytes`, `_push_pages_built`, `_push_transient_pages_built`,
  `_push_failures`, `_floor_requests_received`, `_release_inline_pages`,
  `_release_inline_skipped`, gauges `_push_in_flight_rpcs`,
  `_push_in_flight_rpcs_max`, `_push_in_flight_bytes_max`,
  `_push_cached_bytes`, event stats `local_catalog_push_cycle_us`.
- Unchanged producer additions (gated on `IsSysCatalogChangeStream()`):
  aborted-subtransaction sets always emitted; record op ids stamped.

## 3. Replica: state, C, A[db], waits

`LocalCatalogReplica` (`local_catalog_replica.{h,cc}`), owned by
`TabletServer` (`tablet_server.h:776`, created in `Init` when the
prerequisites hold, started in `Start`, shut down in
`StartShutdown`/`CompleteShutdown`).

- State `LocalCatalogServingState`: `kBootstrapping`, `kServing`,
  `kDisabledLease`, `kReseeding`. `IsServing()` (`:216-228`) is `kServing`
  and not `major_version_upgrade_in_progress_` and not
  `TEST_local_catalog_disable_serving`.
- `c_` (atomic hybrid time, lock-free on the read path) and
  `applied_version_` (map db oid → version, under `mutex_`).
  `PublishAfterApply(new_c, versions)` (`:251-287`) stores a safe time only
  if it is above the current one, takes the maximum per database, and ends
  with `cond_.notify_all()`.
- `OnSafeTimePublished(c)` (`:514`) moves the state to `kServing` once C ≥
  `lease_gap_floor_` (if set) and `applied_version_[db]` ≥
  `published_while_not_serving_[db]` for every database; otherwise it asks
  for a push (up to the floor, or up to now while a version is missing).
  `NotePublishedVersions` (`:494`), called from
  `TabletServer::SetYsqlDBCatalogVersionsUnlocked`, records the highest
  version published per database while the copy is not serving and drops
  databases a report no longer carries.
- Waits: `WaitForAppliedVersion(db, version, deadline)` (floor = clock now)
  and `WaitForSafeTime(target, deadline)` (floor = target) (`:344-387`) loop
  on `cond_`, calling `applier_->RequestPush(floor)` on every check that
  fails; `RequestPush` only raises the pending floor and wakes the apply
  thread, which sends one floor request at a time.
  `HoldsCatalogStateAt(target)` is the non-blocking variant for the
  heartbeat thread.
- Lease hooks: `OnLeaseLost()` (`:402`) sets `lease_gap_floor_` to
  `HybridTime::kMax` and the state to `kDisabledLease`; `OnLeaseGained(t)`
  (`:413`) sets the floor to `t`, moves a serving copy whose C is below `t`
  to `kDisabledLease`, and asks for a push up to `t` when not serving.
  Called from `ysql_lease_manager.cc` on every new lease epoch including the
  first, with `clock()->MaxGlobalNow()`. The lease manager then holds PG's
  start (`ShouldHoldPgStart`: a copy exists and
  `local_catalog_serve_wait_timeout_ms` ≠ 0): `DeferredPgRestart` runs on its
  own thread, polls `IsServing()` every 50 ms until serving, timeout, a newer
  epoch, or lease loss, then calls `RestartPG()` and re-enables the shared
  object lock state.
- `SetMajorVersionUpgradeInProgress` from the heartbeat
  (`heartbeater.cc:550-552`, `TSHeartbeatResponsePB` field 35).
- `RequestReseed()` sets `reseed_requested_`; the applier consumes it with
  `TakeReseedRequest()`.
- `applied_op_id_` is the copy's applied WAL position, reported in every
  heartbeat request, every push answer and every release answer (`heartbeater.cc:391-399`, `TSHeartbeatRequestPB` field
  25) and read by master into `TSDescriptor::local_catalog_applied_op_id_`
  (`ts_descriptor.cc`, `LocalCatalogAppliedOpId()` returns it with the
  heartbeat time).

## 4. Bootstrap and the copy's tablet

`TSTabletManager` (`ts_tablet_manager.cc`), plan2 [§6](plan2.md#6-bootstrap).

- `OpenOrCreateLocalCatalogTablet(local_id, source_id, source_uuid,
  source_addr, force_reseed)` (`:2620-2684`): if the tablet's metadata opens
  and its data state is servable and no re-seed was forced, reuse it
  (`RegisterDataAndWalDir` and a new `TabletPeer`); otherwise delete the
  old data and fetch master's tablet with `RemoteBootstrapClient` set to
  `SetPrivateCopyOfSourceTablet(source_id, local_peer_pb_)`
  (`remote_bootstrap_client.cc`): the session is opened on the source id, the
  fetched superblock is renamed to the local id (`raft_group_id`,
  `kv_store_id`), and the consensus metadata is written as a single-voter
  config of this tserver (`committed_op_index = kInvalidOpIdIndex`, the
  source's current term). `CanReuseLocalCatalogTablet` (`:2612`) is the
  reuse predicate the applier asks first, so a restart contacts master only
  when it must fetch.
- `OpenTabletPeer(meta, peer, deleter, private_copy = true)` (`:1463-1510`):
  `bootstrap_retryable_requests = false` (otherwise bootstrap replays
  master's entire initdb WAL instead of starting at the flushed op id);
  `IsSysCatalogTablet(true)`; the single-voter config is rewritten on every
  open; the peer is not registered with the CDC service, not considered for
  load-balancer moves, not scheduled for metadata validation; its
  mark-dirty callback is `IgnoreLocalCatalogTabletStateChange` (consensus
  invokes it at start; an empty callback crashes). The copy is kept out of
  `tablet_map_` and `ListTablets` skips its id (`:783`), so it never
  appears in a tablet report and master cannot delete it as an orphan
  (verified: master deletes an unknown reported tablet only when its id is
  in the list of previously deleted tablets, and a generated id is a v4 UUID
  whose version nibble cannot match the copy's id).
- `MakeLocalCatalogTabletLeader` (`:2685`): starts an election if the peer
  is not leader. Reads use `RequireLease::kFalse` (`local_catalog_read.cc`),
  so they do not wait `leader_lease_duration_ms` after a restart.
- `DeleteLocalCatalogTablet` (`:2707`): shuts the peer down and deletes the
  data; used by the re-seed path.
- History retention: `AllowedHistoryCutoff` branches on the copy's id
  (`:4015`) to `LocalCatalogAllowedHistoryCutoff()` (`:4004`), which returns
  `Now() - timestamp_syscatalog_history_retention_interval_sec` (4 h, the
  interval master applies to the same data) in both cutoff fields and skips
  the namespace, xCluster and snapshot-schedule lookups. The committed
  cutoff is seeded from the SST frontiers copied from master.
- `tablet.cc:2238`: `TEST_file_to_dump_docdb_writes` skips the copy.
- Not done for the copy: scheduled full compactions and admin compactions
  (both enumerate `tablet_map_`); the global memstore flusher
  ([todos T23](todos.md#221-t23-memstore-flusher)). Automatic RocksDB
  compactions run. WAL GC runs (log GC op registered through
  `RegisterMaintenanceOps`; xCluster retention hook skipped).

## 5. Applier

`LocalCatalogApplier` (`local_catalog_applier.{h,cc}`, formerly
`LocalCatalogPoller`), a `MasterLeaderPollerInterface` driven by
`MasterLeaderPollScheduler`: one thread; `TriggerASAP` on queued pages and on
`RequestPush`; otherwise it runs once per
`local_catalog_floor_request_interval_ms` (1000) to repeat an open floor
request and persist the checkpoint. Plan2 [§4.2](plan2.md#42-applier).

- `Poll()` (`:197`): `ApplyPendingPushes()`, `EnsureLocalTablet()`, wait for
  leadership of the copy's own group, `MaybeSendFloorRequest()`,
  `PersistCheckpoint(false)`.
- `EnsureLocalTablet()` (`:350`): consumes a re-seed request (state
  `kReseeding`, `OnReseedStarted`, `DeleteLocalCatalogTablet`, delete the
  checkpoint file, reset positions); otherwise state `kBootstrapping`. If
  the tablet is not reusable, resolves the master leader, waits for
  `IsInitDbDone` (returns `TryAgain` until then), gets the leader's uuid,
  and calls `OpenOrCreateLocalCatalogTablet`. Then `LoadCheckpoint()`,
  `MakeLocalCatalogTabletLeader`, and `RequestPush(clock now)` so master
  learns the position and pushes a first S.
- Checkpoint file: `LocalCatalogCheckpointPB` (`tserver.proto`), written by
  `PersistCheckpoint(force)` at most once per
  `local_catalog_checkpoint_flush_interval_ms` (1000) when the position
  changed, and at shutdown, after `log()->WaitUntilAllFlushed()`
  ([todos T7](todos.md#27-t7-checkpoint-durable-before-the-wal)). Outside the
  tablet metadata directory. A fresh copy starts at the copied log's latest
  op id.
- Push queue: `ApplyPushedPages(pages, deadline, wait)` (`:472`) puts a
  `PendingPush` into `pending_pushes_`, a multimap keyed by the first page's
  `page_from` index, and waits (or not, for a release to a copy that is not
  serving); a timed-out waiter withdraws its push if the apply thread has
  not taken it. `ApplyPendingPushes` (`:517`) takes the lowest first.
- `DoApplyPages` (`:551`): refuses pages from a master term below the
  highest applied; per page: `page_from` above the position → `gap`, stop;
  `reseed_required` with an end above the position → `RequestReseed`, error;
  end above the position → parse `records`, drop records at or below the
  position (`RecordsAbove`), `ApplyRecords`, `AdvanceTo(end)`; a page with
  `safe_time` whose end is at or below the position after the apply makes
  that S a candidate. The highest candidate is published by
  `PublishSafeTime` (`:446`): `ReadLocalCatalogVersions` at S,
  `PublishAfterApply`, `RecordLag`, `OnSafeTimePublished`.
- `ApplyRecords(changes, stats)` (`:635`): unchanged apply through an
  `XClusterWriteInterface` (external intents keyed by transaction id, apply
  records as apply-external-transaction entries, non-transactional writes
  at their own time), written with `WriteBatchToLocalTablet`; a
  `CHANGE_METADATA` record (always the last of its page) is applied as a
  `ChangeMetadataOperation` with the tablet id rewritten.
- Floor requests: `MaybeSendFloorRequest` (`:212`) sends
  `RequestSysCatalogPush{uuid, applied position, floor}` asynchronously, one
  at a time, repeated after `local_catalog_floor_request_interval_ms` for
  the same floor or at once for a higher one; a failed request resets the
  proxy so the next resolves the master leader again.
- Push RPC: `TabletServiceImpl::PushSysCatalogChanges` →
  `LocalCatalogReplica::HandlePush` (checks `target_uuid`, waits for the
  outcome with the RPC deadline, answers `applied_op_id`, `safe_time`,
  `gap`).
- Flags: `local_catalog_master_rpc_timeout_ms` (30000),
  `local_catalog_floor_request_interval_ms` (1000),
  `local_catalog_checkpoint_flush_interval_ms` (1000),
  `local_catalog_apply_write_timeout_ms` (60000),
  `local_catalog_bootstrap_retry_delay_ms` (1000). Test flags:
  `TEST_local_catalog_pause_applier`, `TEST_local_catalog_fail_push`,
  `TEST_local_catalog_push_reseed`,
  `TEST_local_catalog_pushed_batch_apply_delay_ms`.

## 6. Freshness gate

Plan2 [§4.3](plan2.md#43-freshness-guarantee) specifies a version comparison
on both paths; the code compares times, which is correct but waits longer
([todos T1](todos.md#21-t1-publish-gate-on-applied-versions)).

- Master threads the read time of its version read out of
  `SysCatalogTable::ReadWithRestarts` (`sys_catalog.cc`, new
  `used_read_time` out-parameter) through `ReadYsqlAllDBCatalogVersions`
  and `CatalogManager::GetYsqlAllDBCatalogVersions(use_cache, ...)`
  (`catalog_manager.cc:11140-11167`; with the cache the time is
  `heartbeat_pg_catalog_versions_cache_read_time_`, set at `:14389` on each
  refresh) into `ReleaseObjectLockRequestPB.catalog_versions_read_time`
  (field 15, `tserver.proto`) and
  `TSHeartbeatResponsePB.db_catalog_versions_read_time` (field 36,
  `master_heartbeat_service.cc:358-360`, sent only when the tserver's
  fingerprint differs).
- Release handler `TSLocalLockManager::Impl::ReleaseObjectLocks`
  (`ts_local_lock_manager.cc`): after `WaitToApplyIfNecessary`, if the copy
  has a tablet and the request carries pages, apply them (§7); if serving
  and a read time is present, `WaitForSafeTime(read_time,
  deadline)` (`:577-590`, recorded in `gate_wait_us`; a timeout fails the
  RPC so master retries); then `SetYsqlDBCatalogVersions*` (`:598-606`);
  then the local unlock (`:618-623`). The handler is synchronous, so the
  acknowledgement waits. While not serving the versions are published at
  once.
- Heartbeat (`heartbeater.cc`): when the response carries version data and
  `LocalCatalogCopyHoldsHeartbeatVersions()` is false, the report is held
  (`held_version_report_`, the first one held is kept) and a push up to its
  read time is requested; each later heartbeat first publishes the held
  report once the copy holds its read time. Without the hold, each
  heartbeat's report was read later than the C the previous push reached,
  and with push nothing else moves C, so every report was skipped. Master
  keeps re-sending reports, because the tserver's fingerprint stays
  unchanged until one is published. Single versions are never lowered
  (`SetYsqlDBCatalogVersionsUnlocked`, `tablet_server.cc`, treats a lower
  version as a stale report and FATALs after
  `ysql_stale_catalog_version_min_seconds`; a missing database is deleted
  from the map).
- The DDL's own tserver: `YBCommitTransactionContainingDDL`
  (`pg_yb_utils.c:3179`) calls `YBCPgExitSeparateDdlTxnMode` →
  `FinishTransaction(ddl_mode)` → `DdlAtomicityFinishTransaction` →
  `ReleaseObjectLocksIfNecessary(..., kSync)` (`pg_client_session.cc:3271-3296`),
  and only after that returns does
  `YbCheckNewSharedCatalogVersionOptimization` (`pg_yb_utils.c:3437`) write V
  into shared memory through `YBCPgSetTserverCatalogMessageList`.

## 7. Pages in the lock release

Always on with the feature. Plan2 [§4.4](plan2.md#44-pages-carried-in-the-lock-release).

- `UpdateAllTServers<Req>::PrepareLocalCatalogPushes()`
  (`object_lock_info_manager.cc:1727`), run from `LaunchRpcs` for release
  requests that carry versions and a read time, calls
  `SysCatalogPusher::PrepareRelease(target uuids, deadline)` (`:690`): one
  `RunCycle` (S after the DDL's commit, the DDL's pages built once), then
  per target the cached pages from its position to the cache's end
  (`CachedPagesAfterUnlocked`); when they reach the end and total at most
  `local_catalog_release_inline_max_bytes` (4 MB) they are serialized as a
  `PushSysCatalogChangesRequestPB` into `local_catalog_pushes_`; otherwise
  the target is marked `release_priority` and pushed by the scheduler. A
  target at the end with an S at or above the cycle's gets nothing.
- `UpdateTServer::request()` attaches the payload as
  `ReleaseObjectLockRequestPB.local_catalog_changes` (field 16, now a
  serialized `PushSysCatalogChangesRequestPB`, never persisted).
  `UpdateTServer::Finished` calls `LocalCatalogReleaseDone` →
  `SysCatalogPusher::OnReleaseDone` (`:750`) with the last answer, whose new
  fields `local_catalog_applied_op_id` (3) and `local_catalog_safe_time` (4)
  are filled by `TabletServiceImpl::ReleaseObjectLocks`.
- Tserver (`ts_local_lock_manager.cc:555`): parses the pages and calls
  `ApplyPushedPages(pages, deadline, wait = IsServing())`; a refused, gapped
  or failed apply counts `pushes_refused` and leaves the gate to wait, and
  the gate's `WaitForSafeTime` sends a floor request with the copy's
  position. While a release carrying pages is in flight to a target, the
  scheduler does not push to it unless that target sent a floor request.
- Metrics: master `local_catalog_release_inline_pages`,
  `local_catalog_release_inline_skipped`; tserver
  `local_catalog_pushes_received/applied/refused`.

## 8. Read routing

Plan2 [§5](plan2.md#5-reads). All in `PgClientSessionImpl`
(`pg_client_session.cc`) unless noted.

### 8.1 What the request carries

- `ReadTimeOptionsPB.is_catalog_snapshot` (field 9, `pg_client.proto`): set
  by pggate's `SetupReadTimeOptions` (`pg_txn_manager.cc`) when the op group
  has catalog ops and the legacy mode is off (`pg_session.cc:971-972`).
- `ReadTimeOptionsPB.backend_catalog_version` and
  `backend_catalog_version_db_oid` (fields 10, 11): the version the backend
  believes it is at, from `YbGetLocalCatalogVersion` (`pg_yb_utils.c`,
  `yb_catalog_cache_version` and `MyDatabaseId`, invalid oid outside
  per-database mode) through the new callback `GetLocalCatalogVersion`
  (`ybc_pg_typedefs.h`, `YbcPgLocalCatalogVersion`), set by
  `PgSession::SetBackendCatalogVersion` (`pg_session.cc`) on catalog-snapshot
  requests and on the legacy catalog session's requests. Separate from the
  per-op `ysql_db_catalog_version`, which PG leaves unset on internal scans
  of system relations (`yb_scan_core.c:3701`) and which the tserver
  validates; this field is never validated.

### 8.2 How a catalog snapshot gets its time

The PG snapshot holds only a read-point serial; the hybrid time lives in the
tserver session keyed by that serial.

```
 PG backend                          pggate                                tserver PgClientSession
 GetCatalogSnapshot →
   GetSnapshotData: yb_read_point_handle
   := YbResetTransactionReadPoint(catalog) ──► serial_no_.IncMaxReadTime()  (new serial N)
 catalog scan → Perform ────────────► UpdateReadPointForCatalogOps:
                                        RestoreReadPoint(N)
                                        SetupReadTimeOptions:
                                          read_time_serial_no = N,
                                          is_catalog_snapshot = true,
                                          clamp_uncertainty_window = true ──► SetupSession(kPlain):
                                                                                 SetupPlainSessionReadTime:
                                                                                   save the outgoing serial's time,
                                                                                   restore N from history if present
                                                                                 UpdateReadTime: serial changed and
                                                                                   nothing restored →
                                                                                   SetFreshCatalogSnapshotReadTime
 next scan under the same snapshot → same serial N ────────────────────────────► same time
 InvalidateCatalogSnapshot → next GetCatalogSnapshot allocates N+1 ───────────► new time
```

`SetFreshCatalogSnapshotReadTime` (`:3680-3694`, called from `UpdateReadTime`
at `:4168`, never for SERIALIZABLE): asks
`LocalCatalogReadTimeForFreshSnapshot` (`:3670`) for a time; when the copy
can serve this request it returns C after the waits of §8.3, and the session
read point is fixed to `ReadHybridTime::SingleTime(C)`; otherwise the read
point takes the clamped clock time as before. With a distributed transaction
attached the read point object is the transaction's, but the outgoing
serial's time was saved into `ReadPointHistory` and is restored when the
transaction's serial returns (`RepeatableReadDataSnapshotSurvivesLocalCatalogRead`).

### 8.3 Waits before a local read

`LocalCatalogCompleteTime(replica, read_time_options, deadline)`
(`:3573-3646`), called from both `SetFreshCatalogSnapshotReadTime` and
`TryServeCatalogReadsLocally`:

1. Not serving → invalid (the caller routes to master).
2. Version check: if the request carries a database and version and
   `applied_version(db) < version`, `WaitForAppliedVersion` up to
   `LocalCatalogWaitDeadline` (`local_catalog_serve_wait_timeout_ms`, 300000;
   0 = no wait, -1 = the request's own deadline); on timeout → invalid
   (master). `backend_catalog_version_` is
   recorded on the session.
3. Own-write check: `own_catalog_write_floor_` is set in `HandleCommit`
   (`:4688-4700`) to `clock()->Now()` after a successful DDL-mode commit,
   together with `own_catalog_write_floor_version_` := the last recorded
   backend version; the check waits `WaitForSafeTime(floor)` unless the
   backend's version has moved past the recorded one
   ([todos T12](todos.md#212-t12-own-write-check-version-record)). The
   temporary-relation case is the one this covers.
4. Master fallback check (todos T13): if the session's previous catalog read
   that the copy could have answered went to master, wait (floor request)
   until C reaches this server's clock now, which the master response moved
   past master's read time.
5. Returns `safe_time()`.

### 8.4 The redirect

`TryServeCatalogReadsLocally` (`:3733-3877`), called from `DoPerform` after
`SetupSession` and before the response cache and storage dispatch. Decline
rules, in order, each with a `routing_reason` and a counter:

1. no replica (feature off);
2. `ddl_mode`, `ddl_use_regular_transaction_block` or
   `yb_non_ddl_txn_for_sys_tables_allowed` on the request
   (`reads_to_master_in_ddl`);
3. not `IsCopyServableCatalogRequest` (`:3654-3667`): a plain-session
   request with `is_catalog_snapshot`, or the legacy catalog session (the
   startup preload, parallel workers);
4. a SERIALIZABLE transaction attached (`reads_to_master_serializable`);
5. not serving, or no tablet peer (`reads_to_master_not_serving`);
6. an op that is not a read, or whose table id is absent from the copy's
   metadata (`AllOpsAreReadsOfLocalCatalogTables`, `local_catalog_read.cc`);
7. the waits of §8.3 failed (`reads_to_master_not_serving`);
8. the session's read point has no time, or its time h is above C and the
   copy did not reach h within `LocalCatalogWaitDeadline` after asking
   master for a push up to h (a time from master or an explicit request;
   never clamped down);
9. `PrepareLocalCatalogRead` fails (history below the copy's cutoff).

Served path: `PrepareLocalCatalogRead` (`local_catalog_read.cc`) waits for
the copy's tablet safe time at the read time with `RequireLease::kFalse` and
opens a `ScopedReadOperation`; `ExecuteLocalCatalogReads` runs each op
through `tablet->HandlePgsqlReadRequest` with empty transaction metadata and
`is_explicit_request_read_time = true`, sets the rows sidecar, and fills
`paging_state.read_time` so continuation pages stay at the same time. A
failed execution goes back to PG as the error
([todos T9](todos.md#29-t9-failed-local-read-fallback)). The response cache
setter runs for served reads as for master reads. `reads_served` counts
requests, not ops.

Routing log: `log_local_catalog_read_routing` (runtime) writes one INFO line
per catalog request with the reason and the ops' table ids; the same lines
are VLOG(1). `TEST_local_catalog_master_read_delay_ms` sleeps before every
catalog request the session sends to master, on both catalog paths, to
stand in for a WAN round trip.

## 9. Modes

Plan2 [§10](plan2.md#10-modes-in-which-the-local-tablet-is-not-used).

- initdb: `YBCIsLegacyModeForCatalogOps()` is true with the initdb env var
  set and those reads use session types that never reach the redirect; the
  applier waits for `IsInitDbDone` before fetching the copy.
- YSQL upgrade migrations run with `yb_non_ddl_txn_for_sys_tables_allowed`
  and are declined by rule 2.
- Major version upgrade: `IsServing()` false while the heartbeat reports
  `ysql_major_version_upgrade_in_progress`; pushes keep being applied.
- Legacy catalog session (startup preload, full cache refresh, parallel
  workers): accepted by `IsCopyServableCatalogRequest`; the legacy path asked
  for the clamped clock reading when it had no read time of its own, and
  the chooser now hands it C; `DoPerform` echoes that time back as
  `catalog_read_time` so the rest of the preload runs at the same C. The
  version row is part of the same preload, so the version a backend adopts
  is one the copy holds (plan2 §13).

## 10. Metrics and flags

Tserver metrics (`local_catalog_replica.cc`): counters
`local_catalog_apply_failures`, `reseeds`, `floor_requests_sent`,
`serving_disabled_lease`, `reads_served`, `reads_waited_for_version`,
`reads_waited_for_own_writes`, `reads_to_master_in_ddl`,
`reads_to_master_serializable`, `reads_to_master_not_serving`,
`pushes_received`, `pushes_applied`, `pushes_refused`; gauges
`local_catalog_serving_state`, `local_catalog_safe_time_micros`; histograms
`local_catalog_apply_latency_us`, `lag_us`, `gate_wait_us`. Master metrics:
§2. The copy's tablet metrics are registered in the tserver's
shared registry under master's `sys.catalog` table. Holes and additions:
[todos T26](todos.md#224-t26-observability).

Flags by owner: tserver `enable_local_tserver_catalog`,
`local_catalog_master_rpc_timeout_ms`, `local_catalog_floor_request_interval_ms`,
`local_catalog_checkpoint_flush_interval_ms`,
`local_catalog_apply_write_timeout_ms`, `local_catalog_bootstrap_retry_delay_ms`,
`local_catalog_serve_wait_timeout_ms` (also holds PG's start after a new
lease), `log_local_catalog_read_routing`; master
`local_catalog_push_interval_ms`, `local_catalog_push_min_cycle_interval_ms`,
`local_catalog_push_page_bytes`, `local_catalog_push_cache_mb`,
`local_catalog_push_max_in_flight_rpcs`, `local_catalog_push_max_in_flight_bytes`,
`local_catalog_push_release_reserved_rpcs`, `local_catalog_release_inline_max_bytes`,
`local_catalog_push_unknown_position_lookback_ops`, `local_catalog_push_rpc_timeout_ms`,
`sys_catalog_change_stream_idle_timeout_sec`. Test flags:
`TEST_local_catalog_disable_serving`, `TEST_local_catalog_pause_applier`,
`TEST_local_catalog_fail_push`, `TEST_local_catalog_push_reseed`,
`TEST_local_catalog_pushed_batch_apply_delay_ms`,
`TEST_local_catalog_drop_pushed_batch`,
`TEST_local_catalog_report_applied_op_id_ahead`,
`TEST_local_catalog_master_read_delay_ms`, master
`TEST_local_catalog_push_block_target`, `TEST_local_catalog_push_extra_targets`.

## 11. Tests

`src/yb/yql/pgwrapper/pg_local_catalog-test.cc`, external three-tserver
cluster with object locking, transactional DDL and the feature on; 69 tests
(one disabled). `src/yb/master/sys_catalog_pusher-test.cc`, in-process
`MiniMaster` with in-process push receivers; 10 tests. Results of the push change: worklog2 "Master push
(push-plan.md), 2026-10-07".

| Group | Tests |
|---|---|
| Bootstrap, restart, re-seed | `BootstrapReachesServing`, `CatalogVersionRowsMatchMaster`, `RestartResumesWithoutReseed`, `RestartReusesTheCopyWithoutAskingMaster` (slow-retry fixture), `FirstQueryLatencyAfterRestart`, `CheckpointTooOldReseeds`, `ServingDisabledRoutesToMaster`, `PushFailuresDoNotMoveSafeTime` |
| Read routing and time rule | `ColdBackendServedLocally`, `MasterCatalogReadsDropWhenServing`, `FreshBackendDoesNotPayMasterCatalogLatency`, `CatalogMissInTransactionBlockAfterAWrite`, `RepeatableReadDataSnapshotSurvivesLocalCatalogRead`, `InDdlTransactionBlockReadsMaster`, `NonDdlSysTableSessionReadsMaster`, `ExplicitReadTimeRouting` (above C: served after a push), `ReadOlderThanUserTabletRetentionIsServedLocally` (short-retention fixture), `ReadBelowTheCopysCutoffGoesToMaster` (tight-cutoff fixture) |
| Freshness and versions | `CrossTserverDdlVisibleImmediately`, `OwnTserverDdlServedLocally`, `ReadWaitsForAVersionPublishedWhileNotServing`, `GateHoldsDdlWhileApplierIsPaused`, `TemporaryRelation`, `VersionIncrementingDdlDoesNotPayTheOwnWritesFloor`, `ConcurrentDdlsFromTwoTserversConverge`, open-block tests `OpenDdlBlockDoesNotBlockAnotherTserversDdl`, `OpenDdlBlockDoesNotBlockADdlOnTheSameTserver`, `TwoOpenDdlsDoNotBlockAnotherTserversDdl` |
| Catalog content | `UserCatalogReadMatchesMaster`, `InternalCatalogReadsCheckConstraint`, `RolledBackSubtransactionIsNotVisible`, `NewDatabaseCotablesReachTheCopy`, `NewIndexIsUsedByAnotherTserver`, `NewIndexIsUsedBySameSession`, `SharedCatalogReachesOtherTservers`, `ViewDefinedOnAnotherTserver`, `TypeAndMissingValueAcrossTservers` |
| Tablet identity | `LocalCatalogCopyIsNotInTheReportableTabletSet`, `FullReportWithOrphanDeletionCheckOffLeavesTheCopyAlone` |
| Pages in the release, pushes (push-plan §9.3, §9.5) | `ReleaseCarriesOnePageInline`, `CreateDatabaseIsPagedNotInline`, `SmallInlineCapPagesEveryDdl`, `StragglerIsPushedNotPolled`, `LostPushIsRepairedNextCycle`, `GapFromStaleReportIsRepushed`, `ConcurrentDdlsShareCycles`, `ApplyOutlivingItsCallerDoesNotCrash`, `NewMasterLeaderPushesFromLookback` (multi-master), `TempTableFloorRequestsOneCycle`, `IdleClusterSendsNoPushes`, `FallbackKnobZeroUsesMasterAtOnce` |
| Pushes, second set (push-plan §9.3, §9.5) | `DropSchemaCascadeIsInlineMultiPage`, `MasterSnapshotTimeIsFloorForNextLocalRead`, `FallbackKnobNeverHoldsTheStatement`, `StalledApplyTimesOutAndRecovers`, `TimerCycleCarriesWritesOutsideDdl` (timer fixture, 2 s) |
| Applier, through the push RPC (push-plan §9.2) | `GapIsAnsweredNotApplied`, `OverlapIsFiltered`, `StaleTermAndWrongTargetAreRefused`, `QueueOrdersByPageFrom` |
| Lease rejoin (push-plan §9.4; lease TTL 5 s) | `RejoinCatchesUpFromCache`, `RejoinWithNoDdlFlipsOnEmptyCycle`, `RejoinBelowCacheReadsWal`, `RejoinPgStartHoldTimesOut`, `RejoinBelowRetentionReseeds`, `RejoinPgStartHoldNever`, `RejoinWaveDoesNotHoldDdl`, `ReleaseDuringCatchupGatesTheFlip` |
| Master unit (`sys_catalog_pusher-test`) | `PageBytesEqualProducerRecords`, `PagesSealAtCapAndCycleEnd`, `EmptyCycleCarriesSafeTime`, `FloorIsReachedWithoutMovingTheClock`, `FloorRequestsCoalesce`, `EvictionByBytesOnly`, `InFlightBoundsHold`, `GapAnswerRewinds`, `TermChangeClearsCache`, `BelowRetentionIsReseed` (short-retention fixture) |

Load harness (`pg_local_catalog_load-test.cc`, disabled tests, release builds
only): push scenarios `PushSteady`, `PushFixedLag`, `PushJoinerWave`,
`PushIdle`, `CreateDatabaseFanout` with N in-process push receivers that
master pushes to through `TEST_local_catalog_push_extra_targets`
(`--local_catalog_load_push_targets`, `--local_catalog_load_joiners`);
calibration `RealTserversOnly`; `DdlWalSize`, `DdlWalSizeDatabase` measure
what master pushes to one copy per DDL shape.

Three existing tests that count reads reaching master are pinned to the
feature off (`pg_catalog_version-test.cc`, commit 01317fa1d69). Java
regress schedules with the feature on have not been run
([todos T27](todos.md#225-t27-test-coverage-and-open-ci-items)).

Constraints learned writing the retention tests: "above C" only holds while
the applier is paused; a read time far enough back to fall below a 4 h cutoff
predates the cluster's catalog and PG fails in the relcache build before the
redirect runs, so retention is tested by shortening the intervals and
forcing a flush with `memstore_size_mb=1` under DDL churn;
`TEST_committed_history_cutoff_initial_value_usec` sets only the primary
cutoff while reads are bounded by the cotables cutoff when valid.

## 12. Operational notes

- After touching catalog headers or protos that change the system catalog,
  run `./yb_build.sh reinitdb`: a stale `initial_sys_catalog_snapshot`
  under `build/` produces confusing DDL failures.
- A "Seq Scan instead of Index Scan" failure has always meant one catalog
  row missing from the copy: check A[db] and C against the version the
  backend carried, then the applier's apply log (VLOG(1) "Applied N
  records", VLOG(3) per record) for the transaction that wrote the row.
- Every tablet-wide flag or assertion applies to the copy
  ([todos T32](todos.md#230-t32-tablet-wide-flags)).
- The per-request routing log is turned on with
  `yb-ts-cli --server_address=<ts> set_flag log_local_catalog_read_routing true`.
- Measured on the three-region simulated cluster (100 ms one way to node 3):
  a fresh backend's first query paid 263 ms of master round trips before the
  preload moved to the copy; after it, 11 ms. A first query after any DDL
  still pays about 9 s there, in `MasterDdl::GetTableSchema` calls after the
  tserver table cache is emptied ([todos T17](todos.md#216-t17-changed-table-list-in-the-release)).
- Worklog2 sections worth reading before changing the copy's tablet
  handling: "The copy's own raft group: three defects", "A restart with a
  copy on disk now needs nothing from master", "Why master cannot delete the
  copy as an unknown tablet".
