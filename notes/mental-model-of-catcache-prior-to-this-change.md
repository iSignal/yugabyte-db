
1. All DDL except a few rare ones like create temp table increment a catalog version in the same
txn as their changes. When invalidation messages are enabled, the DDL commit also includes a list
of relcache/catcache entries that this DDL invalidates. The DDL uses an autonomous transaction which is
different from the regular transaction. Only when txn DDL / object locking is enabled, do DDL use the same
txn as the regular user txn.
2. Master propagates catalog versions in heartbeats and, when object locks are enabled, on global
lock releases from all DDL. Tservers receive heartbeats by default every 1 sec and set the latest version into shared memory.
3. PG backends use a catalog version from master to init themselves, then check the shared mem catalog version from tserver every txn/statement boundary. If they see a new version, they update to the new
version by applying invalidation messages (YbRefreshCache). As a result of steps 1-3, a DDL that changes
entries may take up to a heartbeat to propagate to backends.
4. PG backends support a feature called preloading. When additional preload gflags are enabled, they
fetch certain catalog tables on startup and use YbFillCaches to prepopulate all known entries and also
build relcache entries for all known relations. On later updates to catalog version, they apply invalidation
messages just like non preloading cases. Preload requests go through tserver response cache and are
cached there. This feature is intended to reduce cache miss latency to a remote region master.
5. When object locks are enabled, PG backends can apply inval msgs mid-statement during AcceptInvalidationMessages after each lock acquire potentially.
6. PG backends use a relcache init file to bootstrap relcache entries for catalog tables. This file has
the catalog version in it. In the non preloading case, if PG backends don't find this file or need to
rebuild it on a catalog version change, they trigger a separate relcache init conn to build it. This relcache
init conn performs preloading even if preloading is not enabled by gflags and uses the response cache to
do so.
7. General preloading which goes through response cache uses the serialized rpc request (minus a few fields) as the cache key for lookup. Multiple concurrent requests which hit the response cache result in only one
request to the master and others are served from this response when it arrives.
8. A catalog cache miss goes to master and reads catalog tables as of latest time, so it may see changes
outside its catalog version. Certain tests rely on this expectation but users do not. The minimum expectation
is that catalog cache misses at least reflect the changes as of that catalog version.
9. Clarification to point 3 : Though, in general, DDLs may not be visible to backends for a heartbeat delay, there are some special cases today
9.1 On the same tserver, a DDL sometimes increments catalog version locally in shared mem so it is visible
to backends on the same tserver faster. This is only done if no other DDL have incremented catalog version between its start and the commit.
9.2 When a DDL also has docdb changes, other backends may notice docdb schema version increments and retry
schema version mismatches by fetching latest catalog version, applying it and retrying the statement. Code lives in postgres.c.