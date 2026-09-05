# Architecture learnings: tserver-local DocDB copy of the catalog

Consolidated reference distilled from a code-reading pass (2026-06-19). Each
section is a standalone heading you can consult later. Deep detail with full
file:line citations lives in the per-topic docs:
- [[learning-sys-catalog-and-bootstrap]]
- [[learning-cdc-wal-sys-catalog]]
- [[learning-catalog-version-propagation]]
- [[learning-pg-catalog-read-redirect]]
- [[learning-remote-bootstrap-and-read-replica]]
- [[learning-reseed-and-cross-raft-bootstrap]]  (pass 2 — DECIDED bootstrap)
- [[learning-sync-channel-and-gate]]            (pass 2 — DECIDED sync + gate + schema + read-side)
- [[learning-retention-scaling-reseed]]         (pass 2 — retention / 1000-consumer scaling / re-seed)

All paths under `/net/dev-server-sanketh-3/share/code/yugabyte-db`.

---

## Master sys_catalog tablet

- One tablet, hardcoded id `kSysCatalogTabletId = "0"*32`
  (`master/sys_catalog_constants.h:39`). One RocksDB holds **two** kinds of content:
  - DocDB metadata rows in the `sys.catalog` table, keyed `(entry_type INT8,
    entry_id BINARY) -> metadata` (schema `sys_catalog.cc:808-814`; entry types in
    `master_types.proto:31-57`). *We do not want these on tservers.*
  - PG catalog tables (pg_class, ...) as **real co-located DocDB tables**, one per
    `(db_oid, table_oid)` — table_id packs both (`common/entity_ids.cc:125-139`);
    rows prefixed by `cotable_id` UUID in the DocKey (`dockv/doc_key.h:114-124`).
    *These are the payload to replicate.*
- The PG-table schemas live in `RaftGroupMetadata` (`KvStoreInfoPB.tables[]`,
  `tablet/tablet_metadata.h:277-358`). **SST data is meaningless without this
  schema registry.**
- Existing in-tablet "copy the PG catalog" primitive:
  `SysCatalogTable::CopyPgsqlTables` (`sys_catalog.cc:1972-2014`), used by CREATE
  DATABASE to clone template1.
- Row iteration helpers: `ReadNextSysCatalogRow` / `EnumerateSysCatalog`
  (`sys_catalog_writer.cc:62,243`).

## Bootstrap primitives (initdb snapshot, checkpoint restore, local tablet)

- Initial snapshot format = `(rocksdb/ checkpoint, exported_tablet_metadata_changes
  PB)` (`master/sys_catalog_initialization.cc:63-66`). initdb runs in the master's
  embedded PG and writes the master sys_catalog directly.
- **Restore is a filesystem op, not a Raft replay**: hard-link SSTs into the
  tablet RocksDB dir (`TabletSnapshots::RestoreCheckpoint`,
  `tablet/tablet_snapshots.cc:508-545`) + replay ChangeMetadata to register
  schemas (`sys_catalog_initialization.cc:118-162`). Both work on any TabletPeer
  with no live master / no multi-node Raft.
- **Closest template for a fresh local tablet seeded from SSTs:**
  `TSTabletManager::DoCloneTablet` (`ts_tablet_manager.cc:1486-1538`) — new
  `RaftGroupMetadata::CreateNew` + hard-link SSTs + `TABLET_DATA_READY` + open.
- **Single-replica self-leading tablet is supported** — the master sys_catalog is
  exactly this in single-master mode (`SysCatalogTable::CreateNew`,
  `sys_catalog.cc:354-392`, single-peer `RaftConfigPB`).
- Caveats: the static `share/initial_sys_catalog_snapshot` has only
  template0/template1 (per-user-db catalogs are created lazily) — a live seed needs
  a live checkpoint of the master tablet; and SSTs need their ChangeMetadata
  schema set.

## CDC / WAL streaming of sys_catalog (the update channel)

- **Already works today**: master runs a `CDCServiceImpl` (`master/master.cc:319`,
  flag `ysql_yb_enable_implicit_dynamic_tables_logical_replication`);
  `GetServingTablet(kSysCatalogTabletId)` resolves it (`master/master_tserver.cc:85`);
  `cdcsdk_virtual_wal` already polls sys_catalog and decodes per-catalog-table rows
  (`DeterminePubRefreshFromMasterRecord`, `cdc/cdcsdk_virtual_wal.cc:1792`).
- **Two formats, opposite costs:**
  - **CDCSDK logical** (`cdc/cdcsdk_producer.cc`): skips provisional intents
    (`IsIntent(msg)->continue`, `:2161`), reads *committed* intents on the APPLY
    record, emits decoded INSERT/UPDATE/DELETE per cotable stamped with
    `commit_time` (BEGIN/COMMIT wrapped). Apply as plain upserts/deletes — **no
    local IntentsDB, no transaction-apply**. USE THIS.
  - **xCluster raw** (`cdc/xcluster_producer.cc`): ships raw WAL KV incl. intents;
    consumer must keep its own IntentsDB + re-run apply. AVOID.
- **Poller template:** `XClusterPoller` (`tserver/xcluster_poller.cc`) —
  self-rescheduling reactor loop, idle delay default 100ms, tracks
  `(op_id checkpoint, safe_hybrid_time)`, advances checkpoint only after local
  apply (`:656`).
- **Safe-time signal:** `GetChangesResponsePB.safe_hybrid_time = T` => all commits
  <= T delivered (empty batch advances to leader safe time). This is the catch-up
  watermark used for the version gate below.
- Gaps to close: only ~3 catalog tables are "streamable" today
  (`GetStreamableCatalogTables`) — needs generalization to all catalog+shared
  tables, all DBs; DDL/schema-change records need special handling (new cotables,
  new columns); per-db / CREATE DATABASE semantics.

## Catalog version propagation & version<->hybrid-time anchor

- Two propagation channels master->tserver: heartbeat (`master_heartbeat.proto:291-296`,
  ~1s) and, with object locks, the synchronous per-tserver `ReleaseObjectLocks` RPC
  (`tserver.proto:508-547`) carrying version + inval messages +
  `apply_after_hybrid_time`.
- Tserver writes versions into a shared-memory atomic array
  (`tserver_shared_mem.h:146`), read by PG backends each stmt/txn boundary.
- **The exact anchor:** the `pg_yb_catalog_version` bump and the
  `pg_yb_invalidation_messages` insert for `(db_oid, V)` are **one CTE statement in
  the DDL txn** (`pg_proc.dat:8145-8156`) => one commit HT. So the DocDB
  **write_time** of the inval row for `(db_oid, V)` **is** V's commit HT. Not
  surfaced today (master reads values only, `sys_catalog.cc:1874-1928`) but
  `PrimitiveValue::GetWriteTime()` exists -> projectable with no schema change.
- **The gating seam:** `ReleaseObjectLocks` is synchronous and already does a clock
  wait (`WaitToApplyIfNecessary -> WaitUntil`) **before** applying the version and
  ack'ing (`ts_local_lock_manager.cc:486-532`). Insert "wait until local tablet
  safe time >= V's commit_HT" between the wait and `SetYsqlDBCatalogVersions`.
- Wait-pattern precedents already in code: `WaitUntil` (tserver),
  `YbWaitForSharedCatalogVersionToCatchup` (PG-side shm poll, `pg_yb_utils.c:1223`).

## PG catalog read path & redirect chokepoints

- catcache/relcache misses funnel through `systable_beginscan` (YB override,
  `access/index/genam.c:407`) -> `YbBeginScan` -> `YbNewSelect`/`YBCPgNewSelect`
  (`pg_yb_utils.c:9412`).
- Reads ride the `Perform` RPC (no separate read RPC); catalog reads flagged
  `kCatalog` via `is_ysql_catalog_table` (`pg_session.cc:194`). They reach master
  purely because the catalog table's only tablet is `kSysCatalogTabletId` whose
  replicas are masters — **no explicit "route to master" branch**.
- **Best redirect chokepoint:** `PgClientSessionImpl::DoPerform`
  (`tserver/pg_client_session.cc:3308`) — where the response cache already
  short-circuits master reads (`:3342`). Serve from the local tablet when its safe
  time >= required read time / catalog version, else fall through to master.
- Existing "serve catalog without master" precedent: the response cache
  (`tserver/pg_response_cache.cc`), keyed on db_oid + serialized read request minus
  `stmt_id`/`metrics_capture` (`pg_sys_table_prefetcher.cc:212,260`).
- **Consistency hazards:** catalog reads pin a master-leader-chosen read time for
  read-your-writes (`pg_client_session.cc:402-439`, `catalog_read_time_`); a
  lagging local copy can violate it -> must gate on local safe time >= pinned read
  time / required version, with master fallback. Sys-catalog reads also disable
  read-restarts (`global_limit=read`) — the local serve must preserve that.

## RAFT / remote bootstrap / read replicas (ruled out for sync)

- **Invariant:** to receive a tablet's data via consensus you must be a peer in its
  RAFT config (`ADD_SERVER` must be PRE_VOTER/PRE_OBSERVER,
  `consensus/raft_consensus.cc:2724`).
- The **sys_catalog config == the set of masters**, bidirectionally
  (`master/sys_catalog.cc:395-430` and `:249-274`) — adding tserver peers corrupts
  master-identity resolution.
- Per-peer O(N) leader fan-out (own 500ms heartbeat timer + data stream per peer,
  `consensus/consensus_peers.cc:155`); multi-raft batching off by default and only
  batches empty heartbeats. => ~1000 non-voters on one group is untenable.
- RBS copies the whole tablet, no sub-table scoping
  (`tserver/remote_bootstrap_session.cc:162,181`).
- Leader WAL GC does **not** pin on slow non-voters
  (`tablet/tablet_peer.cc:1017`), but **CDC retention barriers do** pin WAL while a
  stream is active (`cdc/cdc_service.cc` `CDCMasterBgTask`) — design the retention
  window deliberately.
- => Use an **out-of-consensus** approach: checkpoint-copy bootstrap + CDCSDK pull
  poller. Read-replica / RBS-as-sync are not viable at scale.

---

## DECIDED design (pass 2) — bottom line

- **Bootstrap/reseed (critical-path):** cross-RAFT-group **remote snapshot transfer**
  (`RemoteSnapshotTransferClient`, no membership, snapshot-only, schemas ride in the
  superblock). NOT consensus RBS as sync, NOT read-replica. Seed = RocksDB
  checkpoint at a committed OpId; start the poller there. See
  [[learning-reseed-and-cross-raft-bootstrap]].
- **Sync channel:** per-tserver poller of the master sys_catalog tablet using the
  **xCluster RAW WAL format** (directly-applicable DocDB KV), applied via the
  **external-intents / external-transactions** path. Local copy is a normal tablet
  (regular + IntentsDB) but needs **no transaction participant and no status
  tablet**. The one blocker is a single guard (`cdc_service.cc:1707`) plus
  XCLUSTER-stream wiring on sys_catalog. See [[learning-sync-channel-and-gate]].
- **IntentsDB is required:** intents stream eagerly (pre-commit, same WAL); they
  can't become regular records until the APPLY record supplies commit_ht, so they
  must be staged. "Both intents + regular DB" is just normal tablet structure.
- **Version gate:** expose version V only when the **local copy's applied
  pg_yb_catalog_version >= V** (analog of `YbWaitForSharedCatalogVersionToCatchup`,
  pointed at the local copy). NOT participant-safe-time (external intents bypass the
  participant) and NOT the producer's xCluster safe-time. Requires in-order apply.
- **Baseline = wait for the master APPLY (commit) record to stream**, then apply
  via `apply_external_transactions`. Master-signaled eager local-apply is a deferred
  latency optimization (prior art: `xcluster_external_apply_bootstrap-test.cc:315`).
- **CDC chosen over Raft observers:** cheaper idle floor (empty GetChanges is O(1),
  no per-consumer server-side timers/state), producer-side cotable filtering, no
  config-change-per-tserver writes into sys_catalog, and the structural blockers
  (config==masters, O(N) fan-out) are decisive regardless of idle efficiency.

## Cross-cutting synthesis: the emerging design shape

1. **Bootstrap** = checkpoint-copy seed (clone/snapshot-restore primitives, invoked
   out-of-band) from a *live* master sys_catalog checkpoint, NOT remote bootstrap.
   Need to decide whether to keep or filter the non-PG `sys.catalog` rows.
2. **Update** = a per-tserver CDCSDK poller (XClusterPoller-shaped) against
   `kSysCatalogTabletId`, applying decoded committed catalog rows to the local
   tablet, tracking `safe_hybrid_time`. Avoids the intents problem entirely.
3. **Correctness gate** = surface `(db_oid, V) -> commit_HT` (write_time of the
   inval row) from master; on the tserver, don't expose version V to backends until
   local tablet safe time >= commit_HT. The synchronous `ReleaseObjectLocks` seam
   is the natural ack-gating point; heartbeat path needs an equivalent gate.
4. **Redirect** = intercept catalog reads in `DoPerform`, serve from the local
   tablet when fresh enough (safe time >= required read time/version), else fall
   back to master. Preserve read-restart suppression.

### Pass-2 resolutions (open questions answered)

- **Schema/new-cotable propagation:** raw stream DOES carry `CHANGE_METADATA_OP`,
  schema-before-rows ordering guaranteed. New consumer component = replay
  CHANGE_METADATA verbatim into local `RaftGroupMetadata` (bypass the stock
  consumer's remap, which ignores CREATE/DROP DATABASE). See
  [[learning-sync-channel-and-gate]].
- **Read-side:** provisional external intents are structurally invisible; pin reads
  at the local apply safe time (`SingleTime`, restarts off) — reuse
  `UpdateReadPointForXClusterConsistentReads`. Aborted-DDL intents GC'd by TTL
  (lower `external_intent_cleanup_secs`). See [[learning-sync-channel-and-gate]].
- **Gate:** one in-memory `local_applied_version[db]` map (poller-maintained) serves
  the ReleaseObjectLocks wait (synchronous, `ts_local_lock_manager.cc:497`), the
  heartbeat clamp (`min(master_reported, local_applied)`, NOT deferral), and the
  redirect freshness check (`PgsqlReadRequestPB.ysql_db_catalog_version`). See
  [[learning-sync-channel-and-gate]].
- **Retention/scaling/re-seed:** WAL retention bounded by 24h cap + 4h sys_catalog
  staleness reset (laggards self-evict); per-stream tracking (slowest-of-N pins WAL
  up to caps); re-seed signal = `CHECKPOINT_TOO_OLD` -> snapshot transfer; idle poll
  must stay < 4h staleness window (empty polls cheap). See
  [[learning-retention-scaling-reseed]].

### Biggest open risks (for later passes)
- Generalizing the "streamable catalog tables" set to the full catalog + shared
  tables across all DBs, incl. schema/DDL changes and CREATE/DROP DATABASE.
- The bootstrap-from-live-checkpoint path (no static snapshot covers a running
  cluster) and how to invoke checkpoint/copy out-of-band without RAFT membership.
- Retention-window sizing vs. long tserver partitions (re-seed story).
- Read-time/version freshness validation on the local serve path, and the fallback
  to master when the local copy lags the pinned read time.
- Filtering (or tolerating) the DocDB-internal `sys.catalog` rows on tservers.
