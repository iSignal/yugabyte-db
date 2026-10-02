================================================================================
Catalog cache misses served from cached full scans (L1 response-cache approach)
================================================================================

Companion to planning.md. planning.md evaluates the option space (L0 shared mem /
L1 in-memory response cache / L2 on-disk). This doc specifies the concrete L1
design we converged on. It is NOT the original PoC's "cache each per-key miss
individually" design - that turned out to be the fallback, not the centerpiece.

Reference PoC: 2fd6b3ed2dc "WIP: Use response cache for catalog cache misses"
(D52423). We reuse its plumbing (systable_beginscan cache-key threading,
SetResponseCacheKey, the pggate->response-cache wiring) but change WHAT is cached
and WHERE filtering happens.

--------------------------------------------------------------------------------
1. Core idea
--------------------------------------------------------------------------------
After a DDL, catcache/relcache inval msgs hit all backends; their later misses
(SearchCatCacheMiss, SearchCatCacheList, RelationBuildDesc, yb_inheritscache)
storm master with Read RPCs.

Instead of caching each per-key miss response at the tserver, SERVE the miss from
the already-cached FULL-TABLE SCAN (the same artifact preload produces) and FILTER
it in PG by the scan key.

   miss on (table T, index I, key K)
     -> fetch T's full scan in index order I   [tserver response-cache HIT;
                                                 master sees <=1 full scan per
                                                 (T, I, catalog_version), shared
                                                 across ALL backends]
     -> filter the returned rows by K in PG
     -> build the ONE matching catcache/relcache entry (or a negative if absent)
     -> discard the rest of the list

Why this shape (vs caching per-key misses):
 - Master traffic is bounded PER (table, index, version), not per distinct key.
   It cannot be amplified by a flood of distinct-key lookups.
 - Negatives are FREE and COMPLETE: a key absent from the full list is a
   definitive negative, with no RPC and no cache entry. (The per-key approach
   must cache each failing key separately -> unbounded negative-key pollution.)
 - Reuses the existing preload full-scan caching; the tserver stays a dumb blob
   cache (PG-side filtering), no new tserver query logic.
 - Cross-backend MEMORY DEDUP without shared memory (see sec. 4).

--------------------------------------------------------------------------------
2. Existing machinery we build on
--------------------------------------------------------------------------------
 - yb_cc_is_fully_loaded (catcache.c): when a CatCache is fully loaded, point
   misses synthesize negatives locally (catcache.c:2059) and list lookups are
   answered from the preloaded cache (catcache.c:2524, YbBuildCatCacheListFromPreloadedCache).
   Today this is reached only by EAGER full preload that materializes every row.
   We reach the same "answer locally from the full list" outcome LAZILY and
   WITHOUT full materialization.
 - Preload full scans: YbPreloadCatalogCache / YbFillCaches issue index-ordered
   full-table scans whose responses are cached at the tserver (PgResponseCache).
   The comment at relcache.c:2792 says preload exists to "prevent master from
   being overloaded with lots of fat read requests ... in case there are lots of
   opened connections" - i.e. preload and this design solve the SAME problem.
 - Internal paging: the prefetcher already pages large scans (fetch_row_limit ->
   multiple RPCs, each cached). Pages are in index order. This is what bounds
   large-table cost (sec. 5).
 - PoC plumbing: systable_beginscan_with_cache_key, YBCPgSetResponseCacheKey,
   PgDocReadOp::SetResponseCacheKey, CacheOptions/CachingInfo on the session.

--------------------------------------------------------------------------------
3. The miss path, step by step
--------------------------------------------------------------------------------
SearchCatCacheMiss / SearchCatCacheList today do systable_beginscan WITH the key
(cur_skey) - i.e. the key predicate is pushed to DocDB (per-key read). We change
the not-fully-loaded path to:

 1. Issue a KEYLESS (nkeys=0) scan of T on index I -> this is the full-scan
    request, keyed for the response cache identically to preload's scan, so it is
    served from the cached full scan (no master). Cache key as in the PoC:
        key_group = db_oid
        key_value = "<kind>:<reloid>:<indexoid>:<catalog_version>" + serialized
                    keyless read request
    kind in {C=catcache, L=list, R=relcache, I=inherits}. catalog_version in the
    key is the primary correctness mechanism (Disable bucket-version is backstop).
 2. FILTER (the core new step, and the main implementation challenge): the cache
    returns the WHOLE table's rows; the lookup wants the one row matching cur_skey.
    Apply the scan key to each returned tuple with
        HeapKeyTest(tuple, tupdesc, nkeys, cur_skey, result)   (access/valid.h)
    - the same predicate test heap scans use. catcache already builds cur_skey
    "suitably for HeapKeyTest" (catcache.c:1221), so reuse it. Do NOT reimplement
    key comparison: opclass/collation/NULL semantics live in sk_func/sk_collation.
 3. Build the matching entry; if no row matches, build a negative entry.
 4. Discard the non-matching rows (see sec. 4).

Relcache builds issue many systable_beginscan calls across helper functions; ALL
of them must be intercepted to "fetch the cached index-ordered full scan, filter
(HeapKeyTest) to the wanted rows". Build-time scan sites in relcache.c to
intercept (confirm the complete set during implementation):
  ScanPgRelation (pg_class), RelationBuildTupleDesc (pg_attribute),
  RelationBuildRuleLock (pg_rewrite), LookupOpclassInfo (pg_opclass, pg_amproc),
  AttrDefaultFetch (pg_attrdef), RelationGetIndexList (pg_index),
  RelationGetStatExtList (pg_statistic_ext), CheckConstraintFetch /
  RelationGetFKeyList / RelationGetExclusionInfo (pg_constraint).
(YBLoadRelations / YBUpdateRelations* are the PRELOAD path - they PRODUCE the
cached full scans, not consume them.) pg_attribute is the large one that relies on
paging + binary search (sec. 5).

yb_inheritscache (FindChildren, GetChildCacheEntryMiss): the PoC already threads
the cache-key param through ybc_systable_begin_default_scan (passing NULL today).
Pass a real "I:<relid>:<version>" key and filter the pg_inherits full scan.

--------------------------------------------------------------------------------
4. No retention
--------------------------------------------------------------------------------
No new per-backend retention structure. The backend builds the one catcache/
relcache entry (positive or negative) and frees the fetched rows; do NOT set
yb_cc_is_fully_loaded. The only persistent per-backend state is the normal
catcache/relcache entries, invalidated as usual - so a repeat lookup of an
already-seen key is a local hit, and only a genuinely-new key re-fetches (a
tserver cache hit, no master). The single shared copy of the full list is the
tserver cache entry.

--------------------------------------------------------------------------------
5. Multiple index orders, paging, and large tables
--------------------------------------------------------------------------------
Multiple index orders: a lookup filters efficiently only if the fetched list is
ordered by the lookup's key. So cache one full scan PER (table, index) used by a
syscache. E.g. pg_class by oid (RELOID) AND by (relname, relnamespace) (RELNAMENSP).
Cost: multiplies cached full-scan entries by #indexes-per-table (usually 1-2).

Paging bounds cost for large tables (pg_attribute, pg_proc, pg_statistic):
 - Memory: stream pages, filter each, discard -> never hold more than one page.
 - Transfer: pages are index-ordered. Two levels:
     * baseline: stream pages with early-stop once the sorted key passes K
       (avg ~half the table worst case the whole table per new key).
     * optimization: expose page-boundary keys so the backend BINARY-SEARCHES to
       the single page whose range covers K and fetches only that page (tserver
       cache hit). Transfer per miss ~= one page, independent of table size.
   The binary-search-over-pages optimization is what makes relcache builds on
   huge pg_attribute cheap. Needs page-boundary metadata cached alongside the
   scan; design detail to settle (store first-key per page, or an ordinal index).

Net: a single mechanism covers all table sizes. No separate individual-miss
cache. (If binary-search-over-pages proves hard, per-key caching of the PoC stays
available as a narrow fallback for the few largest tables - but the goal is to
avoid it.)

--------------------------------------------------------------------------------
6. Cache structure
--------------------------------------------------------------------------------
A lazily-filled scan is the same kind of entry as a preload scan (a full
index-ordered scan response), so it shares the existing PgResponseCache as-is:
same key_group/version/Disable machinery, same capacity/eviction. No new cache
and no preload-vs-miss subsection split (that split was for the rejected per-key
design).


--------------------------------------------------------------------------------
7. DDL correctness
--------------------------------------------------------------------------------
 - DDL write path MUST bypass the cache. A DDL transaction that has performed
   writes must read its own uncommitted catalog writes, not a cached response.
   Gate: BuildCatalogCacheOptions returns nullopt when session->IsDdlMode() (PoC
   already does this). Keep it.
 - Cross-version safety: catalog_version is in the cache key. A backend reading at
   version V computes V-keyed requests; a full scan cached at V was read from a
   consistent V snapshot, so filtering it is consistent for any reader at V. A
   backend that has advanced to V+1 computes a different key -> natural miss ->
   repopulate. Backstop: on DDL commit, response_cache().Disable(db_oid) bumps the
   bucket version (pg_client_session.cc ~4091).
 - Negatives: "key absent from the full V-scan" is correct for V and invalidated
   with V. No special handling, but test it.
 - Filtering correctness: the PG-side filter must reproduce the index scan's
   semantics exactly (operator class, collation, NULL handling) - reuse the
   catcache's existing cur_skey comparison, do not re-implement.
 - read_time determinism: the keyless full-scan request must serialize identically
   across backends (the PoC clears stmt_id + metrics_capture; confirm no
   backend-varying read_time rides in the read_request - catalog read_time lives
   in PgPerformOptionsPB, so likely fine; verify, else sharing degrades to misses).
 - Mid-statement catalog version change (cf. #28002): the version used for the key
   must be the version the lookup is performed at (YbGetCatalogCacheVersion at scan
   time); ensure it does not shift between building the key and reading.

--------------------------------------------------------------------------------
8. Metrics
--------------------------------------------------------------------------------
Goal: see how many misses are served from the response cache vs master.
 - tserver: the existing pg_response_cache_* metrics (hits/queries/entries/evicted)
   cover the shared full-scan cache as-is. master_read_rpc already exists and counts
   fallthrough-to-master (the PoC tests assert on it).
 - PG side: YbNumCatalogCacheMisses / per-id / per-table already exist. Add a
   counter for "miss served from cached full scan" vs "miss that hit master", fed
   by the served-from-cache bit in sec. 9.
 - A "full-scan fetches per (table, version)" counter to confirm master traffic is
   bounded per-table (the core claim).

--------------------------------------------------------------------------------
9. Logging (yb_debug_log_catcache_events)
--------------------------------------------------------------------------------
The GUC should log, per miss, whether the data came from the response cache or
master. PG does not know this today - the tserver does. Plumb a
`served_from_response_cache` bit from the tserver PgPerformResponse back through
pggate to PG, and log it at each miss site:
   "catalog cache miss on cache %d key %s: served from response cache"
   "catalog cache miss on cache %d key %s: read from master"
(The PoC already added yb_debug_log_catcache_events logs in SearchCatCacheMiss but
they do not indicate source; add the source.)

--------------------------------------------------------------------------------
10. Test plan
--------------------------------------------------------------------------------
Base (from PoC, keep/adapt):
 - CatcacheMissResponseCacheSharing: conn B populates, conn C hits, 0 master reads.
 - CatcacheMissResponseCacheInvalidationOnDDL: after a DDL, miss goes to master.
 - pg_libpq BasicFunctionalTest: two conns, same SELECT, restart to clear cache.

Additions for this design:
 - Per-path coverage, sharing across two backends with 0 master reads on the 2nd:
     * catcache point lookup (SearchCatCacheMiss)
     * catcache list lookup (SearchCatCacheList, e.g. functions by name)
     * relcache build (RelationBuildDesc -> pg_class/pg_attribute/pg_index)
     * yb_inheritscache (partitioned/inherited table children)
 - Free-negative test: look up a non-existent object; assert it resolves WITHOUT a
   master read once the full scan is cached, and produces a negative entry.
 - Large-table / paging test: relcache build on a wide table (many pg_attribute
   rows); assert per-miss transfer is bounded (one/few pages, not whole table) -
   validates binary-search-over-pages.
 - Filter-correctness test: lookups exercising collation / opclass / NULL key
   semantics return the same rows as a non-cached index scan.
 - Cache-pressure test: lazily-fill scans for many tables; confirm hot
   (preload-set) scans are not thrashed beyond expectation.
 - Dedup test: many backends do the same lookups; assert tserver holds ~one
   full-scan copy per (table, index, version) and per-backend memory ~ working set.
 - DDL-mid-statement / version-bump correctness test.

--------------------------------------------------------------------------------
11. Open items / what to prototype & measure
--------------------------------------------------------------------------------
 - Per-table, per-index-order caching granularity: preload currently BATCHES many
   tables into one cached response. Misses need to fetch a single table's
   index-ordered scan -> change/extend the caching to per-(table,index) entries.
   Confirm and design this; it is the main new plumbing.
 - Page-boundary metadata for binary-search-over-pages (sec. 5). Without it, large
   tables degrade to streaming with early-stop; measure whether that is acceptable
   before investing in boundary indexing.
 - Whether to keep eager preload on, minimize it, or drop it (sec. 6) - measure
   cold-start vs per-backend memory.
 - Filtering CPU: filtering medium tables per new key in PG - measure vs a per-key
   read. If filtering dominates for some cache, that cache is a candidate for
   per-key caching (the narrow PoC fallback).
 - read_time determinism verification (sec. 7).

 - [ ] Possible better fixes to explore for temp tables at a later point:
0. Just increment catalog version for temp tables as well when we have concurrent ddl flag enabled.
1. after a temp table is created, force a local reload of response cache on the tserver to be rebuilt
from the master. This ensures that though the response cache is at the same version, it will now contain
the temp table. There might be existing code that does something similar for temp namespaces, check for it.
This part is a bit tricky because the next time the cache is rebuilt we need to ensure that it is rebuilt at
a read time that is after the temp table ddl. Given the rebuild can be initiated by any backend, it may be
tricky to ensure this read time is always higher than the last temp table commit time.
2. Alternately, we can just bump up catalog version for temp table ddl as well if this flag is enabled. But
given it is a common operation on many backends, this may be an issue.
3. Another idea is to identify temp table ddl and from that point on, we retry negative cache misses on master
for specific syscaches that could contain temp table entries. Needs some thought.

 - [ ] TODO warming the response cache
    - Add YBCEnsureResponseCache(version): async fire-and-forget warm of the
      (table,index) full-scan set, requests built by the SAME builder as the miss
      path (cache key MUST byte-match). Call at conn start + on version change. We don't really
      need a response from this, we can return an "error" in this case if needed that can be ignored on
      the receiving side perhaps. Or find some other way to return an empty response that is ignored by
      the receiving side. Otherwise it is a lot of memory for every backend to receive full catalog tables.
      (replaces YBRefreshCache eager re-preload). Single-flight bounds the warm burst.
    Risks/deps:
    Tests: Start a backend, wait for a bit for ensure requests to complete. Then verify we get all
    response cache hits from catcache misses.



------------------------------------------------------------------------

------------------------------------------------------------------------
13. TODO / test matrix
------------------------------------------------------------------------
Prototype status (pg_attribute, NO preloading flags):
 [x] catcache/relcache miss -> KEYLESS pg_attribute scan, response-cached at tserver under
     "<reloid>:<indexoid>:<catalog_version>", filtered in PG via HeapKeyTest.
 [x] Cross-backend sharing on one tserver: backend A miss populates, backend B served from cache.
 [x] DDL version bump invalidates: post-DDL backend recomputes key, does not reuse the stale entry.
 [x] Tests live in pg_libpq-test (ExternalMiniCluster, 3 tservers / 1 master), only --vmodule set;
     an initial connection initializes (relcache init file), succeeding connections are the subject.

Why "no preloading flags" for the prototype: eager preload PRE-BUILDS all relcache entries at
connection startup, so a fresh backend never lazily rebuilds a relcache and there is no miss to
serve from the response cache. The user-table relcache path is only exercised when preload is off.

Implemented & passing (pg_libpq-test, ExternalMiniCluster 3 tservers, preload OFF, only --vmodule;
each test: an init connection sets up, then FRESH connections are the subject; "fully served from
cache" == response-cache queries delta equals hits delta == zero new misses == no master read):
 [x] CatcacheResponseCacheSharing - user-table relcache build (pg_attribute via
     AttributeRelidNumIndexId) + ATTNUM point lookup: backend A populates, backend B served from cache.
 [x] CatcacheResponseCacheInvalidationOnDDL - after an unrelated DDL bumps the catalog version, a
     fresh backend recomputes the key, misses the stale entry, and reads master (does not reuse it).
 [x] CatcacheResponseCacheAllCachesSharing (adapts PoC CatcacheMissResponseCacheSharing) - a plpgsql
     function call drives misses across MANY caches (pg_proc by-name LIST + PROCOID, pg_type,
     pg_namespace, pg_language, relcache builds); backend B is served ENTIRELY from cache.
 [x] CatcacheResponseCacheDistinctKeySharing - backend A resolves '+', backend B the DIFFERENT '-'
     operator, still fully served from the shared full pg_operator/pg_proc scans (0 new misses) -
     the headline property: full-scan sharing across DISTINCT keys, impossible under per-key caching.
 [x] CatcacheResponseCacheRelcacheCorrectness - a fresh backend rebuilds a rich schema's relcache
     via the cached scans; DEFAULT, CHECK, FK, RULE and INHERITANCE all behave correctly (the
     PG-side filter reproduces index-scan semantics) - covers pg_attrdef/pg_constraint/pg_rewrite/
     pg_inherits/pg_index.
 [x] CatcacheResponseCachePartitionedRelcacheSharing - backend A builds partitioned parent p1's
     relcache (+ secondary index + partition child); backend B builds a DIFFERENT parent p2 and is
     fully served from the same full scans (distinct-relation sharing, incl. index relcache).
 [x] Regression: PgLibPqTest.TableColocation (DDL/relcache-heavy) passes with the cached path on by
     default - the converted relcache scan sites do not regress relcache builds.

Implementation landed for all of the above (preload OFF):
 [x] catcache point lookups (SearchCatCacheMiss) and list lookups (SearchCatCacheList).
 [x] 10 relcache RelationBuildDesc consumer scan sites (pg_class, pg_attribute, pg_rewrite,
     pg_opclass/pg_amproc, pg_attrdef, pg_index, pg_statistic_ext, pg_constraint x3); the 4
     YB*Relations* preload PRODUCERS deliberately left as-is.
 [x] yb_inheritscache (FindChildren, GetChildCacheEntryMiss) - keyless pg_inherits scan cached.
 [x] Cache key built in yb_scan.c from the ACTUAL scan target: "<reloid>:<scan_target>:<version>",
     scan_target = InvalidOid for base/PK scan or the secondary index oid - so a base-table scan and
     an index scan are never conflated (different row order) while same-scan lookups collapse.
     Gated by YbShouldResponseCacheCatalogRead() (YB + !initdb + !prefetching + flag); callers pass a
     bool, not a key string (more concise than the PoC's threaded-key plumbing).
 [x] served_from_response_cache bit plumbed tserver -> PG (PgPerformResponsePB -> PerformResult ->
     PerformFuture::Data -> PgDocOp -> YBCPgGetResponseCacheHit); logged per miss under
     yb_debug_log_catcache_events ("served from response cache" / "read from master").
 [x] pg_response_cache_capacity default raised 1024 -> 3072 (3x).

Follow-up cases to add
- [x] Disable the usage of this response cache for catcaches if any preloading gflags (boolean or additional tables) is
detected.
- [x] Disable this usage of response cache once a DDL is detected. YbShouldResponseCacheCatalogRead
might be a good spot. For DDL presence, look in postgres.c, there are different functions for
transactional ddl vs regular, both need to be accounted for.
Tests: To test this, run through an existing regression test like TestPgRegressMisc or TestPgRegressTable.java or TestPgRegressIndex.
- [x] Fix temp tables. temp tables don't increment catalog version so we can see an issue with them
during yb.orig.alter_table_rewrite where we use response cache and don't find the temp table cache
entry.
- [ ] INVESTIGATE: can a catalog-miss read carrying response-cache options get entangled with
buffered write operations in the SAME Perform? Observed in TestPgRegressPgAuth yb.port.privileges:
under yb_non_ddl_txn_for_sys_tables_allowed, lo_create buffers a pg_largeobject_metadata write; the
catcache-miss read is then flushed in the same Perform (2 ops), but the response-cache key is built
from ONLY the read op's span, so a cache hit returned 1 response for a 2-op Perform ->
"Wrong number of responses: 1, while 2 expected".
  - Error fires at: src/yb/yql/pggate/pg_client.cc:557 (SCHECK_EQ responses.size()==operations.size()).
  - Cache key built from read-op span only: src/yb/yql/pggate/pg_doc_op.cc PgDocReadOp::SetResponseCacheKey (~line 774-806).
  - We trialed a guard in BuildCatalogCacheOptions (src/yb/yql/pggate/pg_doc_op.cc ~line 59-66) that
    returned nullopt when session->HasBufferedOperations(), but REVERTED it (the only known trigger is
    the artificial no-version-bump GUC flow, handled by disabling caching in TestPgRegressPgAuth).
  - Open question: is this reachable outside yb_non_ddl_txn_for_sys_tables_allowed (e.g. a catcache
    miss during normal buffered DML)? If yes, the proper fix is either flush buffered ops in a
    separate Perform before the cacheable read, or fold the full Perform op-set into the cache key.


- [partially resolved] @repo2-yugabyte-db/src/postgres/src/backend/utils/misc/pg_yb_utils.c#205-209 one subtle question here. when we see a new version in ybrefreshcache, we may invalidate some entries then set this version to the higher value. confirm this is the case. while invalidating entries (inval.c), postgres sometimes can attempt to rebuild relcache entries which call back into relcachebuilddesc. Would such scans read at the old cache version instead of the higher cache version? We may need to maintain a separate version that increments slightly ahead of yb_catalog_cache_version maybe. We can write a test for this case. The test case would use object locking and do something like
session 1: alter table t session 2: select * from t; -> goes fine, then select * from t -> this should wait on session 1
when session 2 gets the lock it will likely try to rebuild the cache entry as part of invalidation msg processing.
  CONFIRMED the ordering: YBRefreshCacheWrapperImpl (postgres.c ~4804/4814) calls
  YbApplyInvalidationMessages() BEFORE YbUpdateCatalogCacheVersion(). So in principle a rebuild
  during inval application reads the old version.
  TEST WRITTEN: pg_libpq-test PgLibPqTest.CatcacheResponseCacheRebuildDuringInvalSeesNewVersion
  (fixture PgCatcacheResponseCacheObjectLockTest, object locking on). session2 builds relcache at V;
  session1 holds an uncommitted ALTER ... ADD COLUMN; session2's SELECT sum(b) blocks on the object
  lock; session1 commits (V+1); session2 acquires the lock, rebuilds during inval processing, and
  MUST see the new column (sum=21). The test PASSES today => the rebuild ends up reading the NEW
  version (rebuild is lazy / post-bump, or active version already reflects V+1), so the
  "separate version ahead of yb_catalog_cache_version" mitigation is NOT needed for this path.
  (Left open: other rebuild-during-apply paths not covered by this scenario.)



 [ ] use_relcache_file ON: confirm succeeding connections that load the init file still exercise
     (or correctly skip) the miss path for user tables.
 [x] catcache point-lookup path (ATTNUM/ATTNAME) standalone coverage (not only via relcache build).
 [ ] Free-negative test: lookup of an absent attribute resolves from the cached full scan with no
     master read and yields a negative entry.
 [x] Large pg_attribute (wide schema / many tables): per-miss transfer bound (paging / early-stop;
     binary-search-over-pages optimization in sec. 5).
 [x] Other syscache.h caches once pg_attribute is reviewed (pg_class, pg_proc, pg_statistic, ...).
 - [x] One issue is that for very large catalog tables, we are not only holding lot of tserver copies
 for each catalog version in memory but we are also passing it all down to each PG. Things to try if
 this becomes an issue
 -- compress the response cache entries
 -- have a mode where response cache is not full table caches, it is for each catcache key, so no
 keyless lookups in this mode. The response cache has to be sized a bit larger maybe.
 -- free PG memory more aggressively somehow?
 - [ ] One possible issue with temp tables. The read time we choose is not consistent across tables
 so it is possible that we have cached pg class without a temp table and pg attribute with a temp table.
 Hopefully it doesn't make a diff to other backends. But one option is to just also increment catalog
 version always, even for temp tables.
 - [x] Cache-key granularity GUC: yb_catalog_cache_key_columns, string, default ''.
   Grammar: comma list of table:N (table-level, applies to all that table's syscaches;
   accepted loss of per-syscache precision). N = # leading index key cols added to the
   cache key and bound as prefix-scan keys; N in {0,1} now, extensible to 2. Unlisted = 0
   (whole table/index). N=1 => one entry per 1st-key value (e.g. pg_attribute by attrelid),
   PG still HeapKeyTest-filters remaining cols.
   Reject unknown table / N > index key count.
   Default override list: 'pg_attribute:1,pg_statistic:1'.
   1-key tables are lazy-on-miss only (not eagerly warmed - prefix values unknown at startup).
   Composes on top of the shared-vs-per-db version/db_oid key prefix (orthogonal).
- [ ] Paging is explicitly disabled, one option could be to let the cache key for the keyless call
 be the serialized rpc similar to pg_sys_table_prefetcher.cc so that multiple rpcs are made and cached
 independently.
 - [ ] While processing inval msgs, we may rebuild relcache entries that have a refcount. In such
 cases, the lookup should use the correct catalog version (one we are going to).
 - [ ] Instead of messing with fixing up guaranteed read times in pg client, is it an option that when a backend learns of a catalog version via shared mem, it also gets the guaranteed time from the tserver to use. Then for response cache lookups it can add the right read time to the call.


--------------------------------------------------------------------------------
15. PROPOSAL: cluster-propagated catalog-version watermark (population read times)
--------------------------------------------------------------------------------
Problem being solved: While doing relcache builds, it is important to scan at a consistent catalog version. Until now in this prototype, it was possible that we use a pg_class cached value at version V, then miss for pg_attribute to master at what we think is version V but is really version V + 1 as there is a change on master at that point. This can result in inconsistencies during relcache build as spurious or missing columns may be seen. It is possible
 this issue is really only specific to the non-object-locking case.
Families 8/11/12 in section for Jenkins run triage were all violations of it. Consumer-side read-time
adoption on hits (the Family 8 attempt, since REVERTED) fixed one path but pinned arbitrarily old
times onto consumers (planner_base_scans_cost_model regression: a fresh session adopted a
pre-UPDATE read time from an old entry and its executor scan of pg_class read stale reltuples) and
risks "Snapshot too old" since miss-path entries have NO renewal (lifetime_threshold_ms = nullopt).

Core idea (extends commit 588c526d511 / D45086): that commit already established the consistency
proof - "reading missed data at the same read time at which catalog version V was read guarantees
the data is consistent with V" - and uses a per-backend version_read_time captured by a master RPC
(YbGetCatalogCacheVersionForTablePrefetching, pg_yb_utils.c:333, with an explicit TODO to remove
the RPC). This proposal lifts that into cluster-propagated, per-version PROOF TIMES. Two proof
sources, both produced by the master's existing heartbeat-rate version poll:

  For each db (AS IMPLEMENTED):
    T_guarantee  - for the LATEST version: the read time of the most recent pg_yb_catalog_version
                   poll that still observed it. Advances on every poll. The poll's read RESULT is
                   the proof; there is no race in CONSUMING a stored T_guarantee later - it is a
                   fixed past time at which V was provably current, regardless of any commit since.
    commit(V)    - for SUPERSEDED versions V: the commit hybrid time of V + 1 minus 1, taken from the
                   DocHybridTime (row write time) of V's row in pg_yb_invalidation_messages. That
                   table writes one immutable row per (db_oid, version) in the same transaction
                   that bumps the version, so the row's write time IS commit(V) - none of the
                   column-update precision trap that makes pg_yb_catalog_version's own row write
                   time unusable (its single row is updated in place). Reading at commit(V+1) - 1 yields
                   exactly V-consistent content.

Implementation path
1. While sending heartbeat responses or ReleaseObjectLocks calls, the master uses DocRowwiseIterator::LastFetchedRowWriteTime to see the hybrid commit time for each row in pg_yb_invalidation_messages table. This gives it the commit time for version V, which it uses to compute guaranteed read time for version V ((commit time of (V + 1)) - 1).
2. Tserver uses heartbeat response to record guaranteed read times and uses it during catalog
cache misses for version V.
3. Existing master fingerprinting to avoid sending same catalog versions again and again was
disabled. Existing heartbeat cache on master also needs work for this path.
- [ ] TODO: fix these paths

--------

Separately, it was observed that many tests were failing because they expect new sessions to see changes in DDL from other sessions prior to heartbeat. Instead, we made a longstanding pending fix to propagate catalog versions for any DDL on commit in the non object locking case
similar to what happens in the object locking case. This is done via PG ReportYsql... to master
being done for all DDL but master side receiver of this RPC then does two things with it
1. Usual DDL atomicity checks if the DDL has docdb schema changes
2. New path - calls ReleaseObjectLocks to propagate catalog versions and their guaranteed
read times to tservers.

