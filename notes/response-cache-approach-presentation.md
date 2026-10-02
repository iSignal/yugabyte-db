================================================================================
Serving catalog cache misses from cached full scans
================================================================================
Summary of response-cache-approach.md. See that document for design detail and
the full issue/test list.

-----------
Context
-----------
How does preloading work today?

At connection start (and on catalog
version change) each backend reads full catalog tables and materializes the
catcache/relcache. Preloading itself issues fat full-table reads to the master,
and materializes a separate full copy of the catalog caches in every backend.

--------------------------------------------------------------------------------
Primary motivation: post-DDL invalidation storm
--------------------------------------------------------------------------------
Despite above, a problem still exists.

When a DDL commits, the catalog version is bumped and invalidation messages are
sent to all YSQL backends. Each backend invalidates the affected catcache and
relcache entries. When those entries are next needed, each backend independently
rebuilds them through SearchCatCacheMiss / SearchCatCacheList / RelationBuildDesc,
issuing catalog read RPCs that terminate at the master. With many backends, or
frequent DDL, these reads arrive at the master together.


--------------------------------------------------------------------------------
Secondary motivations
--------------------------------------------------------------------------------
- Reuse a catalog read across backends: the first backend to read a given
  (table, version) populates a shared tserver cache entry; other backends on the
  same tserver are served without contacting the master.
- Reuse a catalog read across different lookups on the same table: one cached
  full-table scan answers any key lookup on that table by filtering in PG, so
  distinct keys (e.g. different operators, different relations' attributes) share
  one cached entry.
- Obtain preloading's master-offload benefit without preloading's per-backend
  memory cost: catalog data is held once per tserver rather than materialized in
  every backend.

--------------------------------------------------------------------------------
Solution
--------------------------------------------------------------------------------
A catalog cache miss is served from a cached full-table scan rather than a
per-key read to the master:

 1. The miss path issues a keyless scan of the catalog table (or a prefix-bounded
    scan for configured large tables).
 2. The tserver's existing PgResponseCache serves it; the master sees at most one
    scan per (table, scan-target, catalog version), shared across backends.
 3. PG applies the scan key to the returned rows with HeapKeyTest and builds the
    one needed entry; a key absent from the full scan yields a negative entry.

The cache key is the scan target: table oid, the index or base/PK being scanned,
and the catalog version. No new cache is introduced; lazily filled scans share the
existing response cache, capacity, and invalidation (version in the key, plus the
per-key-group Disable on DDL commit). This covers catcache point and list lookups,
the relcache build scans, and the inherits cache.

For large tables (pg_attribute, pg_statistic), the miss issues a scan with the 1st scan key set
instead of a full table scan. This is configurable per catalog table.

DDL: a transaction that has performed catalog writes must read its own
uncommitted writes, so the cached path is bypassed while a DDL is in progress.

Concern: Catalog cache miss for version V may read at different times from master and the populated value from the miss may actually correspond to version
V + 1. This is a real concern in practice if we attempt reuse cache for RelcacheBuildDesc
which scans pg_class, pg_attribute and other tables at a consistent read point to build the
relation descriptor.
Solution: Master sends db_oid -> catalog_version map today. Now it will add some information to this
db_oid -> (catalog_version, upper_bound_read_time). Tservers will include
this info in their local catalog version map and use this to read at the right time
for a given catalog version.
Details: To be able to populate such a map, the master reads the hybrid time for each row
for the invalidation messages table. This table has pkey (db_oid, version) and a row
is never updated once inserted, so we can use this time to definitively know the commit time of a
catalog version. The upper bound read time for a given version is the commit time of its higher version
minus 1. For the highest catalog version, this time is the last time master read this version for
heartbeats.

--------------------------------------------------------------------------------
Limitations
--------------------------------------------------------------------------------

- Temp tables: temp table creation does not bump the catalog version, so a scan
  cached at a given version may omit a temp table; the cached path is disabled
  completely once a session uses temp tables.
- Requires the response cache and is mutually exclusive with preloading: disabled
  when any preloading gflag is set (not a hard requirement but for simplicity)
- Cache is duplicated by index/main table given some sites rely on order.

--------------------------------------------------------------------------------
Issues to be resolved (detailed in response-cache-approach.md)
--------------------------------------------------------------------------------
- A catalog-miss read can be flushed in the same Perform as a buffered write op;
  the cache key is built from the read op only, giving a response-count mismatch.
- Rebuild during invalidation application can read at the pre-bump version (test
  written; currently passes on the covered path).
- How does this work with obj locking / concurrent ddl / txn DDL etc?


--------------------------------------------------------------------------------
What works in the prototype today
--------------------------------------------------------------------------------
Implemented and passing with preloading off (pg_libpq-test, ExternalMiniCluster,
3 tservers / 1 master; an init connection sets up, fresh connections are the
subject; "served from cache" == zero new response-cache misses and no master read):

- Catcache point (ATTNUM/ATTNAME) and list (SearchCatCacheList) lookups served
  from cached scans.
- 10 relcache RelationBuildDesc scan sites converted (pg_class, pg_attribute,
  pg_rewrite, pg_opclass/pg_amproc, pg_attrdef, pg_index, pg_statistic_ext,
  pg_constraint x3); the 4 preload producer scans left as-is.
- yb_inheritscache (pg_inherits) served from cached scan.
- Cross-backend sharing; distinct-key sharing (backend A resolves '+', backend B
  resolves '-', zero new misses); distinct-relation sharing (different
  partitioned parents, including index relcache).
- DDL version bump invalidates correctly; relcache correctness for DEFAULT, CHECK,
  FK, RULE, INHERITANCE preserved; PgLibPqTest.TableColocation passes with the
  cached path on.
- Cached path disabled when any preloading gflag is set, and disabled on DDL, and in backends
permanently once temp tables are used.
- pg_response_cache_capacity default raised 1024 -> 3072.

Branch commits: 402b5cb3e0a / b410e9cec8e "preload from response cache",
f7fa83c5483 "fix shared catalog versions".
