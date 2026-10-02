

Problem

After a DDL, catcache, relcache invalidation msgs are sent out to all PG/YSQL backends.
If the backends require any of the invalidated cache entries later during execution, they go through
SearchCatCacheMiss or RelcacheBuildDesc which then query master. We want to avoid this invalidation
based storm of requests on master.

Proposal 1

Use the local tserver's pg_response_cache.cc in SearchCatCacheMiss. This will ensure that only one
request goes out to master. Other backends, even concurrent ones, wait on this request.

One side effect of this is that all catalog cache misses go through secondary cache now. This
requires care for DDL because when a DDL starts to perform writes, it cannot use the response cache any
more, it needs to use the master for misses so the Read RPC on master can reflect current transaction
writes.

2fd6b3ed2dcde9de278a21c40a298e892aa21687 partially implements this as a proof of concept, we can
use it as a reference.

Note that while catcache lookups are commonly key based, we also have catcache list lookups and
different catalog scans initiated by the backends which also need to be cached.


Test Plan

One test would be to perform a DDL like alter table foo or grant all on table foo to role1; while
an existing YSQL backend is running. Then verify that all misses that result are served from cache
and there are no Read RPCs to master, perhaps verified through pg client session logs or master rpc
metrics.

Open questions (need to be resolved before building)

1. Any other ideas worth considering?
1.1 PG side shared mem cache is one option - however, it is sensitive to crashes as any process
using this cache can crash and require all processes to be rebuilt if the data structure for cache
is in a bad state.

2. A new cache similar to pg_db_cache and pg_response_cache, perhaps with some on disk support? Do
we have a ready to use on disk hash map style structure? A rocksdb SST can work but would it be
too heavyweight? For relcache, we would need to cache arbitrary rpc responses by rpc key.

3. Proposal 1 open question : Response cache today has low limits. If all cache entries will go through this, we might need
to use separate area for preloading requests (which have to be there almost 100%) vs cat / rel cache
rpcs


================================================================================
Resolutions (2026-06-06)
================================================================================

Grounding: studied the PoC commit 2fd6b3ed2dc and the existing response-cache
infra. Key files:
 - src/yb/tserver/pg_response_cache.{h,cc}      (the cache itself)
 - src/yb/tserver/pg_client_session.cc          (Get() on read, Disable() on DDL commit)
 - src/yb/yql/pggate/pg_sys_table_prefetcher.cc (how preload populates it)
 - src/yb/tserver/pg_db_cache.h                 (the other tserver cache)

How the existing response cache already fits this problem
---------------------------------------------------------
The "storm" is really two problems, and PgResponseCache solves both:
 a) thundering herd: N backends miss the SAME entry at the SAME instant.
    Data::RegisterWaiter (pg_response_cache.cc:228) is single-flight - the first
    caller runs the master read, the rest park as waiters and are woken with the
    same response. So N concurrent misses -> 1 master RPC even with no prior
    cached value.
 b) staggered long tail: backends rebuild lazily, spread over time. The stored
    response serves every later arrival until invalidated.

Invalidation is lazy versioning, NOT eager eviction:
 - key_group == database OID (pg_sys_table_prefetcher.cc:310).
 - On DDL commit the session calls response_cache().Disable(silently_altered_db)
   (pg_client_session.cc:~4091), which bumps a per-key_group-bucket version
   (pg_response_cache.cc:413-425). Old entries then fail IsValid()'s
   `version == version_` check (pg_response_cache.cc:224) and are reclaimed by GC
   later. Buckets are hashed (default 512), so a Disable is coarse - it can knock
   out unrelated DBs that share a bucket. Acceptable (just causes re-reads), but
   note it.
 - The PoC ALSO embeds the catalog version in the PG-side key
   ("cache->id:YbGetCatalogCacheVersion()", catcache.c). So a post-DDL backend
   computes a DIFFERENT key and naturally misses the stale entry even before/without
   the Disable bucket-bump. This is belt-and-suspenders and is what makes the
   PoC's InvalidationOnDDL test pass. Recommendation: keep the version in the key;
   it is the primary correctness mechanism for catcache, and the bucket-version
   Disable becomes a secondary cleanup.

================================================================================
The right mental model: three options live at THREE DIFFERENT LAYERS
================================================================================
A backend that misses its in-process catcache/relcache today does:

   PG backend  --(local RPC, shared mem / unix socket)-->  tserver
   tserver     --(if not cached locally)-->  master Read RPC (cross-node)

The three options each insert a cache at a different point on that path:

   L0  PG-side shared memory   : hit avoids the RPC entirely. (option 2)
   L1  tserver in-memory cache : the existing PgResponseCache.  (Proposal 1)
   L2  tserver on-disk cache   : capacity tier behind L1.       (option, Q2)

Key consequence: L1 and L2 are the SAME layer - L2 is just an L2 behind L1, so
"on-disk" is not a competing design, it is an extension of Proposal 1 for
capacity. L0 (shared memory) is genuinely orthogonal and could be layered on top
of either later. So these are not three rival proposals to pick one of; they
stack. The real questions are (1) do we need L2 at all (capacity), and (2) is L0
worth its risk (latency + memory).

------------------------------------------------------------
Q1. Other ideas worth considering? (the full menu)
------------------------------------------------------------
(0) Single-flight only, no stored response. Coalesce concurrent identical misses
    into one master RPC and discard. Already free via Data::RegisterWaiter. Kills
    the herd (a) but not the tail (b). The minimal correctness floor; Proposal 1
    dominates it using the same machinery.

(1=L1) Proposal 1 (PoC): reuse the in-memory PgResponseCache for catcache/relcache
    misses. Infra exists; gets herd+tail; node-shared; DDL invalidation already
    wired (Disable + version-in-key). RAM-bound (Q3). >>> Recommended baseline.

(L2) On-disk capacity tier behind L1 - evaluated in Q2.

(L0) PG shared-memory cache - evaluated in Q1a below.

(3) Push / pre-warm: after DDL, master/tserver proactively pushes the new hot
    catalog rows so backends never miss. Best tail behavior, but needs a delivery
    channel + a "what is hot" policy + per-backend apply. Large design; the
    full-inval / shadow-apply work is arguably the better vehicle for this. Defer.

------------------------------------------------------------
Q1a. DEEP DIVE - L0: PG-side shared-memory catalog cache
------------------------------------------------------------
Why it is attractive (two distinct wins, the second is under-appreciated):
 1. Latency: a hit never crosses into the tserver at all - no RPC, no request
    serialization, no response-cache lookup. Pure local memory read.
 2. Memory: TODAY every backend holds its OWN full catcache/relcache. Verified
    scale: ~83 syscaches per backend (syscache.h SysCacheSize), each able to grow
    large; relcache holds every relation's descriptor. At hundreds/thousands of
    connections this per-backend duplication is the dominant PG memory cost. A
    SHARED cache collapses N copies into 1. That is a bigger structural win than
    the storm itself, and neither L1 nor L2 gives it (they dedup the RPC, not the
    per-backend PG memory).

What already exists (verified - better than expected):
 - src/yb/util/shmem/ has a real, tested toolkit:
     reserved_address_segment.h  : 64_GB region, pointers valid & identical across
                                    all PG processes (no offset juggling needed).
     shared_mem_allocator.h      : malloc/free + MakeUnique in shared memory.
     robust_hash_map.h           : closed-addressing hash map, insert/delete
                                    "robust to process crash" when guarded by a
                                    RobustMutex (24KB impl, has -test.cc).
     robust_mutex.h              : PTHREAD_MUTEX_ROBUST - auto-recovers if the
                                    holder dies.
 - Catalog version is ALREADY published through tserver<->PG shared memory
   (TServerSharedData, tserver_shared_mem.h: catalog_version_ + per-db array),
   so the "what version is current" signal a shared cache needs is already there.

The catch (verified - the risk is concentrated in exactly the part we'd lean on):
 - The ONLY production consumer of this toolkit, ObjectLockSharedState, uses
   FIXED-SIZE std::array + RobustMutexNoCleanup (object_lock_shared_state.cc:86,329).
   It does NOT use the dynamic RobustHashMap or the allocator. A catalog cache
   needs variable-size, dynamically-allocated, evictable entries - i.e. the
   RobustHashMap + allocator path, which is tested but NOT battle-proven in prod.
   So "infra exists" is true but the load-bearing piece is the least-exercised one.
 - Crash blast radius: a PG backend dying while mutating shared state is the
   classic hazard. Postmaster already escalates a crash-with-LWLock-held to "kill
   everything" (YbCrashInUnmanageableState). RobustMutex + the object-lock pattern
   shows the intended mitigation: treat shmem as a best-effort FASTPATH with an
   always-correct fallback (here: fall back to the L1 tserver read). Done that way,
   a corrupt/garbage shared entry is never a wrong answer - just a miss. This is
   the single most important design constraint for L0.
 - PG's own catcache/relcache cannot simply be relocated into shmem: they are
   built on palloc/MemoryContext, embed pointers, hold negative entries and a
   per-backend invalidation queue (SI messages). L0 is therefore a NEW shared
   structure that backends consult before building their local entry - not a
   move of the existing structure. Precedent for "share built catalog state"
   exists but is weak: the relcache init file (ysql_use_relcache_file) is written
   PER-BACKEND, not shared (relcache.c writes dboid_pg_internal.init.db with the
   catalog version inline) - so there is no existing shared catalog-content path
   to extend; L0 builds one.

Two sub-designs - and the choice is the whole ballgame:
 (i)  Shmem MIRROR of response bytes. Keys = same as the response cache
      (db_oid, version, request-bytes); value = serialized response. Backend reads
      from shmem instead of doing the RPC, THEN still parses it and builds its own
      catcache/relcache entry. Benefit = avoid the (local) RPC ONLY.
 (ii) Shmem cache of BUILT entries. Keys = catcache (cache_id, key-args) /
      relcache (relid); value = usable tuples/descriptors. Benefit = avoid RPC +
      avoid per-backend rebuild + dedup per-backend memory. The big win, the hard
      build.

Single-writer reframing (important, de-risks a lot):
 - If the TSERVER is the sole writer and backends are READ-ONLY, the scary part
   disappears: a reader crash cannot corrupt the structure, and one writer means
   no multi-writer races - we do NOT need the unproven dynamic RobustHashMap +
   robust-mutex-recovery path. A simple seqlock / versioned-slot with atomic
   publish suffices (the existing catalog_version atomics are precedent).
 - BUT single-writer forces design (i): the tserver can only write RESPONSE BYTES;
   it cannot build PG-internal catcache/relcache structures. So the SAFE design is
   also the LOW-reward one (RPC avoidance only). The memory win (ii) requires
   backends to consult shared state INSTEAD OF keeping their own copy - i.e.
   replace/front the per-process cache - which needs backends to write (losing
   single-writer safety) or deep PG surgery.
 - Crux (correctly identified): if we do NOT replace the per-process cache, shmem
   is "avoid the local RPC" only - and that RPC is to the LOCAL tserver
   (shmem/unix-socket, sub-ms); L1 already collapses the expensive MASTER RPC. So
   design (i)'s win over L1 is marginal and probably not worth shmem complexity.

Middle path: tserver (single writer) publishes raw catalog TUPLE BYTES into shmem;
backends build only their light index/negative-entry metadata while REFERENCING
the shared tuple storage zero-copy. Dedups the bulk (tuple data = dominant cost),
keeps single-writer safety, avoids most per-backend memory. Still real PG surgery,
but threads the needle between "safe" and "high-reward".

Operational constraint - shared mem is not free everywhere:
 - K8s pods limit shmem: /dev/shm is commonly a 64MB tmpfs by default and committed
   shared pages count against the pod memory cgroup. The 64_GB ReservedAddressSegment
   is only VIRTUAL address space, but actual cached pages are real RAM inside the
   pod limit. A shared cache must be BOUNDED and DEGRADE GRACEFULLY where shmem is
   constrained -> reinforces "best-effort fastpath with RPC fallback".
 - Only helps co-located backends (same node/pod). Fine - the storm is per-node.

Verdict on L0: pursue ONLY if we commit to design (ii) or the middle path (the
memory-dedup win). As a response-bytes mirror (i) it is dominated by L1. Highest
reward (kills RPC AND the per-backend memory blowup), highest risk (crash-safety +
new PG cache layer + the unproven dynamic shmem path if backends write). NOT the
first step; the memory-dedup angle deserves its own sizing study and may justify
L0 on memory grounds independent of the storm.

------------------------------------------------------------
Q2. DEEP DIVE - L2: on-disk tier (capacity, not persistence)
------------------------------------------------------------
Reframed per review: the point of on-disk is NOT crash persistence, it is
CAPACITY beyond L1's ~5%-of-RAM ceiling. That ceiling is real precisely in the
worst-case storm scenario:
 - key_group == db_oid, so EACH database gets its own cache entries. A node
   hosting many DBs (hundreds-thousands possible) multiplies entry count by #DBs.
 - Catcache/relcache misses are full-table or list scans of pg_proc, pg_attribute,
   pg_statistic, pg_amop/amproc - large for wide schemas, and paged into multiple
   responses. The long tail of distinct keys is unbounded by catalog object count.
 - L1 defaults: 1024 entries, 5% of root mem (pg_response_cache.cc:78-87). For a
   busy multi-tenant node this can be far too small to hold the union of all DBs'
   hot catalog responses -> L1 thrashes -> misses fall through to master -> the
   storm we are trying to kill. So L2 is genuinely motivated, not premature.

Feasibility / cost of a RocksDB-backed L2 (verified):
 - A bare rocksdb::DB::Open with hand-built Options is doable (~100 LOC); RocksDB
   need not be wrapped in DocDB/tablet machinery. WAL-OFF is appropriate: the
   cache is fully reconstructible, so losing it on crash is fine, and disabling
   WAL removes the per-miss write-log cost.
 - Hot/cold tiering is automatic: RocksDB's block cache (node-global, ~32% RAM on
   tserver by default) keeps hot blocks in memory; cold entries live in SSTs on
   disk. So L2 effectively IS "L1 semantics but spilling to disk" - which is
   exactly the capacity property we want. Budget note: its block cache contends
   with DocDB's; carve a dedicated sub-budget.
 - Latency math favors L2 strongly: an L2 hit is a LOCAL read - block-cache hit
   (RAM) or a single SST read on local NVMe (~100us) absorbed further by the OS
   page cache. Compare to what it REPLACES: a cross-node master Read RPC (ms-scale
   and the source of the storm). So even a cold L2 hit is far cheaper than the
   miss. L2 hit is slower than L1 hit but that is the only axis where it loses.

Invalidation WITHOUT DeleteRange (verified: YB's rocksdb fork has NO DeleteRange):
 - We do NOT need range deletes. The catalog version is already IN the key (PoC).
   A version bump means new reads compute a new key -> natural L2 miss -> repopulate.
   Stale entries simply become unreachable dead bytes, identical to how L1's
   version check makes old entries unreadable.
 - Reaping the dead bytes is the one extra engineering task vs in-memory: options
   are (i) a compaction filter that drops entries whose embedded version < a
   per-db "min live version", (ii) periodic manual compaction / DeleteFilesInRange
   (that API DOES exist), or (iii) just let size-triggered compaction reclaim them.
 - Churn risk: every DDL orphans a db's entries; a high DDL rate accumulates
   garbage fast. Mitigations: WAL-off, an aggressive TTL/version compaction filter,
   and keeping the instance modest. Worth a back-of-envelope on expected DDL rate
   before committing.

"Arbitrary rpc responses keyed by rpc key" + relcache: ALREADY supported, no new
abstraction. L1's key is (key_group=db_oid, value=opaque bytes); the PoC's value
= PG prefix + serialized PgsqlReadRequestPB. RefCntSlice / PgPerformResponseMsg
serialize cleanly to a RocksDB blob value. Relcache (RelationBuildDesc) is just
more systable_beginscan calls (pg_class, pg_attribute, pg_index, ...); the PoC
already threads response_cache_key through systable_beginscan_with_cache_key, so
relcache rides the same mechanism - "more scans", not a new cache. Only extra work
is choosing keys for the non-key scans (relid+version+indexoid).

Maintenance outline (recreated on startup, no persistence):
 - Startup: destroy any prior dir, open a FRESH empty RocksDB under
   fs_data_dirs/pg_catalog_cache. WAL OFF, auto-compaction on, dedicated/shared
   block cache, compaction filter installed.
 - Keys (same CONTENT as the response cache, flattened into one sorted byte string):
       data key:  D | db_oid(8B) | version(8B BE) | request_bytes(or hash)
                  -> serialized PgPerformResponseMsg + rows_data (== L1's Response)
       index key: V | db_oid(8B) | version(8B BE)
                  -> { first_seen, superseded_at? }
   D|db_oid|version prefix makes a db's / a version's data entries contiguous ->
   enables bulk DeleteFilesInRange (that API DOES exist in the fork).
 - Lookup order: L1 (in-mem) -> L2 (RocksDB Get) -> master; populate downward, and
   promote an L2 hit into L1.
 - Visibility: tserver is one process; memtable reads are immediately visible, so a
   miss-fill is readable by any other backend's next read RPC with no flush.
 - Invalidation: implicit via version-in-key (new reads compute a new key -> L2
   miss -> repopulate). Stale entries are unreachable dead bytes, never served.

 - REAPING (this is where L2 differs from L1's LRU). RocksDB has no data-level LRU
   (its LRU is only the block cache), so old versions must be reaped EXPLICITLY.
   Cannot delete a version's entries the instant a newer version appears: backends
   pick up catalog versions ASYNCHRONOUSLY (idle / long-txn / lagging backends may
   still read at the old version). Crucial property: reaping is a PERFORMANCE knob,
   NOT correctness - a V-keyed entry was populated from a consistent V snapshot, so
   a backend reading at V is consistent with it; reaping it early just forces a
   master re-read. Reap too early = a rare lagging backend loses the hit; reap too
   late = wasted disk.
   Therefore time-based retention with a generous N:
     * Retain by SUPERSESSION time, NOT first-seen. On a miss-fill for (db, V): if
       V > max_seen_version[db], stamp the previous max's index entry with
       superseded_at = now. The current max per db carries no superseded_at and is
       NEVER reaped. (Reaping by first-seen would delete a stable / no-DDL db's
       CURRENT, in-use version after N hours -> self-inflicted mini-storm. The
       supersession clock avoids that.)
     * Reaper: periodic background pass; for each index entry with
       superseded_at < now - N hours, DeleteFilesInRange(D|db|V|...) then drop the
       index entry. N >> typical version-propagation lag (hours is fine; cost of
       over-keeping is only disk).
     * Size-cap backstop: if the instance exceeds its disk budget, reap
       oldest-superseded versions first regardless of N. N governs steady state;
       the size cap is the hard ceiling.
   (A compaction filter keyed on a precise per-db "min live version" was considered
   and rejected: min-live is hard to compute with idle backends; supersession+N
   sidesteps it and degrades gracefully.)
 - Shutdown: close; recreated empty next start.

Verdict on L2: viable and well-motivated for large multi-tenant / wide-schema
nodes. It is an EXTENSION of Proposal 1 (L1 stays the hot tier, L2 the capacity
tier), not a separate design. Recommend: build L1 first, add a metric for L1
eviction/thrash + miss-fallthrough-to-master, and only turn on L2 if that metric
shows L1 is too small in practice. Before building L2, settle the reaper strategy
(compaction filter on embedded version is the cleanest) and the block-cache budget
split with DocDB.

No ready-made lightweight on-disk hashmap exists in the tree (no LMDB/mmap KV);
RocksDB is the realistic substrate, and WAL-off + version-in-key make it a much
better fit than the "heavyweight, churns on every DDL" first impression suggested.

------------------------------------------------------------
Q3. Low limits: preload entries (must be ~100% resident) vs miss entries.
------------------------------------------------------------
Same-layer contention within L1. Today preload and per-miss entries would share
ONE LRU (1024 entries, 5% root mem). A burst of distinct miss keys (the long tail)
can LRU-evict the handful of high-value preload entries every NEW connection needs
-> regressed connection latency, the opposite of the goal.

Quantified: a full preload is ~1 entry per prefetch RPC (+1 per paging
continuation) - tens, not thousands. Miss entries are unbounded by catalog object
count. preload = small+critical; misses = large+best-effort -> isolate them.

Options:
 (a) Separate L1 instance for misses, with its own capacity+mem limit, so it
     CANNOT evict preload entries. Wire Disable(db_oid) to both on DDL commit.
     Small plumbing, strong isolation. >>> Recommended.
 (b) Priority/pinning in one cache (preload last-to-evict). Less plumbing but
     changes LRUCache/MemTracker GC semantics (CollectGarbage walks the tail
     uniformly today, pg_response_cache.cc:457).
 (c) Partition the key_group space. Awkward - overloads db_oid's meaning.

This also composes cleanly with L2: the miss-cache (a) is exactly the instance you
would later back with the on-disk L2 tier, leaving the preload instance pure RAM.
Add a preload-eviction metric first to confirm contention and size the miss cache.

------------------------------------------------------------
Open implementation items to verify before building
------------------------------------------------------------
 - Key determinism across backends: the PoC clears stmt_id + metrics_capture
   before serializing the read request (pg_doc_op.cc, SetResponseCacheKey). Its
   comment also mentions read_time but the code does not clear it - confirm the
   per-op read_request carries no backend-varying read_time (catalog read_time
   lives in PgPerformOptionsPB, not the read_request, so this is likely fine, but
   verify, else cross-backend sharing silently degrades to misses).
 - DDL write path must keep bypassing the cache: BuildCatalogCacheOptions already
   returns nullopt when session->IsDdlMode() (pg_doc_op.cc) - keep this; a DDL's
   own reads must see its uncommitted writes, not a cached response.
 - Negative caching: SearchCatCacheMiss caches "no tuple" results; an empty scan
   response cached under version V is correct for V and invalidated on the next
   version bump - no special handling needed, but add a test.