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
//

#include "yb/master/master_ysql_lease.pb.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/scheduler.h"

#include "yb/tserver/pg_client_service.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_local_lock_manager.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver_fwd.h"
#include "yb/tserver/ysql_lease_manager.h"

#include "yb/server/clock.h"

#include "yb/tserver/local_catalog_replica.h"
#include "yb/tserver/ysql_lease_poller.h"

#include "yb/util/atomic.h"
#include "yb/util/debug-util.h"
#include "yb/util/locks.h"
#include "yb/util/mutex.h"
#include "yb/util/status_log.h"
#include "yb/util/thread.h"

using namespace std::literals;
using namespace std::placeholders;

DEFINE_test_flag(bool, enable_ysql_operation_lease_expiry_check, true,
    "Whether tservers should monitor their ysql op lease and kill their hosted pg "
    "sessions when it expires. Only available as a flag for tests.");

DEFINE_test_flag(bool, pause_ysql_lease_refresh_after_epoch_update, false,
    "Pause after publishing a new ysql lease epoch but before resetting the local "
    "lock manager.");
DEFINE_test_flag(uint64, delay_ysql_lease_expiry_pg_kill_ms, 0,
    "Delay between a tserver detecting YSQL lease expiry and killing hosted PG sessions.");

DECLARE_bool(enable_object_locking_for_table_locks);
DECLARE_int32(local_catalog_serve_wait_timeout_ms);
DECLARE_bool(enable_ysql);

namespace yb::tserver {

class YSQLLeaseManager::Impl {
 public:
  explicit Impl(TabletServer& server);

  Status ProcessLeaseUpdate(const master::RefreshYsqlLeaseInfoPB& lease_refresh_info)
      EXCLUDES(lease_toggle_lock_, lock_);

  Status StartYSQLLeaseRefresher();
  void StartTSLocalLockManager() EXCLUDES(lock_);

  void UpdateMasterAddresses(const server::MasterAddressesPtr& master_addresses);

  void Shutdown();

  std::future<Status> RelinquishLease(MonoDelta timeout) const;
  YSQLLeaseInfo GetYSQLLeaseInfo() EXCLUDES(lock_);


  TSLocalLockManagerPtr ts_local_lock_manager() const EXCLUDES(lock_);

 private:
  // Starts PG once the local catalog copy serves at the bound of the new lease epoch, or once
  // local_catalog_serve_wait_timeout_ms passed, whichever is first. Gives up when a newer epoch
  // arrived meanwhile, since that epoch restarts PG itself.
  void DeferredPgRestart(uint64_t epoch, TSLocalLockManagerPtr lock_manager)
      EXCLUDES(lock_);
  void RestartPGAndEnableLockState(const TSLocalLockManagerPtr& lock_manager);

  tserver::TSLocalLockManagerPtr ResetAndGetTSLocalLockManager() EXCLUDES(lock_);
  void StartTSLocalLockManagerUnlocked() REQUIRES(lock_);

  // ============================================================================
  // START: check_ysql_lease_ methods
  // These methods are run on the check_ysql_lease_ task.
  // They expire the lease when the lease TTL runs out.
  // ============================================================================
  void ScheduleCheckLeaseWithNoLiveLease();
  void ScheduleCheckLease(CoarseTimePoint next_check_time);
  void CheckLeaseStatus();
  std::optional<CoarseTimePoint> CheckLeaseStatusInner() EXCLUDES(lease_toggle_lock_, lock_);
  // ============================================================================
  // END: check_ysql_lease_ methods
  // ============================================================================

  // State is guarded by two locks, lease_toggle_lock_ and lock_.
  // Locking order is:
  //   lease_toggle_lock_, lock_
  // lease_toggle_lock_ is used to synchronize updates to lease liveness.
  // There are two tasks that change lease liveness:
  //   lease_client_ runs a thread which calls ProcessLeaseUpdate and can flip the lease to live.
  //   check_lease_ runs a task which calls CheckLeaseStatus and can flip the lease to expired.
  TabletServer& server_;
  mutable rw_spinlock lock_;
  tserver::TSLocalLockManagerPtr ts_local_lock_manager_ GUARDED_BY(lock_);
  YsqlLeaseClient lease_client_;
  rpc::ScheduledTaskTracker check_lease_;
  // Held while toggling the lease from expired to live and from live to expired.
  mutable Mutex lease_toggle_lock_;
  CoarseTimePoint lease_expiry_time_ GUARDED_BY(lease_toggle_lock_);
  bool lease_is_live_ GUARDED_BY(lock_){false};
  uint64_t lease_epoch_ GUARDED_BY(lock_){0};

  // The thread holding PG's start for the latest lease epoch. Only the lease refresher thread
  // replaces it.
  scoped_refptr<Thread> deferred_pg_restart_;
  std::atomic<bool> shutting_down_{false};
};

// ============================================================================
// Class YSQLLeaseManager::Impl, public methods
// ============================================================================

YSQLLeaseManager::Impl::Impl(TabletServer& server)
    : server_(server),
      lease_client_(server, std::bind(&Impl::ProcessLeaseUpdate, this, _1)),
      check_lease_("check_ysql_lease_liveness", &server_.messenger()->scheduler()) {
  ScheduleCheckLeaseWithNoLiveLease();
}

Status YSQLLeaseManager::Impl::ProcessLeaseUpdate(
    const master::RefreshYsqlLeaseInfoPB& lease_refresh_info) {
  VLOG(2) << __func__;
  CoarseTimePoint new_lease_expiry_time{
      std::chrono::milliseconds(lease_refresh_info.lease_expiry_time_ms())};
  if (new_lease_expiry_time < CoarseMonoClock::Now()) {
    // This function is passed the timestamp from before the RefreshYsqlLeaseRpc is sent.  So it
    // is possible the RPC takes longer than the lease TTL the master gave us, in which case
    // this tserver still does not have a live lease.
    return Status::OK();
  }
  MutexLock l(lease_toggle_lock_);
  // We hold lease_toggle_lock_ for function scope to sychronize with the lease checker.
  // We do not hold lock_ for function scope so that RPCs are not blocked for too long.
  bool restart_pg{false};
  {
    std::lock_guard lock(lock_);
    if (!lease_is_live_ && !lease_refresh_info.new_lease()) {
      // We should not keep using this lease if we thought it has expired. Ignore the master's
      // refresh. The master leader should give us a new lease epoch when we send the next request.
      return STATUS(
          IllegalState, "Master refreshed ysql lease but it has expired. Ignoring lease refresh.");
    }
    if (!lease_is_live_ || lease_refresh_info.new_lease() ||
        lease_epoch_ != lease_refresh_info.lease_epoch()) {
      LOG_IF(
          INFO, lease_refresh_info.new_lease() || lease_epoch_ != lease_refresh_info.lease_epoch())
          << Format(
                 "Received new lease epoch $0 from the master leader. Clearing all pg sessions.",
                 lease_refresh_info.lease_epoch());
      restart_pg = true;
      if (ts_local_lock_manager_ && ts_local_lock_manager_->IsBootstrapped()) {
        ts_local_lock_manager_->StartShutdown();
      }
    }
    lease_expiry_time_ = new_lease_expiry_time;
    lease_is_live_ = true;
    lease_epoch_ = lease_refresh_info.lease_epoch();
  }

  TEST_PAUSE_IF_FLAG(TEST_pause_ysql_lease_refresh_after_epoch_update);

  if (restart_pg) {
    // A new lease epoch means the previous lease had lapsed, and while it was gone master could
    // complete DDLs without this tserver acknowledging their lock releases, so versions advanced
    // with no gate here. Every such DDL committed before the lease was regranted, and
    // MaxGlobalNow is an upper bound on every hybrid time in the cluster right now, so once the
    // local catalog copy is complete up to it, it holds all of them. The copy stays out of the
    // read path until then.
    if (auto* local_catalog = server_.local_catalog_replica()) {
      local_catalog->OnLeaseGained(server_.clock()->MaxGlobalNow());
    }
  }

  // It is safer to end the pg-sessions after resetting the local lock manager.
  // This way, if a new session gets created it will also be reset. But that is better than
  // having it the other way around, and having an old-session that is not reset.
  auto lock_manager = ts_local_lock_manager();
  if (lease_refresh_info.new_lease() && lock_manager) {
    if (lock_manager->IsBootstrapped()) {
      // Reset the local lock manager to bootstrap from the given DDL lock entries.
      lock_manager = ResetAndGetTSLocalLockManager();
    }
    RETURN_NOT_OK(lock_manager->BootstrapDdlObjectLocks(lease_refresh_info.ddl_lock_entries()));
  }
  if (restart_pg) {
    auto* local_catalog = server_.local_catalog_replica();
    if (local_catalog && local_catalog->ShouldHoldPgStart()) {
      // The refresher thread has to keep refreshing the lease, so the wait runs elsewhere. A wait
      // for an older epoch notices the newer epoch and ends; it is joined before the new one
      // starts so that only one exists.
      if (deferred_pg_restart_) {
        WARN_NOT_OK(
            ThreadJoiner(deferred_pg_restart_.get()).Join(),
            "Failed to join the previous deferred PG restart");
        deferred_pg_restart_.reset();
      }
      const auto epoch = lease_refresh_info.lease_epoch();
      auto status = Thread::Create(
          "ysql_lease", "deferred_pg_restart",
          [this, epoch, lock_manager] { DeferredPgRestart(epoch, lock_manager); },
          &deferred_pg_restart_);
      if (status.ok()) {
        return Status::OK();
      }
      LOG(WARNING) << "Could not hold PG's start for the local catalog copy: " << status;
    }
    RestartPGAndEnableLockState(lock_manager);
  }
  return Status::OK();
}

void YSQLLeaseManager::Impl::RestartPGAndEnableLockState(
    const TSLocalLockManagerPtr& lock_manager) {
  WARN_NOT_OK(server_.RestartPG(), "Failed to restart PG postmaster.");
  if (lock_manager) {
    // Re-enable shared memory object lock state as it gets disabled as part of shutdown
    // of the old lock manager. Disabling shared object lock state during switch of lock
    // manager instances is necessary so as to prevent lock leaks.
    lock_manager->EnableSharedLockState();
  }
}

void YSQLLeaseManager::Impl::DeferredPgRestart(
    uint64_t epoch, TSLocalLockManagerPtr lock_manager) {
  auto* local_catalog = server_.local_catalog_replica();
  const auto timeout_ms = FLAGS_local_catalog_serve_wait_timeout_ms;
  const auto deadline = timeout_ms < 0
                            ? CoarseTimePoint::max()
                            : CoarseMonoClock::Now() + MonoDelta::FromMilliseconds(timeout_ms);
  const auto start = CoarseMonoClock::Now();
  LOG(INFO) << "Holding PG's start until the local catalog copy serves at the bound of lease epoch "
            << epoch;
  for (;;) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    {
      SharedLock lock(lock_);
      if (lease_epoch_ != epoch || !lease_is_live_) {
        LOG(INFO) << "Lease epoch " << epoch << " was replaced or lost before the local catalog "
                  << "copy served; not starting PG for it";
        return;
      }
    }
    // The copy's state, not IsServing(): a major version upgrade in progress keeps the copy out of
    // the read path without making PG's start wait for anything.
    if (local_catalog->state() == LocalCatalogServingState::kServing) {
      LOG(INFO) << "Local catalog copy serves after " << MonoDelta(CoarseMonoClock::Now() - start)
                << "; starting PG for lease epoch " << epoch;
      break;
    }
    if (CoarseMonoClock::Now() >= deadline) {
      LOG(WARNING) << "Local catalog copy did not serve within " << timeout_ms << " ms of lease "
                   << "epoch " << epoch << "; starting PG, whose catalog reads go to master "
                   << "until it does";
      break;
    }
    SleepFor(50ms);
  }
  RestartPGAndEnableLockState(lock_manager);
}

Status YSQLLeaseManager::Impl::StartYSQLLeaseRefresher() { return lease_client_.Start(); }

void YSQLLeaseManager::Impl::StartTSLocalLockManager() {
  std::lock_guard l(lock_);
  StartTSLocalLockManagerUnlocked();
}

void YSQLLeaseManager::Impl::UpdateMasterAddresses(
    const server::MasterAddressesPtr& master_addresses) {
  lease_client_.UpdateMasterAddresses(master_addresses);
}

void YSQLLeaseManager::Impl::Shutdown() {
  shutting_down_.store(true, std::memory_order_release);
  if (deferred_pg_restart_) {
    WARN_NOT_OK(
        ThreadJoiner(deferred_pg_restart_.get()).Join(),
        "Failed to join the deferred PG restart");
  }
  check_lease_.StartShutdown();
  check_lease_.CompleteShutdown();
  auto lock_manager = ts_local_lock_manager();
  if (lock_manager) {
    lock_manager->Shutdown();
  }
  lease_client_.Shutdown();
}

std::future<Status> YSQLLeaseManager::Impl::RelinquishLease(MonoDelta timeout) const {
  return lease_client_.RelinquishLease(timeout);
}

YSQLLeaseInfo YSQLLeaseManager::Impl::GetYSQLLeaseInfo() {
  SharedLock lock(lock_);
  YSQLLeaseInfo lease_info;
  lease_info.is_live = lease_is_live_;
  lease_info.lease_epoch = lease_is_live_ ? lease_epoch_ : 0;
  return lease_info;
}

TSLocalLockManagerPtr YSQLLeaseManager::Impl::ts_local_lock_manager() const {
  std::lock_guard l(lock_);
  return ts_local_lock_manager_;
}

// ============================================================================
// Class YSQLLeaseManager::Impl, private methods
// ============================================================================

tserver::TSLocalLockManagerPtr YSQLLeaseManager::Impl::ResetAndGetTSLocalLockManager() {
  if (auto old_lock_manager = ts_local_lock_manager(); old_lock_manager) {
    old_lock_manager->Shutdown();
  }
  std::lock_guard l(lock_);
  StartTSLocalLockManagerUnlocked();
  return ts_local_lock_manager_;
}

void YSQLLeaseManager::Impl::StartTSLocalLockManagerUnlocked() {
  if (server_.options().server_type != TabletServerOptions::kServerType ||
      !FLAGS_enable_object_locking_for_table_locks || !FLAGS_enable_ysql) {
    return;
  }

  ts_local_lock_manager_ = std::make_shared<tserver::TSLocalLockManager>(
      server_.Clock(), &server_ /* TabletServerIf* */, server_ /* RpcServerBase& */,
      server_.tablet_manager()->waiting_txn_pool(), server_.MetricEnt(),
      server_.object_lock_tracker(), server_.object_lock_shared_state_manager());
  ts_local_lock_manager_->Start(server_.tablet_manager()->waiting_txn_registry());
}

void YSQLLeaseManager::Impl::ScheduleCheckLeaseWithNoLiveLease() {
  ScheduleCheckLease(CoarseMonoClock::now() + 1s);
}

void YSQLLeaseManager::Impl::ScheduleCheckLease(CoarseTimePoint next_check_time) {
  check_lease_.Schedule(
      [this, next_check_time](const Status& status) {
        if (!status.ok()) {
          return;
        }
        if (CoarseMonoClock::now() < next_check_time) {
          ScheduleCheckLease(next_check_time);
          return;
        }
        CheckLeaseStatus();
      },
      next_check_time - CoarseMonoClock::now());
}

void YSQLLeaseManager::Impl::CheckLeaseStatus() {
  if (PREDICT_FALSE(!FLAGS_TEST_enable_ysql_operation_lease_expiry_check)) {
    ScheduleCheckLeaseWithNoLiveLease();
    return;
  }
  auto lease_expiry = CheckLeaseStatusInner();
  if (lease_expiry) {
    ScheduleCheckLease(*lease_expiry);
  } else {
    ScheduleCheckLeaseWithNoLiveLease();
  }
}

std::optional<CoarseTimePoint> YSQLLeaseManager::Impl::CheckLeaseStatusInner() {
  // We hold lease_toggle_lock_ for function scope to sychronize with the lease refresher.
  // We do not hold lock_ for function scope so that RPCs are not blocked for too long.
  MutexLock l(lease_toggle_lock_);
  {
    std::lock_guard lock(lock_);
    if (!lease_is_live_) {
      return {};
    }
    if (CoarseMonoClock::now() < lease_expiry_time_) {
      return lease_expiry_time_;
    }
    lease_is_live_ = false;
  }
  if (auto* local_catalog = server_.local_catalog_replica()) {
    local_catalog->OnLeaseLost();
  }
  // todo(zdrudi): make this a fatal?
  LOG(INFO) << "Lease has expired, killing pg sessions.";
  if (FLAGS_TEST_delay_ysql_lease_expiry_pg_kill_ms != 0) {
    LOG(INFO) << "TEST: sleeping for " << FLAGS_TEST_delay_ysql_lease_expiry_pg_kill_ms
              << "ms before killing pg sessions.";
    AtomicFlagSleepMs(&FLAGS_TEST_delay_ysql_lease_expiry_pg_kill_ms);
    LOG(INFO) << "TEST: done sleeping, proceeding to kill pg sessions.";
  }
  WARN_NOT_OK(server_.KillPg(), "Couldn't stop PG");
  return {};
}

// ============================================================================
// Class YSQLLeaseManager, public methods
// ============================================================================

YSQLLeaseManager::YSQLLeaseManager(TabletServer& server)
    : impl_(std::make_unique<Impl>(server)) {}

YSQLLeaseManager::~YSQLLeaseManager() = default;

YSQLLeaseInfo YSQLLeaseManager::GetYSQLLeaseInfo() const { return impl_->GetYSQLLeaseInfo(); }

Status YSQLLeaseManager::StartYSQLLeaseRefresher() { return impl_->StartYSQLLeaseRefresher(); }

void YSQLLeaseManager::StartTSLocalLockManager() {
  return impl_->StartTSLocalLockManager();
}

void YSQLLeaseManager::UpdateMasterAddresses(const server::MasterAddressesPtr& master_addresses) {
  return impl_->UpdateMasterAddresses(master_addresses);
}

void YSQLLeaseManager::Shutdown() {
  return impl_->Shutdown();
}

std::future<Status> YSQLLeaseManager::RelinquishLease(MonoDelta timeout) const {
  return impl_->RelinquishLease(timeout);
}

TSLocalLockManagerPtr YSQLLeaseManager::ts_local_lock_manager() const {
  return impl_->ts_local_lock_manager();
}

}  // namespace yb::tserver
