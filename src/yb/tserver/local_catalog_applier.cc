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

#include "yb/tserver/local_catalog_applier.h"

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
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/unique_lock.h"
#include "yb/util/status_log.h"

DEFINE_RUNTIME_uint32(local_catalog_master_rpc_timeout_ms, 30000,
    "Timeout for one RPC of the tserver-local catalog copy to master: a push request, or a step "
    "of fetching the copy.");

DEFINE_RUNTIME_uint32(local_catalog_checkpoint_flush_interval_ms, 1000,
    "Minimum interval between writes of the tserver-local catalog copy's WAL position to disk. "
    "A restart resumes from the last written position and re-applies the records after it, which "
    "writes the same keys at the same hybrid times and is therefore harmless.");

DEFINE_RUNTIME_uint32(local_catalog_apply_write_timeout_ms, 60000,
    "Timeout for one write of pushed records into the tserver-local catalog copy.");

DEFINE_RUNTIME_uint32(local_catalog_floor_request_interval_ms, 1000,
    "While a wait on the tserver-local catalog copy is open, how often the tserver repeats its "
    "request that master push the copy up to the waited-for hybrid time. A repeat repairs a lost "
    "request and reaches a new master leader.");

DECLARE_uint32(local_catalog_bootstrap_retry_delay_ms);

DEFINE_test_flag(uint32, local_catalog_pushed_batch_apply_delay_ms, 0,
    "Sleep this long inside the apply of pages that master pushed or carried on a lock release, so "
    "that the handler waiting on them gives up first and the handoff outlives its caller.");

DEFINE_test_flag(bool, local_catalog_pause_applier, false,
    "Leave pages that master pushes or carries on lock releases queued and unapplied. C stops "
    "advancing, so the version gate blocks and reads that carry a newer version wait. Floor "
    "requests are still sent.");

DEFINE_test_flag(bool, local_catalog_fail_push, false,
    "Make every push of pages from master fail to apply.");

DEFINE_test_flag(bool, local_catalog_push_reseed, false,
    "Treat every push of pages as one telling the copy that master no longer retains the WAL "
    "after its position, so the copy fetches master's tablet again. Exercises the re-seed path "
    "without having to drive master's WAL retention.");

namespace yb::tserver {

namespace {

const char* const kCheckpointFileName = "local_catalog_checkpoint";

// The records of changes whose operation is above the applied position. A page may start below
// the position the copy holds, and re-applying the operations in between is
// avoided rather than relied on to be harmless.
cdc::GetChangesResponsePB RecordsAbove(const cdc::GetChangesResponsePB& changes, int64_t index) {
  cdc::GetChangesResponsePB retained;
  for (const auto& record : changes.records()) {
    if (OpId::FromPB(record.op_id()).index > index) {
      *retained.add_records() = record;
    }
  }
  return retained;
}

}  // namespace

struct LocalCatalogApplier::FloorRequest {
  cdc::RequestSysCatalogPushRequestPB req;
  cdc::RequestSysCatalogPushResponsePB resp;
  rpc::RpcController controller;
  std::atomic<bool> done{false};
  std::atomic<bool> failed{false};
};

LocalCatalogApplier::LocalCatalogApplier(LocalCatalogReplica& replica, TabletServer& server)
    : replica_(replica),
      server_(server),
      log_prefix_(Format("LocalCatalogApplier [$0]: ", server.permanent_uuid())),
      finder_(server.messenger(), server.proxy_cache(), server.options().GetMasterAddresses()),
      scheduler_(finder_, *this) {}

LocalCatalogApplier::~LocalCatalogApplier() = default;

Status LocalCatalogApplier::Start() { return scheduler_.Start(); }

void LocalCatalogApplier::Shutdown() {
  {
    UniqueLock lock(push_mutex_);
    push_shutdown_ = true;
    for (auto& [_, push] : pending_pushes_) {
      push->result = STATUS(ShutdownInProgress, "The local catalog copy is shutting down");
      push->done = true;
    }
    pending_pushes_.clear();
    push_cond_.notify_all();
    push_cond_.wait(GetLockForCondition(lock), [this]() NO_THREAD_SAFETY_ANALYSIS {
      return push_callers_ == 0;
    });
  }
  scheduler_.Shutdown();
  if (tablet_peer_) {
    WARN_NOT_OK(PersistCheckpoint(/* force= */ true), "Failed to persist the local catalog copy's "
                                                      "WAL position on shutdown");
  }
}

void LocalCatalogApplier::RequestPush(HybridTime floor) {
  if (floor.is_valid()) {
    auto current = wait_target_.load(std::memory_order_acquire);
    while (current < floor.ToUint64() &&
           !wait_target_.compare_exchange_weak(current, floor.ToUint64())) {}
  }
  scheduler_.TriggerASAP();
}

void LocalCatalogApplier::Init() {}

void LocalCatalogApplier::ResetProxy() { proxy_.reset(); }

std::string LocalCatalogApplier::name() { return "local_catalog_applier"; }

const std::string& LocalCatalogApplier::LogPrefix() const { return log_prefix_; }

MonoDelta LocalCatalogApplier::IntervalToNextPoll(int32_t consecutive_failures) {
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
    // checkpoint lock. Retrying at the floor request interval would only add log noise.
    return MonoDelta::FromMilliseconds(FLAGS_local_catalog_bootstrap_retry_delay_ms);
  }
  if (consecutive_failures > 0) {
    // Exponential backoff, capped at one second, so a master that is down does not spin.
    const auto shift = std::min(consecutive_failures, 10);
    return MonoDelta::FromMilliseconds(
        std::min<int64_t>(1000, static_cast<int64_t>(1) << shift));
  }
  {
    std::lock_guard lock(push_mutex_);
    if (!pending_pushes_.empty() && !FLAGS_TEST_local_catalog_pause_applier) {
      return MonoDelta::FromMilliseconds(0);
    }
  }
  // The thread wakes for pushed pages and floors, and otherwise only to repeat an open floor
  // request and to write the applied position to disk.
  return MonoDelta::FromMilliseconds(std::max<uint32_t>(
      FLAGS_local_catalog_floor_request_interval_ms, 1));
}

Status LocalCatalogApplier::Poll() {
  // First, so that a release or a push waiting on its pages is answered as early as possible.
  ApplyPendingPushes();
  RETURN_NOT_OK(EnsureLocalTablet());
  // Nothing can be applied until this copy leads its own single-peer group, and winning that
  // election is asynchronous: the term is only established once the new term's no-op commits. A
  // push that arrives before then is refused, and master re-sends it.
  if (tablet_peer_->LeaderTerm() <= 0) {
    VLOG_WITH_PREFIX(1) << "Waiting to lead the local catalog copy's own group before applying";
    return STATUS(TryAgain, "The local catalog copy does not yet lead its own group");
  }
  MaybeSendFloorRequest();
  return PersistCheckpoint(/* force= */ false);
}

void LocalCatalogApplier::MaybeSendFloorRequest() {
  if (floor_request_) {
    if (!floor_request_->done.load(std::memory_order_acquire)) {
      return;
    }
    if (floor_request_->failed.load(std::memory_order_acquire)) {
      // Most likely the master leader moved; the next request resolves it again.
      proxy_.reset();
    }
    floor_request_.reset();
  }
  auto target = wait_target_.load(std::memory_order_acquire);
  if (target == 0) {
    return;
  }
  if (replica_.safe_time() >= HybridTime(target)) {
    wait_target_.compare_exchange_strong(target, 0);
    return;
  }
  const auto now = MonoTime::Now();
  if (target == last_floor_sent_ && last_floor_sent_at_.Initialized() &&
      now - last_floor_sent_at_ <
          MonoDelta::FromMilliseconds(FLAGS_local_catalog_floor_request_interval_ms)) {
    return;
  }
  const auto timeout = MonoDelta::FromMilliseconds(FLAGS_local_catalog_master_rpc_timeout_ms);
  if (!proxy_) {
    auto proxy = finder_.CreateProxy<cdc::SysCatalogChangeServiceProxy>(timeout);
    if (!proxy.ok()) {
      VLOG_WITH_PREFIX(1) << "No master leader to ask for a push: " << proxy.status();
      return;
    }
    proxy_ = std::move(*proxy);
  }
  auto request = std::make_shared<FloorRequest>();
  request->req.set_requestor_uuid(server_.permanent_uuid());
  if (!checkpoint_.empty()) {
    checkpoint_.ToPB(request->req.mutable_applied_op_id());
  }
  request->req.set_floor(target);
  request->controller.set_timeout(timeout);
  last_floor_sent_ = target;
  last_floor_sent_at_ = now;
  replica_.IncFloorRequestsSent();
  VLOG_WITH_PREFIX(1) << "Asking master to push up to " << LocalCatalogHybridTimeForLog(
                             HybridTime(target))
                      << " from " << checkpoint_ << "; the copy holds "
                      << LocalCatalogHybridTimeForLog(replica_.safe_time());
  floor_request_ = request;
  proxy_->RequestSysCatalogPushAsync(
      request->req, &request->resp, &request->controller, [request] {
        request->failed.store(
            !request->controller.status().ok() || request->resp.has_error(),
            std::memory_order_release);
        request->done.store(true, std::memory_order_release);
      });
}

Result<std::string> LocalCatalogApplier::CheckpointPath() const {
  // Deliberately not in the tablet metadata directory: FsManager::ListTabletIds treats every file
  // there as a tablet id, and on restart the tablet manager would try to parse this file as a
  // tablet superblock and fail to start.
  return JoinPathSegments(server_.fs_manager()->GetDefaultRootDir(), kCheckpointFileName);
}

Status LocalCatalogApplier::LoadCheckpoint() {
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
  LOG_WITH_PREFIX(INFO) << "Resuming from WAL position " << checkpoint_;
  return Status::OK();
}

Status LocalCatalogApplier::PersistCheckpoint(bool force) {
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

Result<std::string> LocalCatalogApplier::GetPeerUuid(const HostPort& host_port, MonoDelta timeout) {
  server::GenericServiceProxy proxy(&finder_.get_proxy_cache(), host_port);
  server::GetStatusRequestPB req;
  server::GetStatusResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(proxy.GetStatus(req, &resp, &rpc));
  return resp.status().node_instance().permanent_uuid();
}

Result<bool> LocalCatalogApplier::IsInitDbDone(const HostPort& master_leader, MonoDelta timeout) {
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

Status LocalCatalogApplier::EnsureLocalTablet() {
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

  const auto timeout = MonoDelta::FromMilliseconds(FLAGS_local_catalog_master_rpc_timeout_ms);

  // A copy already on disk is reopened without asking master anything. Its existence is itself
  // proof that initdb had finished, since the fresh-copy path below refuses to run before that,
  // and the source of a re-fetch is irrelevant when nothing is fetched.
  const bool reuse =
      !reseed &&
      VERIFY_RESULT(server_.tablet_manager()->CanReuseLocalCatalogTablet(kLocalCatalogTabletId));

  HostPort master_leader;
  PeerId master_uuid;
  if (!reuse) {
    // The copy is fetched from the master leader, which is the peer whose WAL master then pushes.
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
  RETURN_NOT_OK(server_.tablet_manager()->MakeLocalCatalogTabletLeader(tablet_peer_));
  // The copy serves only once a page carrying a safe time has been applied, and master pushes to a
  // copy it does not know about only when asked.
  RequestPush(server_.Clock()->Now());
  return Status::OK();
}

std::string LocalCatalogApplier::AppliedRecordStats::ToString() const {
  return Format(
      "writes: $0, deletes: $1, of which provisional (into the intents db): $2, "
      "transaction records: $3, metadata records: $4, by table: $5",
      writes, deletes, provisional, transaction_records, metadata_records, by_table);
}

std::string LocalCatalogApplier::RecordTableName(const cdc::CDCRecordPB& record) const {
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

Status LocalCatalogApplier::PublishSafeTime(HybridTime safe_time) {
  if (!safe_time.is_valid() || safe_time.is_special()) {
    return Status::OK();
  }
  // Everything master certified as present at this safe time is now in the local regular store,
  // so C may advance to it. A[db] is read from the copy at the same time, so the two are published
  // together and a backend can never be told a version whose rows the copy does not hold.
  auto applied_versions = VERIFY_RESULT(ReadLocalCatalogVersions(safe_time));
  replica_.PublishAfterApply(safe_time, applied_versions);
  replica_.RecordLag(server_.Clock()->Now().PhysicalDiff(safe_time));
  replica_.OnSafeTimePublished(safe_time);
  VLOG_WITH_PREFIX(2) << "Published C " << LocalCatalogHybridTimeForLog(safe_time) << " at "
                      << checkpoint_ << ", versions " << AsString(applied_versions);
  return Status::OK();
}

void LocalCatalogApplier::AdvanceTo(const OpId& end) {
  // Compared by index alone. This is a position in master's WAL, whose term changes on a master
  // leader election, and an OpId comparison would then reject a newer position carrying a lower
  // term and leave the copy re-requesting the same records forever.
  if (end.index > checkpoint_.index) {
    checkpoint_ = end;
    replica_.SetAppliedOpId(checkpoint_);
  }
}

Result<LocalCatalogPushOutcome> LocalCatalogApplier::ApplyPushedPages(
    std::shared_ptr<const PushSysCatalogChangesRequestPB> pages, CoarseTimePoint deadline,
    bool wait) {
  int64_t first_from = 0;
  if (!pages->pages().empty() && pages->pages(0).has_page_from()) {
    first_from = pages->pages(0).page_from().index();
  }
  auto push = std::make_shared<PendingPush>();
  push->pages = std::move(pages);
  {
    std::lock_guard lock(push_mutex_);
    if (push_shutdown_) {
      return STATUS(ShutdownInProgress, "The local catalog copy is shutting down");
    }
    pending_pushes_.emplace(first_from, push);
    ++push_callers_;
  }
  auto leave = ScopeExit([this] {
    {
      std::lock_guard lock(push_mutex_);
      --push_callers_;
    }
    push_cond_.notify_all();
  });
  scheduler_.TriggerASAP();
  if (!wait) {
    return LocalCatalogPushOutcome{.applied = replica_.applied_op_id(),
                                   .safe_time = replica_.safe_time()};
  }
  UniqueLock lock(push_mutex_);
  while (!push->done) {
    if (push_shutdown_) {
      return STATUS(ShutdownInProgress, "The local catalog copy is shutting down");
    }
    if (push_cond_.wait_until(GetLockForCondition(lock), deadline) == std::cv_status::timeout) {
      // Withdraw it if the apply thread has not taken it yet; if it has, it owns a reference of
      // its own and completes into the shared state, which this reference keeps alive.
      for (auto it = pending_pushes_.begin(); it != pending_pushes_.end(); ++it) {
        if (it->second == push) {
          pending_pushes_.erase(it);
          break;
        }
      }
      return STATUS(TimedOut, "Timed out waiting for pushed system catalog pages to apply");
    }
  }
  return push->result;
}

void LocalCatalogApplier::CompletePendingPush(
    const std::shared_ptr<PendingPush>& push, Result<LocalCatalogPushOutcome> result) {
  {
    std::lock_guard lock(push_mutex_);
    push->result = std::move(result);
    push->done = true;
  }
  push_cond_.notify_all();
}

void LocalCatalogApplier::ApplyPendingPushes() {
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_pause_applier)) {
    return;
  }
  for (;;) {
    std::shared_ptr<PendingPush> push;
    {
      std::lock_guard lock(push_mutex_);
      if (pending_pushes_.empty()) {
        return;
      }
      push = std::move(pending_pushes_.begin()->second);
      pending_pushes_.erase(pending_pushes_.begin());
    }
    if (!tablet_peer_) {
      // A re-seed took the tablet away, or the copy is still being created. There is nothing to
      // apply into, and making the caller wait out its deadline for that would be pointless.
      CompletePendingPush(
          push, STATUS(IllegalState, "The local catalog copy has no tablet to apply pages to"));
      continue;
    }
    if (tablet_peer_->LeaderTerm() <= 0) {
      CompletePendingPush(
          push, STATUS(IllegalState, "The local catalog copy does not yet lead its own group"));
      continue;
    }
    auto result = DoApplyPages(*push->pages);
    if (!result.ok()) {
      VLOG_WITH_PREFIX(1) << "Could not apply pushed pages: " << result.status();
    }
    CompletePendingPush(push, std::move(result));
  }
}

Result<LocalCatalogPushOutcome> LocalCatalogApplier::DoApplyPages(
    const PushSysCatalogChangesRequestPB& pages) {
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_pushed_batch_apply_delay_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_local_catalog_pushed_batch_apply_delay_ms));
  }
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_fail_push)) {
    replica_.IncApplyFailures();
    return STATUS(IOError, "TEST_local_catalog_fail_push is set");
  }
  if (PREDICT_FALSE(FLAGS_TEST_local_catalog_push_reseed)) {
    LOG_WITH_PREFIX(WARNING) << "TEST_local_catalog_push_reseed is set; re-seeding";
    replica_.RequestReseed();
    return STATUS(IllegalState, "The local catalog copy must be fetched again from master");
  }
  SCHECK_GE(
      pages.master_term(), highest_master_term_, IllegalState,
      Format("Pages from master term $0 are older than the term $1 pages were applied from",
             pages.master_term(), highest_master_term_));
  highest_master_term_ = pages.master_term();

  const auto apply_start = MonoTime::Now();
  LocalCatalogPushOutcome outcome;
  HybridTime publish;
  AppliedRecordStats stats;
  size_t applied_records = 0;
  for (const auto& page : pages.pages()) {
    const auto from = OpId::FromPB(page.page_from());
    const auto end = OpId::FromPB(page.page_end());
    if (from.index > checkpoint_.index) {
      // Not every operation produces a record, so what establishes that nothing is missing is
      // where the page starts, not where its first record sits.
      VLOG_WITH_PREFIX(1) << "A pushed page starts at " << from << ", above the applied position "
                          << checkpoint_ << "; asking master to re-send from there";
      outcome.gap = true;
      break;
    }
    if (page.reseed_required()) {
      if (end.index <= checkpoint_.index && end.index > 0) {
        continue;
      }
      LOG_WITH_PREFIX(WARNING) << "Master's system catalog WAL after " << checkpoint_
                               << " holds a restore or is no longer retained; re-seeding the "
                               << "local catalog copy";
      replica_.RequestReseed();
      return STATUS(IllegalState, "The local catalog copy must be fetched again from master");
    }
    if (end.index > checkpoint_.index) {
      cdc::GetChangesResponsePB changes;
      if (!page.records().empty() && !changes.ParseFromString(page.records())) {
        return STATUS(Corruption, "Could not parse a pushed system catalog page");
      }
      auto status = from.index < checkpoint_.index
                        ? ApplyRecords(RecordsAbove(changes, checkpoint_.index), &stats)
                        : ApplyRecords(changes, &stats);
      if (!status.ok()) {
        replica_.IncApplyFailures();
        return status;
      }
      applied_records += changes.records_size();
      AdvanceTo(end);
    }
    if (page.has_safe_time() && checkpoint_.index >= end.index) {
      const HybridTime safe_time(page.safe_time());
      if (!publish.is_valid() || safe_time > publish) {
        publish = safe_time;
      }
    }
  }
  if (publish.is_valid() && publish > replica_.safe_time()) {
    RETURN_NOT_OK(PublishSafeTime(publish));
  }
  replica_.RecordApplyLatency(MonoTime::Now() - apply_start);
  if (applied_records > 0) {
    VLOG_WITH_PREFIX(1) << "Applied " << pages.pages_size() << " pushed pages, "
                        << applied_records << " records (" << stats.ToString()
                        << ") up to WAL position " << checkpoint_ << "; C is now "
                        << LocalCatalogHybridTimeForLog(replica_.safe_time());
  }
  RETURN_NOT_OK(PersistCheckpoint(/* force= */ false));
  outcome.applied = checkpoint_;
  outcome.safe_time = replica_.safe_time();
  return outcome;
}

Status LocalCatalogApplier::ApplyRecords(
    const cdc::GetChangesResponsePB& changes, AppliedRecordStats* stats) {
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

Status LocalCatalogApplier::WriteBatchToLocalTablet(
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

Status LocalCatalogApplier::ApplyChangeMetadataRecord(const cdc::CDCRecordPB& record) {
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

Result<std::unordered_map<uint32_t, uint64_t>> LocalCatalogApplier::ReadLocalCatalogVersions(
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
