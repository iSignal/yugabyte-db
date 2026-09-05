# Learning: bootstrap / reseed across RAFT groups (cross-group SST transfer)

Source: code reading pass 2 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`. Supersedes the earlier
"remote bootstrap requires membership" framing in
[[learning-remote-bootstrap-and-read-replica]] — that was about the *trigger*
path; the *transfer machinery* is reusable standalone.

## Why this is critical-path (not a "later" concern)

The local copy must be seeded on every tserver add / replace / disk-loss, and
re-seeded whenever a tserver lags past the master's CDC WAL-retention window
(the CDC stream can only carry you forward from a checkpoint). So reseed frequency
~= tserver churn, which at 1000 tservers is routine. Bootstrap is on the critical
path.

## Headline: there are TWO viable cross-RAFT-group seed mechanisms, both deliver schemas

### A. RemoteSnapshotTransferClient (PREFERRED) — purpose-built for this

`src/yb/tserver/remote_snapshot_transfer_client.{h,cc}`. Header says it outright
(`remote_snapshot_transfer_client.h:52-58`): *"the source tablet does not
necessarily have to be in the same RAFT group or even the same universe."* Built
for "xCluster native bootstrap."
- Reuses the production remote-bootstrap file-transfer wire (same
  `RemoteBootstrapServiceProxy`, same chunked `FetchData`, CRC, rate-limiting) —
  `remote_client_base.h:61-124`.
- `Start()` (`remote_snapshot_transfer_client.cc:62-128`) opens a session against
  an arbitrary `source_peer_uuid` + addr; sets `req.tablet_id = source_peer_uuid`
  precisely because source need not be in the same RAFT group (`:78-81`). Rewrites
  `kv_store->rocksdb_dir` to the local target dir (`:124`).
- **Snapshot-only — no WAL.** `InitSnapshotTransferSession`
  (`remote_bootstrap_session.cc:122-133`) does NOT package WAL segments (unlike
  the bootstrap session at `:203`). You get just the RocksDB SSTs + superblock.
- Driven standalone via the `StartRemoteSnapshotTransfer` RPC
  (`ts_tablet_manager.cc:1832-1878`): look up local target tablet, create client,
  `Start()` + `FetchSnapshot()` + `Finish()`.

### B. RemoteBootstrapClient run standalone (fallback) — heavier

Only needs `{source HostPort, source peer_uuid, tablet_id, ProxyCache}`
(`remote_bootstrap_client.h:80-86`); ctor takes only `(tablet_id, FsManager*)`.
- `Start() -> FetchAll() -> Finish()`; none touch local consensus state.
- For a brand-new tablet it calls `RaftGroupMetadata::CreateNew` itself
  (`remote_bootstrap_client.cc:351-423`); `ts_manager` is optional.
- Brings the WAL + intents + cmeta (heavier than snapshot transfer).

## The source side does NO membership check (both paths)

`CreateRemoteSession` (`remote_bootstrap_service.cc:434-499`) checks only:
tablet exists (`GetServingTablet`), `CheckRunning()`, get consensus,
`CheckReadyAsRbsSource()` — the last only rejects if a tablet split is pending
(`raft_consensus.cc:3684-3696`). **It never verifies the requestor is a config
member.** `FetchData` (`remote_bootstrap_service.cc:267-345`) checks only that the
session id exists. So a fresh tserver can pull the master sys_catalog tablet
without joining the masters' RAFT config. `BeginRemoteSnapshotTransferSession`
shares the same `CreateRemoteSession`.

## The one consensus entanglement (RBS path) and how to sever it

RBS downloads the source's committed RAFT config into cmeta
(`remote_bootstrap_client.cc:305,742-757`). For sys_catalog that config = the
masters; if left there, the opened tablet would try to run consensus against the
masters. Sever it by overwriting with a single-peer local config before open:
`ConsensusMetadata::set_committed_config(...)` (`consensus_meta.h:75,166,176,264`),
building a one-peer `RaftConfigPB` (precedent: `SysCatalogTable::SetupConfig`,
`sys_catalog.cc:395-430,463-468`; a single-VOTER config self-elects, no quorum).
(The snapshot-transfer path doesn't bring cmeta, so this is mostly an RBS-path
concern.)

## Schemas come for free (no exported_tablet_metadata_changes needed)

The transferred superblock (`RaftGroupReplicaSuperBlockPB`) carries
`KvStoreInfoPB.tables[]`, and `KvStoreInfo::ToPB` iterates EVERY table — primary
first, then all cotables (`tablet_metadata.cc:733-743`). For the sys_catalog
tablet that's every PG catalog cotable schema (pg_class, ... per
`(db_oid, table_oid)`). The client persists the full list
(`remote_bootstrap_client.cc:290,446,488`). So unlike the initdb-snapshot path,
we do NOT need a separate `exported_tablet_metadata_changes` file — the schemas
ride in the superblock. (See [[learning-sys-catalog-and-bootstrap]] for the
initdb-snapshot alternative.)

## Seed point composes with the poller

The transferred RocksDB is a checkpoint at a well-defined committed OpId
(`CreateSnapshot` returns `last_logged_opid`, `remote_bootstrap_session.cc:147-186`).
Start the CDC poller from that OpId/hybrid-time: seed at X, stream from X.

## Reseed is tombstone-then-replace

RBS does not refresh in place: `HandleReplacingStaleTablet`
(`ts_tablet_manager.cc:1615-1663`) requires the existing tablet be
`TABLET_DATA_TOMBSTONED` (and `LOG(FATAL)`s on a READY sys_catalog in the
production path). For our standalone driver we control the tombstone step
(`DeleteTabletData` with `TABLET_DATA_TOMBSTONED`, `ts_tablet_manager.cc:1911`),
then re-run the transfer. State machine: `TABLET_DATA_COPYING` -> `TABLET_DATA_READY`
(`remote_bootstrap_client.cc:297,487`).

## Maturity

Transfer PRIMITIVE: production-shared with RBS, low risk. The xCluster
orchestration that currently *drives* it (`SetupReplicationWithBootstrapHelper` /
`SnapshotTransferManager`) is explicitly dormant/unmaintained
(`xcluster_bootstrap_helper.h:40-43`) -> drive the low-level RPC ourselves.

## Why NOT remote bootstrap as ongoing sync / read-replica

Both require RAFT membership in the sys_catalog group (config == masters,
bidirectional), O(N-peer) leader fan-out, and config-change-per-tserver writes
*into* sys_catalog. Ruled out for sync. See
[[learning-remote-bootstrap-and-read-replica]] (full) and the efficiency analysis
in [[learning-sync-channel-and-gate]]. Snapshot transfer is for SEEDING only; the
ongoing channel is CDC.
