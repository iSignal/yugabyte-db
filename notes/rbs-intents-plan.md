# Plan: open catalog transactions copied by remote bootstrap (todos T3(b))

Problem statement: [todos.md 2.3 T3(b)](todos.md#23-t3-remote-bootstrap-copies-untrusted-state);
reproduction and CI occurrence: [worklog2.md "Row D"](worklog2.md). Code pointers are against
repo6 (`/net/dev-server-sanketh-3/share/worktrees/repo6`, branch `docdb_local_catalog`, on top of
`cc0b13766e6`, push code uncommitted).

## 1. Two intent formats on the copy

A DocDB tablet stores a transaction's provisional writes in its intents DB in one of two formats.

**Regular intents** (what master's sys catalog tablet holds, and what remote bootstrap copies):

| Record | Key | Value |
|---|---|---|
| strong / weak intent, one per row and per ancestor | `SubDocKey`, intent type, write hybrid time | transaction id, subtransaction id, write id, row value |
| reverse index, one per intent | `kTransactionId`, transaction id, write hybrid time | the intent's key |
| transaction metadata | `kTransactionId`, transaction id | status tablet, isolation, start time |

A read that meets a regular intent asks the transaction participant for the transaction's status,
and the participant asks the coordinator. The participant also resolves the status of every loaded
transaction in the background and removes the intents of a transaction the coordinator reports as
aborted.

**External intents** (what an xCluster target holds, and what the copy's applier writes):

| Record | Key | Value |
|---|---|---|
| one blob per shipped write record | `kExternalTransactionId`, transaction id, local write hybrid time | involved tablet, subtransaction id (when not the minimum), list of (`SubDocKey`, row value) |

- Format: `docdb::CombineExternalIntents` (`src/yb/docdb/intent_format.cc:164-196`).
- The local write hybrid time is appended at write time
  (`NonTransactionalBatchWriter::AddEntryToWriteBatch`, `src/yb/docdb/rocksdb_writer.cc:1336`).
- No row-keyed intent and no reverse index exist. A read never finds these writes and never asks
  for a status. No participant tracks the transaction.
- The APPLY record becomes an `apply_external_transactions` entry. The apply seeks the blobs by
  the transaction-id prefix, writes each pair into the regular DB at the commit hybrid time with a
  rising write id, skips blobs of aborted subtransactions, and deletes the blobs
  (`PrepareApplyExternalIntents`, `rocksdb_writer.cc:1296-1330`;
  `PrepareApplyExternalIntentsBatch`, `:1178-1290`).

The copy's applier uses the xCluster machinery unchanged: `LocalCatalogApplier::ApplyRecords`
(`src/yb/tserver/local_catalog_applier.cc:663`) feeds every pushed record to the
`XClusterWriteInterface`; a record with `transaction_state` becomes one external-intent blob
(`AddRecord`, `src/yb/tserver/xcluster_write_implementations.cc:224-235`).

## 2. What goes wrong

Remote bootstrap copies master's intents DB and WAL tail as they are. A DDL transaction T that is
open at that moment arrives in the regular format, from the copied intents SST files or from the
copy's replay of the copied WAL tail. The applier starts after the last copied WAL entry P, so T's
writes before P are never pushed again.

```
master:   T writes R1 (pg_class, pg_type, ...)          WAL ops <= P
tserver:  remote bootstrap -> R1 on the copy as regular intents, participant tracks T
tserver:  applier starts after P
master:   T writes R2 (pg_yb_catalog_version, invalidation messages), commits at c
tserver:  push -> R2 as external blobs, then T's APPLY
tserver:  external apply finds blobs for R2 only -> regular DB gets R2 at c
tserver:  A[db] reaches T's version (from R2) -> the version gate passes
copy:     R1 stays as regular intents; participant resolves T -> coordinator forgot T -> ABORTED
copy:     participant removes R1 -> pg_class row of T missing until the next re-seed
```

## 3. Answer: convert the copied intents into the applier's format

Yes. After the copy has opened and replayed its WAL, and before the applier applies any pushed
page, every transaction the copy holds in the regular format and has not committed locally can be
rewritten into exactly the blobs the applier would have written, had master pushed T's writes
before P. The APPLY record then finds all of T's rows. Nothing on master changes.

```
copy after open + WAL replay           conversion                         copy afterwards
---------------------------           ----------                         ---------------
intent r1@t1 (T, sub 1, wid 0)  ─┐
intent r2@t2 (T, sub 1, wid 1)   ├──► WRITE records (T, sub, (r, v)) ──► blobs T@h1, T@h2, T@h3
intent r1@t3 (T, sub 2, wid 2)  ─┘    through ApplyRecords
reverse index T -> r1, r2, r1   ───►  IMMEDIATE_CLEANUP of T         ──► no regular intents of T
metadata T                                                               no metadata, participant empty

later push: R2 of T                                                  ──► blobs T@h4, ...
later push: APPLY T at c                                             ──► r1, r2, R2 rows at c
```

Why the result equals the pushed case:

- Same records. A regular strong-write intent holds the write pair's key without the hybrid time
  (`doc_path`) and the write pair's value after the transaction prefix (`DecodeIntentValue().body`)
  (`GetIntentsBatchForCDC`, `src/yb/docdb/docdb.cc:362-505`). The xCluster producer ships the
  same two values from the WAL write batch (`PopulateWriteRecord`,
  `src/yb/cdc/xcluster_producer.cc:163-165`). Packed-row schema versions are the same, because the
  copy carries master's schemas.
- Same order. The conversion walks the reverse index, which is in T's write order on master (the
  order the participant's own apply uses; not verified, test 3 checks it). Its blobs get the copy's current hybrid time, which is
  below every blob of a later push. The external apply reads blobs in key order and assigns write
  ids in that order, so a row that T wrote both before and after P ends with the value of the
  later write.
- Same subtransaction handling. Each converted record carries the intent's subtransaction id. A
  subtransaction that master rolled back is in the aborted set of T's APPLY record, and the apply
  skips its blobs.

## 4. Steps

### Step 1. The copy's participant never resolves status

The copy is a tablet of its own with its own transaction participant, because remote bootstrap
copies master's tablet metadata, which describes a transactional tablet. That participant is not
part of master's Raft group, and no coordinator lists the copy as an involved tablet. It knows T
only because it loaded T's metadata record from the copied intents DB.

Gap: the participant still asks T's coordinator for T's status.

- `LoadFinished` starts a status resolution for every loaded transaction
  (`src/yb/tablet/transaction_participant.cc:1929-1936`, queued by
  `transaction_loader.cc:284-287`); `CheckForAbortedTransactions` (`:2541`) repeats it on a poller.
- The request is an ordinary status RPC to the status tablet named in T's metadata. Any caller
  gets an answer.
- After master has applied T, the coordinator no longer has T and answers ABORTED for an unknown
  transaction (`transaction_coordinator.cc:1378-1381`).
- `TransactionsStatus` (`transaction_participant.cc:2344-2366`) then removes T's intents from the
  copy (`EnqueueRemoveUnlocked`, reason `kStatusReceived`).

```
copy participant                      T's coordinator                     master
----------------                      ---------------                     ------
load T from copied metadata
                                                                          T commits at c, applies
                                      T done on master -> T forgotten
GetStatus(T) ───────────────────────►
            ◄──────────────────────── ABORTED (unknown transaction)
remove T's intents (R1)
```

What the participant does with each answer (`RunningTransaction::UpdateStatus`,
`running_transaction.cc:322-368`; re-check scheduling `UpdateAbortCheckHT`, `:685-698`):

| Coordinator's answer | When | Copy's participant |
|---|---|---|
| PENDING | T still running | keeps T; asks again after `transaction_abort_check_interval_ms` (5 s) |
| COMMITTED | T committed, master has not yet acknowledged its apply to the coordinator | keeps T; never asks again |
| ABORTED | T rolled back, or T committed and the coordinator has already forgotten it | removes T's intents |

The first two answers are harmless. Only the third removes T's rows, and for a committed T it
needs a check that is sent after the coordinator has forgotten T. Checks are sent at load
(`LoadFinished`) and then every 5 s. The cases:

- T commits while remote bootstrap downloads files, and its APPLYING op lands in master's WAL after
  the copied position P. At load the coordinator has already forgotten T, so the check at load
  removes T's intents at once. The window is the length of the download.
- T commits after load. The next periodic check, up to 5 s later, removes T's intents. With the
  conversion this matters only if the conversion has not run yet, for example when the copy's
  election is slow on a loaded host.

With the conversion in place, step 1 closes both cases. Without step 1, the conversion can run
after the removal and find nothing to convert.

Change: a participant option, set when the tablet is opened as a private copy
(`OpenTabletPeer(..., private_copy = true)`, `src/yb/tserver/ts_tablet_manager.cc:2677`), that
starts neither the loader's resolvers nor the abort-check poller. The copy never needs them: after
step 2 it holds no regular intents, and reads at or below C never consult intents.

Not needed: protecting intents SST files from `Tablet::DoCleanupIntentFiles`
(`src/yb/tablet/tablet.cc:1525-1580`). This is generic tablet code, not specific to remote
bootstrap. It runs when a tablet opens (`tablet.cc:1450`) and whenever the intents DB's files
change (`tablet.cc:1415`). It deletes a whole intents SST file when every transaction with an
intent in that file has finished (the file's largest hybrid time is below the start time of the
oldest transaction the participant still tracks). The intents in such a file are no longer needed,
so this frees disk space without waiting for a compaction. It skips a tablet marked "under xCluster
replication" (`tablet.cc:1526`), because external intents are not tracked by the participant.

- Before the conversion: a file that holds T's intents is kept, because the participant tracks T.
- After the conversion: the conversion writes external intents, and the first external-intent
  write sets the "under xCluster replication" mark (`tablet.cc:2185-2186`), so the cleanup no
  longer runs on the copy.

### Step 2. Convert, on the applier thread, before the first page is applied

Placement: `LocalCatalogApplier::Poll` (`local_catalog_applier.cc:211-224`) after the copy leads
its own group (the conversion writes through the copy's Raft write path), and a gate in
`ApplyPendingPushes` / `DoApplyPages` that answers every page "not ready" until the conversion has
finished. Pushes that `EnsureLocalTablet` requested (`:422`) are re-sent by master.

1. Wait until the participant has loaded every transaction (`loader_.WaitAllLoaded()`; `loader_`
   is private to `TransactionParticipant::Impl`, so this needs an accessor).
2. Candidate set: every transaction with a metadata record in the copy's intents DB whose
   `LocalCommitTime(id)` is invalid, which means the copied WAL holds no APPLYING op for it. A
   transaction with a local commit time is applied by the copy's participant into the regular DB,
   during WAL replay or, for a large transaction, by the apply that `LoadFinished` resumes; it is
   not converted. The gate stays closed until those resumed applies have finished.
3. For each candidate T: read its strong-write intents, build one `CDCRecordPB` per intent
   (`operation = WRITE`, `transaction_state = {T, tablets: [master sys catalog tablet id]}`,
   `subtransaction_id`, `changes = [(doc_path, body)]`), and hand them to `ApplyRecords`.
   Reused code:
   - Writing: `LocalCatalogApplier::ApplyRecords` (`local_catalog_applier.cc:663`) unchanged, the
     same call a pushed page makes. It runs the records through `XClusterWriteInterface` ->
     `AddRecord` -> `CombineExternalIntents`, so the blob format, the involved-tablet field and the
     subtransaction header come from the existing code.
   - Reading: `docdb::GetIntentsBatchForCDC` (`src/yb/docdb/docdb.cc:362-505`, reached through
     `Tablet::GetIntentsForCDC`, `tablet.cc:2925`), the reader CDCSDK uses to stream a
     transaction's intents. It walks the reverse index, decodes each strong write into
     (`doc_path`, value body), skips row locks, and continues across calls through
     `ApplyTransactionState` when a transaction exceeds `cdc_max_stream_intent_records`. Two
     changes: add `subtransaction_id` to `IntentKeyValueForCDC` (the decoded value has it,
     `docdb.cc:453`); pass an empty aborted set, because the final aborted set is known only from
     the APPLY record. Its skip of colocated table tombstones (`docdb.cc:470-476`) also drops a
     write that a push would carry; a catalog table tombstone inside a DDL transaction is not
     known to occur, so this stays unless a test shows otherwise.
4. Remove T in the regular format: `participant->Handle` with an `UpdateTxnOperation` of status
   `IMMEDIATE_CLEANUP` for T, the call that `TabletServiceImpl::UpdateTransaction` makes for a
   cleanup request (`src/yb/tserver/tablet_service.cc:1414-1446`). `ProcessCleanup` removes the
   metadata, the reverse index and the intents (`transaction_participant.cc:1150-1195`). This
   removal is a direct intents-DB write, not a Raft operation. Wait until
   `GetNumRunningTransactions() == 0`, because a removal can be deferred to the cleanup queue.
5. Flush the intents DB and the regular DB synchronously.
6. Write the checkpoint file with a new field `intents_converted = true`
   (`LocalCatalogCheckpointPB`, `src/yb/tserver/tserver.proto:518`). Open the page gate.

### Step 3. Restart and crash behavior

`LoadCheckpoint` (`local_catalog_applier.cc:291-314`) writes the checkpoint file at once for a
fresh copy, before the election, because the election appends a local no-op that is not a master
position. That file is written with `intents_converted = false`; step 2.6 rewrites it.

| Crash point | State on restart | Outcome |
|---|---|---|
| before 2.3 | regular intents, `intents_converted = false` | conversion runs |
| after 2.3, before 2.5 | blobs in the copy's WAL; regular intents may come back from replay of the copied WAL tail | conversion runs again and writes a second set of blobs for T; the APPLY writes each pair twice at the same commit time; the later write id holds the same value |
| after 2.5, before 2.6 | blobs durable, regular intents gone | conversion finds no candidate |
| after 2.6 | converted | nothing |

No page is applied before 2.6, so no pushed blob or APPLY of T can precede a repeated conversion.

## 5. Cases

| Case | Result |
|---|---|
| T open at P, commits later | APPLY applies converted and pushed blobs |
| T open at P, rolls back later | blobs never applied; removed after `external_intent_cleanup_secs` (T29) |
| T aborted on master before P, intents not yet removed on master | converted; same as the row above |
| T committed before P, APPLYING op <= P | not a candidate; the copy's participant applied it during replay |
| T committed before P, APPLYING op > P | candidate; the pushed APPLY applies the converted blobs |
| T rolled back a savepoint before P | converted blobs carry the subtransaction id; the APPLY's aborted set skips them |

Not covered: T3(a), entries past master's committed index in the copied WAL tail. Its fix
(truncate the copy's log to the copied committed index before open) must run before the
conversion, so that only committed entries are converted.

## 6. Rejected alternatives

| Alternative | Reason |
|---|---|
| Abort active transactions on master at bootstrap (xCluster's `abort_active_txns_during_xrepl_bootstrap`) | a tserver can bootstrap at any time; this aborts in-flight DDLs cluster-wide |
| Push from the oldest open transaction's first write | the copy cannot learn that position from its intents; master may have garbage-collected that WAL; an idle open DDL holds WAL without bound |
| Apply candidates through the copy's participant (pushed writes of T as regular intents, APPLY as a local `APPLYING` op) | T's writes split across two formats; the participant must keep T across the coordinator forgetting it; mixes participant semantics into the applier for one case |
| Rewrite the downloaded intents DB before open | replay of the copied WAL tail recreates regular intents after the rewrite (the CI occurrence) |

## 7. Tests

Oracle: catalog rows read on the re-seeded tserver with the copy serving equal the same query
with `TEST_local_catalog_disable_serving`.

1. Enable `DISABLED_DdlOpenAcrossReseedIsVisibleAfterCommit`
   (`src/yb/yql/pgwrapper/pg_local_catalog-test.cc:974`).
2. Variant with master's intents DB flushed before the re-seed (rows from the copied SST files)
   and a variant without (rows from WAL tail replay).
3. One catalog row written before and after the re-seed by the same DDL transaction (for example
   `CREATE TABLE` then `ALTER TABLE ... ADD COLUMN` in one block): final row equals master's.
4. `SAVEPOINT a; CREATE TABLE x; ROLLBACK TO a;` before the re-seed, commit after: `x` absent.
5. The DDL rolls back after the re-seed: no rows on the copy.
6. Step 1 race: delay the conversion with a TEST flag until master has applied T and the
   coordinator has forgotten it; rows present.
7. Crash points of section 4 with TEST flags; restart the tserver; rows present, no
   duplicate-visible rows.
