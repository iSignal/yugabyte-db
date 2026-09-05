# Learning: the sync channel (xCluster raw + external intents) and the version gate

Source: code reading pass 2 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`. This is the DECIDED sync
design; it revises the earlier "CDCSDK logical, no IntentsDB" idea in
[[learning-cdc-wal-sys-catalog]] (that format is PG-output, unusable for
repopulating a DocDB tablet).

## Format decision: xCluster RAW WAL format, NOT CDCSDK

We repopulate a DocDB tablet, so we want raw DocDB-encoded KV, not decoded logical
data.
- **xCluster raw = directly applicable.** `xcluster_producer.cc PopulateWriteRecord`
  (`:86`) copies raw WAL bytes verbatim into the record
  (`:163-164` set_key / set_binary_value; comment `:129-130` "avoid
  deserializing on producer and re-serializing on consumer"). Intent batches flow
  through with txn metadata (`:150`). A separate `CDCRecordPB::APPLY` record carries
  `commit_hybrid_time` (`PopulateTransactionRecord`, `:175,193-194`).
- **CDCSDK proto / pgoutput = decoded logical** (`RowMessage` / `DatumMessagePB`,
  `cdc_service.proto:518`, `common.proto:682`) — column names + typed datums;
  would need re-encoding to apply to DocDB. Rejected.

## Why the IntentsDB IS needed (intents can't go straight to regular DB)

A regular DocDB record is keyed `<doc_key>@<DocHybridTime=commit_ht>`. An intent
record is provisional, keyed under the intent space with txn id + write time, value
carries txn metadata. When the producer streams an intent `WRITE_OP`, the txn has
NOT committed, so **commit_ht is unknown** and the abort/commit outcome is unknown
-> you cannot write the regular record yet. So intents MUST be staged until the
APPLY record (which carries commit_ht) arrives. That staging area is the IntentsDB.
=> The local copy is a normal tablet: regular RocksDB + intents RocksDB. "Maintain
both" (the plan's original worry) is TRUE, but it's just the normal tablet
structure, not extra machinery.

## The mechanism: external intents + external transactions (no participant, no status tablet)

This is xCluster's "apply writes from another universe" path, and it fits exactly.
- **Stage:** intent WRITE records -> `CombineExternalIntents`
  (`xcluster_write_implementations.cc:105-186`) -> written into the local IntentsDB
  under prefix `kExternalTransactionId+txn_id` at a local batch HT
  (`rocksdb_writer.cc:1248-1265`), via the normal Write RPC routed to
  `NonTransactionalBatchWriter` (`write_query.cc:505`, `tablet.cc:1957`).
- **Apply:** APPLY record -> `apply_external_transactions{txn_id,
  commit_hybrid_time}` (`xcluster_write_implementations.cc:198`) ->
  `PrepareApplyExternalIntents` (`rocksdb_writer.cc:1177-1215`) seeks the
  `kExternalTransactionId+txn_id` intents and moves them to RegularDB at the
  producer's commit_ht (`ExternalTxnApplyStateData::commit_ht`, `:989-1004`).
- **Crucially this does NOT use the transaction participant** (`put_batch` has no
  `transaction`); it's a pure DocDB direct-writer. So the local tablet needs NO
  transaction participant, NO status tablet, NO contact with the master's status
  tablet — the APPLY record is self-contained (carries commit_ht).
- **1->1 local mode already exists:** `ProcessRecordForLocalTablet`
  (`xcluster_output_client.cc:511-513`), selected by `local_tserver_optimized()`
  (`xcluster_consumer.cc:396-398`) — writes the whole record to one fixed local
  tablet, skipping partition lookup.
- **cotable_ids pass through verbatim** — keys are `dup_key`'d unchanged,
  colocation_id only used to pick a schema-version map (`xcluster_output_client.cc:133-141`).
  Since we bootstrapped identical table_ids from the superblock, no key rewriting.

## Intents are streamed EAGERLY (pre-commit) — same WAL, no separate intents WAL

The producer reads the WAL once and iterates in OpId order, emitting each intent
`WRITE_OP` as encountered, with NO txn-state buffering; the APPLY
(`UPDATE_TRANSACTION_OP`) record is emitted later when its WAL entry is reached
(`xcluster_producer.cc:383-391`). Confirmed against user knowledge: there is one
WAL for intents + regular; intents hit it as written. This is what enables the
(deferred) eager-apply optimization.

## Feasibility: ONE guard blocks xCluster-on-sys_catalog today

- Dispatch is by stream `source_type`, not tablet type: `GetChanges`
  (`cdc_service.cc:1609`) branches `XCLUSTER -> GetChangesForXCluster` else CDCSDK
  (`:1845-1910`). Master already runs a full `CDCServiceImpl` (`master.cc:320`) and
  resolves `kSysCatalogTabletId` (`master_tserver.cc:85`). The xCluster WAL reader
  `ReadReplicatedMessagesForXCluster` is on the generic consensus path
  (`raft_consensus.cc:3824`).
- **The blocker:** `cdc_service.cc:1705-1711` hard-rejects any non-CDCSDK stream on
  a sys_catalog tablet ("Polling sys catalog tablet is only supported for CDC").
- **Work to enable B:** (1) relax that guard for XCLUSTER; (2) create an
  XCLUSTER-source_type stream on the master sys_catalog tablet (xCluster streams
  are created master-side, fits the model; publication used a CDCSDK stream);
  (3) checkpoint/retention — tserver `SetCDCCheckpoint` doesn't support xCluster
  (`cdc_service.cc:1239`); honor sys_catalog CDC retention barriers for the new
  stream. (4) validate `transactional` consistent-WAL path (`ResolveIntents`,
  `xcluster_producer.cc:338`) on sys_catalog (it has a participant, so OK).

## The version gate (DECIDED): applied-version, NOT participant-safe-time

### Why the participant-safe-time proof does NOT transfer

The CDCSDK safe-time soundness (safe_hybrid_time bounded by
`MinRunningHybridTime`, proven in [[learning-catalog-version-propagation]]) relies
on the transaction PARTICIPANT tracking running txns. But external intents BYPASS
the participant -> our local tablet's participant never registers these txns ->
`MinRunningHybridTime` bounds nothing for them. So we must NOT gate on the local
participant safe-time, and must NOT trust the producer's xCluster safe-time signal
(which falls back to raw `LeaderSafeTime`, not bounded by min-running-apply).

### The gate we use

Expose catalog version V to PG backends only when the **local copy's own applied
catalog version >= V** — i.e., after applying streamed txns, read the local
tablet's `pg_yb_catalog_version[db]`. This is the direct analog of the existing
`YbWaitForSharedCatalogVersionToCatchup` (`pg_yb_utils.c:1223`, polls shm version)
but pointed at the local DocDB copy. It checks the concrete fact (is V's data
here?) and sidesteps all producer-safe-time soundness questions.

Requirements / follow-ups:
- **In-order apply** so "applied V" => all <= V applied. Baseline streams APPLY
  records in producer-WAL (commit) order -> naturally in order. (The deferred
  eager-apply optimization must preserve order: only early-apply V once V-1 applied,
  else gate on contiguity.)
- **Read-side:** redirected catalog reads on the local copy must read at the
  applied watermark so un-applied (provisional) external intents are not visible.
  xCluster solves this with consumer safe time; reuse that idea. (Open: exact
  mechanism — pass-2 item.)
- **Abort GC:** aborted DDLs leave external intents with no APPLY -> need cleanup.
  xCluster has handling; confirm for our path. (pass-2 item.)

### Latency (baseline, no optimization)

Catch-up latency = coordinator sends APPLY to master sys_catalog + master WAL
append + triggered poll streams it + local apply. Bounded; fine for rare DDL. The
deferred master-signaled eager-apply (apply already-staged intents at master-given
commit_ht; prior art `xcluster_external_apply_bootstrap-test.cc:315`) would shave
the stream round-trip.

## Gating seam & propagation

- ReleaseObjectLocks (object-locking mode): synchronous per-tserver RPC; insert the
  applied-version wait before `SetYsqlDBCatalogVersions` and ack
  (`ts_local_lock_manager.cc:486-532`). See
  [[learning-catalog-version-propagation]].
- Heartbeat (default mode): async, ~1s; needs an equivalent gate before writing the
  new version into shared memory. (pass-2 item.)
- Poll cadence: trigger an eager poll on the notification (heartbeat /
  ReleaseObjectLocks are free "poll now" signals — no CDC push exists,
  `cdc_service.cc` has no long-poll); idle lazily otherwise.

## Schema / DDL propagation (CHANGE_METADATA): the one substantive new consumer component

Catalog DDL doesn't just write rows — it bumps schema versions and creates/drops
catalog cotables (CREATE/DROP DATABASE mints a whole pg_class/... set per db_oid).
- **The bytes are on the wire.** The xCluster raw producer streams
  `CHANGE_METADATA_OP` (`xcluster_producer.cc:386-401`, emits
  `CDCRecordPB::CHANGE_METADATA` carrying the full `change_metadata_request`;
  `exit_early=true` so it's the last record in its batch). Master writes these for
  every catalog-table creation (`CompleteCreateYsqlSysTable`,
  `catalog_manager.cc:4005-4031`) and one batched `add_multiple_tables` per CREATE
  DATABASE (`CopyPgsqlSysTables`, `:4171-4264`, CM op at `:4256` BEFORE the row copy
  at `:4262`). So **WAL ordering already guarantees schema-before-rows.**
- **The stock xCluster CONSUMER is the wrong tool** —
  `XClusterOutputClient::ProcessChangeMetadataOp` (`xcluster_output_client.cc:520-624`)
  does producer->consumer schema-version REMAPPING (assumes target table exists,
  finds a "compatible" local schema) and explicitly IGNORES `add_multiple_tables`
  and `remove_table_id` (`:528-535`) — i.e. drops CREATE/DROP DATABASE. Its mapping
  is keyed by `ColocationId`, but sys_catalog cotables are keyed by `cotable_id`
  (Uuid), so the machinery doesn't even apply.
- **Our path (simpler than stock xCluster): replay CHANGE_METADATA VERBATIM.**
  Because we're a faithful 1:1 replica (same cotable_ids + same schema versions,
  bootstrapped from the master superblock), drive a local `ChangeMetadataOperation`
  from the streamed record, calling the same `RaftGroupMetadata` mutators the master
  used: `AddTable` / `AlterSchema` / `AddMultipleTables` / `RemoveTable`
  (`change_metadata_operation.cc:228-266`). Versions stay in lockstep -> packed-row
  values need NO rewriting (`UpdatePackedRow` is a no-op with an empty version map).
  This is the one substantive new consumer component, and it's LESS logic than the
  stock consumer's remap path.
- This is the recurring theme: a faithful identical-id replica lets us reuse the
  low-level DocDB primitives and skip all cross-cluster translation.

## Read-side consistency (redirected catalog reads): mostly turnkey

- **Provisional external intents are STRUCTURALLY invisible to reads.** They live as
  one opaque blob under the `kExternalTransactionId` meta keyspace
  (`intent_format.cc:169`), which `IntentAwareIterator` classifies as IntentsDB-meta
  and seeks past (`intent_aware_iterator.cc:923-949`); the read path has zero
  external-intent handling. Only the APPLY physically writes rows into RegularDB.
  So reading the local copy can't expose uncommitted DDL.
- **Read-point = the local tablet's apply safe time, pinned `SingleTime`.** Reuse
  `UpdateReadPointForXClusterConsistentReads` (`pg_client_session.cc:3506-3556`):
  `ReadHybridTime::SingleTime(apply_safe_time)` sets read==local_limit==global_limit
  -> **read restarts auto-disabled** (the analog of the master catalog read's
  `global_limit=read`). The watermark is the poller's `producer_safe_time_`
  (the producer's safe_hybrid_time). For a transactional stream this is
  **ResolveIntents-gated** (`xcluster_producer.cc:337`), so it does NOT run loose
  past committed-but-unapplied txns (corrects the earlier "xcluster safe time is
  loose" caution — that's the non-transactional path). Single tablet -> use this
  tablet's own apply safe time, no cluster-wide min needed.
- **Read at/below apply safe time, never ahead** — the existing consumer WAITS for
  safe time rather than reading provisional data (`pg_client_session.cc:3540-3555`).
- **Per-txn apply atomicity:** each external txn applies as one RocksDB batch at a
  single commit_ht (`rocksdb_writer.cc:1098-1147`), so V's version-row and V's data
  rows are co-visible -> sidesteps the response-cache DDL-atomicity gap
  ([[project_respcache_ddl_atomicity_gap]] analog).
- **Aborted-DDL external intents:** no abort record; GC'd by a TTL compaction filter
  `external_intent_cleanup_secs` (default 24h, `docdb_compaction_filter_intents.cc:223`).
  Zero read impact (meta region). Consider LOWERING the TTL so aborted catalog DDLs
  don't squat in the local IntentsDB.

## Gate mechanics: one in-memory applied-version map serves everything

Maintain `local_applied_catalog_version_[db_oid]` in the poller, bumped as it
applies each version-bump txn (mirror `ysql_db_catalog_version_map_` /
`get_ysql_db_catalog_version`, `tablet_server.h:324-331`). O(1) gate checks; no
per-check DocDB scan. (For cold-start/resync, the master read primitive
`ReadYsqlDBCatalogVersionImplWithReadTime` (`sys_catalog.cc:1006-1115`) is portable
to any local TabletPeer — `TableReadData` (`:2276`) is generic; lift it into a free
function.)

Three consumers of that map:
1. **ReleaseObjectLocks gate (synchronous, prompt).** Insert at
   `ts_local_lock_manager.cc:497`, after `WaitToApplyIfNecessary` and BEFORE
   `SetYsqlDBCatalogVersions*` (`:501-520`). The request carries target versions
   (`db_catalog_version_data`, `tserver.proto:535`). The handler is synchronous
   (`tablet_service.cc:3936-3966`) so blocking delays the ack. Use the `Wait()`
   backoff helper (`backoff_waiter.cc:56-85`) until `local_applied[db] >= target`;
   on timeout return non-OK -> master retries (safe).
2. **Heartbeat path (async): CLAMP, don't defer.** Do NOT skip the shm write inside
   `SetYsqlDBCatalogVersionsUnlocked` — that fights the monotonicity/staleness-FATAL
   machinery (`tablet_server.cc:1547-1617`). Instead feed
   `min(master_reported, local_applied)` per db in the `SetYsqlDBCatalogVersions`
   wrapper (`tablet_server.h:284-288`): if local is behind, you feed the old version
   -> harmless `==` branch, no advance, no FATAL; when the poller catches up, the
   next heartbeat advances cleanly. Clamp `kTemplate1Oid` too (it doubles as the
   legacy global `ysql_catalog_version_`, `tablet_server.cc:1680-1684`).
3. **Read-redirect freshness (DoPerform).** Per-op catalog version is on the read op
   itself: `PgsqlReadRequestPB.ysql_db_catalog_version` (field 36) + `ysql_db_oid`
   (field 37), set by `SetCatalogVersion` (`pg_client_session.cc:4135-4137`) — NOT in
   `PgPerformOptionsPB`. Predicate: serve locally iff
   `local_applied[op.ysql_db_oid] >= op.ysql_db_catalog_version`, else just skip the
   redirect and use the normal master route (no new fallback to build). Mirrors
   `WaitForYsqlBackendsCatalogVersion` (`tablet_service.cc:2484-2498`).

The gate guarantees a backend only sees version V after the local copy has V, so a
redirected read tagged V finds V locally; combined with the apply-safe-time read
point, the read returns a consistent <= V snapshot excluding provisional/aborted
intents.

## Gate VARIANT (recommended): hybrid-time watermark ("raft time"), not version

Instead of (or alongside) the applied-version gate, gate on a hybrid-time watermark
sent by the master. Equivalent in correctness AND latency (both become true exactly
when V's txn is applied locally), but keeps the poller catalog-agnostic.

### Key correctness facts (verified)
- **Propagate a HYBRID TIME, not a Raft OpId.** OpIds are local to a Raft group; the
  local copy is a *different* single-peer group with its own OpIds, so "wait for
  master OpId X" is meaningless. Hybrid time is a global clock and is the right
  cross-tablet "caught up to" currency.
- **Send commit_HT(V) / the version-read HT, NOT the master's instantaneous
  safe-time/OpId at release.** At release time V's txn is committed but its APPLY
  isn't in the sys_catalog WAL yet, so the master's own sys_catalog safe time is held
  BELOW commit_HT(V) (MinRunningHybridTime bound) — sending "now" would be too early.
  The master knows commit_HT(V)/read_HT independent of its apply progress.
- **The watermark to wait on is the poller's producer_safe_time_, NOT the local
  tablet's MVCC safe time.** External writes set only the record's encoded write
  timestamp via external_hybrid_time (`rocksdb_writer.cc:195-199`,
  `write_operation.cc:139-144`); the op's MVCC time comes from the CONSUMER clock, and
  `mvcc.cc` has no external_hybrid_time handling. So `TabletPeer::WaitForSafeTime(T)`
  with T in producer time is the WRONG domain. The poller advances
  `producer_safe_time_` only AFTER a batch is applied (`xcluster_poller.cc:667-670`),
  exposed via `GetSafeTime()` (`:267-295`). `GetSafeTime() >= T` means every producer
  commit with producer-HT <= T is applied locally. (This is the same value the
  xcluster safe-time service uses, min'd across tablets — `xcluster_safe_time_service.cc:331-390`;
  for our single tablet use the poller value directly, freshest.)

### Plumbing
- **Master watermark source:** the version-read HT is chosen in `ReadWithRestarts`
  (`sys_catalog.cc:945` `read_time.read = SafeTime(...)`) but currently discarded —
  thread it out through `ReadYsqlAllDBCatalogVersions` ->
  `GetYsqlAllDBCatalogVersions` -> `PopulateDbCatalogVersionCache`
  (`object_lock_info_manager.cc:1025-1053`). It's >= commit_HT(V) and in producer
  time (comparable to producer_safe_time_). commit_HT(V) itself is NOT on the release
  path (would need plumbing from the DDL-commit layer) — read-HT is cheaper.
- **New proto field required:** `apply_after_hybrid_time` (tserver.proto:522) is a
  pure local-CLOCK wait (`WaitToApplyIfNecessary` -> `WaitUntil`, `clock.cc:30-43`) —
  semantically unrelated and load-bearing for acquire-ordering; DON'T reuse it. Add
  e.g. `catalog_data_hybrid_time` to `ReleaseObjectLockRequestPB` (and the heartbeat
  path), and gate on `XClusterPoller::GetSafeTime() >= catalog_data_hybrid_time` — a
  new wait path distinct from `WaitToApplyIfNecessary`.

### HT-gate vs version-gate trade-off
- HT gate: poller stays generic (no catalog-row parsing); reuses the read watermark.
  Cost: new proto field + thread the read-HT out of ReadWithRestarts.
- Version gate: no new field (versions already in the message); but poller must parse
  pg_yb_catalog_version writes to track applied version; per-db.
- Redirect freshness: the read op carries an integer version, so the version gate's
  integer compare is more natural there. RECOMMENDED: HT gate for version EXPOSURE
  (generic poller), and for the redirect rely on the gate (read at the local apply
  safe time; the gate guarantees the backend only sees V after local reached HT(V)),
  with the per-op version check as an optional safety assertion. Maintaining both
  watermarks is cheap if we want belt-and-suspenders.

## Note: the inval-row write_time anchor is unnecessary

Neither gate variant needs the "project the inval-row write_time as commit_HT" idea
([[learning-catalog-version-propagation]] sec 6):
- Version gate compares integer versions.
- HT-watermark gate uses the version-READ HT (already chosen in ReadWithRestarts,
  `sys_catalog.cc:945`), which is >= commit_HT(V) and simpler to surface than
  projecting a per-row write_time.
Keep the write_time anchor documented only as a fallback if a tighter
per-version commit_HT (rather than the read-HT upper bound) is ever needed.
