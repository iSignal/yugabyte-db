# Learning: remote bootstrap & read-replica feasibility (and why both are blocked)

Source: code reading pass 1 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`.

## Bottom line

Both candidate mechanisms — (a) remote-bootstrap (RBS) the sys_catalog to each
tserver, and (b) make each tserver a non-voter (OBSERVER) read replica of master
sys_catalog — are blocked by the same invariant: **to receive a tablet's data via
consensus you must be a peer in that tablet's RAFT config**, and the sys_catalog
config is hardwired to the ~3 masters. Neither scales to ~1000 tservers. This is
the main reason the design should use an out-of-consensus pull channel
(CDCSDK) instead — see [[learning-cdc-wal-sys-catalog]].

## 1. Remote bootstrap is leader-driven and REQUIRES RAFT membership

- RBS copies the **entire tablet**: a RocksDB checkpoint (all SSTs) + WAL segments
  + tablet superblock + consensus metadata.
  - Source: `CreateSnapshot()` ->
    `tablet->snapshots().CreateCheckpoint(checkpoint_dir_, ...)`
    (`src/yb/tserver/remote_bootstrap_session.cc:162`), then
    `*kv_store->mutable_rocksdb_files() = ListFiles(checkpoint_dir_)` (`:181`) —
    the whole RocksDB, not a subset. WAL: `GetSegmentsSnapshot` (`:297`).
  - Client persists superblock, sets `TABLET_DATA_COPYING`, downloads files, then
    `Finish()` -> `TABLET_DATA_READY`
    (`remote_bootstrap_client.cc:255,297`; `ts_tablet_manager.cc:1779-1794`).
- **Membership is required.** Flow:
  1. Node added to config via `ChangeConfig(ADD_SERVER)`; the added server **must**
     be `PRE_VOTER` or `PRE_OBSERVER` (`raft_consensus.cc:2724-2730` — explicit
     `InvalidArgument` otherwise).
  2. The new peer responds to the leader's consensus RPC with `TABLET_NOT_FOUND`,
     setting `peer->needs_remote_bootstrap = true`
     (`consensus_queue.cc:1500-1514`).
  3. Leader builds the RBS request and sends `StartRemoteBootstrap` to the peer
     (`consensus_queue.cc:1090` `GetRemoteBootstrapRequestForPeer`); source can be
     the leader or a "closest" follower (`:1115-1167`).
  4. Peer runs `TSTabletManager::StartRemoteBootstrap` (`ts_tablet_manager.cc:1665`)
     — invoked *because the leader told it to*, after it's in the config.
  5. After RBS, leader promotes PRE_VOTER->VOTER / PRE_OBSERVER->OBSERVER via
     `CHANGE_ROLE` (`raft_consensus.cc:2772-2797`).
  -> There is no "arbitrary node bootstraps from the leader without joining the
  group." The RBS *source* can be a non-leader follower, but the *target* must be
  a config member. RBS is structurally the catch-up mechanism for a config member
  whose logs the leader can't ship.

## 2. RBS copies the whole tablet — no sub-tablet scoping

Confirmed: full RocksDB (all SSTs in the checkpoint), no mechanism to bootstrap a
subset of tables. The sys_catalog tablet holds all PG catalog data *and* DocDB
metadata in one RocksDB; RBS gives you everything indivisibly. SSTs are scoped per
RocksDB instance (regular + intents), not per table. (The plan's worry 2.1 — "we
also get all other entries of sys catalog" — is real, with no in-code escape.)

## 3. Read replicas / RAFT non-voters (OBSERVER)

- Peer roles (`consensus/metadata.proto:46-65`): `PRE_VOTER=0`, `VOTER=1`,
  `PRE_OBSERVER=2` (transient, being RBS'd), `OBSERVER=3` (async replica:
  "doesn't participate in any decisions ... only accepts update requests and allows
  read requests"). OBSERVER is the read-replica primitive.
- Added via the same `ChangeConfig(ADD_SERVER)` with `member_type=PRE_OBSERVER`
  (`raft_consensus.cc:2724`), then RBS, then `CHANGE_ROLE` to OBSERVER (`:2793`).
- **Scaling cost is per-peer, per-tablet** (no hard cap constant, but O(N) fan-out):
  - Each tracked peer gets its own `Peer` object with its **own periodic heartbeat
    timer** and a `raft_pool_token_` for `SendNextRequest`
    (`consensus_peers.cc:155-167,233`). Default `raft_heartbeat_interval_ms` = 500
    (non-TSAN) (`raft_consensus.cc:94`). So the leader sends an independent
    UpdateConsensus RPC stream to *every* peer; N non-voters = N heartbeat RPCs
    every 500ms + N data streams.
  - Only mitigation `enable_multi_raft_heartbeat_batcher` is **off by default**
    (`multi_raft_batcher.cc:29`) and batches only *empty* heartbeats, not data.
  - `all_replicated_op_id` / `all_applied_op_id` iterate over **all** peers incl.
    observers and short-circuit on the first unsuccessful peer
    (`consensus_queue.cc:1189-1216`) — degraded bookkeeping with many flaky peers.
  -> Architecture is built for ~single-digit voters + a modest number of
  observers per tablet. No accommodation for hundreds of non-voters on one group.

## 4. Master sys_catalog config IS the set of masters

- `SysCatalogTable::SetupConfig` (`src/yb/master/sys_catalog.cc:395-430`) builds
  the RAFT config directly from `options.GetMasterAddresses()` — one VOTER per
  master address.
- On load, the master *re-derives* its master-address list **from** the sys_catalog
  config peers (`sys_catalog.cc:249-274`). So there's a **bidirectional assumption
  that the sys_catalog RAFT config == the set of masters** (typically 3).
- Adding tserver UUIDs as peers would corrupt master-address resolution and
  master-list RPCs; the config-change path already has special handling/concerns
  about stuck PRE_VOTERs (`raft_consensus.cc:2583-2588`).
- **No existing "master sys_catalog read replica" feature** exists.

## 5. WAL retention & catch-up-after-partition

- Knobs (`consensus/log.cc`): `log_min_seconds_to_retain` default **900s (15 min)**,
  runtime-configurable (`:106-114`); `log_min_segments_to_retain` default 2 (`:100`);
  `wal_retention_secs()` = max(per-tablet, `log_min_seconds_to_retain`)
  (`:1561-1578`).
- **Leader WAL GC does NOT pin on the slowest non-voter.**
  `TabletPeer::GetEarliestNeededLogIndex` (`tablet_peer.cc:1017-1153`) computes the
  GC floor from local state only (latest entry, anchors, pending ops, last
  committed op id, flushed op id, txn coordinator, xrepl/CDC). It does **not**
  consult `all_replicated_op_id` or per-follower positions. So a partitioned/slow
  non-voter does not hold back GC — unlike CDC/xrepl, which *does* pin via
  `GetXReplMinReplicatedIndex`.
- When the leader needs ops a peer is missing and they're GC'd:
  `ReadFromLogCache` returns NotFound -> "logs necessary to catch up peer ... have
  been garbage collected" (`consensus_queue.cc:635-642`); for `TABLET_NOT_FOUND`
  peers it sets `needs_remote_bootstrap` and triggers RBS (`:1500-1514`). So **RBS
  is the recovery path for a peer that fell behind GC'd logs** — but only because
  it's still a tracked config member.

## Feasibility assessment (to ~1000 tservers)

**(a) RBS of sys_catalog to each tserver:**
- Hard: RBS requires the target be a sys_catalog RAFT config member
  (`raft_consensus.cc:2724`). "RBS to each tserver" => "add each tserver to the
  sys_catalog config" => collides with sec 4 (config==masters) + sec 3 (O(N) fan-out).
- Hard: copies the whole tablet, no sub-table scoping (sec 2).
- A *one-time-seed-only* variant (RBS to seed an independent local tablet, not a
  config member) is conceivable but needs entirely new out-of-band plumbing — the
  trigger path is leader/config-driven — and gives only a point-in-time checkpoint
  with no built-in stay-synced mechanism.
- Verdict: usable only as a one-time seed with new plumbing; the standard RBS
  lifecycle is unusable at 1000 tservers.

**(b) Each tserver as an OBSERVER read replica:**
- Hard (fan-out): 1000 observers = master leader fanning ~1000 UpdateConsensus
  RPCs every 500ms for one tablet + 1000 data streams per DDL.
- Hard (identity coupling): adding 1000 tserver UUIDs corrupts master-address
  derivation; no master-read-replica support exists.
- Soft: queue bookkeeping degrades with flaky peers; partitioned observers
  routinely need full-tablet RBS re-seeds.
- Verdict: not feasible as-is.

## Net implication

Scaling either to 1000 tservers means either (i) a 1000-peer RAFT group (untenable
fan-out + master-identity coupling), or (ii) **a new sync channel outside
consensus** (pull-based snapshot + incremental changes). RBS provides only the
point-in-time-snapshot half and is entangled with config membership.
-> Bootstrap via a checkpoint-copy seed
([[learning-sys-catalog-and-bootstrap]]); stay synced via a CDCSDK pull poller
([[learning-cdc-wal-sys-catalog]]). The good news from sec 5: an out-of-consensus
consumer that does NOT pin master WAL GC is the safer pattern, but it means a
long-partitioned tserver needs a fresh checkpoint re-seed (and the CDC retention
barrier mechanism *does* pin WAL while a stream is active — design the retention
window deliberately).
