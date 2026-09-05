# Learning: PG catalog read path & where to redirect to a local tablet

Source: code reading pass 1 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`.

## Executive summary — three candidate redirect chokepoints

1. **Meta-cache tablet resolution** (`src/yb/client/meta_cache.cc`) — most
   transparent. Catalog reads go to master with no explicit `if(catalog)` branch:
   a catalog table's partition list contains exactly one tablet,
   `kSysCatalogTabletId`, whose Raft replicas *are the masters*. Redirect =
   "make the catalog table's `RemoteTablet` resolve to the local tablet." But this
   bypasses the read-time validation that the tserver layer has context for ->
   riskiest for consistency.
2. **`PgClientSessionImpl::DoPerform`** (`src/yb/tserver/pg_client_session.cc:3308`)
   — **cleanest engineering chokepoint**. Every catalog read RPC lands here; the
   response cache already short-circuits master reads here (`:3342`), and
   `use_catalog_session()` selects catalog semantics. A local-tablet read can be
   served here exactly as the response cache is, reusing the catalog-version +
   read-time plumbing.
3. **PG-side `YbNewSelect` / `YBCPgNewSelect`** — single PG funnel for catalog
   reads, but furthest from read-time/version machinery; better as a routing hint.

**Dominant hazard:** catalog reads use a master-leader-chosen, session-pinned
snapshot read time (read-your-writes). A lagging local copy can violate this.

## 1. catcache/relcache miss path -> one PG funnel (systable_beginscan)

- catcache miss: `SearchCatCacheMiss`
  (`src/postgres/src/backend/utils/cache/catcache.c:2028`); scans at `:2085`
  `table_open` + `:2125` `systable_beginscan` + `:2135` `systable_getnext`. YB
  fast-path short-circuits at `:2059` (`yb_cc_is_fully_loaded` + negative cache).
- relcache miss: `RelationBuildDesc` -> `ScanPgRelation`
  (`src/postgres/src/backend/utils/cache/relcache.c:521`), same
  `systable_beginscan` at `:564`.
- Funnel: `systable_beginscan` is YB-overridden
  (`src/postgres/src/backend/access/index/genam.c:407`):
  `if (IsYBRelation(heapRelation)) return ybc_systable_beginscan(...)` ->
  `ybc_systable_beginscan` (`yb_scan.c:4221`) -> `YbBeginScan` (`yb_scan.c:3882`).
- `YbBeginScan` builds the request via `YbNewSelect` (`:3919`), sets catalog
  version (`:3963`), executes via `ybcFetchNextHeapTuple` (`:574` `YBCPgExecSelect`,
  `:580` `YBCPgDmlFetch`).
- `YbNewSelect` (`src/postgres/src/backend/utils/misc/pg_yb_utils.c:9412`) is the
  **single PG->pggate boundary**: `YBCPgNewSelect(YBCGetDatabaseOid(rel),
  YbGetRelfileNodeId(rel), prepare_params, ...)`.
  -> PG-side redirect chokepoint = `YbNewSelect`/`YBCPgNewSelect` (catalog
  identified by `IsSystemRelation(rel)` / `is_ysql_catalog_table`); execution
  chokepoint = `YBCPgExecSelect`/`YBCPgDmlFetch`.

## 2. How catalog reads reach storage — the Perform RPC

Reads become `PgsqlReadOp`, flushed via `PgSession::Perform`
(`src/yb/yql/pggate/pg_session.cc:837`) -> `pg_client_.PerformAsync(...)` (`:975`).
**No separate Read RPC — reads and writes ride `Perform`** (`PgPerformRequestPB`
with `ops[]` + `PgPerformOptionsPB`). Handler: `PgClientServiceImpl::Perform` ->
`DoPerform` (`pg_client_session.cc:3308`).

A read is recognized as catalog by `GetRequiredSessionType`
(`pg_session.cc:194`):
```c
return op.is_read() && table.schema().table_properties().is_ysql_catalog_table()
       && !YBCIsInitDbModeEnvVarSet() ? SessionType::kCatalog : SessionType::kRegular;
```
`kCatalog` sets `use_catalog_session=true` in perform options
(`RunHelper::Flush`, `pg_session.cc:512`).

## 3. Why catalog reads route to master — tablet metadata, not a branch

- `use_catalog_session` does NOT select master; it only controls read-time
  semantics + a dedicated session object.
- Master selection is a meta-cache fact: `kSysCatalogTabletId`
  (`sys_catalog_constants.h:39`); its single `TabletInfo` is created at master
  startup (`catalog_manager.cc:1953`) and its replicas are the masters. Catalog
  tables carry `is_ysql_catalog_table=true` (`catalog_manager.cc:3907`) and map to
  that one tablet. Meta-cache never evicts it
  (`src/yb/client/meta_cache.cc:2533,2544`). Op-level identity:
  `YBOperation::IsYsqlCatalogOp()` (`src/yb/client/yb_op.cc:414`).
- On tserver, `DoPerform`->`SetupSession` maps `use_catalog_session()` ->
  `PgClientSessionKind::kCatalog` (`pg_client_session.cc:3592`) and **forbids
  reading catalog from followers** (`SCHECK(!options.read_from_followers())`).
  -> The redirect target: make the catalog table resolve to a local `RemoteTablet`
  (or intercept in `DoPerform`) instead of `kSysCatalogTabletId`.

## 4. Preloading & the tserver response cache (existing "serve without master")

- Prefetcher: `src/yb/yql/pggate/pg_sys_table_prefetcher.{h,cc}`. `Register()`
  appends sys tables; `Prefetch()` reads them in one `Perform`. Cache modes
  `YbcPgSysTablePrefetcherCacheMode` (`ybc_pg_typedefs.h:603`):
  `TRUST_CACHE_AUTH / TRUST_CACHE / RENEW_CACHE_SOFT / RENEW_CACHE_HARD`.
- PG entry points: `YbRegisterSysTableForPrefetching` (`pg_yb_utils.c:6808`),
  `YBCStartSysTablePrefetching` (`relcache.c:2626`), `YbPreloadRelCacheImpl` /
  `YbRunWithPrefetcher` (`relcache.c:2652,2997`); filling: `YbPreloadCatalogCache`
  (`syscache.c:1502`), `YbFillCaches` (`relcache.c:2556`).
- Preload gflags (`src/yb/yql/pggate/ybc_gflags.cc`):
  `ysql_catalog_preload_additional_tables` (`:74`),
  `ysql_catalog_preload_additional_table_list` (`:79`),
  `ysql_minimal_catalog_caches_preload` (`:91`),
  `ysql_enable_read_request_caching` (`pg_sys_table_prefetcher.cc:51`, default true
  — master switch for the response cache).
- Response cache: `src/yb/tserver/pg_response_cache.{h,cc}`. LRU keyed by
  `Key{KeyGroup group; std::string value}` (`:299`), `group = db_oid`. Enabled in
  `DoPerform` by `options.has_caching_info()` (`pg_client_session.cc:3342`); a hit
  replays cached row-data sidecars and **skips the DocDB/master read entirely**
  (`:3347` early return; `Apply()` at `:920`). Invalidated on DDL commit via
  `response_cache().Disable(...)` (`:4171`), bumping `bucket.version`
  (`pg_response_cache.cc:438`).
- Cache key: `BuildCacheKey` (`pg_sys_table_prefetcher.cc:260`). `key_value` =
  (1) version block (`is_db_catalog_version_mode` char + varint catalog version,
  `:239`); (2) length-prefixed `catalog_read_time` PB (`:280`); (3) each op's
  serialized `LWPgsqlReadRequestPB`, length-prefixed (`:281-285`).
  `TemporaryClearInsignificantFields` (`:212`) **excludes exactly `stmt_id` and
  `metrics_capture`**. `key_group = db_oid`. This is precisely the plan's
  "serialized RPC request minus a few fields."
  -> The response cache is the architectural precedent for serving a catalog read
  without touching master. A local-tablet redirect is the same move but serving
  from a real tablet (fresh data) rather than a frozen-snapshot blob.

## 5. Relcache init file (YB specifics)

- Filename `pg_internal.init` (`relcache.h:27`); in DB-catalog-version mode,
  per-database for both shared and non-shared, with `.db` suffix + db-OID prefix
  (`RelCacheInitFileName`, `relcache.c:316`).
- Catalog version written into the file right after the magic:
  `write_relcache_init_file` writes `YbGetCatalogCacheVersion()`
  (`relcache.c:9498`).
- Staleness check on load (`load_relcache_init_file`, `relcache.c:8908`): if
  backend version > stored version, try incremental `YbTryRevalidateRelcacheFile`
  (`:6503`, replays inval messages); on failure unlink. Clean load adopts the
  file's version (`:9394`).
- "Relcache init connection": new backends needing a rebuild trigger
  `YbTriggerInternalRelcacheBuild` (`:6466`) -> a dedicated internal superuser
  connection `YB_RELCACHE_INIT_BACKEND` (`tablet_server.cc:1404,1457`;
  `yb_internal_conn.c:41`) builds+writes the file once per DB.
- Invalidation on DDL: `RelationCacheInitFilePreInvalidate` (`relcache.c:9761`)
  unlinks files under `RelCacheInitLock` before broadcasting SI messages.
- Gflags: `ysql_enable_relcache_init_optimization` (`ybc_gflags.cc:135`, true),
  `ysql_use_relcache_file` (`pggate_flags.cc:185`, true).

## 6. Read-time / consistency — the primary hazard

Catalog reads use a master-chosen, session-pinned snapshot:
- First catalog read after reset: master leader picks the read time;
  `ProcessUsedReadTime` (`pg_client_session.cc:402-439`) captures it, sets
  `global_limit = read` to **suppress read-restarts** for sys-catalog (`:427`),
  returns it in `resp.catalog_read_time`.
- pggate pins it: `pg_client.cc:612` -> `pg_perform_future.cc:75`
  `TrySetCatalogReadPoint(...)` -> `PgSession::catalog_read_time_`
  (`pg_session.h:84,275`), reused for later catalog reads
  (`pg_session.cc:841-847`) => read-your-writes within the snapshot for DDLs the
  session performed.
- After invalidations the tserver forces the latest snapshot (`UpdateReadTime`,
  `pg_client_session.cc:3648-3699`; `use_catalog_session()` forces `ResetReadPoint`).

**Hazards for a local-tablet redirect:**
1. **Lag / pinned read time.** Session expects to read at a specific master-chosen
   HT. A lagging local tablet may not have applied up to it -> stale read or can't
   honor the pinned `catalog_read_time_`. Must guarantee local tablet safe time >=
   pinned read time, else fall back to master.
2. **Read-your-writes after the session's own DDL.** Session bumps
   `catalog_read_time_` to "latest" after invalidations; a lagging copy misses its
   just-committed changes. The catalog version in requests (and in the
   response-cache key) is the existing freshness guard — a redirect must validate
   the local copy is at/after the required catalog version before serving (mirror
   `YbTryRevalidateRelcacheFile` / response-cache version-bucket logic).
3. **No read-restart machinery on the local path.** Sys-catalog reads disable
   read-restarts (`global_limit = read`); the local serve must preserve that.

## Recommended redirect points, ranked

- **`DoPerform` (`pg_client_session.cc:3308`)** — best. Same place the response
  cache intercepts; add "serve from local catalog tablet if its safe time >=
  required read time / catalog version, else fall through to master." Reuses
  existing catalog-version + read-time plumbing and the `use_catalog_session`
  signal.
- **Meta-cache tablet resolution** — most transparent but bypasses read-time
  validation; riskier.
- **PG-side `YbNewSelect`/`YBCPgNewSelect`** — single funnel but furthest from
  read-time/version machinery; better as a routing hint than the serve point.

The freshness guard (local safe time vs required HT/version) ties directly to
[[learning-catalog-version-propagation]] and the poller safe time in
[[learning-cdc-wal-sys-catalog]].
