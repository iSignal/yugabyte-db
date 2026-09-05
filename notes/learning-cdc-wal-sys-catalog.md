# Learning: CDC/WAL streaming of the master sys_catalog tablet (the update channel)

Source: code reading pass 1 (2026-06-19). All paths under
`/net/dev-server-sanketh-3/share/code/yugabyte-db`.

## TL;DR

YugabyteDB **already streams the master sys_catalog tablet via the standard CDC
`GetChanges` path today** (for logical-replication publication refresh). The
master process runs a `CDCServiceImpl`, and `GetServingTablet(kSysCatalogTabletId)`
resolves the sys_catalog tablet, so `GetChanges` works against it like any user
tablet.

The decisive simplifier: **CDCSDK `GetChanges` returns already-committed,
commit-time-ordered, decoded per-PG-catalog-table row records — NOT raw intents.**
We would apply them as plain upserts/deletes to the local tablet; **no local
IntentsDB and no transaction-apply machinery needed** — provided we use the
CDCSDK logical format and not the xCluster raw-WAL format.

## 1. Master runs a CDC service over sys_catalog

- `Master::RegisterServices` (`src/yb/master/master.cc:319-323`) registers a
  `CDCServiceImpl` when
  `FLAGS_ysql_yb_enable_implicit_dynamic_tables_logical_replication`.
- `MasterTabletServer::GetServingTablet` (`src/yb/master/master_tserver.cc:85-88,
  135-140`) resolves only `kSysCatalogTabletId`. So `CDCServiceImpl::GetChanges`
  (`src/yb/cdc/cdc_service.cc:1609`) works against the sys_catalog tablet.
- Producer has explicit sys_catalog handling: `cdcsdk_producer.cc:979`
  (`is_sys_catalog_tablet = tablet->metadata()->IsSysCatalog()`),
  `GetTableInfoForSysCatalogTable` (`:948`) maps each decoded row's `cotable_id`
  to the right PG catalog table. `IsStreamableCatalogTable` /
  `GetStreamableCatalogTables` (`cdc_service.cc:4870`) gate which catalog tables
  are streamable.
- `CDCMasterBgTask` (`cdc_service.cc`, ~100ms loop) manages sys_catalog CDC
  retention barriers (`SetAllCDCRetentionBarriers`) so the WAL/intents/history
  aren't GC'd ahead of the slowest stream (`:3252-3299`, `:3661`).

## 2. Closest prior art: cdcsdk_virtual_wal already polls sys_catalog

`src/yb/cdc/cdcsdk_virtual_wal.cc` (the server-side engine behind PG logical
replication's walsender):
- `InitVirtualWALInternal` (`:206`) adds PG catalog tables to the poll list
  (`:242-252`) and `GetTabletListAndCheckpoint` per table (`:259-270`); for
  catalog tables this resolves to the sys_catalog tablet, seeding
  `tablet_next_req_map_` with `from_op_id, write_id, safe_hybrid_time` (`:390-401`).
- `GetChangesInternal` (`:894`) loops the poll list, calls `GetChanges` per
  tablet, then `AddRecordsToTabletQueue` + `UpdateTabletCheckpointForNextRequest`.
- `GetNextRecordToBeShipped` (`:1134`) pops from a priority queue
  (`FindConsistentRecord`, `:1171`) ordered by `CDCSDKUniqueRecordID`, holding
  commit records until all same-commit-time DMLs have shipped.
- Master tablet records are consumed internally, never shipped (`:634`):
  ```cpp
  if (tablet_id == master::kSysCatalogTabletId) {
    auto pub_refresh_required = DeterminePubRefreshFromMasterRecord(...);
    ... continue;
  }
  ```
- `DeterminePubRefreshFromMasterRecord` (`:1792`) switches on
  `record->row_message().table_id()` against `pg_class_table_id_`,
  `pg_publication_rel_table_id_`, `pg_replication_origin_table_id_`, reads op type
  and PG column values. **This proves sys_catalog changes arrive as fully-decoded,
  per-PG-catalog-table logical row records**, not raw KV.

## 3. Record format

`CDCSDKProtoRecordPB` with a `RowMessage` (`cdc_service.proto`).
`RowMessage.Op = {UNKNOWN, INSERT, UPDATE, DELETE, BEGIN, COMMIT, DDL, TRUNCATE,
READ, SAFEPOINT}`. Each carries `transaction_id`, `commit_time`, `table_id`,
`primary_key`, before/after images in `repeated DatumMessagePB new_tuple/old_tuple`,
plus `pg_lsn`, `record_time`, `schema`. Row-level, txn-commit ordered.

## 4. Intents are handled for us (the key worry, resolved)

In `GetChangesForCDCSDK` (`cdcsdk_producer.cc:2656`) the WAL reader **filters out
provisional intents** (`:2161`, `:2275`):
```cpp
if (IsIntent(msg) || (IsUpdateTransactionOp(msg) &&
                      msg->transaction_state().status() != TransactionStatus::APPLYING)) {
  continue;
}
```
It keeps only single-shard writes and the commit/APPLY record. On the APPLY
record (`:2960`) it captures `commit_timestamp` and calls
`ProcessIntents` (`:1826`), which reads the **committed** intents from IntentsDB
(`Tablet::GetIntentsForCDC`, `tablet.cc:2705`), decodes them to row records via
`PopulateCDCSDKIntentRecord` (`:956`) stamped with the txn's `commit_time`, wrapped
in BEGIN/COMMIT. Cross-txn ordering by commit time (`SortConsistentWALRecords`,
`:2085`). Large txns spill across batches via `pending_intents` +
`ApplyTransactionState` (`:3008`).

**=> CDCSDK gives committed data only, commit-time ordered. Apply as plain
upserts/deletes. No IntentsDB, no transaction-apply on the consumer.**

### Contrast: xCluster raw format is the HARD path — avoid it

`xcluster_producer.cc:86` `PopulateWriteRecord` copies raw encoded WAL KV bytes
*including* intent writes (`:163-164`) and emits a separate `APPLY` record
(`PopulateTransactionRecord`, `:175`). The consumer must write intents into its
own IntentsDB and re-run transaction apply — exactly the "local tablet has both
intents and regulardb" cost the plan feared. **Use CDCSDK, not xCluster.**

## 5. Poller architecture template: XClusterPoller

`src/yb/tserver/xcluster_poller.cc/.h` — event-driven, each step schedules the
next: `SchedulePoll -> DoPoll -> HandleGetChangesResponse -> ApplyChanges ->
HandleApplyChangesResponse -> SchedulePoll`.
- Interval flags (`:43-54`): `async_replication_idle_delay_ms` default **100ms**
  (the plan's "poll every 100ms" precedent), `async_replication_max_idle_wait`
  (3 empty polls before idling), `replication_failure_delay_exponent` (backoff).
- `SchedulePoll` (`:302`) posts to the reactor (`xcluster_async_executor.cc:135`).
- State between polls (`xcluster_poller.h:172-190`): `op_id_` (checkpoint, init
  `MinimumOpId`), `producer_safe_time_` (atomic), `idle_polls_`, `poll_failures_`,
  `apply_failures_`.
- `DoPoll` (`:394`) sends `from_checkpoint = op_id_`.
- **Checkpoint advances only after local apply succeeds** (`HandleApplyChangesResponse`,
  `:656`): `op_id_ = response.last_applied_op_id`, `UpdateSafeTime(...)`.

(The CDCSDK walsender consumer is driven by the PG walsender loop instead, with
per-batch sleeps `yb_walsender_poll_sleep_duration_nonempty_ms`=1ms /
`_empty_ms`=10ms, `pg_wrapper.cc:344-351`. For a background tserver poller the
XClusterPoller shape is the better model.)

## 6. Safe-time semantics: how we know we're caught up to hybrid time T

`GetChangesResponsePB.safe_hybrid_time` ("safe time to be used on the target")
means: the producer has streamed **every change with commit_time <= T** for that
tablet. Once applied, the consumer has all changes up to T. Even an empty batch
advances T to the tablet leader's safe time.
- Producer computes it: if more messages, T = HT of last record; if WAL drained,
  T jumps to `tablet_peer->LeaderSafeTime()` (xCluster `GetSafeTimeForTarget`,
  `xcluster_producer.cc:271-301`; CDCSDK `GetCDCSDKSafeTimeForTarget`,
  `cdcsdk_producer.cc:695`, `:2442`, `:3271`).
- There is also an explicit **SAFEPOINT record** appended at end-of-batch carrying
  `commit_time = safe_time` (`PopulateCDCSDKSafepointOpRecord`, `:2030`; consumed
  by the VirtualWAL to advance `virtual_wal_safe_time_`,
  `cdcsdk_virtual_wal.cc:652`).

**=> Correlation with catalog version:** sys_catalog DML rows arrive decoded and
stamped with `commit_time`; `safe_hybrid_time >= H` proves all commits <= H are
delivered. So "local copy reflects catalog version V" = "applied the row writing
V" AND "safe_hybrid_time >= that row's commit_time". Single tablet, so no
cross-tablet MIN needed. See [[learning-catalog-version-propagation]].

## What's easy vs hard

**Easy / already solved:** GetChanges on sys_catalog works today (flag-gated);
CDCSDK returns committed, decoded, per-catalog-table rows directly appliable;
`safe_hybrid_time` gives a clean catch-up signal; XClusterPoller is a reusable
loop; sys_catalog retention barriers already exist.

**Hard / watch-outs:**
- **Format choice is load-bearing**: CDCSDK (logical) avoids intents; xCluster
  (raw) forces a local IntentsDB. Pick CDCSDK.
- **Streamable-table set is currently tiny**: today only pg_class,
  pg_publication_rel, pg_replication_origin are wired (`GetStreamableCatalogTables`).
  A full catalog copy needs this generalized to *all* catalog + shared tables
  across *all* databases.
- **DDL/schema-version records** get special VirtualWAL handling (no LSN assigned;
  checkpoint held until DDL acked, `cdcsdk_virtual_wal.cc:1061-1068`). A
  catalog-copy poller must decide how to apply schema changes (new cotables / new
  columns) to its local tablet metadata, not just row data.
- **Per-db / new-database semantics**: CREATE DATABASE creates new cotables; the
  poller/local tablet must learn the new table schemas (ChangeMetadata-equivalent)
  before applying their rows.
- Whole path is feature-flag gated and scoped today to one namespace's
  publication tables; a general "copy the entire catalog" use is beyond what's
  exercised and needs streamable-set, retention, and per-db generalization.

### Key files
`src/yb/cdc/cdcsdk_virtual_wal.cc/.h`, `cdcsdk_producer.cc`,
`cdc_service.cc/.proto`, `xcluster_producer.cc` (contrast),
`src/yb/master/master.cc`, `master_tserver.cc`, `sys_catalog_constants.h`,
`src/yb/tserver/xcluster_poller.cc/.h`, `xcluster_async_executor.cc`,
`src/yb/master/xcluster/xcluster_safe_time_service.cc`,
`src/yb/tserver/xcluster_consumer.cc`.
