# Implementation of the tserver-local catalog copy

What is in the code today, by component, with pointers. Branch
`docdb_local_catalog` at `8cd70bea5b4`, worktree
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
  (`tablet_server.cc`, warning logged), and master registers its change
  service under the same condition (`master.cc`).
- The copy exists on every tserver as tablet `kLocalCatalogTabletId`
  (`00000000000000000000000000000001`, `common/constants.h:48`), a private
  single-voter copy of master's `kSysCatalogTabletId`.
- Branch delta: 55 files, about 5500 lines; new files
  `src/yb/cdc/sys_catalog_change_service.{h,cc}`,
  `src/yb/tserver/local_catalog_{replica,poller,read}.{h,cc}`,
  `src/yb/yql/pgwrapper/pg_local_catalog-test.cc`.

Component map:

| Component | Files | plan2 |
|---|---|---|
| Master poll service | `cdc/sys_catalog_change_service.{h,cc}`, `cdc/cdc_service.proto`, `cdc/xcluster_producer.cc`, `cdc/cdc_producer.h`, `cdc/xrepl_stream_metadata.{h,cc}`, `master/master.{h,cc}` | [§4.1](plan2.md#41-master-poll-service) |
| Replica state and waits | `tserver/local_catalog_replica.{h,cc}` | [§4.2](plan2.md#42-poller), [§7](plan2.md#7-failure-handling) |
| Bootstrap and tablet | `tserver/ts_tablet_manager.{h,cc}`, `tserver/remote_bootstrap_client.{h,cc}`, `tablet/tablet.cc` | [§6](plan2.md#6-bootstrap) |
| Poller and apply | `tserver/local_catalog_poller.{h,cc}`, `tserver/tserver.proto` (`LocalCatalogCheckpointPB`) | [§4.2](plan2.md#42-poller) |
| Freshness gate | `tserver/ts_local_lock_manager.cc`, `tserver/heartbeater.cc`, `master/master_heartbeat.proto`, `master/master_heartbeat_service.cc`, `master/catalog_manager.cc`, `master/sys_catalog.cc`, `tserver/tserver.proto` | [§4.3](plan2.md#43-freshness-guarantee) |
| Release push | `master/object_lock_info_manager.cc`, `master/ts_descriptor.{h,cc}`, `tserver/heartbeater.cc` | [§4.4](plan2.md#44-change-records-carried-in-the-lock-release) |
| Read routing | `tserver/pg_client_session.{h,cc}`, `tserver/local_catalog_read.{h,cc}`, `tserver/pg_client.proto`, `yql/pggate/pg_session.{h,cc}`, `yql/pggate/pg_txn_manager.cc`, `yql/pggate/ybc_pg_typedefs.h`, `postgres/.../pg_yb_utils.c` | [§5](plan2.md#5-reads) |
| Lease and upgrade hooks | `tserver/ysql_lease_manager.cc`, `tserver/heartbeater.cc` | [§7.1](plan2.md#71-lease-loss-lease-regain-and-tserver-restart), [§10](plan2.md#10-modes-in-which-the-local-tablet-is-not-used) |

## 2. Master poll service

`SysCatalogChangeServiceImpl` (`sys_catalog_change_service.cc`), registered
on master's high-priority RPC pool when the feature is on (`master.cc`,
`RegisterService(FLAGS_master_xrepl_svc_queue_length, ..., kHigh)`). One RPC:

```
GetSysCatalogChangesRequestPB  { requestor_uuid, from_checkpoint }
GetSysCatalogChangesResponsePB { error, changes (GetChangesResponsePB: records,
                                 checkpoint, safe_hybrid_time), have_more_messages,
                                 slice_from (push only), reseed_required }
```

- Per-requester state: `streams_`, a map from tserver uuid to a
  `StreamMetadata` created on first request by `GetOrCreateStream`
  (`:74-83`), initialized through `InitForSysCatalogChangeStream`
  (`xrepl_stream_metadata.cc`): WAL record format, xCluster source type,
  transactional mode, `is_sys_catalog_change_stream_ = true`. No entry in
  the master catalog, no `cdc_state` row, no WAL retention hook; never
  persisted, so a new master leader starts each requester afresh. Entries
  idle for `sys_catalog_change_stream_idle_timeout_sec` (3600) are pruned on
  every call (`:63-70`).
- `DoGetSysCatalogChanges` (`:103-172`) builds an `XClusterGetChangesContext`
  and calls `cdc::GetChangesForXCluster` (`xcluster_producer.cc:289`)
  unchanged except for the four additions below. The safe-time logic is
  therefore xCluster's: leader safe time, `ResolveIntents` against every
  running transaction's coordinator (`transaction_participant.cc:1342`),
  the majority-replicated index captured with it, S emitted only in the
  response whose checkpoint reaches that index; at most one computation per
  `xcluster_consistent_wal_safe_time_frequency_ms` (250 ms) per requester
  while catalog transactions are open. Pseudo code in plan2 §4.1.
- Additions to the producer path, all gated on `IsSysCatalogChangeStream()`
  or on optional context fields so other streams pay nothing:
  - aborted-subtransaction sets are always emitted for this stream,
    independent of `xcluster_enable_subtxn_abort_propagation`
    (`xcluster_producer.cc`, two checks);
  - every record is stamped with the op id of the WAL entry it came from
    (`stamp_record_op_ids`), which the release push needs to slice one batch
    per target;
  - `force_apply_safe_time` (`cdc_producer.h`) makes the producer compute a
    new S regardless of the throttle; set by the release path only;
  - `update_on_snapshot_op_func` (`cdc_producer.h`) is called for every
    snapshot operation the batch crosses; the service's callback stops the
    batch at a `RESTORE_SYS_CATALOG` operation and answers
    `reseed_required` with no records (`:140-148`, `:159-169`). Scheduled
    snapshots also submit `CREATE_ON_MASTER`/`DELETE_ON_MASTER` snapshot ops
    on the system catalog tablet, which the filter lets through.
- `SPLIT_OP` is refused with `IllegalState` (`update_on_split_op_func`).
- `GetChangesForRelease(from, batch, deadline)` (`:93-100`) is the in-process
  entry the release push uses, with `force_apply_safe_time = true`.

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
- `OnSafeTimePublished(c)` (`:442-465`) moves the state to `kServing`: from
  `kBootstrapping`/`kReseeding` on the first published C above
  `lease_gap_floor_` (if set), from `kDisabledLease` once C ≥ the floor. In
  `kServing` it does nothing ([todos T2](todos.md#22-t2-serving-condition-on-entering-serving)).
- Waits: `WaitForAppliedVersion(db, version, deadline)` and
  `WaitForSafeTime(target, deadline)` (`:314-356`) loop on `cond_`, calling
  `poller_->RequestImmediatePoll()` on every check that fails
  ([todos T10](todos.md#210-t10-waiter-driven-polls)).
  `HoldsCatalogStateAt(target)` (`:356-362`) is the non-blocking variant for
  the heartbeat thread: true when `safe_time() >= target`, otherwise it
  requests an immediate poll.
- Lease hooks: `OnLeaseLost()` (`:370-379`) sets `lease_gap_floor_` to
  `HybridTime::kMax` and the state to `kDisabledLease`; `OnLeaseGained(t)`
  (`:381-393`) sets the floor to `t` and requests a poll. Called from
  `ysql_lease_manager.cc:311` and `:161-172` (`restart_pg`, i.e. every new
  lease epoch including the first, with `clock()->MaxGlobalNow()`).
- `SetMajorVersionUpgradeInProgress` from the heartbeat
  (`heartbeater.cc:550-552`, `TSHeartbeatResponsePB` field 35).
- `RequestReseed()` sets `reseed_requested_`; the poller consumes it with
  `TakeReseedRequest()`.
- `applied_op_id_` is the copy's applied WAL position, reported in every
  heartbeat request (`heartbeater.cc:391-399`, `TSHeartbeatRequestPB` field
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
  reuse predicate the poller asks first, so a restart contacts master only
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

## 5. Poller and apply

`LocalCatalogPoller` (`local_catalog_poller.{h,cc}`), a
`MasterLeaderPollerInterface` driven by `MasterLeaderPollScheduler`
(`master_leader_poller.cc`): one thread, `Poll()` on an interval,
`TriggerASAP()` to cut the interval short (`RequestImmediatePoll`, `:125`).
Plan2 [§4.2](plan2.md#42-poller).

- `Poll()` (`:161`): `EnsureLocalTablet()`, then `ApplyPendingPush()`, then
  `FetchAndApplyOnce()` unless `TEST_local_catalog_pause_poller`.
- `EnsureLocalTablet()` (`:261-320`): consumes a re-seed request (state
  `kReseeding`, `OnReseedStarted`, `DeleteLocalCatalogTablet`, delete the
  checkpoint file, reset positions); otherwise state `kBootstrapping`. If
  the tablet is not reusable, resolves the master leader, waits for
  `IsInitDbDone` (returns `TryAgain` until then), gets the leader's uuid,
  and calls `OpenOrCreateLocalCatalogTablet`. Then `LoadCheckpoint()` and
  `MakeLocalCatalogTabletLeader`.
- Checkpoint file: `LocalCatalogCheckpointPB` (`tserver.proto`), written by
  `PersistCheckpoint(force)` (`:213-236`) at most once per
  `local_catalog_checkpoint_flush_interval_ms` (1000) when the position
  changed, and at shutdown; it waits `log()->WaitUntilAllFlushed()` first
  ([todos T7](todos.md#27-t7-checkpoint-durable-before-the-wal) for the fsync
  half). The file lives outside the tablet metadata directory
  (`FsManager::ListTabletIds` treats every file there as a tablet id). A
  fresh copy with no file starts at the copied log's latest op id
  (`:193-195`).
- `FetchAndApplyOnce()` (`:321-442`): `GetSysCatalogChanges` with
  `requestor_uuid` and `from_checkpoint`; on RPC failure `IncPollFailures`;
  on `CHECKPOINT_TOO_OLD` `RequestReseed`; on `reseed_required`
  `RequestReseed`; else `ApplyRecords`, then `PublishAppliedBatch`, then
  `RecordLag`, `OnSafeTimePublished`, `PersistCheckpoint`.
  `have_more_messages_` makes `IntervalToNextPoll` (`:135`) return zero.
- `ApplyRecords(resp, stats)` (`:605-705`): uses an `XClusterWriteInterface`
  (`xcluster_write_implementations.cc`: `CombineExternalIntents` stages a
  transaction's rows as one external-intent blob keyed by the transaction
  id; an APPLY record becomes an apply-external-transaction entry with the
  commit time and the aborted-subtransaction set) and writes each produced
  request through `WriteBatchToLocalTablet` (`:706-731`): a `WriteQuery` on
  the copy's peer with `leader_term`, no RPC context, bounded by
  `local_catalog_apply_write_timeout_ms` (60 s). The write lands through
  the non-transactional batch writer (`rocksdb_writer.cc`
  `NonTransactionalBatchWriter`, `PrepareApplyExternalIntents`). A
  `CHANGE_METADATA` record flushes what precedes it and is applied as a
  `ChangeMetadataOperation` on the copy with the record's request verbatim
  and the tablet id rewritten (`ApplyChangeMetadataRecord`, `:732`).
  `SPLIT_OP` is an error. Per-record VLOG(3) names the destination store.
- `PublishAppliedBatch` (`:443-473`): advances `checkpoint_` and the replica's
  applied op id when the response's end position is above the current one
  (index comparison); if the response carries a valid safe time, reads the
  copy's `pg_yb_catalog_version` at that time (`ReadLocalCatalogVersions`,
  `:755`) and publishes C and A[db] together; otherwise publishes nothing new.
- Pushed batches: `ApplyPushedBatch(batch, deadline)` (`:475-503`) parks the
  batch as `pending_push_` (one slot; a second push gets `TryAgain`), wakes
  the poll thread, and waits on `push_cond_`; a timed-out waiter withdraws
  its push. `ApplyPendingPush` (`:514`) runs on the poll thread before the
  next pull; `DoApplyPushedBatch` (`:543-604`) refuses a batch with
  `reseed_required` (and re-seeds), requires `slice_from` at or below the
  applied position, drops records at or below it, applies the rest, and
  publishes S only after the whole retained batch.
- Flags: `local_catalog_poll_interval_ms` (100),
  `local_catalog_poll_rpc_timeout_ms` (30000),
  `local_catalog_checkpoint_flush_interval_ms` (1000),
  `local_catalog_apply_write_timeout_ms` (60000),
  `local_catalog_bootstrap_retry_delay_ms` (1000). Test flags:
  `TEST_local_catalog_pause_poller`, `TEST_local_catalog_fail_poll`,
  `TEST_local_catalog_poll_checkpoint_too_old`,
  `TEST_local_catalog_pushed_batch_apply_delay_ms`.

## 6. Freshness gate

Plan2 [§4.3](plan2.md#43-freshness-guarantee) specifies a version comparison
on both paths; the code compares times ([todos T1](todos.md#21-t1-freshness-guarantee-on-applied-versions)).

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
  (`ts_local_lock_manager.cc:521-623`): after `WaitToApplyIfNecessary`, if
  the copy is serving and the request carries a pushed batch, apply it
  (§5); if serving and a read time is present, `WaitForSafeTime(read_time,
  deadline)` (`:577-590`, recorded in `gate_wait_us`; a timeout fails the
  RPC so master retries); then `SetYsqlDBCatalogVersions*` (`:598-606`);
  then the local unlock (`:618-623`). The handler is synchronous, so the
  acknowledgement waits. While not serving the versions are published at
  once.
- Heartbeat (`heartbeater.cc:137-145`, `:560-573`): when the response
  carries version data and `LocalCatalogCopyHoldsHeartbeatVersions()` is
  false, the whole report is skipped and an immediate poll requested; the
  next heartbeat re-sends it, because the tserver's fingerprint stays
  unchanged. Single versions are never lowered
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

## 7. Release push

`enable_local_catalog_release_push` (master, runtime, default `false`,
`object_lock_info_manager.cc:93`); plan2 [§4.4](plan2.md#44-change-records-carried-in-the-lock-release).

- `UpdateAllTServers<Req>::PrepareLocalCatalogPushes()` (`:1772-1900`), run
  from `LaunchRpcs` for release requests that carry versions and a read
  time: collects each live-lease tserver's reported position from
  `TSDescriptor::LocalCatalogAppliedOpId()`, skipping reports older than
  `local_catalog_release_push_report_max_age_ms` (5000) and targets more
  than `local_catalog_release_push_max_lag_ops` (1000) behind the freshest;
  fetches one batch from the lowest remaining position through
  `GetChangesForRelease` (forced S; the shared stream `master-release-push`);
  carries nothing when the batch has an error, crossed a restore, or has no
  safe time; slices the batch per target (records above the target's
  position, `slice_from` = the target's position, the batch's checkpoint and
  S), serializes it, drops slices over
  `local_catalog_release_push_max_bytes` (1 MB), and stores the rest in
  `local_catalog_pushes_` keyed by uuid. `UpdateTServer::request()`
  (`:2122-2124`) attaches the payload as
  `ReleaseObjectLockRequestPB.local_catalog_changes` (field 16, bytes,
  never persisted: `md.IgnoreField` at `:682`).
- Tserver: `ts_local_lock_manager.cc:555-575` parses and applies the batch
  through `LocalCatalogReplica::ApplyPushedBatch` before the gate wait;
  failure counts `pushes_refused` and requests an immediate poll.
- Metrics on master: `local_catalog_release_pushes_sent`,
  `local_catalog_release_pushes_skipped`, `local_catalog_release_push_bytes`,
  `local_catalog_release_push_fetch_us`. On the tserver:
  `local_catalog_pushes_received/applied/refused`.
- Cost items before default-on: [todos T16](todos.md#215-t16-release-push-cost-and-default).

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
   `LocalCatalogWaitDeadline` (`local_catalog_serve_wait_timeout_ms`, 5000,
   `:236-241`); on timeout → invalid (master). `backend_catalog_version_` is
   recorded on the session.
3. Own-write check: `own_catalog_write_floor_` is set in `HandleCommit`
   (`:4688-4700`) to `clock()->Now()` after a successful DDL-mode commit,
   together with `own_catalog_write_floor_version_` := the last recorded
   backend version; the check waits `WaitForSafeTime(floor)` unless the
   backend's version has moved past the recorded one
   ([todos T12](todos.md#212-t12-own-write-check-version-record)). The
   temporary-relation case is the one this covers.
4. Returns `safe_time()`.

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
8. the session's read point has no time, or its time is above C (a time
   from master or an explicit request; never clamped down);
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
  poller waits for `IsInitDbDone` before fetching.
- YSQL upgrade migrations run with `yb_non_ddl_txn_for_sys_tables_allowed`
  and are declined by rule 2.
- Major version upgrade: `IsServing()` false while the heartbeat reports
  `ysql_major_version_upgrade_in_progress`; the poller keeps running.
- Legacy catalog session (startup preload, full cache refresh, parallel
  workers): accepted by `IsCopyServableCatalogRequest`; the legacy path asked
  for the clamped clock reading when it had no read time of its own, and
  the chooser now hands it C; `DoPerform` echoes that time back as
  `catalog_read_time` so the rest of the preload runs at the same C. The
  version row is part of the same preload, so the version a backend adopts
  is one the copy holds (plan2 §13).

## 10. Metrics and flags

Tserver metrics (`local_catalog_replica.cc:59-155`): counters
`local_catalog_poll_failures`, `apply_failures`, `reseeds`,
`serving_disabled_lease`, `reads_served`, `reads_waited_for_version`,
`reads_waited_for_own_writes`, `reads_to_master_in_ddl`,
`reads_to_master_serializable`, `reads_to_master_not_serving`,
`pushes_received`, `pushes_applied`, `pushes_refused`; gauges
`local_catalog_serving_state`, `local_catalog_safe_time_micros`; histograms
`local_catalog_poll_latency_us`, `apply_latency_us`, `lag_us`,
`gate_wait_us`. The copy's tablet metrics are registered in the tserver's
shared registry under master's `sys.catalog` table. Holes and additions:
[todos T26](todos.md#224-t26-observability).

Flags by owner: tserver `enable_local_tserver_catalog`,
`local_catalog_poll_interval_ms`, `local_catalog_poll_rpc_timeout_ms`,
`local_catalog_checkpoint_flush_interval_ms`,
`local_catalog_apply_write_timeout_ms`, `local_catalog_bootstrap_retry_delay_ms`,
`local_catalog_serve_wait_timeout_ms`, `log_local_catalog_read_routing`;
master `enable_local_catalog_release_push`,
`local_catalog_release_push_max_bytes`, `local_catalog_release_push_max_lag_ops`,
`local_catalog_release_push_report_max_age_ms`,
`sys_catalog_change_stream_idle_timeout_sec`, `sys_catalog_change_rpc_timeout_ms`.
Test flags: `TEST_local_catalog_disable_serving`, `TEST_local_catalog_pause_poller`,
`TEST_local_catalog_fail_poll`, `TEST_local_catalog_poll_checkpoint_too_old`,
`TEST_local_catalog_pushed_batch_apply_delay_ms`,
`TEST_local_catalog_drop_pushed_batch`,
`TEST_local_catalog_report_applied_op_id_ahead`,
`TEST_local_catalog_master_read_delay_ms`.

## 11. Tests

`src/yb/yql/pgwrapper/pg_local_catalog-test.cc`, external three-tserver
cluster with object locking, transactional DDL and the feature on; 48 tests
at HEAD, all passing in release; D57801 carries `Jenkins: release`.

| Group | Tests |
|---|---|
| Bootstrap, restart, re-seed | `BootstrapReachesServing`, `CatalogVersionRowsMatchMaster`, `RestartResumesWithoutReseed`, `RestartReusesTheCopyWithoutAskingMaster` (slow-retry fixture), `FirstQueryLatencyAfterRestart`, `CheckpointTooOldReseeds`, `ServingDisabledRoutesToMaster`, `PollFailuresDoNotMoveSafeTime` |
| Read routing and time rule | `ColdBackendServedLocally`, `MasterCatalogReadsDropWhenServing`, `FreshBackendDoesNotPayMasterCatalogLatency`, `CatalogMissInTransactionBlockAfterAWrite`, `RepeatableReadDataSnapshotSurvivesLocalCatalogRead`, `InDdlTransactionBlockReadsMaster`, `NonDdlSysTableSessionReadsMaster`, `ExplicitReadTimeRouting`, `ReadOlderThanUserTabletRetentionIsServedLocally` (short-retention fixture), `ReadBelowTheCopysCutoffGoesToMaster` (tight-cutoff fixture) |
| Freshness and versions | `CrossTserverDdlVisibleImmediately`, `OwnTserverDdlServedLocally`, `ReadWaitsForAVersionPublishedWhileNotServing`, `GateHoldsDdlWhilePollerIsPaused`, `TemporaryRelation`, `VersionIncrementingDdlDoesNotPayTheOwnWritesFloor`, `ConcurrentDdlsFromTwoTserversConverge`, open-block tests `OpenDdlBlockDoesNotBlockAnotherTserversDdl`, `OpenDdlBlockDoesNotBlockADdlOnTheSameTserver`, `TwoOpenDdlsDoNotBlockAnotherTserversDdl` |
| Catalog content | `UserCatalogReadMatchesMaster`, `InternalCatalogReadsCheckConstraint`, `RolledBackSubtransactionIsNotVisible`, `NewDatabaseCotablesReachTheCopy`, `NewIndexIsUsedByAnotherTserver`, `NewIndexIsUsedBySameSession`, `SharedCatalogReachesOtherTservers`, `ViewDefinedOnAnotherTserver`, `TypeAndMissingValueAcrossTservers` |
| Tablet identity | `LocalCatalogCopyIsNotInTheReportableTabletSet`, `FullReportWithOrphanDeletionCheckOffLeavesTheCopyAlone` |
| Release push (push fixture, push on) | `ReleaseCarriesTheDdlsOwnRecords`, `PushWithAGapIsRefusedAndPulled`, `OverTheCapMasterCarriesNothing`, `StaleReportGetsNoRecords`, `NewMasterLeaderCarriesNothingUntilReportsArrive`, `RecordlessOpsDoNotRefuseTheSlice` (multi-master), `ConcurrentDdlsDoNotStormRefusals`, `ApplyOutlivingItsCallerDoesNotCrash`, `AStragglerDoesNotDisableThePushForOthers` |

Three existing tests that count reads reaching master are pinned to the
feature off (`pg_catalog_version-test.cc`, commit 01317fa1d69). Java
regress schedules with the feature on have not been run
([todos T27](todos.md#225-t27-test-coverage-and-open-ci-items)).

Constraints learned writing the retention tests: "above C" only holds while
the poller is paused; a read time far enough back to fall below a 4 h cutoff
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
  backend carried, then the poller's apply log (VLOG(1) "Applied N
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
