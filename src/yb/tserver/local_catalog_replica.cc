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

#include "yb/tserver/local_catalog_replica.h"

#include <ctime>

#include "yb/gutil/stringprintf.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/local_catalog_applier.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver.pb.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/thread.h"
#include "yb/util/unique_lock.h"

DECLARE_bool(enable_object_locking_for_table_locks);
DECLARE_int32(local_catalog_serve_wait_timeout_ms);
DECLARE_bool(ysql_enable_concurrent_ddl);

// Same default as enable_object_locking_for_table_locks and ysql_enable_concurrent_ddl, which the
// copy requires.
#ifdef NDEBUG
constexpr bool kLocalTserverCatalogDefault = true;
#else
constexpr bool kLocalTserverCatalogDefault = false;
#endif
DEFINE_NON_RUNTIME_bool(enable_local_tserver_catalog, kLocalTserverCatalogDefault,
    "Serve YSQL catalog reads from a tserver-local DocDB copy of the master system catalog "
    "tablet instead of sending them to master. Master keeps the copy current by pushing its "
    "system catalog tablet's WAL; master remains the only writer.");
TAG_FLAG(enable_local_tserver_catalog, advanced);
TAG_FLAG(enable_local_tserver_catalog, experimental);

DEFINE_RUNTIME_uint32(local_catalog_bootstrap_retry_delay_ms, 1000,
    "Delay between attempts to create the tserver-local copy of the master system catalog "
    "tablet.");

DEFINE_test_flag(bool, local_catalog_disable_serving, false,
    "Keep the tserver-local catalog copy out of the read path even after it has bootstrapped. "
    "Catalog reads go to master, and the version gate publishes versions without waiting.");

METRIC_DEFINE_counter(server, local_catalog_apply_failures,
    "Local catalog apply failures", yb::MetricUnit::kUnits,
    "Number of pushes of system catalog pages that failed to apply to the local catalog tablet.");

METRIC_DEFINE_counter(server, local_catalog_reseeds,
    "Local catalog re-seeds", yb::MetricUnit::kUnits,
    "Number of times the local catalog tablet was replaced by a fresh copy of master's, because "
    "master no longer retained the WAL after its position, the WAL held a restore, or the copy "
    "could not be "
    "opened.");

METRIC_DEFINE_counter(server, local_catalog_serving_disabled_lease,
    "Local catalog serving disabled by lease loss", yb::MetricUnit::kUnits,
    "Number of times local catalog serving was disabled because the YSQL lease lapsed.");

METRIC_DEFINE_counter(server, local_catalog_reads_served,
    "Local catalog reads served", yb::MetricUnit::kRequests,
    "Number of catalog read operations answered from the local catalog tablet.");

METRIC_DEFINE_counter(server, local_catalog_reads_waited_for_version,
    "Local catalog reads that waited for a version", yb::MetricUnit::kRequests,
    "Number of catalog read operations that had to wait for the copy to apply the catalog "
    "version the backend carried.");

METRIC_DEFINE_counter(server, local_catalog_reads_waited_for_own_writes,
    "Local catalog reads that waited for the session's own catalog writes",
    yb::MetricUnit::kRequests,
    "Number of catalog read operations that had to wait for the copy to hold catalog rows the "
    "reading session had written itself in a transaction that incremented no catalog version, "
    "which a temporary relation's DDL is.");

METRIC_DEFINE_counter(server, local_catalog_reads_to_master_in_ddl,
    "Local catalog reads routed to master because the backend is in DDL",
    yb::MetricUnit::kRequests,
    "Number of catalog read operations sent to master because the backend was executing a DDL, "
    "or was in a transaction block in which a DDL had already run, so its own uncommitted catalog "
    "rows exist only on master.");

METRIC_DEFINE_counter(server, local_catalog_reads_to_master_serializable,
    "Local catalog reads routed to master because the transaction is SERIALIZABLE",
    yb::MetricUnit::kRequests,
    "Number of catalog read operations sent to master because a SERIALIZABLE transaction was "
    "attached to the session. Such a transaction sends no read time and no clamp request, so its "
    "catalog snapshot has no hybrid time of its own and each of its catalog reads runs at the "
    "master tablet's own latest safe time.");

METRIC_DEFINE_counter(server, local_catalog_reads_to_master_not_serving,
    "Local catalog reads routed to master because the copy is not serving",
    yb::MetricUnit::kRequests,
    "Number of catalog read operations sent to master because the local copy was bootstrapping, "
    "re-seeding, disabled by lease loss, or the read used a snapshot kind the copy cannot serve.");

METRIC_DEFINE_gauge_uint32(server, local_catalog_serving_state,
    "Local catalog serving state", yb::MetricUnit::kUnits,
    "Whether the tserver may answer catalog reads from its local copy of the master system "
    "catalog tablet: 0 bootstrapping, 1 serving, 2 disabled by lease loss, 3 re-seeding.");

METRIC_DEFINE_gauge_uint64(server, local_catalog_safe_time_micros,
    "Local catalog safe time", yb::MetricUnit::kMicroseconds,
    "C, as microseconds since the epoch: the highest hybrid time for which the local copy holds "
    "every catalog change committed at or below it. Zero until the first page with a safe time "
    "has been applied.");

METRIC_DEFINE_event_stats(server, local_catalog_apply_latency_us,
    "Local catalog apply latency", yb::MetricUnit::kMicroseconds,
    "Time from receiving a change-stream response to publishing its safe time as C.");

METRIC_DEFINE_event_stats(server, local_catalog_lag_us,
    "Local catalog lag", yb::MetricUnit::kMicroseconds,
    "The tserver clock minus C at the moment C is published.");

METRIC_DEFINE_counter(server, local_catalog_pushes_received,
    "Local catalog pushes received", yb::MetricUnit::kRequests,
    "Number of lock releases that arrived carrying system catalog change records.");

METRIC_DEFINE_counter(server, local_catalog_pushes_applied,
    "Local catalog pushes applied", yb::MetricUnit::kRequests,
    "Number of pushed system catalog batches this copy applied, each of which let a lock release "
    "publish its catalog versions without waiting for a separate push.");

METRIC_DEFINE_counter(server, local_catalog_pushes_refused,
    "Local catalog pushes refused", yb::MetricUnit::kRequests,
    "Number of pushed system catalog batches this copy could not use, because the batch did not "
    "continue its applied position, could not be parsed, or failed to apply. Each is followed by "
    "a request that master push from the copy's position, and the release waits for that push.");

METRIC_DEFINE_counter(server, local_catalog_floor_requests_sent,
    "Local catalog floor requests sent", yb::MetricUnit::kRequests,
    "Number of requests this tserver sent master to push its local catalog copy up to a hybrid "
    "time some caller was waiting for.");

METRIC_DEFINE_event_stats(server, local_catalog_gate_wait_us,
    "Local catalog version gate wait", yb::MetricUnit::kMicroseconds,
    "Time the object lock release handler waited for C to reach the hybrid time at which master "
    "read the catalog versions it is publishing.");

namespace yb::tserver {

bool LocalCatalogPrerequisitesMet() {
  return FLAGS_enable_object_locking_for_table_locks && FLAGS_ysql_enable_concurrent_ddl;
}

std::string LocalCatalogHybridTimeForLog(HybridTime ht) {
  if (!ht.is_valid()) {
    return "<invalid>";
  }
  if (ht.is_special()) {
    return ht.ToString();
  }
  const auto micros = ht.GetPhysicalValueMicros();
  const std::time_t seconds = static_cast<std::time_t>(micros / MonoTime::kMicrosecondsPerSecond);
  struct tm broken_down;
  localtime_r(&seconds, &broken_down);
  return StringPrintf(
      "%" PRIu64 " (%02d:%02d:%02d.%06" PRIu64 ")", micros, broken_down.tm_hour,
      broken_down.tm_min, broken_down.tm_sec, micros % MonoTime::kMicrosecondsPerSecond);
}

LocalCatalogReplica::LocalCatalogReplica(
    TabletServer& server, const scoped_refptr<MetricEntity>& metric_entity)
    : server_(server),
      metric_entity_(metric_entity),
      log_prefix_(Format("LocalCatalogReplica [$0]: ", server.permanent_uuid())) {}

LocalCatalogReplica::~LocalCatalogReplica() = default;

void LocalCatalogReplica::Init() {
  applier_ = std::make_unique<LocalCatalogApplier>(*this, server_);
  apply_failures_ = METRIC_local_catalog_apply_failures.Instantiate(metric_entity_);
  reseeds_ = METRIC_local_catalog_reseeds.Instantiate(metric_entity_);
  serving_disabled_lease_ = METRIC_local_catalog_serving_disabled_lease.Instantiate(metric_entity_);
  reads_served_ = METRIC_local_catalog_reads_served.Instantiate(metric_entity_);
  reads_waited_for_version_ =
      METRIC_local_catalog_reads_waited_for_version.Instantiate(metric_entity_);
  reads_waited_for_own_writes_ =
      METRIC_local_catalog_reads_waited_for_own_writes.Instantiate(metric_entity_);
  reads_to_master_in_ddl_ = METRIC_local_catalog_reads_to_master_in_ddl.Instantiate(metric_entity_);
  reads_to_master_serializable_ =
      METRIC_local_catalog_reads_to_master_serializable.Instantiate(metric_entity_);
  reads_to_master_not_serving_ =
      METRIC_local_catalog_reads_to_master_not_serving.Instantiate(metric_entity_);
  serving_state_ = METRIC_local_catalog_serving_state.Instantiate(
      metric_entity_, static_cast<uint32_t>(LocalCatalogServingState::kBootstrapping));
  safe_time_micros_ = METRIC_local_catalog_safe_time_micros.Instantiate(metric_entity_, 0);
  apply_latency_ = METRIC_local_catalog_apply_latency_us.Instantiate(metric_entity_);
  lag_ = METRIC_local_catalog_lag_us.Instantiate(metric_entity_);
  gate_wait_ = METRIC_local_catalog_gate_wait_us.Instantiate(metric_entity_);
  pushes_received_ = METRIC_local_catalog_pushes_received.Instantiate(metric_entity_);
  pushes_applied_ = METRIC_local_catalog_pushes_applied.Instantiate(metric_entity_);
  pushes_refused_ = METRIC_local_catalog_pushes_refused.Instantiate(metric_entity_);
  floor_requests_sent_ = METRIC_local_catalog_floor_requests_sent.Instantiate(metric_entity_);
}

bool LocalCatalogReplica::IsServing() const {
  return !FLAGS_TEST_local_catalog_disable_serving &&
         !major_version_upgrade_in_progress_.load(std::memory_order_acquire) &&
         state() == LocalCatalogServingState::kServing;
}

void LocalCatalogReplica::SetMajorVersionUpgradeInProgress(bool in_progress) {
  if (major_version_upgrade_in_progress_.exchange(in_progress, std::memory_order_acq_rel) !=
      in_progress) {
    LOG(INFO) << LogPrefix() << "YSQL major version upgrade in progress: " << in_progress
              << "; local catalog serving is " << (in_progress ? "suspended" : "allowed again");
  }
}

void LocalCatalogReplica::SetState(LocalCatalogServingState state) {
  auto old = state_.exchange(state, std::memory_order_acq_rel);
  if (old != state) {
    LOG(INFO) << LogPrefix() << "Serving state " << old << " -> " << state;
  }
  serving_state_->set_value(static_cast<uint32_t>(state));
  std::lock_guard lock(mutex_);
  cond_.notify_all();
}

uint64_t LocalCatalogReplica::applied_version(uint32_t db_oid) const {
  std::lock_guard lock(mutex_);
  auto it = applied_version_.find(db_oid);
  return it == applied_version_.end() ? 0 : it->second;
}

tablet::TabletPeerPtr LocalCatalogReplica::tablet_peer() const {
  std::lock_guard lock(mutex_);
  return tablet_peer_;
}

void LocalCatalogReplica::PublishAfterApply(
    HybridTime new_c, const std::unordered_map<uint32_t, uint64_t>& applied_versions) {
  std::lock_guard lock(mutex_);
  bool moved = false;
  std::string moved_versions;
  for (const auto& [db_oid, version] : applied_versions) {
    auto& current = applied_version_[db_oid];
    if (version > current) {
      moved = true;
      moved_versions += Format("$0{db $1: $2 -> $3}", moved_versions.empty() ? "" : ", ", db_oid,
                               current, version);
    }
    current = std::max(current, version);
  }
  if (!moved_versions.empty()) {
    VLOG(1) << LogPrefix() << "Applied catalog versions moved: " << moved_versions;
  }
  if (new_c.is_valid()) {
    // C is monotonic: a later response's safe time is at or above an earlier one's, but a leader
    // change on master can re-send an earlier one, and a read at a time the copy already covered
    // must not become uncoverable.
    auto existing = c_.load(std::memory_order_acquire);
    if (new_c.ToUint64() > existing) {
      moved = true;
      c_.store(new_c.ToUint64(), std::memory_order_release);
      safe_time_micros_->set_value(new_c.GetPhysicalValueMicros());
      // The one place C changes, so the one place worth watching to see the copy advance. A page
      // with no records still advances it, and the applier's own line only prints for pages that
      // carried records.
      VLOG(1) << LogPrefix() << "Safe time advanced "
              << LocalCatalogHybridTimeForLog(HybridTime(existing)) << " -> "
              << LocalCatalogHybridTimeForLog(new_c) << ", lag "
              << server_.Clock()->Now().PhysicalDiff(new_c) << ", state " << state();
    } else {
      VLOG(2) << LogPrefix() << "Ignoring a safe time at or below the one already held: "
              << LocalCatalogHybridTimeForLog(new_c) << " vs "
              << LocalCatalogHybridTimeForLog(HybridTime(existing));
    }
  }
  if (moved) {
    cond_.notify_all();
  }
}

Result<LocalCatalogPushOutcome> LocalCatalogReplica::ApplyPushedPages(
    std::shared_ptr<const PushSysCatalogChangesRequestPB> pages, CoarseTimePoint deadline,
    bool wait) {
  SCHECK(applier_, IllegalState, "The local catalog copy has no applier");
  return applier_->ApplyPushedPages(std::move(pages), deadline, wait);
}

Status LocalCatalogReplica::HandlePush(
    const PushSysCatalogChangesRequestPB& req, PushSysCatalogChangesResponsePB* resp,
    CoarseTimePoint deadline) {
  SCHECK_EQ(
      req.target_uuid(), server_.permanent_uuid(), InvalidArgument,
      "Pushed system catalog pages are meant for another tserver");
  IncPushesReceived();
  auto outcome = ApplyPushedPages(
      std::make_shared<const PushSysCatalogChangesRequestPB>(req), deadline, /* wait= */ true);
  if (!outcome.ok()) {
    IncPushesRefused();
    // Master learns the position anyway, so a retry starts from the right page.
    applied_op_id().ToPB(resp->mutable_applied_op_id());
    return outcome.status();
  }
  if (outcome->gap) {
    IncPushesRefused();
  } else {
    IncPushesApplied();
  }
  outcome->applied.ToPB(resp->mutable_applied_op_id());
  if (outcome->safe_time.is_valid()) {
    resp->set_safe_time(outcome->safe_time.ToUint64());
  }
  resp->set_gap(outcome->gap);
  return Status::OK();
}

void LocalCatalogReplica::RequestPush(HybridTime floor) {
  if (applier_) {
    applier_->RequestPush(floor);
  }
}

void LocalCatalogReplica::SetAppliedOpId(const OpId& op_id) {
  std::lock_guard lock(mutex_);
  applied_op_id_ = op_id;
}

OpId LocalCatalogReplica::applied_op_id() const {
  std::lock_guard lock(mutex_);
  return applied_op_id_;
}

Status LocalCatalogReplica::WaitForAppliedVersion(
    uint32_t db_oid, uint64_t version, CoarseTimePoint deadline) {
  UniqueLock lock(mutex_);
  for (;;) {
    auto it = applied_version_.find(db_oid);
    if (it != applied_version_.end() && it->second >= version) {
      return Status::OK();
    }
    if (shutdown_.load(std::memory_order_acquire)) {
      return STATUS(ShutdownInProgress, "Local catalog replica is shutting down");
    }
    if (applier_) {
      applier_->RequestPush(server_.Clock()->Now());
    }
    if (cond_.wait_until(GetLockForCondition(lock), deadline) == std::cv_status::timeout) {
      return STATUS_FORMAT(
          TimedOut,
          "Local catalog copy did not reach catalog version $0 of database $1; it is at $2",
          version, db_oid,
          applied_version_.contains(db_oid) ? applied_version_[db_oid] : 0);
    }
  }
}

Status LocalCatalogReplica::WaitForSafeTime(HybridTime target, CoarseTimePoint deadline) {
  UniqueLock lock(mutex_);
  for (;;) {
    if (HybridTime(c_.load(std::memory_order_acquire)) >= target) {
      return Status::OK();
    }
    if (shutdown_.load(std::memory_order_acquire)) {
      return STATUS(ShutdownInProgress, "Local catalog replica is shutting down");
    }
    if (applier_) {
      applier_->RequestPush(target);
    }
    if (cond_.wait_until(GetLockForCondition(lock), deadline) == std::cv_status::timeout) {
      return STATUS_FORMAT(
          TimedOut, "Local catalog copy did not reach hybrid time $0; it is complete up to $1",
          target, HybridTime(c_.load(std::memory_order_acquire)));
    }
  }
}

bool LocalCatalogReplica::HoldsCatalogStateAt(HybridTime target) {
  if (!target.is_valid() || safe_time() >= target) {
    return true;
  }
  applier_->RequestPush(target);
  return false;
}

void LocalCatalogReplica::RequestReseed() {
  reseed_requested_.store(true, std::memory_order_release);
  std::lock_guard lock(mutex_);
  cond_.notify_all();
}

void LocalCatalogReplica::OnLeaseLost() {
  // Until master grants a new lease there is no hybrid time the copy could reach that would
  // prove the copy holds everything master committed during the gap, so the floor is set to a
  // time C can never reach. OnLeaseGained replaces it with the real bound.
  lease_gap_floor_.store(HybridTime::kMax.ToUint64(), std::memory_order_release);
  if (state() != LocalCatalogServingState::kDisabledLease) {
    serving_disabled_lease_->Increment();
    SetState(LocalCatalogServingState::kDisabledLease);
  }
}

void LocalCatalogReplica::OnLeaseGained(HybridTime lease_grant_time) {
  if (lease_grant_time.is_valid()) {
    lease_gap_floor_.store(lease_grant_time.ToUint64(), std::memory_order_release);
  }
  if (state() == LocalCatalogServingState::kServing && lease_grant_time.is_valid() &&
      safe_time() < lease_grant_time) {
    // A new epoch can arrive before this tserver noticed the old lease run out, and master may
    // have completed DDLs without it meanwhile. The copy proves it holds them only once C passes
    // the new bound.
    serving_disabled_lease_->Increment();
    SetState(LocalCatalogServingState::kDisabledLease);
  }
  if (state() != LocalCatalogServingState::kServing) {
    // Serving resumes only once the copy has applied pages whose safe time is at or after the
    // grant, so that every version master published while the lease was gone is present here.
    // OnSafeTimePublished performs that check and moves the state to kServing.
    if (applier_) {
      applier_->RequestPush(lease_grant_time);
    }
  }
}

Status LocalCatalogReplica::Start() {
  return applier_->Start();
}

void LocalCatalogReplica::StartShutdown() {
  shutdown_.store(true, std::memory_order_release);
  std::lock_guard lock(mutex_);
  cond_.notify_all();
}

void LocalCatalogReplica::CompleteShutdown() {
  if (applier_) {
    applier_->Shutdown();
  }
  tablet::TabletPeerPtr peer;
  {
    std::lock_guard lock(mutex_);
    peer.swap(tablet_peer_);
  }
  if (peer) {
    // Nothing else holds this peer, so nothing else can have started its shutdown.
    if (peer->StartShutdown(tablet::DisableFlushOnShutdown::kFalse, tablet::AbortOps::kTrue)) {
      peer->CompleteShutdown();
    } else {
      LOG(DFATAL) << "The local catalog copy's tablet peer was already shutting down";
    }
  }
}

bool LocalCatalogReplica::TakeReseedRequest() {
  return reseed_requested_.exchange(false, std::memory_order_acq_rel);
}

void LocalCatalogReplica::SetTabletPeer(const tablet::TabletPeerPtr& peer) {
  std::lock_guard lock(mutex_);
  tablet_peer_ = peer;
}

void LocalCatalogReplica::OnReseedStarted() {
  reseeds_->Increment();
  c_.store(0, std::memory_order_release);
  safe_time_micros_->set_value(0);
  std::lock_guard lock(mutex_);
  applied_version_.clear();
}

bool LocalCatalogReplica::ShouldHoldPgStart() {
  if (FLAGS_local_catalog_serve_wait_timeout_ms == 0 ||
      shutdown_.load(std::memory_order_acquire)) {
    return false;
  }
  if (tablet_peer()) {
    return true;
  }
  auto reusable = server_.tablet_manager()->CanReuseLocalCatalogTablet(kLocalCatalogTabletId);
  return reusable.ok() && *reusable;
}

void LocalCatalogReplica::NotePublishedVersions(const DBCatalogVersionDataPB& versions) {
  std::lock_guard lock(mutex_);
  if (state() == LocalCatalogServingState::kServing) {
    published_while_not_serving_.clear();
    return;
  }
  std::unordered_map<uint32_t, uint64_t> reported;
  for (const auto& entry : versions.db_catalog_versions()) {
    reported[entry.db_oid()] = entry.current_version();
  }
  for (auto it = published_while_not_serving_.begin();
       it != published_while_not_serving_.end();) {
    it = reported.contains(it->first) ? std::next(it) : published_while_not_serving_.erase(it);
  }
  for (const auto& [db_oid, version] : reported) {
    auto& recorded = published_while_not_serving_[db_oid];
    recorded = std::max(recorded, version);
  }
}

void LocalCatalogReplica::OnSafeTimePublished(HybridTime c) {
  const auto floor = HybridTime(lease_gap_floor_.load(std::memory_order_acquire));
  const auto current = state();
  if (current == LocalCatalogServingState::kServing) {
    return;
  }
  // kDisabledLease: master may have completed DDLs while the lease was gone, without this tserver
  // acknowledging their lock releases. Only a safe time at or after the new lease grant proves the
  // copy holds all of them. kBootstrapping, kReseeding: the first certified safe time, past the
  // lease bound when one is set.
  if (floor.is_valid() && c < floor) {
    if (floor != HybridTime::kMax && applier_) {
      applier_->RequestPush(floor);
    }
    return;
  }
  {
    // Versions the tserver published to its backends while the copy was not serving were not
    // gated on it, so the copy must hold every one of them before a backend may read it.
    std::lock_guard lock(mutex_);
    for (const auto& [db_oid, version] : published_while_not_serving_) {
      auto it = applied_version_.find(db_oid);
      const auto applied = it == applied_version_.end() ? 0 : it->second;
      if (applied < version) {
        VLOG_WITH_PREFIX(1) << "Not serving yet at C " << LocalCatalogHybridTimeForLog(c)
                            << ": database " << db_oid << " was published at version " << version
                            << " while the copy holds " << applied;
        // C has to move again for this to be re-checked, and nothing else asks master to move it.
        if (applier_) {
          applier_->RequestPush(server_.Clock()->Now());
        }
        return;
      }
    }
    published_while_not_serving_.clear();
  }
  lease_gap_floor_.store(0, std::memory_order_release);
  SetState(LocalCatalogServingState::kServing);
}

void LocalCatalogReplica::IncApplyFailures() { apply_failures_->Increment(); }
void LocalCatalogReplica::IncReadsServed() { reads_served_->Increment(); }

void LocalCatalogReplica::IncPushesReceived() { pushes_received_->Increment(); }

void LocalCatalogReplica::IncFloorRequestsSent() { floor_requests_sent_->Increment(); }

void LocalCatalogReplica::IncPushesApplied() { pushes_applied_->Increment(); }

void LocalCatalogReplica::IncPushesRefused() { pushes_refused_->Increment(); }
void LocalCatalogReplica::IncReadsWaitedForVersion() { reads_waited_for_version_->Increment(); }
void LocalCatalogReplica::IncReadsWaitedForOwnWrites() {
  reads_waited_for_own_writes_->Increment();
}
void LocalCatalogReplica::IncReadsToMasterInDdl() { reads_to_master_in_ddl_->Increment(); }
void LocalCatalogReplica::IncReadsToMasterSerializable() {
  reads_to_master_serializable_->Increment();
}
void LocalCatalogReplica::IncReadsToMasterNotServing() {
  reads_to_master_not_serving_->Increment();
}
void LocalCatalogReplica::RecordApplyLatency(MonoDelta delta) {
  apply_latency_->Increment(delta.ToMicroseconds());
}
void LocalCatalogReplica::RecordGateWait(MonoDelta delta) {
  gate_wait_->Increment(delta.ToMicroseconds());
}
void LocalCatalogReplica::RecordLag(MonoDelta delta) {
  lag_->Increment(std::max<int64_t>(delta.ToMicroseconds(), 0));
}

}  // namespace yb::tserver
