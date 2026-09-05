// Copyright (c) YugabyteDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/tserver/local_catalog_poller.h"

#include "yb/cdc/cdc_service.pb.h"

#include "yb/consensus/log.h"

#include "yb/common/entity_ids.h"
#include "yb/common/pg_catversions.h"
#include "yb/common/read_hybrid_time.h"

#include "yb/docdb/doc_rowwise_iterator.h"

#include "yb/dockv/doc_key.h"
#include "yb/dockv/reader_projection.h"

#include "yb/fs/fs_manager.h"

#include "yb/gutil/strings/util.h"

#include "yb/master/master_admin.proxy.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/qlexpr/ql_expr.h"

#include "yb/rpc/rpc_controller.h"

#include "yb/server/server_base.proxy.h"

#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/write_query.h"

#include "yb/tserver/local_catalog_replica.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver.messages.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/xcluster_write_interface.h"

#include "yb/util/async_util.h"
#include "yb/util/env_util.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/path_util.h"
#include "yb/util/pb_util.h"
#include "yb/util/status_format.h"
#include "yb/util/unique_lock.h"
#include "yb/util/status_log.h"

DEFINE_RUNTIME_uint32(local_catalog_poll_interval_ms, 100,
    "Interval between polls of the master system catalog tablet's WAL by the tserver-local copy "
    "of that tablet, when the previous poll had nothing left to fetch.");

DEFINE_RUNTIME_uint32(local_catalog_poll_rpc_timeout_ms, 30000,
    "Timeout for one change request to the master system catalog tablet.");

DEFINE_RUNTIME_uint32(local_catalog_checkpoint_flush_interval_ms, 1000,
    "Minimum interval between writes of the tserver-local catalog copy's WAL position to disk. "
    "A restart resumes from the last written position and re-applies the records after it, which "
    "writes the same keys at the same hybrid times and is therefore harmless.");

DEFINE_RUNTIME_uint32(local_catalog_apply_write_timeout_ms, 60000,
    "Timeout for one write of polled records into the tserver-local catalog copy.");

DECLARE_uint32(local_catalog_bootstrap_retry_delay_ms);

DEFINE_test_flag(bool, local_catalog_pause_poller, false,
    "Stop the tserver-local catalog copy from fetching from master. C stops advancing on its own, "
    "so the version gate blocks and reads that carry a newer version wait. Records that arrive on "
    "a lock release are still applied, which is how the push path is tested without pulls.");

DEFINE_test_flag(uint32, local_catalog_pushed_batch_apply_delay_ms, 0,
    "Sleep this long inside the apply of a batch that arrived on a lock release, so that the "
    "release handler waiting on it gives up first and the handoff outlives its caller.");

DEFINE_test_flag(bool, local_catalog_fail_poll, false,
    "Make every change request to the master system catalog tablet fail.");

DEFINE_test_flag(bool, local_catalog_poll_checkpoint_too_old, false,
    "Answer every change request to the master system catalog tablet with CHECKPOINT_TOO_OLD, the "
    "error master returns once it has garbage collected WAL the copy never consumed. Exercises the "
    "re-seed path without having to drive master's WAL retention.");

namespace yb::tserver {

namespace {

const char* const kCheckpointFileName = "local_catalog_checkpoint";

}  // namespace

LocalCatalogPoller::LocalCatalogPoller(LocalCatalogReplica& replica, TabletServer& server)
    : replica_(replica),
      server_(server),
      log_prefix_(Format("LocalCatalogPoller [$0]: ", server.permanent_uuid())),
      finder_(server.messenger(), server.proxy_cache(), server.options().GetMasterAddresses()),
      scheduler_(finder_, *this) {}

LocalCatalogPoller::~LocalCatalogPoller() = default;

Status LocalCatalogPoller::Start() { return scheduler_.Start(); }

void LocalCatalogPoller::Shutdown() {
  scheduler_.Shutdown();
  if (tablet_peer_) {
    WARN_NOT_OK(PersistCheckpoint(/* force= */ true), "Failed to persist the local catalog copy's "
                                                      "WAL position on shutdown");
  }
}

void LocalCatalogPoller::RequestImmediatePoll() { scheduler_.TriggerASAP(); }

void LocalCatalogPoller::Init() {}

void LocalCatalogPoller::ResetProxy() { proxy_.reset(); }

std::string LocalCatalogPoller::name() { return "local_catalog_poller"; }

const std::string& LocalCatalogPoller::LogPrefix() const { return log_prefix_; }

MonoDelta LocalCatalogPoller::IntervalToNextPoll(int32_t consecutive_failures) {
  if (!tablet_peer_ && !setup_attempted_) {
    // The delay below is a retry interval, so it must not precede the first attempt: a tserver
    // that already has a copy would otherwise answer nothing for that interval after every
    // restart.
    setup_attempted_ = true;
    return MonoDelta::FromMilliseconds(0);
  }
  if (!tablet_peer_) {
    // Still creating the copy, which can take a while: master's initdb has to finish first, and
    // the remote bootstrap sessions of the cluster's tservers serialize on the source tablet's
    // checkpoint lock. Retrying at the poll interval would only add log noise.
    return MonoDelta::FromMilliseconds(FLAGS_local_catalog_bootstrap_retry_delay_ms);
  }
  if (consecutive_failures > 0) {
    // Exponential backoff, capped at one second, so a master that is down does not spin.
    const auto shift = std::min(consecutive_failures, 10);
    return MonoDelta::FromMilliseconds(
        std::min<int64_t>(1000, static_cast<int64_t>(1) << shift));
  }
  if (have_more_messages_) {
    return MonoDelta::FromMilliseconds(0);
  }
  return MonoDelta::FromMilliseconds(FLAGS_local_catalog_poll_interval_ms);
}

Status LocalCatalogPoller::Poll() {
  // First, so that a release waiting on a pushed batch is answered as early as possible, and so
  // that pushed and pulled batches never interleave. A pushed batch is applied even while pulling
  // is paused: the pause exists to stop this copy fetching, and a release that carried records
  // has a caller of its own waiting on it.
  ApplyPendingPush();
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_pause_poller)) {
    return Status::OK();
  }
  RETURN_NOT_OK(EnsureLocalTablet());
  return FetchAndApplyOnce();
}

Result<std::string> LocalCatalogPoller::CheckpointPath() const {
  // Deliberately not in the tablet metadata directory: FsManager::ListTabletIds treats every file
  // there as a tablet id, and on restart the tablet manager would try to parse this file as a
  // tablet superblock and fail to start.
  return JoinPathSegments(server_.fs_manager()->GetDefaultRootDir(), kCheckpointFileName);
}

Status LocalCatalogPoller::LoadCheckpoint() {
  auto path = VERIFY_RESULT(CheckpointPath());
  auto* env = server_.fs_manager()->env();
  if (!env->FileExists(path)) {
    // No position recorded yet: the local tablet was just fetched, so the position to resume from
    // is the last entry its replayed log holds.
    auto consensus = VERIFY_RESULT(tablet_peer_->GetConsensus());
    checkpoint_ = tablet_peer_->log()->GetLatestEntryOpId();
    persisted_checkpoint_ = OpId();
    replica_.SetAppliedOpId(checkpoint_);
    LOG_WITH_PREFIX(INFO) << "No recorded WAL position; resuming from the last entry of the copied "
                          << "log: " << checkpoint_;
    return PersistCheckpoint(/* force= */ true);
  }
  LocalCatalogCheckpointPB pb;
  RETURN_NOT_OK_PREPEND(
      pb_util::ReadPBContainerFromPath(env, path, &pb),
      Format("Unable to read the local catalog copy's WAL position from $0", path));
  checkpoint_ = OpId::FromPB(pb.checkpoint());
  persisted_checkpoint_ = checkpoint_;
  replica_.SetAppliedOpId(checkpoint_);
  LOG_WITH_PREFIX(INFO) << "Resuming the poll from WAL position " << checkpoint_;
  return Status::OK();
}

Status LocalCatalogPoller::PersistCheckpoint(bool force) {
  if (checkpoint_ == persisted_checkpoint_) {
    return Status::OK();
  }
  const auto now = MonoTime::Now();
  if (!force && last_checkpoint_flush_.Initialized() &&
      now - last_checkpoint_flush_ <
          MonoDelta::FromMilliseconds(FLAGS_local_catalog_checkpoint_flush_interval_ms)) {
    return Status::OK();
  }
  if (tablet_peer_) {
    RETURN_NOT_OK(tablet_peer_->log()->WaitUntilAllFlushed());
  }
  auto path = VERIFY_RESULT(CheckpointPath());
  LocalCatalogCheckpointPB pb;
  checkpoint_.ToPB(pb.mutable_checkpoint());
  RETURN_NOT_OK_PREPEND(
      pb_util::WritePBContainerToPath(
          server_.fs_manager()->env(), path, pb, pb_util::OVERWRITE, pb_util::SYNC),
      Format("Unable to write the local catalog copy's WAL position to $0", path));
  persisted_checkpoint_ = checkpoint_;
  last_checkpoint_flush_ = now;
  return Status::OK();
}

Result<std::string> LocalCatalogPoller::GetPeerUuid(const HostPort& host_port, MonoDelta timeout) {
  server::GenericServiceProxy proxy(&finder_.get_proxy_cache(), host_port);
  server::GetStatusRequestPB req;
  server::GetStatusResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(proxy.GetStatus(req, &resp, &rpc));
  return resp.status().node_instance().permanent_uuid();
}

Result<bool> LocalCatalogPoller::IsInitDbDone(const HostPort& master_leader, MonoDelta timeout) {
  master::MasterAdminProxy proxy(&finder_.get_proxy_cache(), master_leader);
  master::IsInitDbDoneRequestPB req;
  master::IsInitDbDoneResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(proxy.IsInitDbDone(req, &resp, &rpc));
  SCHECK(
      !resp.has_error(), IllegalState,
      Format("IsInitDbDone failed: $0", resp.error().ShortDebugString()));
  return resp.done();
}

Status LocalCatalogPoller::EnsureLocalTablet() {
  const bool reseed = replica_.TakeReseedRequest();
  if (tablet_peer_ && !reseed) {
    return Status::OK();
  }

  if (reseed) {
    LOG_WITH_PREFIX(INFO) << "Re-seeding the local catalog copy";
    replica_.SetState(LocalCatalogServingState::kReseeding);
    replica_.OnReseedStarted();
    auto peer = tablet_peer_;
    tablet_peer_.reset();
    replica_.SetTabletPeer(nullptr);
    RETURN_NOT_OK(server_.tablet_manager()->DeleteLocalCatalogTablet(kLocalCatalogTabletId, peer));
    auto path = VERIFY_RESULT(CheckpointPath());
    WARN_NOT_OK(
        server_.fs_manager()->env()->DeleteFile(path),
        "Failed to delete the local catalog copy's recorded WAL position");
    checkpoint_ = OpId();
    persisted_checkpoint_ = OpId();
  } else {
    replica_.SetState(LocalCatalogServingState::kBootstrapping);
  }

  const auto timeout = MonoDelta::FromMilliseconds(FLAGS_local_catalog_poll_rpc_timeout_ms);

  // A copy already on disk is reopened without asking master anything. Its existence is itself
  // proof that initdb had finished, since the fresh-copy path below refuses to run before that,
  // and the source of a re-fetch is irrelevant when nothing is fetched.
  const bool reuse =
      !reseed &&
      VERIFY_RESULT(server_.tablet_manager()->CanReuseLocalCatalogTablet(kLocalCatalogTabletId));

  HostPort master_leader;
  PeerId master_uuid;
  if (!reuse) {
    // The copy is fetched from the master leader, which is the peer whose WAL the poll then reads.
    master_leader = VERIFY_RESULT(finder_.UpdateMasterLeaderHostPort(timeout));

    // initdb writes the system catalog from master's own embedded PG, and the initial system
    // catalog snapshot restore replaces the tablet's files wholesale. Copying the tablet before
    // initdb has finished would copy a half-built catalog and then have its files replaced
    // underneath the copy. Nothing is lost by waiting: pggate routes every catalog read to master
    // while initdb runs, because YBCIsLegacyModeForCatalogOps is true in initdb mode and only the
    // non-legacy path marks a read as a catalog snapshot read.
    if (!VERIFY_RESULT(IsInitDbDone(master_leader, timeout))) {
      return STATUS(TryAgain, "Waiting for initdb to finish before copying the system catalog");
    }

    master_uuid = VERIFY_RESULT(GetPeerUuid(master_leader, timeout));
  }

  tablet_peer_ = VERIFY_RESULT(server_.tablet_manager()->OpenOrCreateLocalCatalogTablet(
      kLocalCatalogTabletId, master::kSysCatalogTabletId, master_uuid, master_leader,
      /* force_reseed= */ reseed));
  replica_.SetTabletPeer(tablet_peer_);
  RETURN_NOT_OK(LoadCheckpoint());
  return server_.tablet_manager()->MakeLocalCatalogTabletLeader(tablet_peer_);
}

Status LocalCatalogPoller::FetchAndApplyOnce() {
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_fail_poll)) {
    replica_.IncPollFailures();
    return STATUS(IOError, "TEST_local_catalog_fail_poll is set");
  }

  const auto timeout = MonoDelta::FromMilliseconds(FLAGS_local_catalog_poll_rpc_timeout_ms);
  if (!proxy_) {
    proxy_ = VERIFY_RESULT(finder_.CreateProxy<cdc::SysCatalogChangeServiceProxy>(timeout));
  }

  cdc::GetSysCatalogChangesRequestPB req;
  req.set_requestor_uuid(server_.permanent_uuid());
  if (!checkpoint_.empty()) {
    checkpoint_.ToPB(req.mutable_from_checkpoint()->mutable_op_id());
  }

  cdc::GetSysCatalogChangesResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  const auto poll_start = MonoTime::Now();
  auto status = proxy_->GetSysCatalogChanges(req, &resp, &rpc);
  replica_.RecordPollLatency(MonoTime::Now() - poll_start);
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_poll_checkpoint_too_old) && status.ok() &&
      !resp.has_error()) {
    resp.mutable_error()->set_code(cdc::CDCErrorPB::CHECKPOINT_TOO_OLD);
    StatusToPB(
        STATUS(NotFound, "TEST_local_catalog_poll_checkpoint_too_old is set"),
        resp.mutable_error()->mutable_status());
  }
  if (!status.ok()) {
    replica_.IncPollFailures();
    return status;
  }
  if (resp.has_error()) {
    replica_.IncPollFailures();
    auto error = StatusFromPB(resp.error().status());
    if (resp.error().code() == cdc::CDCErrorPB::CHECKPOINT_TOO_OLD) {
      // Master has garbage collected WAL this copy had not consumed, so it cannot catch up
      // incrementally. Only a fresh copy of master's tablet can make it current again.
      LOG_WITH_PREFIX(WARNING) << "Master no longer retains the WAL at " << checkpoint_
                               << "; re-seeding the local catalog copy: " << error;
      replica_.RequestReseed();
    }
    return error;
  }

  const auto apply_start = MonoTime::Now();
  AppliedRecordStats stats;
  auto apply_status = ApplyRecords(resp, &stats);
  if (!apply_status.ok()) {
    replica_.IncApplyFailures();
    return apply_status;
  }

  const auto& changes = resp.changes();
  have_more_messages_ = resp.have_more_messages();

  HybridTime new_c;
  std::unordered_map<uint32_t, uint64_t> applied_versions;
  RETURN_NOT_OK(PublishAppliedBatch(changes, stats, &new_c, &applied_versions));
  if (!changes.records().empty()) {
    VLOG_WITH_PREFIX(1) << "Applied " << changes.records().size() << " records ("
                        << stats.ToString() << ") up to WAL position " << checkpoint_
                        << "; C is now " << LocalCatalogHybridTimeForLog(new_c) << ", versions "
                        << AsString(applied_versions)
                        << (have_more_messages_ ? ", master has more to send" : "");
  }
  replica_.RecordApplyLatency(MonoTime::Now() - apply_start);
  if (new_c.is_valid()) {
    replica_.RecordLag(server_.Clock()->Now().PhysicalDiff(new_c));
    replica_.OnSafeTimePublished(new_c);
  }

  RETURN_NOT_OK(PersistCheckpoint(/* force= */ false));
  return Status::OK();
}

std::string LocalCatalogPoller::AppliedRecordStats::ToString() const {
  return Format(
      "writes: $0, deletes: $1, of which provisional (into the intents db): $2, "
      "transaction records: $3, metadata records: $4, by table: $5",
      writes, deletes, provisional, transaction_records, metadata_records, by_table);
}

std::string LocalCatalogPoller::RecordTableName(const cdc::CDCRecordPB& record) const {
  if (record.key().empty()) {
    return "unknown";
  }
  Slice key(record.key(0).key());
  dockv::DocKeyDecoder decoder(key);
  Uuid cotable_id;
  auto has_cotable_id = decoder.DecodeCotableId(&cotable_id);
  if (!has_cotable_id.ok()) {
    return "undecodable";
  }
  if (!*has_cotable_id) {
    // Master's own metadata rows live in the primary table of the system catalog tablet.
    return "sys.catalog";
  }
  const auto table_id = cotable_id.ToHexString();
  if (tablet_peer_) {
    auto table_info = tablet_peer_->tablet_metadata()->GetTableInfo(table_id);
    if (table_info.ok()) {
      return (*table_info)->table_name;
    }
  }
  return table_id;
}

Status LocalCatalogPoller::PublishAppliedBatch(
    const cdc::GetChangesResponsePB& changes, const AppliedRecordStats& stats, HybridTime* new_c,
    std::unordered_map<uint32_t, uint64_t>* applied_versions) {
  if (changes.has_checkpoint() && changes.checkpoint().has_op_id()) {
    auto batch_end = OpId::FromPB(changes.checkpoint().op_id());
    // Compared by index alone. This is a position in master's WAL, whose term changes on a master
    // leader election, and an OpId comparison would then reject a newer position carrying a lower
    // term and leave the poll re-requesting the same records forever.
    if (batch_end.index > checkpoint_.index) {
      checkpoint_ = batch_end;
      replica_.SetAppliedOpId(checkpoint_);
    }
  }

  // Everything master certified as present at this safe time is now in the local regular store,
  // so C may advance to it. A[db] is read from the copy at the same time, so the two are published
  // together and a backend can never be told a version whose rows the copy does not hold.
  if (changes.has_safe_hybrid_time()) {
    auto safe_time = HybridTime(changes.safe_hybrid_time());
    if (safe_time.is_valid() && !safe_time.is_special()) {
      *new_c = safe_time;
      *applied_versions = VERIFY_RESULT(ReadLocalCatalogVersions(*new_c));
    }
  }
  replica_.PublishAfterApply(*new_c, *applied_versions);
  if (new_c->is_valid()) {
    replica_.RecordLag(server_.Clock()->Now().PhysicalDiff(*new_c));
    replica_.OnSafeTimePublished(*new_c);
  }
  return PersistCheckpoint(/* force= */ false);
}

Status LocalCatalogPoller::ApplyPushedBatch(
    const cdc::GetSysCatalogChangesResponsePB& batch, CoarseTimePoint deadline) {
  auto push = std::make_shared<PendingPush>();
  push->batch = batch;
  {
    std::lock_guard lock(push_mutex_);
    if (pending_push_) {
      // One pushed batch at a time. A second release's records are of no use to this copy until
      // the first has been applied, and its sender can wait for the poller instead.
      return STATUS(TryAgain, "Another pushed system catalog batch is already being applied");
    }
    pending_push_ = push;
  }
  RequestImmediatePoll();
  UniqueLock lock(push_mutex_);
  while (!push->done) {
    if (push_cond_.wait_until(GetLockForCondition(lock), deadline) == std::cv_status::timeout) {
      // Withdraw it if the poll thread has not taken it yet; if it has, it owns a reference of
      // its own and completes into the shared state, which this reference keeps alive. Either way
      // this caller stops waiting and falls back to the gate wait.
      if (pending_push_ == push) {
        pending_push_.reset();
      }
      return STATUS(TimedOut, "Timed out waiting for a pushed system catalog batch to apply");
    }
  }
  return push->result;
}

void LocalCatalogPoller::CompletePendingPush(
    const std::shared_ptr<PendingPush>& push, const Status& status) {
  {
    std::lock_guard lock(push_mutex_);
    push->result = status;
    push->done = true;
  }
  push_cond_.notify_all();
}

void LocalCatalogPoller::ApplyPendingPush() {
  std::shared_ptr<PendingPush> push;
  {
    std::lock_guard lock(push_mutex_);
    // Moved out of the slot, so it is empty for the whole apply and a second release does not
    // wait behind this one only to find its own batch stale.
    push = std::move(pending_push_);
    pending_push_.reset();
  }
  if (!push) {
    return;
  }
  if (!tablet_peer_) {
    // A re-seed took the tablet away between the handoff and now. There is nothing to apply into,
    // and making the caller wait out its deadline for that would be pointless.
    CompletePendingPush(
        push, STATUS(IllegalState, "The local catalog copy has no tablet to apply a pushed batch"));
    return;
  }
  CompletePendingPush(push, DoApplyPushedBatch(push->batch));
}

Status LocalCatalogPoller::DoApplyPushedBatch(const cdc::GetSysCatalogChangesResponsePB& batch) {
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_pushed_batch_apply_delay_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_local_catalog_pushed_batch_apply_delay_ms));
  }
  const auto& changes = batch.changes();

  // What establishes that nothing is missing is where master sliced from, not where the first
  // record sits. Master fetched everything above that position, so every record between it and
  // the batch's end is in the slice; but plenty of WAL operations produce no record at all -- a
  // history cutoff, the no-op of an election, a transaction status update that is not an apply --
  // so the first record's position can be well above this copy's own and nothing be absent.
  SCHECK(
      batch.has_slice_from(), IllegalState,
      "A pushed system catalog batch did not say which position it was sliced from");
  const auto slice_from = OpId::FromPB(batch.slice_from());
  SCHECK_LE(
      slice_from.index, checkpoint_.index, IllegalState,
      Format(
          "A pushed system catalog batch was sliced from $0, above the applied position $1, so "
          "records between them would be missing",
          slice_from, checkpoint_));

  // Master slices from a position it learned by heartbeat, which is up to one heartbeat old, so a
  // slice normally repeats records this copy already has. Records carry the position of the
  // operation that produced them and one operation's records all share it, so dropping on the
  // index never splits an operation.
  cdc::GetSysCatalogChangesResponsePB retained;
  auto* retained_changes = retained.mutable_changes();
  *retained_changes->mutable_checkpoint() = changes.checkpoint();
  if (changes.has_safe_hybrid_time()) {
    retained_changes->set_safe_hybrid_time(changes.safe_hybrid_time());
  }
  for (const auto& record : changes.records()) {
    if (OpId::FromPB(record.op_id()).index > checkpoint_.index) {
      *retained_changes->add_records() = record;
    }
  }

  AppliedRecordStats stats;
  auto apply_status = ApplyRecords(retained, &stats);
  if (!apply_status.ok()) {
    replica_.IncApplyFailures();
    return apply_status;
  }

  HybridTime new_c;
  std::unordered_map<uint32_t, uint64_t> applied_versions;
  RETURN_NOT_OK(PublishAppliedBatch(*retained_changes, stats, &new_c, &applied_versions));
  VLOG_WITH_PREFIX(1) << "Applied a pushed batch of " << retained_changes->records().size()
                      << " records (" << stats.ToString() << ") up to WAL position " << checkpoint_
                      << "; C is now " << LocalCatalogHybridTimeForLog(new_c) << ", versions "
                      << AsString(applied_versions);
  return Status::OK();
}

Status LocalCatalogPoller::ApplyRecords(
    const cdc::GetSysCatalogChangesResponsePB& resp, AppliedRecordStats* stats) {
  const auto& changes = resp.changes();
  if (changes.records().empty()) {
    return Status::OK();
  }

  // Records are shipped in master WAL order and must be applied in that order, because a
  // transaction's write records must be staged before its apply record turns them into rows.
  // A CHANGE_METADATA record is the last record of a response, so the batch accumulated before it
  // is written first.
  std::unique_ptr<XClusterWriteInterface> write_strategy;
  ResetWriteInterface(&write_strategy);
  bool have_pending_writes = false;

  const auto flush = [this, &write_strategy, &have_pending_writes]() -> Status {
    if (!have_pending_writes) {
      return Status::OK();
    }
    while (auto write_request = write_strategy->FetchNextRequest()) {
      RETURN_NOT_OK(WriteBatchToLocalTablet(write_request));
    }
    have_pending_writes = false;
    return Status::OK();
  };

  for (const auto& record : changes.records()) {
    if (VLOG_IS_ON(3)) {
      const auto txn_id =
          record.has_transaction_state()
              ? Slice(record.transaction_state().transaction_id()).ToDebugHexString()
              : std::string();
      if (record.operation() == cdc::CDCRecordPB::APPLY) {
        // Moves the transaction's external intents into the regular store at its commit time.
        VLOG_WITH_PREFIX(3) << "APPLY of txn " << txn_id << " committed at "
                            << LocalCatalogHybridTimeForLog(
                                   HybridTime(record.transaction_state().commit_hybrid_time()))
                            << ", aborted subtransaction ranges "
                            << record.transaction_state().aborted().set().size();
      } else {
        VLOG_WITH_PREFIX(3)
            << cdc::CDCRecordPB::OperationType_Name(record.operation()) << " at "
            << LocalCatalogHybridTimeForLog(HybridTime(record.time())) << " on "
            << RecordTableName(record)
            // A record that belongs to a transaction is stored as an external intent, keyed by the
            // transaction rather than by the row, and becomes readable only when that
            // transaction's APPLY record arrives. Everything else is written straight to the
            // regular store at master's own hybrid time.
            << (record.has_transaction_state() ? Format(" -> intents db, txn $0", txn_id)
                                               : " -> regular db")
            << (record.key().empty()
                    ? ""
                    : Format(" key $0", dockv::DocKey::DebugSliceToString(record.key(0).key())));
      }
    }
    switch (record.operation()) {
      case cdc::CDCRecordPB::WRITE:
        ++stats->writes;
        if (record.has_transaction_state()) {
          ++stats->provisional;
        }
        ++stats->by_table[RecordTableName(record)];
        break;
      case cdc::CDCRecordPB::DELETE:
        ++stats->deletes;
        if (record.has_transaction_state()) {
          ++stats->provisional;
        }
        ++stats->by_table[RecordTableName(record)];
        break;
      case cdc::CDCRecordPB::APPLY:
        ++stats->transaction_records;
        break;
      case cdc::CDCRecordPB::CHANGE_METADATA:
        ++stats->metadata_records;
        break;
      case cdc::CDCRecordPB::SPLIT_OP:
        break;
    }
    if (record.operation() == cdc::CDCRecordPB::CHANGE_METADATA) {
      RETURN_NOT_OK(flush());
      RETURN_NOT_OK(ApplyChangeMetadataRecord(record));
      continue;
    }
    if (record.operation() == cdc::CDCRecordPB::SPLIT_OP) {
      return STATUS(
          IllegalState, "The master system catalog tablet cannot split, yet a SPLIT_OP arrived");
    }
    RETURN_NOT_OK(write_strategy->ProcessRecord(
        ProcessRecordInfo{
            .tablet_id = kLocalCatalogTabletId,
            // The copy has the same schema versions as master's tablet, so no packed-row schema
            // version is remapped.
            .schema_versions_map = {},
            .colocation_id = kColocationIdNotSet},
        record));
    have_pending_writes = true;
  }
  return flush();
}

Status LocalCatalogPoller::WriteBatchToLocalTablet(
    const std::shared_ptr<WriteRequestMsg>& write_request) {
  auto tablet = VERIFY_RESULT(tablet_peer_->shared_tablet());
  auto leader_term = tablet_peer_->LeaderTerm();
  SCHECK_GT(
      leader_term, 0, IllegalState,
      "The local catalog copy is not the leader of its own single-peer group yet");

  auto response = rpc::SharedMessage<WriteResponseMsg>();
  auto deadline = CoarseMonoClock::Now() +
                  MonoDelta::FromMilliseconds(FLAGS_local_catalog_apply_write_timeout_ms);
  auto query = std::make_unique<tablet::WriteQuery>(
      leader_term, deadline, tablet_peer_.get(), tablet, /* rpc_context= */ nullptr,
      response.get());
  query->set_client_request(std::cref(*write_request));

  Synchronizer synchronizer;
  query->set_callback(synchronizer.AsStdStatusCallback());
  tablet_peer_->WriteAsync(std::move(query));
  RETURN_NOT_OK(synchronizer.Wait());
  if (response->has_error()) {
    return StatusFromPB(response->error().status());
  }
  return Status::OK();
}

Status LocalCatalogPoller::ApplyChangeMetadataRecord(const cdc::CDCRecordPB& record) {
  SCHECK(
      record.has_change_metadata_request(), InvalidArgument,
      "A CHANGE_METADATA record must carry a change metadata request");
  auto tablet = VERIFY_RESULT(tablet_peer_->shared_tablet());
  auto leader_term = tablet_peer_->LeaderTerm();
  SCHECK_GT(
      leader_term, 0, IllegalState,
      "The local catalog copy is not the leader of its own single-peer group yet");

  auto operation =
      std::make_unique<tablet::ChangeMetadataOperation>(tablet, tablet_peer_->log());
  auto* request = operation->AllocateRequest();
  request->CopyFrom(record.change_metadata_request());
  // The record names master's tablet; the operation must name the copy's.
  request->dup_tablet_id(kLocalCatalogTabletId);

  Synchronizer synchronizer;
  operation->set_completion_callback(synchronizer.AsStdStatusCallback());
  tablet_peer_->Submit(std::move(operation), leader_term);
  return synchronizer.Wait();
}

Result<std::unordered_map<uint32_t, uint64_t>> LocalCatalogPoller::ReadLocalCatalogVersions(
    HybridTime read_time) {
  std::unordered_map<uint32_t, uint64_t> versions;
  auto tablet = VERIFY_RESULT(tablet_peer_->shared_tablet());
  auto table_info_result = tablet->metadata()->GetTableInfo(kPgYbCatalogVersionTableId);
  if (!table_info_result.ok()) {
    // The copy predates initdb, or a major version upgrade renamed the table. Either way there is
    // no version to report; C still advances.
    return versions;
  }
  const auto& table_info = *table_info_result;
  const auto& schema = table_info->schema();
  auto db_oid_id = VERIFY_RESULT(schema.ColumnIdByName(master::kDbOidColumnName));
  auto version_id = VERIFY_RESULT(schema.ColumnIdByName(master::kCurrentVersionColumnName));

  // The rows written for this response landed at master's hybrid times, which are below the
  // hybrid times of the local operations that carried them, so the copy's MVCC safe time is at or
  // above read_time by the time those operations applied. Waiting makes that explicit rather than
  // assumed.
  RETURN_NOT_OK(tablet->SafeTime(
      tablet::RequireLease::kTrue, read_time,
      CoarseMonoClock::Now() +
          MonoDelta::FromMilliseconds(FLAGS_local_catalog_apply_write_timeout_ms)));

  dockv::ReaderProjection projection(schema);
  auto iter = VERIFY_RESULT(tablet->NewUninitializedDocRowIterator(
      projection, ReadHybridTime::SingleTime(read_time), kPgYbCatalogVersionTableId));
  auto request_scope = VERIFY_RESULT(tablet->CreateRequestScope());
  RETURN_NOT_OK(iter->InitForTableType(table_info->table_type));

  qlexpr::QLTableRow row;
  while (VERIFY_RESULT(iter->FetchNext(&row))) {
    auto db_oid_value = row.GetValue(db_oid_id);
    auto version_value = row.GetValue(version_id);
    SCHECK(
        db_oid_value && version_value, Corruption,
        "A pg_yb_catalog_version row in the local catalog copy is missing a column");
    versions.emplace(
        db_oid_value->get().uint32_value(),
        static_cast<uint64_t>(version_value->get().int64_value()));
  }
  return versions;
}

}  // namespace yb::tserver
