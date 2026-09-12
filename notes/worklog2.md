# Work log — implementation of local-tserver-docdb-catalog (plan2/implementation2)

Repo: `/net/dev-server-sanketh-3/share/worktrees/repo6`, branch `docdb_local_catalog`,
base `94f5a6cd7dd`.
Build/test: `./yb_build.sh fastdebug --clang21 --cxx-test <test>`.
Target mode only: object locking + concurrent (transactional) DDL.

## 2026-09-03 — Session start

- Read `plan2.md` and `implementation2.md`.
- Repo6 is plain master; it does not carry the `approach3-local-catalog` PG shadow
  commit that `implementation2.md` took its line numbers from. The design is
  substrate-independent, so this only means line numbers must be re-derived and the
  shadow test file is not present for direct porting.
- Next: survey every code location named in the plan to confirm the symbols exist.

## Survey findings that change the plan

1. `implementation2.md` §5.1 states that the storage layer (master) picks the catalog
   snapshot's read time and echoes it back as `used_read_time`. In this tree that is not what
   happens: `PgClientSessionImpl::UpdateReadTime` already picks the time on the tserver when
   `clamp_uncertainty_window` is set, by calling
   `session.read_point()->SetCurrentReadTime(ClampUncertaintyWindow::kTrue)`
   (`src/yb/tserver/pg_client_session.cc:3793`). The catalog snapshot therefore already owns a
   tserver-chosen hybrid time from its first read. Consequence for phase 5: the redirect only has
   to replace that clock-derived time with `ReadHybridTime::SingleTime(C)`; no `used_read_time`
   interception is needed, and `MakeUsedReadTimeApplier` returns an empty applier by itself
   because the read point already carries a time.

2. The plan's "master-sourced serial" bookkeeping (a side map from read-time serial to source) is
   unnecessary. A read point this feature set is `SingleTime(C)`, and C only ever moves forward, so
   a locally sourced time is always at or below the current C. A time picked from the tserver clock
   (the case where the copy was not serving at the snapshot's first read) is at or above the
   current wall clock and therefore above C. Comparing the read point's time against C is
   sufficient: at or below C means serve locally at that exact time, above C means route the op to
   master.

3. The master's CDC service is registered only when
   `ysql_yb_enable_implicit_dynamic_tables_logical_replication` is on
   (`src/yb/master/master.cc:327-331`), and `MasterTabletServer::EnableCDCService` returns early on
   the same flag. Both must also fire for `enable_local_tserver_catalog`. The service context
   already restricts lookups to `kSysCatalogTabletId`
   (`src/yb/master/master_tserver.cc:81-91`), so no other master-side plumbing is needed to serve
   the stream.

4. Bootstrap uses remote *bootstrap*, not the snapshot-transfer client the plan names.
   `RemoteSnapshotTransferClient::FetchSnapshot` requires a snapshot that already exists on the
   source and copies only its files; it ships neither the WAL nor a committed WAL position, and the
   plan needs both (§6.1 step 1 and step 4). `RemoteBootstrapClient` ships the superblock, the SST
   files and the WAL segments, and the master already registers
   `tserver::RemoteBootstrapServiceImpl` over its catalog manager
   (`src/yb/master/master.cc:337-343`), so the master side needs no new service.

5. The local tablet must stay out of `TSTabletManager::tablet_map_`. Every tablet in that map is
   reported to master in the heartbeat's tablet report (`GenerateTabletReport`
   `src/yb/tserver/ts_tablet_manager.cc:3316`, `StartFullTabletReport` `:3399`), and master's
   report processing would treat a tserver-hosted copy of the system catalog tablet as a stray
   replica and order it deleted. The replica therefore holds its own `TabletPeerPtr` and
   `TSTabletManager` gains a dedicated open path that does not register it.

## Phase 0 — flags, metrics, component skeleton

New files `src/yb/tserver/local_catalog_replica.{h,cc}`.

- gflag `enable_local_tserver_catalog` (non-runtime, default false), defined in
  `local_catalog_replica.cc`. The master library links the tserver library, so master reads the
  same flag; `src/yb/master/master.cc` declares it.
- Supporting flags: `local_catalog_poll_interval_ms` (100), `local_catalog_poll_rpc_timeout_ms`,
  `local_catalog_checkpoint_flush_interval_ms` (1000), `local_catalog_apply_write_timeout_ms`,
  `local_catalog_bootstrap_retry_delay_ms`, and the test flags
  `TEST_local_catalog_disable_serving`, `TEST_local_catalog_pause_poller`,
  `TEST_local_catalog_fail_poll`.
- All eight counters and four histograms of the plan, registered on the tserver's server-level
  metric entity.
- `LocalCatalogReplica` holds the serving state, C, the applied-version map, the local tablet peer
  and the poller. `TabletServer` creates it in `Start()` before `RegisterServices()` (so that
  `PgClientServiceImpl` can capture the pointer) and starts its poll thread after
  `tablet_manager_->Start()`.

## Phase 1 — master: change stream on the system catalog tablet

Deviation from the plan, recorded above as finding 3 and expanded here: instead of lifting the
sys-catalog guard in `CDCServiceImpl::GetChanges` and creating a real xCluster stream, a dedicated
service ships the records.

Why: `CDCServiceImpl::GetChanges` does far more than read the WAL. It resolves the stream through
the master catalog (`StreamMetadata::InitOrReloadIfNeeded`), requires a `cdc_state` row for the
checkpoint (`GetLastCheckpoint`), and writes that row back on every call
(`UpdateCheckpointAndActiveTime`). A per-tserver stream polling every 100 ms would therefore need
a catalog entry per tserver and would write to `cdc_state` at the poll rate. The design needs none
of that: each copy persists its own WAL position next to its own tablet.

What was added:
- `SysCatalogChangeService` in `src/yb/cdc/cdc_service.proto`, one method
  `GetSysCatalogChanges(GetSysCatalogChangesRequestPB) -> GetSysCatalogChangesResponsePB`. The
  response embeds `GetChangesResponsePB` so the producer writes into it unchanged.
- `src/yb/cdc/sys_catalog_change_service.{h,cc}`: keeps one in-memory `StreamMetadata` per
  requesting tserver uuid and calls `cdc::GetChangesForXCluster`. The per-requestor state is what
  makes the safe time trustworthy: the producer reports S only once every record up to the WAL
  position it captured with S has been included in a response to that same requestor.
- `StreamMetadata::InitForSysCatalogChangeStream` in `src/yb/cdc/xrepl_stream_metadata.{h,cc}`:
  sets WAL record format, XCLUSTER source type and transactional mode without going to the master
  catalog. Transactional mode is the point: only that path calls
  `TransactionParticipant::ResolveIntents` before reporting an apply safe time, which is what
  guarantees no transaction can still commit at or below S.
- `src/yb/master/master.cc` registers the service at high priority when the flag is on. The
  master's existing CDC service context already restricts tablet lookups to `kSysCatalogTabletId`.

Version read time for the gate:
- `SysCatalogTable::ReadWithRestarts` and `ReadYsqlAllDBCatalogVersions` gained an optional
  `used_read_time` out-parameter, set to the hybrid time the successful read ran at.
- `CatalogManager::GetYsqlAllDBCatalogVersions` propagates it, and the heartbeat version cache
  stores the read time of its last refresh in
  `heartbeat_pg_catalog_versions_cache_read_time_` so the cached path can report it too.
- `ReleaseObjectLockRequestPB.catalog_versions_read_time` (field 15) and
  `TSHeartbeatResponsePB.db_catalog_versions_read_time` (field 34) carry it to the tserver.
  `ObjectLockInfoManager::Impl::PopulateDbCatalogVersionCache` and
  `MasterHeartbeatService::FillHeartbeatResponse` fill them.

## Phase 2 — tserver: local tablet creation and bootstrap

Deviation from the plan (finding 4): the copy is fetched by remote bootstrap, not by the
snapshot-transfer client, because the plan needs a committed WAL position to start the poll from
and `RemoteSnapshotTransferClient` ships neither the WAL nor a position, and requires a snapshot
that already exists on the source.

- `RemoteBootstrapClient::SetPrivateCopyOfSourceTablet(source_tablet_id, local_peer_pb)` puts the
  client into a mode where the tablet's id here differs from its id on the source:
  - `Start()` asks the source for `source_tablet_id` but rewrites the received superblock's
    `raft_group_id` and `kv_store_id` to the local id, because
    `RaftGroupMetadata::LoadFromSuperBlock` refuses a superblock whose id differs from the
    metadata's.
  - `WriteConsensusMetadata()` writes a committed config holding this server alone, carrying over
    the source's term, so the copy's consensus elects it leader and never contacts the master
    peers.
- `TSTabletManager::OpenTablet` was split: the body moved to `OpenTabletPeer(meta, peer, deleter,
  private_copy)` and `OpenTablet` looks the peer up in `tablet_map_` and delegates. For a private
  copy `OpenTabletPeer`:
  - builds the `Tablet` with `is_sys_catalog = kTrue`. That is what gives the copy a transaction
    participant: the system catalog table is not declared transactional, but both the replayed log
    and the polled records carry catalog transactions whose intents must be resolved and applied
    (`src/yb/tablet/tablet.cc:798-802`).
  - forces the consensus config back to this server alone after `BootstrapTablet` returns. The
    replayed log is master's, so it carries master's `CHANGE_CONFIG_OP`s and the replay had just
    installed master's peers into the consensus metadata; consensus is started from that metadata
    a few lines later.
  - skips the load-balancer bookkeeping, which is keyed on tablets in `tablet_map_`.
- `TSTabletManager::OpenOrCreateLocalCatalogTablet` reopens the copy when its metadata is on disk
  in a servable state, and otherwise deletes whatever is there and fetches a fresh one.
  `DeleteLocalCatalogTablet` shuts the peer down and deletes data, WAL, consensus metadata and
  superblock.
- `TSTabletManager::Init` removes `kLocalCatalogTabletId` from the list of tablets it opens, for
  the reason in finding 5.
- The poll position is persisted in `LocalCatalogCheckpointPB` (new message in
  `src/yb/tserver/tserver.proto`) in a file next to the tablet's metadata, at most once per
  `local_catalog_checkpoint_flush_interval_ms` and once more on shutdown. Re-applying a suffix of
  the WAL after a restart is harmless: a write record writes the same key with the same value at
  the same hybrid time, and an apply record for a transaction whose external intents are already
  gone applies nothing.

## Phase 3 — tserver: poller and apply

New files `src/yb/tserver/local_catalog_poller.{h,cc}`.

Deviation from the plan: the poller is built on `MasterLeaderPollScheduler` /
`MasterLeaderPollerInterface` (`src/yb/tserver/master_leader_poller.h`), the same base the YSQL
lease refresher uses, rather than on `XClusterPoller`. That base already owns a thread, resolves
the master leader, retries with backoff, and exposes `TriggerASAP()` for the immediate poll the
gate and the read path need. Its `Poll()` is synchronous, which makes "apply every record, then
publish" a straight-line function instead of the chain of callbacks `XClusterPoller` needs.

One `Poll()` does, in order:
1. `EnsureLocalTablet()`: on the first poll, and after a re-seed request, create or reopen the
   copy and load its WAL position. A fresh copy has no recorded position, so the position is the
   last entry of the log that remote bootstrap brought over.
2. `GetSysCatalogChanges` from that position. `CHECKPOINT_TOO_OLD` means master has garbage
   collected WAL the copy never consumed, so the copy cannot catch up incrementally and a re-seed
   is requested.
3. `ApplyRecords`: records are applied in master WAL order. Write and apply records accumulate in
   an `XClusterWriteInterface` batch (reused from `xcluster_write_implementations.cc`, with an
   empty schema-version map since the copy's schema versions are master's), and the batch is
   written to the copy through `tablet::WriteQuery` with `external_hybrid_time` set. A
   `CHANGE_METADATA` record first flushes the accumulated batch, then applies a
   `ChangeMetadataOperation` built from the record and retargeted at the local tablet id. A
   `SPLIT_OP` is refused: the system catalog tablet cannot split.
4. Publish: C is set to the response's safe time and A[db] is read from the copy's own
   `pg_yb_catalog_version` at that same time, in one `PublishAfterApply` call, so a waiter can
   never observe a version whose rows the copy does not hold.
5. Persist the WAL position, on the flush cadence.

## Phase 4 — tserver: version gate

- Lock release: `TSLocalLockManager::Impl::ReleaseObjectLocks` waits for
  `C >= req.catalog_versions_read_time()` before publishing the versions, records
  `local_catalog_gate_wait_us`, and returns non-OK on timeout so master retries. The RPC handler
  is synchronous, so the wait delays the acknowledgement, and the DDL's client is told success only
  after every tserver has acknowledged.
- Heartbeat: `HeartbeatPoller` skips the response's version data entirely while
  `C < db_catalog_versions_read_time`, and asks the poller to poll immediately.

  Deviation from the plan, which says to clamp each database's version to A[db]. Clamping is not
  safe in this tree: `TabletServer::SetYsqlDBCatalogVersionsUnlocked` treats a version below the
  one it already holds as a stale master report, and after
  `ysql_stale_catalog_version_min_seconds` of that it calls `LOG(FATAL)`
  (`src/yb/tserver/tablet_server.cc:1674-1693`). A database the copy has no version row for yet
  would clamp to 0 and trip exactly that path. Omitting such a database instead is also unsafe:
  the version data is a full report, and a database missing from it is deleted from the tserver's
  map. Skipping the whole response loses nothing, because the next heartbeat re-sends the full
  report.

## Phase 5 — tserver: read redirect and read-time rule

New files `src/yb/tserver/local_catalog_read.{h,cc}` hold the read execution;
`PgClientSessionImpl::TryServeCatalogReadsLocally` holds the decision.

- `ReadTimeOptionsPB.is_catalog_snapshot` (field 9) is a new explicit flag, set by
  `PgTxnManager::SetupReadTimeOptions`. It is not inferred from `clamp_uncertainty_window`, which
  is skipped whenever the read time is supplied explicitly or a reset is requested.
- The redirect sits in `DoPerform` after `SetupSession` and before `PrepareOperations`. It serves
  the request locally only when: the flag is on and the copy is serving; the request carries a
  catalog snapshot serial on the plain session; the backend is not in DDL; there is no distributed
  transaction; and every operation is a read of a table the copy holds. Every other case is
  counted and falls through to master.
- Version wait: for each operation carrying `ysql_db_oid` and `ysql_db_catalog_version`, the
  request waits until `A[db] >= V`. On the tserver that ran the DDL the backend advances its own
  version at commit, before this tserver's poller has the commit, and this is where that gap is
  closed.
- Own-writes floor: `PgClientSessionImpl::HandleCommit` records `clock()->Now()` after a
  successful DDL commit, and the redirect waits for C to reach it. A DDL on a temporary relation
  increments no catalog version, so the per-operation version check cannot see it; the commit
  response propagated the commit hybrid time into this server's clock, so a reading taken after it
  is at or above the commit.

  Deviation from the plan, which has pggate record and send the commit hybrid time. No commit
  hybrid time is exposed to the client today (`client::CommitCallback` is
  `void(const Status&)`), and the tserver session already knows when its own DDL committed, so the
  floor lives there and no protocol or PG change is needed.
- Read time: on a catalog snapshot's first local read the plain session's read point is set to
  `ReadHybridTime::SingleTime(C)`, which the session then saves into its read point history under
  that serial, so every later read under the same snapshot restores the same time. A snapshot
  whose read point already holds a time at or below C is read at that exact time. A snapshot whose
  time is above C had its first read served by master, from the tserver clock; that operation is
  routed to master rather than read locally at an earlier time, which would move the snapshot
  backwards.

  Deviation from the plan (finding 2): no side map from serial to "locally sourced" is needed. C
  only moves forward, so a locally sourced time is always at or below C, and a clock-derived time
  is always above it.

## Phase 6 — lease hooks

- `YSQLLeaseManager::Impl::CheckLeaseStatusInner` calls `OnLeaseLost()` when the lease expires,
  which moves the copy to `kDisabledLease`, increments
  `local_catalog_serving_disabled_lease`, and takes it out of the read path.
- `ProcessLeaseUpdate` calls `OnLeaseGained(clock()->MaxGlobalNow())` on a new lease epoch.
  Serving resumes only once the poller has applied a response whose safe time is at or above that
  floor. `MaxGlobalNow` is an upper bound on every hybrid time in the cluster at that instant, so
  it bounds the commit of every DDL master completed while the lease was gone.

  Deviation from the plan, which wants master's own lease grant hybrid time in the refresh
  response. That field does not exist and the plan lists adding it as a follow-up; the local upper
  bound is correct and only costs the copy at most one extra poll before it resumes serving.

## Addendum handling: initdb, YSQL upgrade, major version upgrade, explicit read times

`implementation2.md` gained an addendum on these while the phases were being written, and the user
overrode one of its decisions.

- initdb: the copy is not used, as the addendum requires, and no pggate change was needed.
  `YBCIsLegacyModeForCatalogOps()` already returns true when `YBCIsInitDbModeEnvVarSet()`
  (`src/yb/yql/pggate/ybc_pggate.cc:2084`), and `is_catalog_snapshot` is set only on the
  non-legacy path (`pg_session.cc:943`), so an initdb-mode read is never eligible for the
  redirect. The poller additionally refuses to create the copy until master answers
  `IsInitDbDone`: initdb writes the catalog from master's own embedded PG and the initial system
  catalog snapshot restore replaces the tablet's files wholesale, so a copy taken earlier would
  hold a half-built catalog whose files are then swapped underneath it.

  The same legacy-mode condition also covers `YBCIsSysTablePrefetchingStarted()`, so the catalog
  preload path is never served from the copy. That is a real limitation of the feature as
  specified, not an oversight: the redirect is defined on catalog snapshot reads, and prefetching
  is a separate mechanism with its own session kind.

- YSQL upgrade (minor): a migration session writes catalog rows outside a DDL with
  `yb_non_ddl_txn_for_sys_tables_allowed` set, so like a DDL backend it can hold uncommitted
  catalog rows that exist only on master. `TryServeCatalogReadsLocally` routes such a session's
  reads to master and counts them as in-DDL. Ordinary sessions on the same cluster keep being
  served locally; the migrations' version increments reach them through the version row and the
  gate like any other DDL's.

- Major version upgrade: postponed on the user's instruction, against the addendum's rules. The
  user's reason: the upgrade window has two catalog version tables in play, and DDLs such as
  `REFRESH MATERIALIZED VIEW CONCURRENTLY` do run inside it. Nothing was implemented for it. Two
  properties the code has anyway limit the damage if the flag is on during such an upgrade: an
  upgrade-process session sets `yb_non_ddl_txn_for_sys_tables_allowed` and so reads master, and a
  read of a cotable the copy's metadata does not yet hold is routed to master by
  `AllOpsAreReadsOfLocalCatalogTables`. Neither is tested, and the combination is not claimed to
  work.

- Explicit read times (`yb_read_time`, `ysql_dump --read-time`): the read point already carries the
  caller's h, and the rule "serve at h when h <= C, otherwise master, never clamp" is the same
  comparison the redirect already makes. The missing half was history retention: a read below the
  copy's retention cutoff now routes to master instead of failing, because
  `PrepareLocalCatalogRead` registers the read time with the retention policy and its
  `SnapshotTooOld` is treated as "not servable here".

## Bring-up: defects found by running phase 7's bootstrap test

The test suite is `src/yb/yql/pgwrapper/pg_local_catalog-test.cc`, on an `ExternalMiniCluster`
with three tservers rather than `PgMiniTestBase`: `PgMiniTestBase` starts PG on one tserver only,
and the cross-tserver tests need a PG backend per tserver. That fixture reads counters over the
tserver metrics endpoint, so two gauges were added for what a separate process cannot otherwise
see: `local_catalog_serving_state` and `local_catalog_safe_time_micros`.

Defects fixed, in the order the runs exposed them:

1. `RaftConfigPB::set_opid_index` does not exist; the field is `committed_op_index`. Dropping it
   entirely was wrong: `VerifyRaftConfig(COMMITTED_QUORUM)` requires a committed config to have it
   set (`src/yb/consensus/quorum_util.cc:183-189`), so the consensus metadata flush failed with
   "Committed configs must have opid_index set". Both single-peer configs now set it to
   `kInvalidOpIdIndex`, matching `SysCatalogTable::SetupConfig`.

2. The copy's tablet peer was constructed with a default-constructed mark-dirty callback, on the
   reasoning that a tablet outside `tablet_map_` has nothing to report. Consensus invokes that
   callback as soon as it starts ("Calling mark dirty synchronously for reason code
   CONSENSUS_STARTED", `src/yb/consensus/raft_consensus.cc:3802`), and an empty `Callback` is a
   null dereference: every tserver took a SIGSEGV the moment the copy's consensus started. The
   callback is now a live no-op.

3. Tablet bootstrap replayed master's entire initdb WAL -- 27644 operations across seven
   segments, about five seconds in fastdebug -- even though the copied SSTs were already flushed
   to the last op. The cause is `bootstrap_retryable_requests`: with it on, the optimizer refuses
   to start at the flushed op id and walks back to the earliest retained segment. The copy serves
   no client requests, so it has no retryable requests to rebuild; it is now off for the private
   copy.

4. Three tservers remote bootstrapping the same master tablet at once collide on that tablet's
   rocksdb checkpoint lock ("Unable to acquire checkpoint lock",
   `src/yb/tablet/tablet_snapshots.cc:1238`). This is not a defect in the copy: the poll retries
   and the tservers serialize. The retry interval while the copy does not yet exist was raised to
   `local_catalog_bootstrap_retry_delay_ms` so that waiting for initdb and for the checkpoint lock
   does not produce a warning per 100 ms.

## Bring-up: the read redirect

The first run with the redirect in place served nothing: `local_catalog_reads_served` stayed 0
while `local_catalog_reads_to_master_not_serving` grew, which the counter dump in the test
attributed to the read-time branch rather than to any eligibility check.

Cause. `PgClientSessionImpl::UpdateReadTime` runs inside `SetupSession`, before the redirect's hook,
and for a catalog snapshot with an empty read point it takes the time from this server's clock
(`SetCurrentReadTime(ClampUncertaintyWindow::kTrue)`, `pg_client_session.cc:3793`). A clock reading
is always above C, so the hook's "the snapshot already has a time above C, an earlier read must
have used it, route to master" branch fired on the snapshot's *first* read, every time.

Fix. `UpdateReadTime` now records, in `catalog_read_time_picked_this_request_`, that the time on the
read point came from this request rather than from an earlier read under the same snapshot. The
redirect replaces the time with `SingleTime(C)` in exactly that case and in the empty case, and
keeps it otherwise. The flag is reset at the top of `UpdateReadTime`, so it describes only the
request being served.

The clock pick was left in place rather than suppressed for eligible requests: it is what stops a
catalog read that does go to master from being answered below the session's own just-committed
DDL, because master's system catalog tablet safe time is held back by concurrent in-flight catalog
writes (the reasoning recorded at `pg_session.cc:930-940`).

Order of operations in the hook was also changed so that a request that ends up going to master
still carries the read time it arrived with: the candidate time is computed, the copy's history
retention is checked with `PrepareLocalCatalogRead`, and only then is the read point written.

## Bring-up: the per-operation version wait does not fire in the normal path

Phase 7 test 3 as specified asserts that the session that ran a DDL waits for its own tserver's
poller before its next catalog read. It does not, and the reason is the design working: the
exposure gate runs in the lock-release handler on *every* tserver, including the one that ran the
DDL, so C has already passed the time master read the new versions at before the DDL's commit
returns to the client. By the time the session issues its next read, `A[db]` is already at V.

The test asserts what actually holds -- the read is served locally and returns the session's own
changes -- and no longer asserts that the counter is untouched, since the wait became reachable
once the version started being sent; see "The version wait for cache-miss reads" below, which
supersedes this entry's conclusion that the wait is only a safety net.

## Bring-up: full-suite run, four defects

Sixteen of the twenty tests passed on the first full run. The four failures were:

1. `local_catalog_poll_failures` did not count injected failures. `TEST_local_catalog_fail_poll`
   returned before the counter was touched, so the test that asserts "a failed poll is counted and
   C does not move" could see the failures in the log but not in the metric. The injection now
   increments the counter like any other poll failure.

2. The poll position was persisted as a file inside the tablet metadata directory.
   `FsManager::ListTabletIds` treats every file in that directory as a tablet id, so on restart
   the tablet manager tried to load `local_catalog_checkpoint` as a tablet superblock and the
   tserver died with "Could not init Tablet Manager: Failed to open tablet metadata for tablet:
   local_catalog_checkpoint". The file now lives in the fs root instead.

3. `RolledBackSubtransactionIsNotVisible` needed `ysql_yb_enable_ddl_savepoint_support`: without
   it PG refuses to interleave a SAVEPOINT with a DDL in one transaction block, so the scenario the
   test exists to exercise could not be set up.

4. Phase 7 test 4/9's premise does not hold in PG. A repeatable-read transaction does not keep one
   catalog snapshot across statements: accepting invalidation messages drops the catalog snapshot,
   so a table another session creates mid-transaction becomes visible to a later
   `SELECT count(*) FROM pg_class` in the same transaction -- the run showed 391 rows before and
   393 after, the new table plus its primary key index. Stability across statements is therefore
   not a property the copy owes.

   The test was rewritten as an equivalence test, which is the property that matters: the same
   sequence is run twice, once answered from the copy and once with
   `TEST_local_catalog_disable_serving` set so master answers it, and the row-count deltas the two
   runs observe must be equal.

## The version wait for cache-miss reads: the gap, and how it was closed

Plan §5.3 step 2 has the tserver wait for `A[db] >= V` before answering a catalog read, where V is
the version the read op carries. As first implemented, that wait never ran for the read class this
feature exists to serve, because the op carries no version.

The evidence chain, all in the PG fork:

- PG fills `ysql_db_oid` and `ysql_db_catalog_version` on a read op in exactly one place,
  `YbSetCatalogCacheVersion`, and its call site is guarded by
  `if (!(is_internal_scan && IsSystemRelation(table)) && ...)`
  (`src/postgres/src/backend/access/yb_scan/yb_scan_core.c:3701`).
- A catcache miss and a relcache build both reach DocDB through `systable_beginscan`, which
  dispatches to `ybc_systable_beginscan` (`src/postgres/src/backend/access/index/genam.c:422`),
  which reaches `ybc_systable_begin_default_scan` and passes `is_internal_scan = true`
  (`src/postgres/src/backend/access/yb_scan/yb_catalog_scan.c:354`). The table is a system
  relation, so both halves of the guard hold and the version is not sent.
- The guard's own comment says the same thing and calls it known looseness: "For tighter
  correctness, it should be sent for syscatalog requests, but this will result in more cases of
  catalog version mismatch. TODO(jason): revisit this for #15080."

Until this was closed, the invariant rested on the exposure gate alone. A backend gets version V
only from shared memory, and no path publishes V there before C has passed the hybrid time master
read V at: the lock-release handler waits, and the heartbeat path skips the response's version
data. The DDL's own backend is not an exception, because master sends the release to every tserver
with a live lease including that one
(`src/yb/master/object_lock_info_manager.cc:817`) and the handler is synchronous, so the DDL's
commit cannot return before its own tserver's copy holds it.

That argument holds, but it left a designed safeguard inoperative, so the version is now sent.

The field the op would have carried is also the field the tserver validates in
`CatalogVersionChecker` (`src/yb/tserver/read_query.cc:408`), so widening its use would have turned
the version check on for every internal system-relation scan -- the "more cases of catalog version
mismatch" the guard defers, and on the feature-off path too. The version therefore travels in a
separate pair of fields that is never validated:

- `PgPerformOptionsPB.ReadTimeOptionsPB.backend_catalog_version` and
  `backend_catalog_version_db_oid` (`src/yb/tserver/pg_client.proto`).
- A new `YbcPgCallbacks` entry, `GetLocalCatalogVersion`, returns
  `YbcPgLocalCatalogVersion{db_oid, version}` from PG. `YbGetLocalCatalogVersion`
  (`src/postgres/src/backend/utils/misc/pg_yb_utils.c`) reports `yb_catalog_cache_version` with
  `MyDatabaseId`, and leaves the oid invalid outside per-database catalog version mode, where the
  version is global and cannot be compared against the per-database versions the copy tracks.
- `PgSession::SetupPerformOptions` sets both fields for a catalog snapshot request only
  (`src/yb/yql/pggate/pg_session.cc`).
- `TryServeCatalogReadsLocally` waits on those fields instead of walking the ops.

The db oid is carried explicitly rather than taken from the tserver session's own `database_oid_`:
that member is populated from `options.namespace_id()`, which pggate derives from the operations'
relations and which skips `template1`, so a request that reads only shared catalogs can leave it
unset.

Two tests cover it. `ReadWaitsForAnUnappliedVersion` freezes one tserver's poller, raises the
catalog version with a DDL from another tserver -- whose own gate is satisfied there, so the commit
returns -- and asserts that a read on the frozen tserver blocks on the version instead of being
answered from the stale copy, then returns the right row once the poller is released.
`OwnTserverDdlServedLocally` keeps asserting that the wait is not needed in the normal path,
because the gate has already done the work.

## Phase 7 status, as of the first full run

`pgwrapper_pg_local_catalog-test`: 20 of 20 tests pass on a three-tserver `ExternalMiniCluster`
with object locking, concurrent transactional DDL, invalidation messages and
`enable_local_tserver_catalog` all on.

Two plan items had no test at this point: test 14, the forced re-seed on `CHECKPOINT_TOO_OLD`, and
phase 6's lease loss. Both were written afterwards and the second of them found a real defect; see
"Phase 7: final test inventory" below for the list that is current, which supersedes the
test-by-test table this section used to carry.

## Bring-up: a real gap in the lease-loss rule, found by writing the phase 6 test

Writing the lease test exposed a hole in the phase 6 implementation, not just in the test. On lease
loss the replica moved to `kDisabledLease` but set no floor, and `OnSafeTimePublished`'s
`kDisabledLease` branch reads "no floor, or C is past it, therefore serve again". With no floor,
the very next poll -- 100 ms later, while the lease was still gone -- put the copy straight back
into the read path. The counter and the log said the lease loss had been handled; the state said
otherwise.

`OnLeaseLost` now sets the floor to `HybridTime::kMax`, a time C can never reach, so the copy stays
out of the read path for as long as no new lease has been granted. `OnLeaseGained` replaces it with
`clock()->MaxGlobalNow()` at the moment the new epoch is observed, which is the bound above every
DDL master could have completed during the gap, and the first poll to pass that bound restores
serving. The test asserts the copy stays non-serving across ten polls while the lease is gone,
which is what caught it.

Two more test-side corrections in the same round: `master_ysql_operation_lease_ttl_ms` is a master
flag and was being passed to tservers as well, which made every tserver refuse to start; and the
lease-loss path kills this tserver's PG backends and a new lease epoch restarts the postmaster, so
the post-regain connection has to be retried until PG accepts again.

## Phase 7: final test inventory

`src/yb/yql/pgwrapper/pg_local_catalog-test.cc`, 27 tests, all 27 passing in one run, on a
three-tserver
`ExternalMiniCluster` with object locking, concurrent transactional DDL, invalidation messages and
`enable_local_tserver_catalog` all on.

| Test | Plan item |
|---|---|
| `BootstrapReachesServing` | 12: the copy serves on every tserver, with a poll and an apply actually recorded |
| `CatalogVersionRowsMatchMaster` | phase 2's A[db] check, as an equivalence test against master |
| `ColdBackendServedLocally` | 1: cold backend, warm copy |
| `CrossTserverDdlVisibleImmediately` | 2: a DDL on T1 visible to a backend on T2 the moment it returns |
| `OwnTserverDdlServedLocally` | 3, corrected: the gate already made the copy current, so no wait |
| `InDdlTransactionBlockReadsMaster` | 5 |
| `TemporaryRelation` | 6: the own-writes floor |
| `UserCatalogReadMatchesMaster` | 4 and 9, corrected to an equivalence test against master |
| `InternalCatalogReadsCheckConstraint` | 7 |
| `RolledBackSubtransactionIsNotVisible` | the aborted-subtransaction set in apply records |
| `NewDatabaseCotablesReachTheCopy` | schema-change records for a new database's cotables |
| `NewIndexIsUsedByAnotherTserver`, `NewIndexIsUsedBySameSession` | the in-place `pg_class.relhasindex` update |
| `SharedCatalogReachesOtherTservers` | shared catalogs and the global-DDL version bump |
| `ViewDefinedOnAnotherTserver` | pg_rewrite rules resolved from the copy |
| `TypeAndMissingValueAcrossTservers` | pg_type and `pg_attribute.attmissingval` |
| `PollFailuresDoNotMoveSafeTime` | 15 |
| `CheckpointTooOldReseeds` | 14 |
| `GateHoldsDdlWhilePollerIsPaused` | phase 4's gate, and 16 |
| `RestartResumesWithoutReseed` | 13 |
| `LeaseLossTakesTheCopyOutOfTheReadPath` | phase 6 |
| `ServingDisabledRoutesToMaster` | the routing rule and its counter |
| `NonDdlSysTableSessionReadsMaster` | the YSQL upgrade rule from the addendum |
| `MasterCatalogReadsDropWhenServing` | 1's other half: master's catalog read count actually falls |
| `ReadWaitsForAVersionPublishedWhileNotServing` | the version wait, on the path the gate does not cover |
| `VersionIncrementingDdlDoesNotPayTheOwnWritesFloor` | the own-writes floor is skipped when a version moved |
| `CatalogMissInTransactionBlockAfterAWrite` | pins the distributed-transaction limitation |

Added after the first full pass, to close the two items that had been left without coverage:

- `CheckpointTooOldReseeds` (phase 7 test 14). The `CHECKPOINT_TOO_OLD` the poll would get once
  master has garbage collected WAL the copy never consumed is injected with a new test flag,
  `TEST_local_catalog_poll_checkpoint_too_old`, rather than produced by shrinking master's
  retention: shrinking it far enough to lose a stopped poller's position also disturbs every other
  tablet in the mini cluster. The injected error takes the same branch, so the test drives the real
  re-seed path and then asserts the fresh copy holds the pre-re-seed rows and serves a DDL that
  landed after it.
- `LeaseLossTakesTheCopyOutOfTheReadPath` (phase 6). Lease loss is forced by turning off this
  tserver's lease refresh RPCs, and the test asserts the copy stays out of the read path across ten
  polls, that a DDL from another tserver still commits and is correct meanwhile, and that after the
  lease is regained the copy serves again and holds that DDL.
- `CatalogVersionRowsMatchMaster` (phase 2's A[db] check, expressed from outside the tserver). The
  version rows the copy holds must be the rows master holds: the same
  `SELECT ... FROM pg_yb_catalog_version` is answered from the copy and then, with the copy out of
  the read path, by master, and the two results must be byte-identical.

## Regression runs

Both of these ran before the target-mode defaults flip described below, so they exercised the
feature-off path in fastdebug's previous mode (object locking and transactional DDL blocks off).
The local-catalog suite sets the three flags explicitly in its own fixture, so the flip should not
change it; the confirming re-run on the final tree is recorded at the end of this log.

- `pgwrapper_pg_catalog_perf-test` with the flag off: 35 of 35 pass. This is the test that covers
  the catalog read path, the response cache and the master read-RPC counts, so it is the one that
  would notice the shared code the redirect touches (`UpdateReadTime`, `SetupSession`, the response
  cache setter) having changed behaviour when the feature is off.

## What is not done

- Master WAL retention for the per-tserver streams. Nothing was added: the copy relies on master's
  ordinary log retention (`log_min_seconds_to_retain`, 900 s by default), which at a 100 ms poll
  interval is four orders of magnitude of headroom, and falling outside it re-seeds. Automating a
  retention policy per stream is the plan's own follow-up item.
- Major version upgrade, per the user's instruction. The guard is in (serving is suspended while
  master reports an upgrade in progress) but no upgrade-mode behaviour is designed or tested.
- The load study (a standalone poll client at N=1000 streams) and the eager-apply optimisation
  (carrying the DDL's WAL records in the lock-release message), both listed in the plan as
  follow-ups.
- Java regress schedules with the flag on (`yb.orig.schema`, `yb.orig.select`, `yb.orig.guc`,
  `yb.orig.query_consistent_snapshot`, `TestPgRegressPgTable`). These are Java tests and each
  schedule is a long run; not attempted in this session.

## Known rough edges in what is done

- `LocalCatalogReplica::applied_version` and the two wait functions take one mutex per call, and
  the redirect calls `applied_version` once per operation. That mutex is on the catalog read path
  of every backend on the tserver. It is uncontended in the common case (the poller holds it only
  briefly, once per poll) but it is a shared lock where a lock-free read would do.
- Three tservers remote bootstrapping the copy at cluster start serialise on the master tablet's
  rocksdb checkpoint lock, so the last one waits for the other two. With the retry interval at
  `local_catalog_bootstrap_retry_delay_ms` this costs a second or two per tserver at startup.
- The version-wait timeout fails the request rather than falling back to master. A timeout means
  the client's own deadline was about to expire anyway, but routing to master would be the more
  forgiving behaviour.

## Target-mode defaults for every build flavor

On the user's instruction, the three flags the design requires now default to on in every build
flavor, removing the release/debug divergence the plan called out as the source of mode-dependent
test failures:

- `ysql_yb_ddl_transaction_block_enabled` (`kEnableDdlTransactionBlocks`) was true only under
  `NDEBUG`; the `#ifdef` is gone in both `src/yb/common/common_flags.cc` and its copy in
  `src/yb/yql/pggate/util/ybc_guc.h`.
- `enable_object_locking_for_table_locks` (`kEnableObjectLockingForTableLocks`) followed the same
  `#ifdef`; it now follows `kEnableDdlTransactionBlocks` unconditionally.
- `ysql_enable_concurrent_ddl` defaulted to false in every flavor and now defaults to true. It is a
  preview flag, but a preview flag left at its default needs no
  `allowed_preview_flags_csv` entry (`src/yb/util/flags/flags.cc:455`).

Every validator these flags participate in still holds with the new defaults: concurrent DDL
requires object locking, object locking requires transactional DDL blocks and invalidation messages
(true by default) and a nonzero `refresh_waiter_timeout_ms` no larger than
`master_ts_rpc_timeout_ms` (both 30000 in every flavor), and the object-lock fastpath requires
shared-memory PG client (both true off Mac).

This changes the mode debug and fastdebug builds run in, so a broad test impact across the existing
suites is expected; that is what the Jenkins run is for.

## Published

- Local commit `5eea0006ec8` on branch `docdb_local_catalog`, one commit on top of
  `94f5a6cd7dd`.
- `./build-support/lint.sh --rev HEAD^`: 0 errors, 0 warnings. Three lint findings were fixed
  first: two over-long lines, and the include order in `pg_client_session.cc` and
  `heartbeater.cc`, where the new headers had been put above the system headers instead of in the
  `yb/tserver` group.
- Draft revision https://phorge.dev.yugabyte.com/D57801, then updated with the message
  `trigger jenkins` to start the unit test run.

`arc diff` needs an editor that actually writes the message file: with `EDITOR=true` it treats the
unmodified template as an abort, and it opens the editor even with `--verbatim` and with `-F`. The
message handed to it drops the `Claude-Session:` trailer and turns `Upgrade/Rollback safety:` into
a `##` heading, because arc reads any line of the form `Name:` at column zero as a Differential
field and rejects the ones it does not know. The commit message itself keeps the trailer.

`worklog2.md` is deliberately not committed: it is this session's execution log, not part of the
change under review.

## Peer review of D57801: what was acted on and what was declined

A review from the session that owns the plan raised eleven items. Ten were acted on. One was
declined, and the reason is a fact about the code that the review had wrong.

### Item 1, declined and then withdrawn by the reviewer

The reviewer confirmed the `session.cc:305-307` fact independently and withdrew the item.

The review's reasoning was that the exclusion is unnecessary, because a catalog op runs under the
catalog snapshot serial rather than the transaction's read point. That is not how the read point is
addressed here. `YBSession::read_point()` returns the *transaction's* read point whenever a
transaction is attached to the session (`src/yb/client/session.cc:305-307`:
`return transaction ? &transaction->read_point() : non_transactional_read_point.get()`).

So with the exclusion removed, the redirect's two branches both do damage:

- On the snapshot's first read it would call `session.SetReadPoint(SingleTime(C))`, replacing the
  distributed transaction's data snapshot with a catalog hybrid time. Every later user-table read
  in that transaction would run at C.
- On a later read it would find the transaction's read time on the read point and, if that time is
  at or below C, answer the catalog read at the transaction's snapshot -- showing the catalog as of
  the transaction's start rather than as of the version the backend has been told.

The observation underneath the item is still correct and worth having: because
`BeginTransactionIfNecessary` attaches a plain-session transaction as soon as the isolation level
leaves NON_TRANSACTIONAL, every catalog miss after a write in a transaction block goes to master.
Closing that needs the catalog read time held somewhere other than the session read point, which
is a design change rather than a check removal, so it is a follow-up and not part of this change.
`CatalogMissInTransactionBlockAfterAWrite` pins the current behaviour under both REPEATABLE READ
and READ COMMITTED so that a later change has a test to flip, and the reason is now a comment at
the check itself.

### Acted on

- Item 2. The review is right that `ReadWaitsForAnUnappliedVersion` as I wrote it could not pass:
  pausing tserver 0's poller also blocks tserver 0's release-gate acknowledgement, so the DDL on
  tserver 1 would never have returned -- which is exactly what
  `GateHoldsDdlWhilePollerIsPaused` demonstrates. Replaced by
  `ReadWaitsForAVersionPublishedWhileNotServing`, which disables serving *and* pauses the poller so
  the gate publishes the version without waiting, then re-enables serving with the copy still
  frozen. That is the "release acknowledged while the copy was not serving" path.
- Item 3. The `backend_catalog_version` plumbing is committed rather than left uncommitted, and for
  the reason the review gives, which is stronger than the one I had: versions published while the
  copy is not serving are ungated, so the first applied response that flips the state to kServing
  can carry an S below the read time of a version a backend already holds. The request-level wait
  is the only thing that closes that, so it is required. The stale "clamps the versions" wording on
  `master_heartbeat.proto` field 34 is corrected to describe the skip.
- Item 4. The own-writes floor was being paid after every DDL, costing that session's next catalog
  read up to a poll interval, because the floor is a clock reading taken after the release round
  trip and is therefore normally above C. It is now skipped when the backend's catalog version has
  moved past what it was when that transaction committed: the transaction did increment a version,
  so the version wait covers it and the gate has already pushed C past that version's read time.
  This is computed on the tserver from the versions the session's own requests carry, so it needs
  no new PG signal, and it degrades conservatively -- an unknown prior version keeps the floor.
- Item 5. `TemporaryRelation` proved nothing about the floor, because `DISCARD PLANS` drops the
  plan cache and not the relcache, so the select could be answered from catcache entries the
  CREATE populated. It now runs a second no-increment DDL (`ALTER TABLE tt ADD COLUMN`) to
  invalidate the relcache entry, reads the new column and `pg_attribute`, and asserts the new
  `local_catalog_reads_waited_for_own_writes` counter fired.
  `VersionIncrementingDdlDoesNotPayTheOwnWritesFloor` asserts the other half of item 4.
- Item 6. A version wait or own-writes wait that times out now routes the read to master and counts
  under `reads_to_master_not_serving`, rather than failing the request. A stuck poller is a serving
  outage, which the design answers with master.
- Item 7. The local path returned before `RefreshHistoryRetentionPinFromSharedMemory()`, which sits
  at the tail of `DoPerform` (`src/yb/tserver/pg_client_session.cc:3520`), so a session served
  locally for a long time would have stopped refreshing its master-side history pin. It is now
  called before the local path returns.
- Item 8. Master kept one `StreamMetadata` per requesting tserver for the life of the process.
  `GetOrCreateStream` now drops entries idle for longer than
  `sys_catalog_change_stream_idle_timeout_sec` (one hour).
- Item 9. Confirmed, no code change. The real error comes from
  `PeerMessageQueue::ReadFromLogCacheForXRepl`, which attaches
  `CDCErrorPB::CHECKPOINT_TOO_OLD` to a NotFound status when the log position has been garbage
  collected (`src/yb/consensus/consensus_queue.cc:777`); the service maps it through
  `CDCError::ValueFromStatus` onto `resp.error().code()`, which is the field the poller branches
  on. The injected path and the real path converge on that branch. Asserting the code in the test
  would be circular, since the injection sets the code; the reseeds counter already shows the
  poller's branch fired.
- Item 11. Left as the recorded rough edge; `applied_version()` still takes the mutex per request.

### Item 10

The half worth having is done and is recorded under "The effect, measured" above: rather than bend
`pg_catalog_perf-test`'s exact-count assertions, the measurement lives in this feature's own suite
as `MasterCatalogReadsDropWhenServing`.

Both Java regress schedules pass with the flag on, so this item is closed.

| Schedule | Result |
| --- | --- |
| `TestPgRegressPgTable` | 3 tests, no failures and no errors, 1202 s |
| `TestPgRegressMisc#testPgRegressMiscSerial` | all 11 files of `yb_misc_serial_schedule` pass, 42 s of pg_regress |

The second schedule is the one that carries `yb.orig.schema`, `yb.orig.select`, `yb.orig.guc` and
`yb.orig.query_consistent_snapshot`. Its log shows all three tservers moving from `kBootstrapping`
to `kServing` at 08:07:00-08:07:01, before pg_regress reached its first query, and shows the poller
refusing to copy anything until initdb finished ("Waiting for initdb to finish before copying the
system catalog", `local_catalog_poller.cc:265`), which is the initdb exclusion behaving as
intended. The log records no per-read routing counts, so these runs establish that the target mode
does not regress with the copy in the read path; the routing counters in the C++ suite are what
establish that reads are actually served from the copy.

Neither run needed triage against the known-issues list, because neither had a failure.

The flag reaches both master and tservers
through `./yb_build.sh --extra-daemon-flags "--enable_local_tserver_catalog=true"`, which
`MiniYBCluster` adds to the flags common to both daemon types
(`java/yb-client/src/test/java/org/yb/minicluster/MiniYBCluster.java:241`). No test edit is needed
for the rest of the target mode, because the defaults flip above already turns object locking,
transactional DDL blocks and concurrent DDL on in fastdebug.

## The effect, measured

Phase 7 test 1 asks for the served counter to grow *and* master's catalog read count to stay
unchanged. The suite only ever asserted the first half, so the feature's whole purpose was
untested. `MasterCatalogReadsDropWhenServing` measures it: the same five-iteration cold-backend
query loop, counted through master's
`handler_latency_yb_tserver_TabletServerService_Read` total, costs

- 2 master catalog reads with the copy serving,
- 40 with `TEST_local_catalog_disable_serving` set.

An exact zero is not assertable and the test does not claim it: a fresh connection's own startup
work reaches master regardless, because authentication and the catalog preload path run on the
legacy catalog session, which `YBCIsLegacyModeForCatalogOps` keeps off the copy.

## Review round 2: the reopened transaction check, three new test groups, and the copy's retention

### The transaction exclusion is gone

`TryServeCatalogReadsLocally` no longer routes a request to master because a distributed
transaction is attached. The exclusion rested on the claim that pinning the session read point to
C would overwrite the transaction's data snapshot. It does write to that object -- with a
transaction attached `YBSession::read_point()` is the transaction's read point
(`src/yb/client/session.cc:305-307`) -- but the write is undone by machinery that already exists:
a catalog snapshot has its own read time serial number, and `SetupPlainSessionReadTime` saves
whatever time the object holds into the read point history under the outgoing serial before the
catalog serial becomes current (`src/yb/tserver/pg_client_session.cc:4281-4283`). pggate switches
the serial back after the catalog operations (`PgSession::RunAsync` -> `RestoreReadPoint`), which
restores the transaction's time from history. The existing clamp path already wrote a clock time
into the same object in the same window, so C is not a new kind of write.

`RepeatableReadDataSnapshotSurvivesLocalCatalogRead` is the test that would catch a leak: a
REPEATABLE READ transaction counts 10 rows, another session commits an 11th, the transaction
writes so that a distributed transaction is attached, its catalog miss is asserted served from the
copy, and the re-count must still be 10. It is 10, and 11 only after COMMIT.
`CatalogMissInTransactionBlockAfterAWrite` now asserts `reads_served` instead of the master
counter.

### The copy's history retention follows master's, not the user tablet rule

`TSTabletManager::AllowedHistoryCutoff` now branches on the copy's tablet id and returns
`Clock()->Now() - timestamp_syscatalog_history_retention_interval_sec` (4 h) in both cutoff
fields, which is the interval master applies to this same data
(`CatalogManager::AllowedHistoryCutoffProvider`, `src/yb/master/catalog_manager_ext.cc:3822`).
`SanitizeHistoryCutoff` combines provider and clock policy with a per-field minimum
(`src/yb/tablet/tablet_retention_policy.cc:227`, `src/yb/docdb/docdb_compaction_context.cc:1806`),
and the minimum of two hybrid times is the earlier one, so the 4 h value wins over the 15 min of
`timestamp_history_retention_interval_sec`. The generic path's namespace backfill, xCluster
safe-time lookup and snapshot-schedule cleanup are skipped for the copy, all three being
meaningless for a tablet master does not know about.

One premise offered for this change does not hold: the namespace backfill does not fail for the
copy and does not pin the cutoff at `kMin`. Master's system catalog metadata carries
`kSystemNamespaceId` (`src/yb/master/sys_catalog.cc:390`) and remote bootstrap copies it into the
copy's superblock (`src/yb/tserver/remote_bootstrap_client.cc:389`), so
`BackfillNamespaceIdIfNeeded` returns at its first check. A 40-minute suite log contains no
occurrence of its failure message. The retention mismatch was the whole of the gap.

### The checkpoint file may no longer lead the applied data

`PersistCheckpoint` waits for `tablet_peer_->log()->WaitUntilAllFlushed()` before recording a WAL
position. Master ships only what follows the recorded position, so a crash that lost the copy's
log tail while the file claimed those records were consumed would leave the copy permanently
short of rows it reports as present.

### New tests

| Test | What it pins |
| --- | --- |
| `OpenDdlBlockDoesNotBlockAnotherTserversDdl` | an open DDL block does not delay another tserver's DDL, and C passes that DDL's commit on every copy while the block is open |
| `OpenDdlBlockDoesNotBlockADdlOnTheSameTserver` | the same with both sessions on one tserver |
| `TwoOpenDdlsDoNotBlockAnotherTserversDdl` | the same with two uncommitted DDLs in the block |
| `ConcurrentDdlsFromTwoTserversConverge` | ten simultaneous DDL pairs from two tservers all return, are readable on a third, and every copy converges on master's catalog version |
| `ExplicitReadTimeRouting` | a read time below C is served locally; one above C goes to master |
| `ReadOlderThanUserTabletRetentionIsServedLocally` | the copy keeps system catalog history, not user tablet history |
| `ReadBelowTheCopysCutoffGoesToMaster` | the retention decline routes to master rather than failing the query |

Test 17 rests on master certifying a safe time without waiting for open transactions. A
transaction still open is reported PENDING and drops out of the resolver's set once the status
tablet's clock passes the resolve time
(`src/yb/tablet/transaction_participant.cc:1377-1379`); what does hold the response is a
transaction committed at or below the resolve time whose intents are not yet applied, in the 10 ms
re-poll loop at `:1453`.

Three things had to be learned the hard way while writing the read time tests, and they constrain
any future test of this area:

- `ASSERT_OK` expands to a `do`/`while` block and cannot carry a `<<` stream.
- "Above C" is not a property a wall clock reading keeps. C catches up to any such reading within
  a poll interval, so the case only exists while the copy is behind, and the test has to pause the
  poller to hold that condition still.
- A read time far enough in the past to fall below a 4 h cutoff also predates the cluster's
  catalog, and PG then fails in relation cache build before the redirect sees the read: the
  counters do not move at all. Retention has to be tested by bringing the cutoff forward
  (shortened intervals plus a small memtable so a flush publishes it), never by reaching further
  back.

`PollFailuresDoNotMoveSafeTime` read the safe time gauge before setting the failure injection flag
and asserted it had not moved, which raced the poll in flight during the `SetFlag` RPC. It now
reads the gauge only after a failure has been counted, and asserts across three more failures.
Three repetitions pass.

Suite state: 35 tests, all passing.

### Why master cannot delete the copy as an unknown tablet

Three independent reasons, each sufficient on its own. Recorded so the concern is settled.

1. **The copy is never reported.** An incremental report is built from `dirty_tablets_` and each id
   is resolved through `tablet_map_`, with misses reported only as removals
   (`src/yb/tserver/ts_tablet_manager.cc:3377-3400`); a full report comes from
   `GetTabletPeersUnlocked`, which is `tablet_map_` (`:3424`). The copy is erased from the startup
   id list (`:782`), `OpenOrCreateLocalCatalogTablet` never inserts it, its mark-dirty callback is
   the live no-op `IgnoreLocalCatalogTabletStateChange`, and the copy's fetch uses
   `InitRemoteClient` directly and so never calls `MarkTabletBeingRemoteBootstrapped`, which is
   the only writer of `tablets_being_remote_bootstrapped_` (`:1856`) -- the third set a full
   report folds in (`:3375-3380`).
2. **Master deletes an unknown reported tablet only if it once knew it.**
   `DeleteOrphanedTabletReplica` returns early unless
   `IsDeletedTabletLoadedFromSysCatalog(tablet_id)` holds, with
   `master_enable_deletion_check_for_orphaned_tablets` true by default
   (`src/yb/master/master_heartbeat_service.cc:98, 666-675`). The copy's id was never a master
   tablet, so the check takes the skip branch.
3. **The id is outside the space master can generate.** Master's tablet ids come from
   `GenerateObjectId`, which renders a `boost::uuids::random_generator` value as hex
   (`src/yb/util/oid_generator.cc:56-64`), so it is a v4 UUID whose version nibble is fixed at 4.
   `00000000000000000000000000000001` has a zero there and can never be produced, so no id-space
   reservation is needed.

Master also never acquires a record of the copy later: its `tablet_map_` is loaded from
`SysTabletsEntryPB` rows written by table and tablet creation, and nothing writes such a row for
this tablet. The remote bootstrap source service serves files without registering anything, and
the change service keys its state on the requesting tserver's uuid rather than on a tablet. The
classification would therefore be permanent, which is why exclusion is the design rather than
waiting for master to learn about it.

Two guard tests hold reason 1 and reason 2 in place:
`LocalCatalogCopyIsNotInTheReportableTabletSet` asserts the copy's id is absent from
`ListTablets` on every tserver, that being the observable form of "not in `tablet_map_`", and
`FullReportWithOrphanDeletionCheckOffLeavesTheCopyAlone` disables the orphan deletion check on
every master, restarts a tserver so that it sends a full report, and asserts the copy is still
serving and still answering reads afterwards.

One correction to an earlier note in this log: the consequence of reporting the copy would be
master ordering *the copy* deleted, not master's own system catalog tablet. The two are different
tablets -- `00000000000000000000000000000000` for master's
(`src/yb/master/sys_catalog_constants.h:39`) and `...0001` for the copy
(`src/yb/tserver/local_catalog_replica.cc:123`) -- and the copy only inherits the source's table
id and cotables, which is what makes PG catalog reads against it work.

## The copy's own raft group: three defects found on a live cluster and in the suite

Observed on a three-node yugabyted cluster and then reproduced in the suite. All three are fixed.

### The copy waited out a failure detection interval before it could apply anything

The copy is a raft group whose only member is itself, and it comes up as a follower. Nothing
elects a single-voter group on open, so the first thing that made it leader was the failure
detector: on the live cluster, `ReportFailDetected: Starting NORMAL_ELECTION` fired 2.37 s after
the peer started, and until then every apply was rejected with "The local catalog copy is not the
leader of its own single-peer group yet". The visible costs were `local_catalog_apply_failures` at
7 on a healthy node and 11 in a fresh-cluster test, and the copy answering nothing for the first
seconds after any restart. `TSTabletManager::MakeLocalCatalogTabletLeader` now elects it and waits
for `LeaderTerm() > 0`, because `StartElection` returns before the new term's no-op commits and
the first apply would otherwise still race it.

### Electing at open corrupted the resume position

Winning an election appends a no-op to the copy's own log, so
`log()->GetLatestEntryOpId()` -- which `LoadCheckpoint` uses when no checkpoint file exists --
returned `{term 2, index 609}`, a position in the copy's log rather than in master's WAL. Two
consequences, both observed:

- `OpId` orders by term before index, so the monotonic guard rejected master's `{term 1,
  index 612}` and every later checkpoint. The poller re-requested the same range forever: 2516
  identical applies of the same three records, C frozen, and `master has more to send` on every
  response.
- Master's index 609 was a real operation that the copy skipped, so the copy silently lacked a
  record it reported as present.

Because C was frozen while the serving state stayed `kServing`, the heartbeat gate withheld
catalog versions from that tserver indefinitely, and a `CREATE DATABASE` never became connectable
there: `Failed to find suitable shared memory index for db 16384`, a 60 s FATAL in
`NewDatabaseCotablesReachTheCopy`. Fixed by electing only after the poller has read its resume
position, and by comparing checkpoints on index alone -- master's WAL term changes on a master
leader election, so the term comparison was wrong regardless of the election change.

### Requiring a leader lease made the copy refuse reads after every restart

`PrepareLocalCatalogRead` waited on `SafeTime(RequireLease::kTrue, ...)`. A freshly elected leader
cannot satisfy that until the previous term's lease can no longer be held, bounded by
`leader_lease_duration_ms` (2 s), so the first catalog-missing query after a restart cost 1.30 s
against 0.05 s from master. The lease exists so that a deposed leader cannot answer reads a new
leader has already superseded; the copy's group has one voter and no other peer can ever hold that
lease, so the requirement bought nothing. With `RequireLease::kFalse` the same query costs
0.038 s, which is faster than master's 0.051 s. What the wait still guarantees is unchanged: the
in-flight local operations carrying master's records must drain before a read at C can see them.

Electing earlier only moved the lease window earlier; it did not remove this cost, and a rerun
after the election fix measured 1.220 s again. The two fixes are independent.

### Logging added for all of this

`--vmodule=local_catalog_poller=1` gives one line per poll that carried records, with counts by
the table each record belongs to (`pg_class`, `pg_attribute_relid_attnam_index`, and `sys.catalog`
for master's own metadata rows, which the copy applies but PG never reads). Level 3 logs every
record with its operation, hybrid time, table and doc key. `ts_local_lock_manager=1` logs a DDL
waiting in the exposure gate, with the target time and what the copy holds. `pg_client_session=1`
logs the version wait, the own-writes floor wait, and the hybrid time a catalog snapshot is pinned
to. The tablet safe-time wait in `PrepareLocalCatalogRead` logs at level 1 when it blocks for more
than a millisecond -- the wait that hid the lease problem until it was instrumented.

Suite state: 38 tests, all passing.

## Two questions answered against the code, both worth keeping

### How a stalled poller becomes a cluster-wide DDL problem

Nothing in the code today moves the serving state out of `kServing` because of poll or apply
failures. `OnLeaseLost`, a re-seed request and a major version upgrade are the only transitions
away from it. So any condition that freezes C while the copy stays serving produces the same
symptom, and there are several:

- Master leader change, or master unreachable from that one tserver. The live cluster showed these
  as `Failed to poll ...: Connection refused`; the serving state is untouched.
- Apply failures on the copy: the write timeout
  (`local_catalog_apply_write_timeout_ms`, 60 s), a full disk, a RocksDB error, or the
  leader-term race that was fixed above.
- A record the copy refuses on principle, such as the `SPLIT_OP` check, which fails every poll
  forever while the copy goes on serving.
- The poll thread blocked in one slow write, since there is a single poller per tserver.

While C is frozen and the state says serving, three things follow. The heartbeat gate withholds
db catalog versions from that tserver, so a new database never reaches its shared memory and
backends cannot connect to it -- observed as
`Failed to find suitable shared memory index for db 16384`. The release-lock gate on that tserver
times out after roughly 35 s, so DDLs fail for every session in the cluster, not only on that
node. Reads on that node keep being answered at the stale C until a version wait times out and
falls back to master.

Plan section 7.2's rule closes it: after repeated poll failures, or when C lags by more than a
threshold, take the copy out of the read path. The gate is then skipped for that tserver, the
heartbeat publishes versions again, and its reads go to master, which is the graceful degradation
the design intends. This is still unimplemented.

### Uncommitted DDL records are in the copy, and no read can see them

A DDL that has not committed does ship to the copy: the poller's records carry the transaction id
(`txn 0E87718F...` in the logs), and the write path stores them in the copy's intents DB as
external intents. What makes them invisible is the key layout rather than any filtering:
`CombineExternalIntents` stores them under `kExternalTransactionId` + the transaction id
(`src/yb/docdb/intent_format.cc:169-177`), a per-transaction key, so a read that seeks by doc key
never traverses them.

No transaction status lookup is involved, and none would help: the copy has no participant record
for master's transaction. The only thing that materialises those writes is an APPLY record from
master, which `NonTransactionalBatchWriter::PrepareApplyExternalIntents`
(`src/yb/docdb/rocksdb_writer.cc:1296-1330`) uses to rewrite the batch into the regular store at
the commit hybrid time and delete the intent. Until then a query returns the pre-DDL row, which is
what the uncommitted `ALTER TABLE` experiment showed.

Two details in that experiment's log confirm the mechanism from the other side: every
"Applied N records" line reports `transaction records: 0`, so no APPLY record had arrived, and one
poll in the middle of the open transaction reports `C is now <invalid>`, which is master
withholding the safe time while the transaction is pending rather than the copy losing its
position.

### The poller log now names the destination of every record

Reading a live DDL through the level 3 log left two things unanswerable: which records went to the
intents db and which straight to the regular store, and what an APPLY record carried. The routing
rule is in `XClusterWriteImplementation::ProcessRecord`
(`src/yb/tserver/xcluster_write_implementations.cc:193-236`) and is decidable from the record
alone, so the log now states it:

- `record.operation() == APPLY` -> an `apply_external_transactions` entry carrying the commit
  hybrid time and the aborted subtransaction ranges. This is the record that makes a transaction's
  writes readable. Logged as `APPLY of txn <id> committed at <ht>, aborted subtransaction ranges
  <n>` instead of the old, useless `on unknown`.
- `record.has_transaction_state()` -> `CombineExternalIntents`, so one write pair keyed by the
  transaction. Logged as `-> intents db, txn <id>`.
- neither -> direct write pairs stamped with master's own hybrid time. Logged as
  `-> regular db`. Master's own metadata rows, which PG never reads, take this path.

The per-poll summary counts them too: "writes: 34, deletes: 0, of which provisional (into the
intents db): 34, transaction records: 0". A poll whose provisional count equals its write count
carried nothing a reader can see yet.

Verified on `OwnTserverDdlServedLocally`: 34 provisional writes across `pg_type`, `pg_depend` and
their indexes, then `APPLY of txn F4904D3A... committed at ...`, then the versions in the summary
line move.

One thing this made obvious about the gate log: it lives in `ts_local_lock_manager`, so
`--vmodule=local_catalog_poller=3` alone will not show a DDL waiting for the copy.
`ts_local_lock_manager=1` has to be in the same vmodule list.

### A log for C moving, and hybrid times printed in both forms

There was no log for the safe time update itself. The poller's per-poll line mentioned C, but it
only prints for polls that carried records, and C advances on record-free polls as well, so the
copy could move forward silently. `LocalCatalogReplica::PublishAfterApply` now logs at the one
place the decision is made:

```
Safe time advanced 1788561700756929 (22:41:40.756929) -> 1788561700879008 (22:41:40.879008),
lag 0.014s, state kBootstrapping
Applied catalog versions moved: {db 13665: 0 -> 1}, {db 5: 0 -> 1}, {db 4: 0 -> 1}
```

The version line reports only the databases whose applied version actually moved, and a safe time
at or below the one already held is logged at level 2 rather than silently dropped, so a stuck
poller is visible as repeated "Ignoring a safe time at or below the one already held".

Both live under `--vmodule=local_catalog_replica=1`, which is a different module from the poller;
the fixture's vmodule list now carries `local_catalog_poller=3,local_catalog_replica=1,`
`local_catalog_read=1,pg_client_session=1,ts_local_lock_manager=1,heartbeater=1`.

Every hybrid time in these logs now prints as the physical microseconds followed by the time of
day, through `LocalCatalogHybridTimeForLog` (`src/yb/tserver/local_catalog_replica.cc`). The raw
microseconds are what compare against other hybrid times, the `local_catalog_safe_time_micros`
gauge and `yb_read_time`; the time of day is what lines up with the log's own timestamps. It uses
local time, matching the glog prefix, and renders an invalid time as `<invalid>` and a special one
by name, so `C is now <invalid>` still reads as before.

### A restart with a copy on disk now needs nothing from master, and waits for nothing

Two costs were paid on every restart that already had a copy:

- `EnsureLocalTablet` resolved the master leader and called `IsInitDbDone` before opening anything,
  although a copy on disk is itself proof that initdb had finished: the fresh-copy path refuses to
  run before it. The reuse decision now happens first, from local metadata alone
  (`TSTabletManager::CanReuseLocalCatalogTablet`), and the master round trips are made only on the
  fresh-copy path.
- `MasterLeaderPollScheduler` calls `IntervalToNextPoll` at the top of its loop, before the first
  poll (`src/yb/tserver/master_leader_poller.cc:119-122`), and that function returned
  `local_catalog_bootstrap_retry_delay_ms` (1 s) whenever no tablet was open yet. So a tserver with
  a copy in hand slept a second before its first attempt. The delay is a retry interval and now
  applies only from the second attempt on.

`PgLocalCatalogSlowRetryTest.RestartReusesTheCopyWithoutAskingMaster` holds both. Its fixture sets
`local_catalog_bootstrap_retry_delay_ms=60000`, so a regression that applies the interval before
the first attempt cannot pass unnoticed: serving would resume a minute later instead of at once.
The test asserts master's `IsInitDbDone` count is unchanged across the restart, no re-seed
happened, serving resumed inside 20 s, and the reopened copy answers reads. Measured: serving
resumed 0.227 s after `Restart()` returned.

### The fixture's log level had to come back down

Turning the poller's level 3 on for the whole suite was a mistake and it broke a test. Level 3
logs one line per applied row, which produced 170k such lines in a 540k-line suite log, and the
slowdown was enough that `LeaseLossTakesTheCopyOutOfTheReadPath` timed out after 120 s waiting for
the copy to leave the read path: the log showed all three copies still `state kServing` and
advancing safe time when the wait expired. With the fixture at
`local_catalog_poller=1` the same test passes three times in a row at 21 s each. The other modules
stay at level 1, which is one line per poll rather than one per row. Level 3 is for a single test
or a live cluster.

## Phase 8, in progress: change records carried in the lock release

The feature commit is `fdb5577087b`, amended to hold everything above with the review annotations
stripped, lint clean, and the revision deliberately not updated. Phase 8 is being built on top as
a separate change.

### Deviation from the step list: records need op ids, and had none

The plan has master fetch one change batch and slice it per target, and has each tserver check that
a pushed slice starts where its applied position ends. Neither is expressible against the wire
format as it stood: `CDCRecordPB` carries a hybrid time, an operation, keys and values, but no WAL
position (fields 1-15, nothing resembling an op id). The poller never needed one, because master
only ever sends it records after the position it asked from.

Added `optional OpIdPB op_id = 16` to `CDCRecordPB`, stamped in `GetChangesForXCluster`'s message
loop from the op id it already computes per message, and only when
`stream_metadata->IsSysCatalogChangeStream()` holds, so xCluster's wire size and behaviour are
untouched. One message can yield several records, one per row it wrote, and they all carry that
message's position.

### Step 1 and 2 are in

- `TSHeartbeatRequestPB.local_catalog_applied_op_id = 25`. The plan said the next free number after
  33; that is the *response*'s numbering, and the request's highest field was 24.
- The poller publishes its applied position to `LocalCatalogReplica` whenever the checkpoint moves,
  and also on load, so a restarted copy reports where it is before its first fetch. The heartbeat
  thread reads it under the replica's mutex; the field is left unset when the copy does not exist.
- `TSDescriptor::UpdateFromHeartbeat` stores the op id with the heartbeat's own timestamp, and
  `LocalCatalogAppliedOpId()` returns both, so master can tell a stale report from a current one.
  An invalid op id is the state of every tserver until it heartbeats a new master leader, which is
  exactly the case the plan wants to fall back to pulling.

Remaining: master's release fan-out (fetch once from the minimum reported position, slice per
target, cap the payload), the tserver's `ApplyPushedBatch` on the poller thread with the
index-only gap check, the counters and flags, and the Phase 8 tests.

### Phase 8 is implemented and its own tests pass

The DDL path no longer has to wait for a poll. Master fetches the change records once at release
fan-out time and each tserver's release request carries the slice above that tserver's own reported
position, so the copy reaches the release's catalog version read time by applying what arrived
rather than by polling.

Master side, in `UpdateAllTServers<Req>::PrepareLocalCatalogPushes`, run once before the release
RPCs go out and only for a release that carries catalog versions:

- Targets whose reported position is older than `local_catalog_release_push_report_max_age_ms`
  (5 s) are left to pull, which is the state of every tserver until it has heartbeated the current
  master leader. No fresh reports at all means no push for anyone.
- One in-process fetch from the lowest fresh position, through
  `SysCatalogChangeServiceImpl::GetChangesForRelease` on a stream of master's own keyed
  `master-release-push`, with `force_apply_safe_time` set: master is releasing a transaction that
  has just committed, and the 250 ms recomputation throttle would otherwise hand back a safe time
  from before that commit.
- A batch without a safe time is not sent at all, since there would be nothing for the receiver to
  publish.
- Per target, records whose index is above that target's reported position. An empty slice is
  still sent when the target is already at or past the batch's end, because applying nothing and
  publishing that safe time is exactly what lets its gate pass; an empty slice for a target that
  is behind the end is counted as skipped instead.
- Over `local_catalog_release_push_max_bytes` (1 MB), the target is sent nothing.

Tserver side, in the release handler before the gate wait: the payload is parsed, handed to
`LocalCatalogPoller::ApplyPushedBatch`, and applied on the poll thread, so pushed and pulled
batches never interleave. Records at or below the applied position are dropped, which is normal
because a report is up to a heartbeat old. The first retained record's index must equal the
applied index plus one -- index only, never terms, for the reason the poll checkpoint is also
index-based. A gap, a parse failure or an apply error is counted as a refusal, triggers an
immediate poll, and falls through to the gate wait, so nothing about correctness depends on a push
arriving.

Flags: `enable_local_catalog_release_push` (off), the payload cap, the report age bound. Counters:
`local_catalog_release_pushes_sent`, `..._skipped` and `..._push_bytes` on master;
`local_catalog_pushes_received`, `..._applied` and `..._refused` on each tserver.

| Test | Result |
| --- | --- |
| `ReleaseCarriesTheDdlsOwnRecords` | pass. With every poller paused, a DDL completed in 0.481 s and all three copies advanced only through what the release carried; a cold backend then read the new table from its copy. |
| `PushWithAGapIsRefusedAndPulled` | pass. |
| `OverTheCapMasterCarriesNothing` | pass. |
| `StaleReportGetsNoRecords` | pass. |
| `ConcurrentDdlsDoNotStormRefusals` | pass, with zero refusals across ten concurrent DDL pairs and every copy converging on master's version. |
| `NewMasterLeaderCarriesNothingUntilReportsArrive` | pass. |

Three test-construction facts worth keeping:

- Dropping a push cannot produce a gap. A copy that discards a batch never reports the advanced
  position, so master keeps slicing from where the copy actually is. A gap needs the *report* to
  run ahead of the copy, which is what a restart from a checkpoint file written before the last
  applies looks like; `TEST_local_catalog_report_applied_op_id_ahead` produces it.
- The gap is only reachable with the poller held off, because otherwise the poller closes it
  before the push arrives and the push is then a legitimate no-op. That is why
  `TEST_local_catalog_pause_poller` now stops only fetching: a pushed batch still applies while it
  is set, since the pause exists to stop the copy pulling and a release that carried records has
  its own caller waiting.
- A step-down needs more than one master. `PgWrapperTestBase::GetNumMasters()` returns 1, so that
  test has its own three-master fixture, and the master-side counters and flag changes address
  `cluster_->GetLeaderMaster()` rather than `cluster_->master()`.

`YB_TEST_LOCAL_CATALOG_PUSH=1` turns the push on for every test in the file, which is how the
suite is run for equivalence without duplicating fixtures.

### Equivalence with the push on, and a flake that is not Phase 8's

`YB_TEST_LOCAL_CATALOG_PUSH=1` over the whole file: 44 of 45 pass. The single failure is
`LeaseLossTakesTheCopyOutOfTheReadPath`, timing out after 120 s waiting for the copy to leave the
read path once the lease lapsed.

That failure is unrelated to the push, established by running the same test both ways under the
same load: with the push on, 2 of 3 repetitions pass and iteration 2 fails at 129 s; with the push
off, 2 of 3 pass and iteration 2 fails at 129 s. Identical. The host has 4 CPUs
(`NumCPUs determined to be: 4`) and the repeat runner starts 4 four-node clusters at once, on top
of the two yugabyted clusters already running, and the test depends on a 5 s lease TTL being
honoured. The flag flip itself is applied (`Changed flag
'TEST_tserver_enable_ysql_lease_refresh' from 'true' to 'false'` at the right moment), so what
fails is the detection of the lapse under CPU starvation rather than the test's setup.

Left as a known flake rather than papered over: it is a timing assumption in a test written in an
earlier phase, it reproduces without any Phase 8 code in the path, and 120 s at a 5 s TTL is 24
TTLs, so the fix is to understand why detection stalls entirely under starvation rather than to
lengthen the wait.

### Phase 8 review: two defects, four improvements

Reviewed by the peer session against plan2 section 12. Both defects were real and are fixed.

**A use-after-free in the handoff, worse than the review said.** `ApplyPendingPush` copied the raw
`PendingPush*` under the lock, released it, and applied; the waiter's timeout branch then cleared
the slot and returned, destroying the stack object while the poll thread was about to write the
outcome into it. The second dangling reference the review did not name: `PendingPush::batch` was a
pointer to the *release handler's* stack `batch`, so the apply itself read freed memory on that
path. Both are fixed by making the handoff jointly owned -- `PendingPush` holds a copy of the
batch, `pending_push_` is a `shared_ptr`, `ApplyPendingPush` moves it out of the slot under the
lock so the slot is free for the whole apply, and completion happens under the lock. The waiter's
timeout drops its reference and returns; the poll thread's own reference keeps the shared state
alive. `ApplyOutlivingItsCallerDoesNotCrash` holds it, using a test flag that sleeps inside the
apply for longer than the release deadline.

**The contiguity check refused legitimate slices.** `GetChangesForXCluster` emits no record for a
`NO_OP`, a `HISTORY_CUTOFF_OP`, or anything its switch does not handle, so requiring the first
record's index to equal the applied index plus one made every DDL preceded by such an operation
fall back to a poll -- and master's system catalog propagates history cutoffs regularly. What
establishes that nothing is missing is the position master sliced from, not where the first record
sits: master fetched everything above it, so every record between it and the batch's end is in the
slice. `GetSysCatalogChangesResponsePB.slice_from` (field 4) now carries that position, set only
when master carries a batch on a release, and the tserver refuses only when it is above its own
applied position, which means the report ran ahead of the copy. Master sends the empty slice with
it rather than counting it skipped. `RecordlessOpsDoNotRefuseTheSlice` produces exactly this shape
with a master step-down, whose new leader writes a no-op between the copies' positions and the
DDL: all three tservers apply the push and none refuses. Under the old rule all three would have
refused.

The four smaller items: a lag cap (`local_catalog_release_push_max_lag_ops`, 1000) so one tserver
with fresh heartbeats but a stalled poller cannot drag the fetch back and leave master carrying
nothing to anyone; a pending push failed at once when a re-seed has taken the tablet away, instead
of the caller waiting out its deadline; a master histogram
(`local_catalog_release_push_fetch_us`) around the in-process fetch, which runs synchronously on
the release path; and "no fresh report" now counted as skipped, which makes the assertion in
`StaleReportGetsNoRecords` mean something. `ConcurrentDdlsDoNotStormRefusals` is tightened from
`2 * kIterations` to 2, and the observed number is 0.

Writing the straggler test exposed something about the product rather than the test. Pausing a
poller on a copy that is still serving makes that copy's gate block every release, so a loop of
DDLs took 35 s each with master retrying, and the test hung. That is pre-Phase-8 behaviour and
exactly the plan section 7.2 gap: a stalled but serving copy holds up DDL cluster-wide. The test
therefore also disables serving on the straggler, which takes its gate out of the path and leaves
only the lag cap under test. What the test proves is that master drops the straggler and the others
still get their pushes; it does not reproduce the truncated-batch pathology that motivated the cap,
which needs far more lag than a handful of DDLs.

Suite: 48 tests. Push off, 47 pass; push on, 47 pass. The remaining failure is
`LeaseLossTakesTheCopyOutOfTheReadPath`, the load-sensitive flake recorded above.
`ReadBelowTheCopysCutoffGoesToMaster` also failed once in these runs, for the reason its own
comment predicted -- the shortened cutoff is only published by a flush, and a 1 MB memtable does
not fill reliably from catalog writes alone. Adding `--db_write_buffer_size=65536` to that fixture
makes the flush happen after a handful of DDLs; three repetitions pass.

### The lease-loss test was removed rather than repaired

`LeaseLossTakesTheCopyOutOfTheReadPath` is gone, and with it the fixture's short lease settings.
Two reasons, the first being the one that matters: lease expiry kills that tserver's PG sessions
(`YSQLLeaseManager::Impl::CheckLeaseStatusInner` calls `OnLeaseLost()` and then `KillPg()`,
`src/yb/tserver/ysql_lease_manager.cc:296-302`), so with no session left to read from the copy,
"the copy left the read path" was asserted on a state counter rather than on anything a client
could observe. The second is that it was the suite's only flaky test, timing out after 120 s on a
loaded 4 CPU host with identical results whether Phase 8's push was on or off.

Removing it also let `master_ysql_operation_lease_ttl_ms=5000` and
`ysql_lease_refresher_interval_ms=500` go from the fixture. Their comment said they existed only
so that this test would not have to wait out the default TTL, and a 5 s lease under load was a
standing source of fragility for every other test in the file.

What should replace it is recorded as a TODO in implementation2.md: a tserver partitioned from
master long enough to lose its lease and to miss a DDL, then rejoining, with its copy withholding
reads until it can prove it holds what it missed, asserting where the reads went rather than a
counter.

### CI on Phorge: the first run died in the rebase step, and why the flag defaults left the change

The 2026-09-03 CI run of D57801 (diff 314667, the 44-file state) did execute the suites, 13.0k to
13.8k items per lane, and failed 16 tests on `alma8-clang21-release`, 27 on `alma8-gcc15-fastdebug`
and 28 on `alma9-clang21-asan`. The 2026-09-06 run (diff 315026, the 66-file state) never reached
a test: each of the three lanes failed within minutes in a build step named `Rebase`, with one
failed item and no `base_commit_id` attribute recorded.

The Jenkins console shows the sequence. The job checks out the diff's base commit
`94f5a6cd7dd`, applies all 66 files of the patch cleanly, and then merges the current tip of
`origin/master` on top, because `yugabyte-db-phabricator` sets `YB_AUTO_REBASE=auto` for itself
whatever the trigger asked for:

    [18:18:09] YB_AUTO_REBASE=auto
    [18:18:09] Setting YB_AUTO_REBASE=auto by default for job yugabyte-db-phabricator
    [18:18:11] + git merge -m 'Merge origin/master' origin/master
               CONFLICT (content): Merge conflict in src/yb/common/common_flags.cc
               Automatic merge failed; fix conflicts and then commit the result.
    [18:18:11] MERGE FAILED

The conflicting file is one this change should never have touched. The first commit removed the
`#ifdef NDEBUG` guards around `kEnableDdlTransactionBlocks` and
`kEnableObjectLockingForTableLocks` in `src/yb/common/common_flags.cc`, and around the mirrored
`kEnableDdlTransactionBlocks` macro in `src/yb/yql/pggate/util/ybc_guc.h`, and set
`ysql_enable_concurrent_ddl` to true, so that a debug build would default to the object-locking
plus concurrent-DDL mode this feature requires. Master's `bfa38fee338` ("[#29490] YSQL: Enable
concurrent DDL by default [Part-2]") now rewrites the same two regions, hence the conflict.

Those flips were never needed: the test fixture already passes
`--enable_object_locking_for_table_locks=true`, `--ysql_enable_concurrent_ddl=true` and
`--ysql_yb_ddl_transaction_block_enabled=true` to its own cluster
(`src/yb/yql/pgwrapper/pg_local_catalog-test.cc:77-80`), so every test in this change runs in the
target mode without them. Carrying them in the diff changed the default mode for every other test
in the tree on the debug lanes, which is a plausible source of part of the 09-03 failure counts.
Both files are therefore back to master's content, and the change no longer alters any flag
default. The stack is rebased onto `002a8169830`.

### What the 2026-09-03 run actually found

Three of the four failure clusters on that run trace to the flag defaults above, and the fourth is
a defect in this change.

The flag-default cluster is the one that hurts. `ysql_enable_concurrent_ddl` carries
`FLAG_REQUIRES_FLAG_VALIDATOR(enable_object_locking_for_table_locks)`, so once the change made it
default to true, every test that starts a daemon with `--enable_object_locking_for_table_locks=false`
failed flag validation and the daemon aborted before the test began:

    [m-1] E0903 common_flags.cc:246] Invalid value '0' for flag
      'enable_object_locking_for_table_locks': Required by ysql_enable_concurrent_ddl to be true
    [m-1] F0903 flags.cc:689] Check failed: _s.ok()
    Bad status: Failed to start masters.: Unable to start Master at index 0: rc=134

That is `PgConcurrentCreateOrReplaceCrashTest.ConcurrentCreateOrReplaceWithDropCrash`,
`TestPgRegressDDLIsolationNoTxnDDLNoObjectLocking.testWithHighHeartbeatDelay`,
`PgMasterDDLReadRestartProbeTest.DDLStaleSafeTimeReadRestart` and
`PgWaitQueuesTestWithoutObjectLocking.TestDDLHaveWaitStartTimeSet`.

The residual failures -- `PgRowLockTest` (three tests), `TestPgExplicitLocks`,
`PgObjecLocksTestOutOfOrderMessageHandling`, `PgCatalogVersionTest.DBCatalogVersion`,
`SkipIntentsBasicTest` (two), the `PgLibPqTest` transaction-conflict tests and
`YbAdminSnapshotScheduleTest.PgsqlCreateTable` -- were checked against the three scheduled master
builds of the same day, on the same three lanes. None of them fails on master, which failed 2 to 6
tests per lane against this change's 18, 16 and 14. They are all in lock or catalog-version
semantics, which the reverted defaults changed for every test in the tree, so the next CI run is
what settles whether the revert takes them with it.

### `BootstrapReachesServing`: a pre-election poll cycle was counted as an apply failure

The test asserts `local_catalog_apply_failures` is zero once every copy is serving, and CI saw 3 on
the fastdebug lane and 4 on the release lane. The count matched, exactly, the number of times one
tserver logged

    LocalCatalogPoller: Failed to poll: Illegal state: The local catalog copy is not the leader of
    its own single-peer group yet: -1 vs 0

`MakeLocalCatalogTabletLeader` started the copy's election and then waited for
`LeaderTerm() > 0` with `tablet_start_warn_threshold_ms`, a warning threshold of 500 ms pressed
into service as a wait budget. Winning a single-voter election is decided locally but the term is
only established when the new term's no-op commits, which on a loaded CI host takes longer than
that. The wait then timed out, but the tablet peer was already stored, so the next cycle skipped
setup, fetched records, and had every apply rejected for want of leadership -- once per second
until the no-op committed, each rejection counted as a failure to apply catalog data.

The fix removes the budget rather than raising it, because any value is wrong: a poll thread
blocked on the commit applies nothing either. Both apply paths now check the condition before
attempting anything. The poll path ends the cycle with `TryAgain`, which the scheduler already
backs off on, and the push path answers the release with `IllegalState` so it waits for the poll,
as it does whenever a copy cannot take a push. Neither touches the apply-failure counter, which
goes back to meaning what the test asserts: catalog data that could not be applied.

### The straggler test's timing, and what the first clean CI run showed

The run on diff 315613 (2026-09-10, base `dbbd541e661`) is the first that tested this change in
master's own mode. One test failed unrecovered across the three lanes,
`TestPgRegressParallel#testPgRegressBigParallel`, which was already failing on scheduled master
builds b436 to b438. None of the eighteen failures from the 09-03 run came back, which settles the
question left open there: the whole cluster was the flag defaults, including the three tests that
neither `bfa38fee338` adapted nor a flag of their own protected.

Getting there took two fixes the rebase itself demanded. Master's tip had moved 64 commits, and
`remote_bootstrap_client.h` conflicted where both sides had added private members at the same
point, which is a keep-both. Then `TSHeartbeatResponsePB` field 34, taken here for
`db_catalog_versions_read_time`, had been taken upstream for `cluster_ysql_db_pins_ready`. Git
merged that cleanly because the two declarations are different lines; `protoc` refused it. The
field is renumbered to 36, and 35 and the request's 25 are clear, since master's response tops out
at 34 and its request at 24.

`AStragglerDoesNotDisableThePushForOthers` failed three of ten asan attempts on the same
assertion, that a healthy tserver's `local_catalog_pushes_applied` moved. The cause is a second
reason master can skip a target, distinct from the lag cap the test exercises: a reported position
older than `local_catalog_release_push_report_max_age_ms`, five seconds by default, is not trusted
and that target is left to pull. On a loaded sanitizer build a heartbeat can arrive later than
that, so a healthy tserver was skipped for staleness and the test read it as the cap denying the
push. The test now pins the age bound to 60 s, which does not weaken it: the straggler keeps
heartbeating and is dropped by the cap on the position those heartbeats carry, not by staleness.

The fixed three second sleep that preceded the measurement is also gone. It was waiting for the
straggler's frozen position to reach master with no way to observe whether it had. Master drops a
target only while preparing a release, so the wait now runs its own DDLs and ends when master's
`local_catalog_release_pushes_skipped` moves, which is the direct evidence that the drop is
happening.

### What running the whole suite with the feature on found

D57801's own CI says little about the feature, because `enable_local_tserver_catalog` defaults off
and only `pg_local_catalog-test` turns it on. D58090 is a throwaway revision carrying this stack
plus a commit that defaults the flag on under `NDEBUG`, so the release lane ran its entire suite
with catalog reads served from the copy. Against a no-flip run of the same stack, which had one
unrecovered failure, the release lane failed 201, of which 188 were new. They fall into four
groups, and only one of them is a defect in the feature.

**Tests that turn object locking off, about 80 of them.** The flip left the copy serving in a mode
the design does not support, since its correctness rests on object locking serializing DDL against
catalog readers. 68 are in `pg_vector_index-test`, whose fixture sets
`enable_object_locking_for_table_locks = false`. Their signature is a backend timing out after 30 s
in `Database <oid> is not ready in Yugabyte shared memory`. This is the experiment's own doing, but
it exposed a real gap: nothing stopped a user from the same configuration. Both start sites now
check `LocalCatalogPrerequisitesMet()`.

**The catalog snapshot's time was taken from, and written to, the attached transaction's read
point.** `YBSession::read_point()` (`src/yb/client/session.cc:305-312`) returns the attached
transaction's own `ConsistentReadPoint` whenever a transaction exists, and
`TryServeCatalogReadsLocally` used that read point for both halves of its work. Each half was
wrong in a different way.

Reading it. A PG catalog snapshot carries its own read time serial number, separate from the
transaction's, and the session moves between the two by saving the read point under the outgoing
serial number and restoring it under the incoming one in `SetupPlainSessionReadTime`
(`src/yb/tserver/pg_client_session.cc:4287-4304`). `ReadPointHistory::Restore`
(`src/yb/tserver/pg_client_session_util.h:123-136`) leaves the read point untouched when the
incoming serial number has no saved entry, which is the case on a catalog snapshot's first read.
The read point therefore still held the transaction's read time, and the code took that as the
catalog snapshot's existing time: it either read the copy at the transaction's snapshot time or
compared the transaction's time against C and routed to master. The catalog snapshot's time must
come from the catalog snapshot.

Writing it. `session.SetReadPoint(read_time)` put C on the transaction's read point. A
serializable transaction must carry no read time at all, so the next operation on that session was
rejected at `src/yb/client/async_rpc.cc:496`, "Read time should NOT be specified for serializable
isolation", and the process aborted. A snapshot isolation transaction would instead have its read
point replaced by a `ReadHybridTime::SingleTime`, which drops the local, global and in-transaction
limits that bound its uncertainty window.

The fix holds the pinned time in the session against the catalog snapshot's own read time serial
number, in `local_catalog_read_time_serial_no_` and `local_catalog_read_time_`. With a transaction
attached that record is the only source consulted and the session's read point is not touched.
With no transaction attached the session's read point still carries the catalog snapshot's time,
because there is no other snapshot on it, so it is read and written exactly as before; that is what
makes a later read of the same snapshot that master has to answer run at the same time.
`ResetReadPoint` drops the record along with the read point. Nothing else needed the read point:
`PrepareLocalCatalogRead` already receives the time as an argument.

The failing parameterisation is
`DeferredModeAddCheckConstraint/TransactionalDdlInSerializableTxn`, not the serializable one
without transactional DDL. `PgReadAfterCommitVisibilityDdlTest::SetUp` drives
`enable_object_locking_for_table_locks` and `ysql_enable_concurrent_ddl` from its `transactional_ddl`
parameter, so only the two `TransactionalDdl*` parameterisations meet `LocalCatalogPrerequisitesMet()`
and start the copy at all.

Verified locally against the fix, each binary run on its own, never two at a time, with
`--enable_local_tserver_catalog=true`:

- All four parameterisations of `PgReadAfterCommitVisibilityDdlTest.DeferredModeAddCheckConstraint`
  pass, including `TransactionalDdlInSerializableTxn`, which is the one that aborted. No
  `Read time should NOT be specified for serializable isolation` fatal appears in any of the four
  runs.
- `pg_local_catalog-test` passes all 47 of its tests.
- `YbAdminSnapshotScheduleTestWithYsql.TransactionDuringPITR` passes, with the copy confirmed
  serving on all three tservers.
- `TransactionDuringPITRRepro23399` and `Pgsql/DBColocated_PITR` in
  `yb-admin-snapshot-schedule-test` are still to be re-run.

Reproducing the D58090 lane in a fastdebug build takes more than
`--enable_local_tserver_catalog=true`. Both prerequisites default to the value of
`kEnableDdlTransactionBlocks`, which `src/yb/common/common_flags.cc:169-173` defines as true only
under `NDEBUG`, so in a fastdebug build they are off and `LocalCatalogPrerequisitesMet()`
(`src/yb/tserver/local_catalog_replica.cc:154`) refuses to start the copy. A test that does not set
them itself, which is every test outside `pg_local_catalog-test` and
`pg_read_after_commit_visibility-test`, therefore runs with no copy at all, and its pass proves
nothing about the feature. The first `TransactionDuringPITR` run here was void for exactly that
reason: it passed in 17.7 s with zero `local_catalog_replica` lines in the log. The daemons need the
whole ladder, which is the one `pg_local_catalog-test` installs:

```
--enable_local_tserver_catalog=true
--ysql_yb_enable_invalidation_messages=true
--enable_object_locking_for_table_locks=true
--allowed_preview_flags_csv=ysql_enable_concurrent_ddl
--ysql_enable_concurrent_ddl=true
--ysql_yb_ddl_transaction_block_enabled=true
```

`yb_build.sh --extra-daemon-flags` carries them to master and tserver through the
`YB_EXTRA_DAEMON_FLAGS` environment variable, which `ExternalDaemon::StartProcess`
(`src/yb/integration-tests/external_daemon.cc:333`) appends last, after the fixture's own flags, so
they win. Every run below is confirmed against the
`local_catalog_replica.cc:227 Serving state kBootstrapping -> kServing` line in the log rather than
against the test's exit status alone.

**Tests that count catalog RPCs to master.** `TestPgFollowerReads.testPgSysCatalogNoFollowerReads`
reads `Expected 167 to be greater than 167` and
`PgCatalogVersionConnManagerTest.TestConnectionManagerRelCacheInitRpcCount` reads `Which is: 0`.
The count did not rise because the copy answered the read. These assertions become wrong the day
the feature ships on, and are not wrong today.

**Tests that dump every DocDB write on the node.** `conflict_resolve_keys_verification-itest` sets
`TEST_file_to_dump_docdb_writes` and compares the dump against a golden file. The copy's applies
are writes on that node, so five lines of a system catalog row appear that the golden file cannot
contain, and all 17 of that binary's failures are this. Excluding the private copy from that dump
would fix it, in the same spirit as its exclusion from `tablet_map_`, the metadata validator, CDC
registration and load balancer moves.

**Tests that restore master's system catalog with PITR.** `yb-admin-snapshot-schedule-test` failed
39 of the 188. The two that were held back above were run here, and the first one names a pair of
real defects.

### A master PITR restore never reaches the copy, and the version wait then holds the query for the whole request deadline

`YbAdminSnapshotScheduleTestWithYsql.TransactionDuringPITRRepro23399` fails with the ladder on and
passes in 16399 ms with `--enable_local_tserver_catalog=false` on the same fixture, with no
`local_catalog` line in the log. The account below comes from one failing
run with `--vmodule=pg_client_session=2,local_catalog_replica=2,local_catalog_poller=1` plus a gdb
thread dump of all three tservers taken while the stall was in progress.

A point-in-time restore of master's system catalog raises `pg_yb_catalog_version` as part of the
restore itself. On m-3 at 02:51:40.974220, `restore_sys_catalog_state.cc:206` logged `PITR:
Incrementing pg_yb_catalog version of DocPath(DocKey(CoTableId=...1f4a, [], [16384]), [ColumnId(1)])
to 4`, and all three masters logged the same increment, because each of them recomputes the restored
rows locally while it applies the restore.

That is the point: the restore's rows are computed during apply and written straight to RocksDB,
never as a replicated write. `CatalogManager::RestoreSysCatalogFastPitr`
(`src/yb/master/catalog_manager_ext.cc:3161-3216`) fills a `docdb::DocWriteBatch` while it applies
the `RESTORE_SYS_CATALOG` operation, then hands it to `RestoreSysCatalogState::WriteToRocksDB`
(`src/yb/master/restore_sys_catalog_state.cc:933-938`), which calls `yb::WriteToRocksDB`
(`src/yb/tablet/restore_util.cc:346-365`). That helper moves the batch into a `rocksdb::WriteBatch`,
stamps hand-built consensus frontiers with the restore's own op id and hybrid time, and writes it
through `Tablet::WriteToRocksDB`. No `WriteOperation` is submitted and no second consensus round
happens, so the only WAL entry the whole restore leaves behind is the snapshot operation itself,
op id 1.724 (`master_snapshot_coordinator.cc:605`, 02:51:40.928689), whose payload is the restore
request and not the rows.

The change stream discards that entry. `GetChangesForXCluster` switches on each replicate message's
operation type and produces records only for `UPDATE_TRANSACTION_OP`, `WRITE_OP`, `SPLIT_OP` and
`CHANGE_METADATA_OP` (`src/yb/cdc/xcluster_producer.cc:404-424`); a snapshot operation falls through
to `default:` and yields nothing, while the loop still advances the checkpoint past it. ts-1's
poller went from WAL position 1.723 to 1.728 in one cycle, applying four `sys.catalog` rows and
nothing else (`local_catalog_poller.cc:383`, 02:51:42.559219), and its applied version for database
16384 stayed at 3 from then on, still 3 in the last poll cycle of the run at 02:54:34.839058.

Master meanwhile publishes version 4 to every tserver by heartbeat 1.1 s after the restore:
`tablet_server.cc:2360`, `Invalidating db PgTableCache caches since catalog version incremented for
[{16384, 4}]`, on ts-3 at 02:51:42.065953 and ts-2 at 02:51:42.096344. Every PG backend on the node
therefore sends its next perform request with `backend_catalog_version = 4`, which the copy can
never satisfy.

```
  master                     copy on ts-1                 PG backend on ts-1
    |                             |                              |
 1.724 RESTORE_SYS_CATALOG        |                              |
  apply: rows -> RocksDB         poll 1.723 -> 1.728            |
  (no WAL row records)           4 sys.catalog rows,            |
  pg_yb_catalog_version 3->4     applied {16384, 3}             |
    |                             |                              |
 heartbeat {16384, 4} ----------> tserver shared memory -------> perform, backend version 4
    |                             |                              |
    |                        WaitForAppliedVersion(16384, 4, deadline=600s)
    |                             |   blocks; applied stays 3
    |                             |                              |
    |                             |                        600 s later: PG reports
    |                             |                        "Timed out waiting kResponseSent"
```

The wait is the second defect. `TryServeCatalogReadsLocally` (`src/yb/tserver/pg_client_session.cc`
:3588-3612) compares the request's `backend_catalog_version` against the copy's applied version for
that database and, when the copy is behind, calls
`replica->WaitForAppliedVersion(db_oid, backend_version, deadline)` with the perform request's own
deadline. The fallback to master on a failed wait is therefore unreachable in practice: the client
has already given up by the time the wait expires. The live stack shows exactly this, on ts-1
thread 55 (LWP 3200607, `shmem_exchange_`):

```
pthread_cond_timedwait
std::condition_variable::wait_until
LocalCatalogReplica::WaitForAppliedVersion (db_oid=16384, version=4)  local_catalog_replica.cc:320
PgClientSession::TryServeCatalogReadsLocally                          pg_client_session.cc:3602
PgClientSession::DoPerform                                            pg_client_session.cc:3439
PgClientSession::DoHandleSharedExchangeQuery                          pg_client_session.cc:2606
PgClientSession::HandleSharedExchangeQuery                            pg_client_session.cc:2641
PgClientSession::ProcessSharedRequest                                 pg_client_session.cc:2659
                                                                      pg_client_service.cc:443
SharedExchangeRunnable::Run                                           tserver_shared_mem.cc:511
```

In the run that carried the thread dump the wait ended only at cluster shutdown:
`pg_client_session.cc:3603`, `Wait for version 4 of database 16384 ended after 176.865s: Shutdown in
progress (yb/tserver/local_catalog_replica.cc:315): Local catalog replica is shutting down`. In the
earlier, unperturbed run it consumed the whole 600 s perform deadline and PG raised `Timed out
waiting kResponseSent, state: kProcessingRequest`.

Two separate fixes follow, and neither is written yet.

- **Divergence.** A restore of master's system catalog changes rows the copy cannot learn about
  through the stream, so the copy has to be rebuilt. The re-seed machinery for this already exists:
  `LocalCatalogPoller::EnsureLocalTablet` (`src/yb/tserver/local_catalog_poller.cc:261`) deletes the
  local tablet and its checkpoint file and re-runs `OpenOrCreateLocalCatalogTablet` with
  `force_reseed`, and today only a `CHECKPOINT_TOO_OLD` error from master triggers it. A restore
  must trigger it too, either by the change service reporting that it crossed a snapshot operation
  or by master carrying a restore marker in the poll response.
- **Liveness.** The version wait must have its own short bound rather than the request deadline, so
  that a version the copy cannot reach costs one bounded wait and then a read served by master,
  instead of the client's entire timeout.

### The same mechanism fails `Pgsql/DBColocated_PITR`

`YbAdminSnapshotScheduleTestWithYsqlColocationRestoreParam.Pgsql/DBColocated_PITR` fails for the
same reason, which rules out anything specific to the transaction the other test runs across the
restore. The test creates a colocated table, takes a timestamp, updates the row, restores the
schedule to that timestamp, and reconnects; the failure is at
`src/yb/tools/yb-admin-snapshot-schedule-test.cc:1444`, the `ConnectToRestoredDb()` that follows the
restore, after 602.4 s:

```
Bad status: Network error (yb/yql/pgwrapper/libpq_utils.cc:636): Connect failed:
  connection to server at "127.35.28.1", port 20087 failed:
  FATAL: Timed out waiting kResponseSent, state: kProcessingRequest, passed: 602.422s
```

The sequence in that run matches the account above line for line. Master restored at op id 1.734
(03:01:42); the heartbeat published `{db 16384: version 4}` at 03:01:43; ts-2 logged
`pg_client_session.cc:3596` `Waiting for the local catalog copy to apply version 4 of database
16384; it has applied 3` at 03:01:44.188393; the copies' applied versions were still `{16384: 3}` at
03:04:57 and remained there until the test tore the cluster down.

### The fix: master tells a poller that its batch crossed a restore, and the poller re-seeds

**Divergence.** The change service now watches for the restore operation itself and answers with an
instruction to rebuild rather than with records.

- `XClusterGetChangesContext` gains an optional callback, `update_on_snapshot_op_func`
  (`src/yb/cdc/cdc_producer.h:58,70`). It is called for every snapshot operation the batch crosses
  and returns whether the batch must stop at that operation. The existing `update_on_split_op_func`
  is the pattern: only the caller knows what its stream must do with such an operation, so the
  producer's loop asks rather than decides. The two other callers of `GetChangesForXCluster`
  (xCluster's own `CDCServiceImpl::GetChanges`, master's release push) leave it empty and pay one
  branch per snapshot operation.
- `GetChangesForXCluster` gains `case consensus::OperationType::SNAPSHOT_OP`
  (`src/yb/cdc/xcluster_producer.cc:422-427`), which invokes the callback when one is set. Before
  this the operation fell through `default:` and the loop advanced the checkpoint past it, which is
  precisely how the restore went unnoticed.
- The system catalog change stream's callback (`src/yb/cdc/sys_catalog_change_service.cc:140-148`)
  accepts a snapshot operation whose request is anything but `RESTORE_SYS_CATALOG` and stops the
  batch on that one. The filter is necessary rather than defensive: master submits
  `CREATE_ON_MASTER` and `DELETE_ON_MASTER` snapshot operations on its own system catalog tablet for
  every scheduled snapshot (`src/yb/master/master_snapshot_coordinator.cc:1819,1849`), which in
  these tests is every 6 s, and treating those as restores would put every copy in a re-seed loop.
- `DoGetSysCatalogChanges` then clears the records it had collected and answers
  `reseed_required` (`src/yb/cdc/cdc_service.proto`, `GetSysCatalogChangesResponsePB` field 5)
  instead (`src/yb/cdc/sys_catalog_change_service.cc:159-169`). Records read before the restore are
  discarded with the rest: the restore overwrote whatever they carried, and the requestor is about
  to throw its copy away.
- `LocalCatalogPoller::FetchAndApplyOnce` (`src/yb/tserver/local_catalog_poller.cc:368-377`) treats
  `reseed_required` the way it already treats `CHECKPOINT_TOO_OLD`: it calls
  `LocalCatalogReplica::RequestReseed()` and returns without applying anything. The next poll cycle
  enters `EnsureLocalTablet`, which takes the request, deletes the local tablet and its checkpoint
  file, and re-fetches master's tablet with `force_reseed`
  (`src/yb/tserver/local_catalog_poller.cc:261-315`).
- The applied versions correct themselves because they are read from the copy rather than
  accumulated from records: `PublishAppliedBatch` calls `ReadLocalCatalogVersions` against the
  copy's own `pg_yb_catalog_version` at each batch's safe time
  (`src/yb/tserver/local_catalog_poller.cc:464`), and `OnReseedStarted` clears the published map
  (`src/yb/tserver/local_catalog_replica.cc:433`). The first poll after the re-seed therefore
  publishes `{16384: 4}` from the restored rows, and the waiting backends wake.
- Master's release push path fetches one batch and slices it to each tserver
  (`src/yb/master/object_lock_info_manager.cc:1839`). A batch that crossed a restore carries no
  records and no safe time, so the push is skipped and the tservers' own pollers re-seed
  (`src/yb/master/object_lock_info_manager.cc:1848-1854`). `DoApplyPushedBatch` also refuses such a
  batch and requests a re-seed (`src/yb/tserver/local_catalog_poller.cc:547-553`), so a future
  caller that does forward one cannot apply records across a restore.

**Liveness.** `TryServeCatalogReadsLocally`'s two waits now have their own bound, so a copy that
cannot reach what the backend was told costs one bounded wait and then a read served by master.

- New flag `local_catalog_serve_wait_timeout_ms`, default 5000 ms
  (`src/yb/tserver/pg_client_session.cc:164-168`). At the default poll interval of 100 ms that is
  50 poll cycles and about five heartbeats, so a copy that is merely behind still catches up inside
  the bound and keeps serving.
- `LocalCatalogWaitDeadline` (`src/yb/tserver/pg_client_session.cc:224-229`) is the smaller of the
  request's deadline and now plus that bound, so a request with a nearer deadline keeps its own.
  Both the version wait (`pg_client_session.cc:3619`) and the wait for the session's own catalog
  writes (`pg_client_session.cc:3649`) use it. The existing fallback on a failed wait -- count it
  against serving and return false, which sends the read to master -- is now reachable.

### The catalog snapshot's read time lives on the plain session's read point

Review question: the pinned time was held in a side map keyed by read time serial number, and the
write to the session's read point was skipped whenever a transaction was attached. That skip was
wrong, and the side map hid the defect it was compensating for.

**One counter, two snapshots, distinct numbers.** A YSQL backend draws every read time serial
number from a single shared counter, `next_read_time_serial_no`
(`src/yb/yql/pggate/pggate.cc:703`), through `PgTxnManager::SerialNo::NextReadTimeSerialNo`
(`src/yb/yql/pggate/pg_txn_manager.cc:186-190`), which returns `fetch_add(1) + 1`. The two snapshot
kinds take different values from it:

- A transaction snapshot calls `IncReadTime` (`pg_txn_manager.cc:211`), which sets both `read_time_`
  and `max_read_time_` to the new number, so the number becomes current immediately.
- A catalog snapshot calls `IncMaxReadTime` (`pg_txn_manager.cc:216`), which advances only
  `max_read_time_`. `read_time_` stays on the transaction snapshot's number, and the catalog
  snapshot's number is returned to PostgreSQL as the snapshot's read point handle
  (`PgTxnManager::ResetTransactionReadPoint`, `pg_txn_manager.cc:475-490`).

The catalog snapshot's number becomes current only for the duration of a statement's catalog
operations: `PgSession::RunAsync` calls `UpdateReadPointForCatalogOps`
(`src/yb/yql/pggate/pg_session.cc:1203-1218, 1251-1253`), which restores the handle, and restores
the previous read point after the flush (`pg_session.cc:1304`). Two snapshots therefore never hold
the same serial number in non-legacy mode.

**What the two snapshots do share is the register the time is kept in.** `YBSession::read_point()`
returns the attached transaction's read point when there is a transaction, and the session's own
otherwise (`src/yb/client/session.cc:306`). One `ConsistentReadPoint` object therefore holds
whichever snapshot's time is current, and `PgClientSession::SetupPlainSessionReadTime`
(`src/yb/tserver/pg_client_session.cc:4325-4342`) multiplexes the object between snapshots by serial
number: on a change it saves the object's momento under the outgoing number
(`ReadPointHistory::Save`, `src/yb/tserver/pg_client_session_util.h:140`) and restores the incoming
number's momento into the same object (`Restore`, `pg_client_session_util.h:124`). The momento
carries `read_time_`, `restart_read_ht_`, `local_limits_` and `restarts_`
(`src/yb/common/consistent_read_point.h:95-118`), so the round trip loses nothing.

With transaction serial 7 at time T7 and catalog serial 8 at time C:

```
PG backend sends          one ConsistentReadPoint on the tserver      ReadPointHistory
------------------------  ------------------------------------------  ----------------
stmt 1  serial 7  ----->  [ T7 ]                                     {}
stmt 2  serial 8  ----->  save 7:T7 ; Restore(8) misses               {7:T7}
                          ResetReadPoint -> [ ] ; clamp -> [ C ]
stmt 3  serial 7  ----->  save 8:C ; Restore(7) hits -> [ T7 ]        {7:T7, 8:C}
stmt 4  serial 8  ----->  save 7:T7 ; Restore(8) hits -> [ C ]        {7:T7, 8:C}
```

**Consequence for `TryServeCatalogReadsLocally`.** At the moment it runs, the read point never holds
the transaction's time. `UpdateReadTime` ran first, and for a catalog snapshot's serial number it
took one of three paths:

| Path | Register contents on entry to `TryServeCatalogReadsLocally` |
|---|---|
| `Restore` missed (first read of the snapshot) | `ResetReadPoint` (`pg_client_session.cc:3990`) emptied it, then the clamp at `:4035-4039` filled it with a time taken from this server's clock |
| `Restore` hit | the time an earlier read of this same catalog snapshot used |
| serial unchanged from the previous request | the same, kept by the `else` branch at `:3991` |

The condition that suppressed the write -- a transaction is attached, so the register must be
holding the transaction's snapshot -- is therefore false in all three. The register holds the
catalog snapshot's time, and writing the copy's time C to it is what the existing mechanism then
saves under the catalog snapshot's serial number for the snapshot's later reads.

**Damage the side map caused.** Within one catalog snapshot, a read the copy answered used C, while
a read master had to answer -- a relation the copy does not hold, or a wait that reached
`local_catalog_serve_wait_timeout_ms` -- used the clamped time, which is later than C. One snapshot
had two read times, and the later one was on the reads that crossed to master.

**The change.** The side storage (`local_catalog_read_time_serial_no_`, `local_catalog_read_time_`)
is deleted, and so is the request-scoped flag (`catalog_read_time_picked_this_request_`) that a
later draft used to tell a time this request had just clamped from this server's clock apart from a
time an earlier read of the same snapshot had already used. Both existed only because the copy's
time was written after the clamp had put a clock reading in the register. The time is now chosen
once, at the single place where a catalog snapshot that has no time gets one.

- `UpdateReadTime` reaches the clamp branch for a fresh catalog snapshot with the register empty,
  because `ResetReadPoint` emptied it when `Restore` missed. The branch is guarded by
  `!session.read_point()->GetReadTime()` and calls `SetFreshCatalogSnapshotReadTime`
  (`pg_client_session.cc:4065-4071`), which writes the copy's complete time C when the copy may
  answer this backend's catalog reads, and the clamped clock reading otherwise
  (`pg_client_session.cc:3644-3658`).
- `TryServeCatalogReadsLocally` no longer writes a read time. It reads the register and compares
  the snapshot's time against C, so every read under one snapshot runs at that one time whether the
  copy or master answers it.
- A snapshot that took the clock reading is above C and reads from master, and is never pulled back
  to C, which would show the backend an older state than the snapshot has already returned. C
  advances at every poll and passes that time within about one poll interval, after which the copy
  answers the same snapshot at the snapshot's own unchanged time.

**The read restart keeps the clock reading.** An earlier draft also routed the read-restart branch
of `UpdateReadTime` through the same choice, so that a restarted snapshot could land on C as well.
That was a defect rather than a fix: at a restart the register still holds the snapshot's previous
time, and C can be below it, so writing C there moves the snapshot backwards to a state older than
one of its own earlier reads returned. The branch keeps the plain clamped clock reading
(`pg_client_session.cc:3958-3973`), and the copy treats that new time exactly as it treats a fresh
snapshot's clock reading: master answers until C passes it.

### Serializable isolation no longer serves catalog reads from the copy

`PgTxnManager::SetupReadTimeOptions` returns before the catalog clamp for a serializable transaction
(`src/yb/yql/pggate/pg_txn_manager.cc:856-863`), so such a request carries neither a read time nor
`clamp_uncertainty_window`. On the tserver the register stays empty for that serial number, and each
catalog read reaches the master tablet with no read time, so the tablet picks its own latest safe
time per operation. The copy cannot reproduce that, and pinning the snapshot to one time is what the
two `RSTATUS_DCHECK`s in `UpdateReadTime` forbid for a serializable transaction
(`pg_client_session.cc:3939-3941, 4036-4038`).

- `TryServeCatalogReadsLocally` returns false when the attached transaction's isolation is
  `SERIALIZABLE_ISOLATION` (`pg_client_session.cc:3591-3595`); every such catalog read goes to
  master.
- The new counter `local_catalog_reads_to_master_serializable`
  (`src/yb/tserver/local_catalog_replica.cc:102,204-205,485-487`) counts them, so the exclusion is
  visible in the metrics rather than inferred from a lower serve rate.
- Legacy mode is unaffected because it never reaches this path. `YbSkipPgSnapshotManagement`
  (`src/postgres/src/backend/utils/misc/pg_yb_utils.c:8808-8835`) skips PostgreSQL snapshot
  management for serializable only in legacy mode, and in legacy mode a catalog snapshot gets no
  read point handle at all (`src/postgres/src/backend/storage/ipc/procarray.c:2681`), so its catalog
  reads run under the transaction snapshot's serial number. This is the only place where the two
  snapshots do reuse one serial number, and the local copy is a non-legacy-mode feature.

**Verification.** Both PITR tests pass with all the above in the build: `TransactionDuringPITRRepro23399`
in 20786 ms, and `YbAdminSnapshotScheduleTestWithYsqlColocationRestoreParam.Pgsql/DBColocated_PITR`
in 25411 ms.

### Three test groups fail for reasons that have nothing to do with catalog correctness

The 2026-09-03 default-on run's four failure groups are listed above. Two of them are the feature's
own defects and are fixed above (the missing prerequisite check, the PITR re-seed). The remaining
19 failures are tests whose assertions the copy's existence invalidates, and each is fixed in the
test rather than in the feature.

**`conflict_resolve_keys_verification-itest`, 17 failures: the copy's writes landed in a golden
file.** `Tablet::WriteToRocksDB` appends every key/value pair it writes to the file named by
`TEST_file_to_dump_docdb_writes`, and that test compares the whole file against an expected
sequence of conflict-resolution keys. The copy is a tablet on the same tserver, so its applies of
master's WAL records were dumped into the same file and the comparison failed on records the test
never asked for. The dump now skips the copy (`src/yb/tablet/tablet.cc:2238`).

That check needs the copy's tablet id in `yb/tablet`, which cannot link against `yb_tserver`, so
`kLocalCatalogTabletId` moved from `src/yb/tserver/local_catalog_replica.{h,cc}` to
`src/yb/common/constants.h:43`. Nothing else about the id changed.

**`TestPgFollowerReads.testPgSysCatalogNoFollowerReads` and
`PgCatalogVersionConnManagerTest.TestConnectionManagerRelCacheInitRpcCount`, 2 failures: both
assert on a count of catalog reads that reach master.** The first asserts that system catalog reads
do not take follower reads, by reading the master leader's read counters; the second asserts an
exact number of catalog RPCs during a connection manager relcache build. A copy that answers those
reads locally drops both counts, so each assertion now measures where the read went instead of
whether the read happened. Both fixtures pin `enable_local_tserver_catalog=false`
(`src/yb/yql/pgwrapper/pg_catalog_version-test.cc:3213`,
`java/yb-pgsql/src/test/java/org/yb/pgsql/TestPgFollowerReads.java:66`). Coverage of the same
assertions with the copy on is not lost by pinning the flag, because there is nothing to cover:
with the copy serving, the RPC these tests count does not exist.

**What is left.** Of the 39 `yb-admin-snapshot-schedule-test` failures, the mechanism is fixed and
two of the binary's tests were run locally and pass; the other 37 have not been run with the fix.
That binary is the one place where the next default-on run can still produce failures in numbers.
