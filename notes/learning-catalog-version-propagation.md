# Learning: catalog version propagation & version<->hybrid-time correlation

Source: code reading pass 1 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`.

## Why this matters for the design

The correctness contract for the local catalog copy: when a tserver learns of a
new catalog version V, it must NOT expose V to PG backends until its local copy
contains V's writes. This doc establishes (a) how V propagates today and where we
can gate it, and (b) that V can be tied to an exact hybrid time.

## Executive summary

- The `pg_yb_catalog_version` bump and the `pg_yb_invalidation_messages` insert
  for `(db_oid, V)` happen in **one SQL statement inside the same DDL txn**, so
  they share **one commit hybrid time**. Therefore the DocDB **write_time** of the
  inval-message row for `(db_oid, V)` **is** V's DDL commit HT. (No existing code
  surfaces this write_time yet — master reads only column *values* — but it exists
  at the storage layer and is projectable.)
- There is already a synchronous tserver RPC (`ReleaseObjectLocks`) carrying the
  new version + inval messages + an `apply_after_hybrid_time` clock-wait, applied
  **before** ack. This is the natural seam to gate version exposure on local-tablet
  catch-up.
- The catch-up wait pattern already exists twice: `WaitUntil` (tserver clock
  busy-wait) and `YbWaitForSharedCatalogVersionToCatchup` (PG-side shm poll).

## 1. Heartbeat path: master -> tserver shared memory

- Proto (`src/yb/master/master_heartbeat.proto`): `TSHeartbeatResponsePB` carries
  `ysql_catalog_version` (`:291`), `ysql_last_breaking_catalog_version` (`:293`),
  `db_catalog_version_data` (`:295`, per-db `DBCatalogVersionDataPB`),
  `db_catalog_inval_messages_data` (`:296`). Request carries
  `ysql_db_catalog_versions_fingerprint` (`:249`) so master can skip resending.
- Master fill: `FillHeartbeatResponse`
  (`src/yb/master/master_heartbeat_service.cc:297-368`):
  `GetYsqlAllDBCatalogVersions(use_cache, &versions, &fingerprint)`; if fingerprint
  matches, omit data; else fill per-db versions and (when
  `FLAGS_ysql_yb_enable_invalidation_messages`) inval messages from
  `GetYsqlCatalogInvalationMessages`.
- Tserver receive: `src/yb/tserver/heartbeater.cc:498-554` ->
  `SetYsqlDBCatalogVersionsWithInvalMessages(...)` / `SetYsqlDBCatalogVersions(...)`.
- Shm write: `SetYsqlDBCatalogVersionsUnlocked`
  (`src/yb/tserver/tablet_server.cc:1499-1688`): maintains in-memory
  `ysql_db_catalog_version_map_` (db_oid -> `CatalogVersionInfo{current_version,
  last_breaking_version, shm_index, ...}`), monotonicity-checks (stale -> FATAL),
  writes `shared_object()->SetYsqlDbCatalogVersion(shm_index, new_version)` (`:1662`).
- Shm layout (`src/yb/tserver/tserver_shared_mem.h`): atomic array
  `std::atomic<uint64_t> db_catalog_versions_[kMaxNumDbCatalogVersions]` (`:146`),
  release/acquire access (`:92-100`); global is `catalog_version_` (`:142`).
- PG read: `PgApiImpl::GetSharedCatalogVersion(db_oid)`
  (`src/yb/yql/pggate/pggate.cc:2041-2067`) — global or per-db via cached
  `shm_index`.

## 2. Per-db vs global

Per-database is the supported mode (heartbeater.cc:498 comment: "the only
supported mode"). Source of truth = `pg_yb_catalog_version` (one row per db:
`db_oid, current_version, last_breaking_version`). Master reads via
`CatalogManager::GetYsqlDBCatalogVersion` / `GetYsqlAllDBCatalogVersions`
(`catalog_manager.cc:10942`, `:11001`) backed by
`sys_catalog_->ReadYsqlDBCatalogVersion` / `ReadYsqlAllDBCatalogVersions`; a
background `heartbeat_pg_catalog_versions_cache_` (`:10944-10959`) is used by
heartbeat, bypassable with `use_cache=false`.

## 3. ReleaseObjectLocks path — the gating seam

Flow: PG backend (post-DDL) -> `CatalogManager::ReleaseObjectLocksGlobal`
(`catalog_manager.cc:7011`) -> `ObjectLockInfoManager::UnlockObject`
(`object_lock_info_manager.cc:1099`) -> per-tserver `ReleaseObjectLocks` RPC ->
`TSLocalLockManager::Impl::ReleaseObjectLocks`.

- Per-tserver request carries version data (`src/yb/tserver/tserver.proto:508-547`):
  `ReleaseObjectLockRequestPB` has `apply_after_hybrid_time` (`:522`),
  `db_catalog_version_data` (`:535`), `db_catalog_inval_messages_data` (`:539`).
  Comment (`:525-534`): this is an optimization to update tservers "instead of
  waiting for the next heartbeat." (The *global* master request does NOT carry
  version data, only `apply_after_hybrid_time` — `master_ddl.proto:855`.)
- Master injects fresh versions right before fan-out: `PopulateDbCatalogVersionCache`
  (`object_lock_info_manager.cc:1025-1076`, called `:1163`) reads fresh
  `GetYsqlAllDBCatalogVersions(use_cache=false)` and
  `GetYsqlCatalogInvalationMessages(use_cache=false)`. Not persisted to sys-catalog
  (stripped in `ReleaseRequestToPersist`, `:601-602`); re-populated each retry.
- Tserver processing + existing wait (`src/yb/tserver/ts_local_lock_manager.cc:486-532`):
  1. `WaitToApplyIfNecessary(req, deadline)` (`:497`, impl `:452-461`): if
     `apply_after_hybrid_time` set, `WaitUntil(clock, sleep_until, deadline)` — a
     clock busy-wait (`src/yb/common/clock.cc:30-43`) until local hybrid clock
     reaches the HT.
  2. Apply version: `SetYsqlDBCatalogVersionsWithInvalMessages(...)` (`:506-508`).
  3. Release locks (`:522-531`).
- The RPC handler `TabletServiceImpl::ReleaseObjectLocks`
  (`src/yb/tserver/tablet_service.cc:3936`) is **synchronous** — no ack until
  `ReleaseObjectLocks` returns. **A wait inserted between steps 1 and 2 directly
  delays the ack.**
- Caveat: on the normal DDL release path `apply_after_hybrid_time` is typically
  *unset* (only set for lost-message / out-of-order-acquire cases,
  `object_lock_info_manager.cc:509-511`, `:596`), so today no HT wait happens
  before applying. This is exactly the seam to add: "block until local catalog
  tablet applied-time >= V's commit HT before `SetYsqlDBCatalogVersions` and ack."

## 4. pg_yb_invalidation_messages — schema, atomicity, the HT anchor

- Schema (`src/postgres/src/include/catalog/pg_yb_invalidation_messages.h`):
  `db_oid(oid), current_version(int64), message_time(int64 seconds-since-epoch),
  messages(bytea)`. **PK = (db_oid, current_version)** (`:53`). OID 8080.
- **Version bump + inval insert are one CTE statement in the DDL txn.**
  `YbIncrementMasterDBCatalogVersionTableEntryImpl`
  (`src/postgres/src/backend/catalog/yb_catalog/yb_catalog_version.c:440-519`)
  calls SQL builtin `yb_increment_db_catalog_version_with_inval_messages`, body in
  `src/postgres/src/include/catalog/pg_proc.dat:8145-8156`:
  ```
  ... with changed_version as (
    update pg_yb_catalog_version set current_version = current_version + 1, ...
    returning db_oid, current_version)
  insert into pg_yb_invalidation_messages
    select changed_version.db_oid, current_version,
           extract(epoch from clock_timestamp())::bigint, messages
    from changed_version returning current_version
  ```
  => the version UPDATE and the inval INSERT for `(db_oid, V)` commit atomically at
  one hybrid time. `message_time` is wall-clock `clock_timestamp()` used only for
  TTL purge, NOT an HT.
- Master read: `ReadYsqlCatalogInvalationMessages`
  (`src/yb/master/sys_catalog.cc:1874-1928`) projects only
  `db_oid, current_version, messages` (`:1890`) into a map keyed by
  `(db_oid, current_version)`. It does **not** read `message_time` and does
  **not** read the row's DocDB write_time (uses a safe-time read).
- Backend read: tserver keeps an in-memory inval-message queue keyed by version;
  backends fetch contiguous range x+1..x+k via
  `PgGetTserverCatalogMessageListsRequestPB`
  (`src/yb/tserver/pg_client.proto:1120-1131`). The queue must have no holes.

## 5. Backend apply path

Two gates by object-locking mode:
- **Object locking OFF (default):** per-statement gate in main loop
  (`postgres.c:6978`) -> `YBCheckSharedCatalogCacheVersion`
  (`src/postgres/src/backend/tcop/postgres.c:5219-5258`): compares
  `YbGetSharedCatalogVersion()` vs `YbGetCatalogCacheVersion()`; if behind, refresh.
  Only when NOT in a transaction (`:5226`).
- **Object locking ON:** per-transaction gate — `AcceptInvalidationMessages`
  (`inval.c:993`, from `StartTransaction` via `AtStart_Cache`) -> `YbMaybeRefreshCache`
  (`inval.c:885`).
- Full vs incremental: `YBRefreshCacheWrapperImpl` (`postgres.c:4743-4869`):
  incremental (fetch `YBCGetTserverCatalogMessageLists` + `YbApplyInvalidationMessages`)
  when inval messages enabled and gap is contiguous and within
  `ysql_max_invalidation_message_queue_size`; else full `YBRefreshCache`
  (resets caches + `YBPreloadRelCache` / `YbFillCaches`).
  `YbApplyInvalidationMessages` (`pg_yb_utils.c:8841`) validates all messages
  before applying. `YbFillCaches` (`relcache.c:2555`) = full preload.
- **Existing catch-up wait:** after a DDL the executing backend pushes its new
  version+messages into local shm via `YBCPgSetTserverCatalogMessageList`
  (`pg_yb_utils.c:3125`); on TryAgain it falls back to
  `YbWaitForSharedCatalogVersionToCatchup(new_version)`
  (`pg_yb_utils.c:1223-1310`) which polls shm (100ms) until version >= V.

## 6. Deriving the commit HT of version V

- **No existing mechanism** maps version V -> commit HT. Master tracks versions as
  integer column values read at a safe-time read (`catalog_manager.cc:10942/11001`
  -> `sys_catalog.cc:1006`), plus a value cache and fingerprint.
- **But the data exists and is feasible to surface.** Because the version bump and
  the inval row for `(db_oid, V)` commit in one txn (sec 4), the DocDB write_time
  of the `pg_yb_invalidation_messages` row for `(db_oid, V)` equals V's commit HT.
  DocDB rows carry an `EncodedDocHybridTime write_time` per row
  (`src/yb/docdb/doc_rowwise_iterator.cc:787-790, 879-880`); `PrimitiveValue`
  exposes `GetWriteTime()`. The master's inval read could be extended to also
  surface each row's write_time -> an authoritative `(db_oid, V) -> commit_HT` map
  at the leader, **no schema change needed**.

## Design implication (cleanest correlation + gating)

1. Extend the master's inval-message read to return per-row `write_time`, giving
   `(db_oid, V, commit_HT)`.
2. Attach `commit_HT` to the `ReleaseObjectLocks` request (and/or heartbeat).
3. In `ts_local_lock_manager.cc:486` (between `WaitToApplyIfNecessary` and
   `SetYsqlDBCatalogVersions`), wait until the **local catalog tablet's
   applied/safe time >= commit_HT** before exposing V. The synchronous RPC makes
   this a natural ack-gating point; `WaitUntil` /
   `YbWaitForSharedCatalogVersionToCatchup` establish the wait pattern.

The local-tablet safe time comes from the CDCSDK `safe_hybrid_time` the poller
tracks — see [[learning-cdc-wal-sys-catalog]].

### Key file:line index
| Concern | Location |
|---|---|
| HB response version fields | `master_heartbeat.proto:291-296` |
| Master fills versions+inval into HB | `master_heartbeat_service.cc:297-368` |
| Tserver receives HB -> shm | `heartbeater.cc:498-554` |
| Shm write (per-db slot) | `tablet_server.cc:1499-1688` |
| Shm atomic array | `tserver_shared_mem.h:92-100,146` |
| PG reads shm version | `pggate.cc:2041-2067` |
| ReleaseObjectLock req carries version+HT | `tserver.proto:508-547` |
| Master injects fresh versions | `object_lock_info_manager.cc:1025-1076,1163` |
| Tserver: wait -> apply -> unlock | `ts_local_lock_manager.cc:452-461,486-532` |
| Clock busy-wait | `common/clock.cc:30-43` |
| Sync release RPC handler | `tablet_service.cc:3936-3966` |
| inval table schema/PK | `pg_yb_invalidation_messages.h:31-53` |
| version+inval in one stmt | `pg_proc.dat:8145-8156`; `yb_catalog_version.c:440-519` |
| Master reads inval (no write_time) | `sys_catalog.cc:1874-1928` |
| Backend version gate (per-stmt) | `postgres.c:5219-5258,6978` |
| Backend full-vs-incremental | `postgres.c:4743-4878` |
| Existing catch-up wait (PG) | `pg_yb_utils.c:1223-1310,3083-3141` |
| DocDB per-row write_time | `doc_rowwise_iterator.cc:787-790,879-880`; `dockv/primitive_value` GetWriteTime |
