# Learning: WAL retention, stream scaling (1000 consumers), and re-seed detection

Source: code reading pass 2 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`. Operational concerns for the
xCluster-raw poll design at scale. See [[learning-sync-channel-and-gate]] for the
sync mechanism and [[learning-reseed-and-cross-raft-bootstrap]] for the seed path.

## Upfront: the format/stream-type constraint (reaffirmed)

`cdc_service.cc:1705-1711` hard-rejects any non-CDCSDK stream on the sys_catalog
tablet ("Polling sys catalog tablet is only supported for CDC"). The production
path that polls master sys_catalog today is **CDCSDK** (logical replication /
replication slots, driven by `CDCMasterBgTask`). The raw (xCluster) format comes
from `GetChangesForXCluster`. So to get raw format on sys_catalog we must lift the
1707 guard + create an XCLUSTER-source_type stream on `kSysCatalogTabletId`. The
retention/staleness/cdc_state machinery below is shared and works for either stream
type once the guard allows it.

## Q1 — What bounds master sys_catalog WAL retention

WAL GC: `TabletPeer::RunLogGC` -> `GetEarliestNeededLogIndex`
(`tablet_peer.cc:1144`) -> `log_index_needed_by_cdc`; applied in
`Log::GetSegmentsToGCUnlocked` (`log.cc:1441-1447`) via
`GetXReplMinReplicatedIndex` (`log.cc:1481-1489`, min of local
`cdc_min_replicated_index_` and the registered xcluster callback wired at
`ts_tablet_manager.cc:2358`).

Retention is bounded back to the slowest consumer's checkpoint, BUT with HARD CAPS
that override the CDC pin (`log_reader.cc:321-325`, gated by
`enable_log_retention_by_op_idx`, default true):
- **Age cap:** `log_max_seconds_to_retain` (default **24h**, `log_reader.cc:56`).
- **Space cap:** `log_stop_retaining_min_disk_mb` (default 100GB, `:61`).
- **Time-based floor for xcluster tablets:** `wal_retention_secs()` raises to
  `max(configured, cdc_wal_retention_time_secs)` (default **8h**,
  `log.cc:244,1561-1578`).
- **Intents/history:** `cdc_intent_retention_ms` (default **8h**, `log.cc:236`),
  validated `<= cdc_wal_retention_time_secs*1000`. For sys_catalog, intent/history
  barriers are managed by `CDCMasterBgTask -> SetAllCDCRetentionBarriers`
  (`cdc_service.cc:3274`), with special-case initial-barrier handling (`:3259`).

=> A stuck consumer cannot pin the WAL indefinitely.

## Q2 — One shared stream vs per-tserver streams at 1000 consumers

- `cdc_state` is keyed `(tablet_id HASH, stream_id RANGE)` with a `checkpoint`
  column (`cdc_state_table.cc:503-508`) — one checkpoint row per (stream, tablet).
- Producer retention = MIN checkpoint across all stream rows for the tablet
  (xCluster `ProcessEntryForXCluster`, `cdc_service.cc:2630`; CDCSDK
  `PopulateTabletMinCheckpoint`, `:2603`; `GetXClusterMinRequiredIndex`, `:3047`).
- **Shared stream (one stream_id for all 1000 tservers):** one checkpoint row,
  last-writer-wins. Avoids the laggard-pin, but loses per-consumer progress
  tracking — re-seed detection becomes all-or-nothing. Not viable for independent
  per-tserver progress.
- **Per-stream (1000 streams, 1000 cdc_state rows):** correct per-consumer
  tracking, but the slowest of 1000 pins the master WAL up to the caps in Q1.

### The safety valve: staleness reset (laggards self-evict)
`RunLogGC` calls `reset_cdc_min_replicated_index_if_stale()` before computing
retention (`tablet_peer.cc:870`). If a stream hasn't refreshed its barrier within
`cdc_min_replicated_index_considered_stale_secs` (**1800s/30min** normal,
**14400s/4h for sys_catalog**, `tablet_peer.cc:108-115,1207-1210`), its barrier is
reset to int64-max (`:1229-1292`), unpinning the WAL. A dead/partitioned consumer
stops pinning after the staleness window and is then forced to re-seed (Q3). This
is the "retain only to a global cap, laggards re-seed" behavior we wanted — already
implemented.

### Scaling cost / sharp edge
1000 rows in `cdc_state` (YCQL system table). Producer min-index map
(`xcluster_tablet_min_opid_map_`) is refreshed periodically by full-scanning
cdc_state (`cdc_service.cc:3035-3057,3844`); per-GC cost is O(1) per tablet but the
refresh scan is O(streams). **Fail-safe sharp edge:** if the map goes stale (refresh
didn't run within `max_staleness_secs`), `GetXClusterMinRequiredIndex` returns
**0 -> retain ALL WAL** (`cdc_service.cc:3040-3044`) — disk-growth risk on master at
1000 streams. Watch the refresh-scan cost and staleness.

## Q3 — How a consumer detects it must re-seed

Two reusable signals:
1. **Reactive (natural fit):** GetChanges returns `CHECKPOINT_TOO_OLD` when the
   requested OpId has been GC'd — `ReadFromLogCacheForXRepl`
   (`consensus_queue.cc:761-780`) returns NotFound -> `CDCErrorPB::CHECKPOINT_TOO_OLD`
   (`cdc_service.proto:93`). The xCluster poller catches it
   (`xcluster_poller.cc:553-555`, stores `REPLICATION_MISSING_OP_ID`). On this
   error: trigger remote snapshot transfer, then resume polling from the new
   checkpoint.
2. **Proactive (pre-flight):** `IsBootstrapRequired` RPC
   (`cdc_service.cc:5140-5197` -> `IsBootstrapRequiredForTablet`,
   `xcluster_producer_bootstrap.cc:70-105`) tries to read `min_op_id+1`; NotFound
   -> re-bootstrap required. Reusable to decide whether to seed before polling.

## Q4 — Retention window vs. lazy-idle + triggered poll

- **Empty polls keep a consumer "current" cheaply.** With no new messages the
  producer echoes back `from_op_id` (`xcluster_producer.cc:462-463`); the consumer
  writes it back via `UpdateCheckpointAndActiveTime` (`cdc_service.cc:2096`),
  refreshing `last_replication_time` / active-time and the staleness timer even with
  zero data.
- **HARD CONSTRAINT:** the idle poll interval must be comfortably shorter than
  `cdc_min_replicated_index_considered_stale_secs` for sys_catalog (**4h**), or the
  master treats the consumer as dead, resets the barrier, GCs the WAL, and the next
  poll gets `CHECKPOINT_TOO_OLD` -> forced re-seed. Polling every few minutes (or
  <= 1h) when idle keeps every consumer alive at negligible cost (empty polls are
  cheap). The eager-on-DDL trigger handles freshness; the lazy heartbeat exists
  purely to avoid the staleness reset.
- Silent-WAL note: low write volume itself is not a risk (RunLogGC finds nothing to
  GC). The risk is purely the consumer-side poll cadence vs the staleness window.

## Scaling risks (for pass 2 design)

1. **xCluster-on-sys_catalog blocked** (`cdc_service.cc:1707`) — lift the guard +
   XCLUSTER stream. (The single most important new-code item for the sync channel.)
2. **Slowest-of-1000 pins the WAL** up to the 4h staleness window and the 24h hard
   cap. Mitigated (not eliminated) by the staleness reset.
3. **cdc_state scan cost & retain-everything fail-safe** at 1000 streams — watch the
   producer min-index refresh; staleness there -> retain all WAL (disk growth).
4. **Lazy-poll vs 4h staleness window** — keep idle interval well under it.

Key files: `consensus/log.cc:1426-1489,1556-1578`, `consensus/log_reader.cc:238-333`,
`tablet/tablet_peer.cc:866-882,1144-1152,1200-1292`,
`cdc/cdc_service.cc:1705-1711,2095-2114,2603-2644,3035-3058,3228-3282`,
`cdc/cdc_state_table.cc:503-508`, `consensus/consensus_queue.cc:761-780`,
`cdc/xcluster_producer_bootstrap.cc:70-105`, `tserver/xcluster_poller.cc:462,553`.
