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

#include "yb/master/sys_catalog_pusher.h"

#include <algorithm>

#include "yb/cdc/cdc_error.h"
#include "yb/cdc/cdc_producer.h"
#include "yb/cdc/xrepl_stream_metadata.h"

#include "yb/consensus/consensus.messages.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/object_lock_info_manager.h"
#include "yb/master/sys_catalog_constants.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"

#include "yb/rpc/rpc_context.h"
#include "yb/rpc/rpc_controller.h"

#include "yb/server/clock.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_participant.h"

#include "yb/tserver/backup.pb.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/net/net_util.h"
#include "yb/util/string_util.h"
#include "yb/util/metrics.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/thread.h"
#include "yb/util/unique_lock.h"

using namespace std::literals;

DEFINE_RUNTIME_uint32(local_catalog_push_interval_ms, 300000,
    "Interval of master's timer push cycle for the tserver-local catalog copies. The timer cycle "
    "carries catalog writes made outside DDL, such as master's own; DDLs are pushed by their lock "
    "release and waits by floor requests. A tick finds nothing to do and costs nothing when the "
    "system catalog WAL has not moved and every copy is at its end.");

DEFINE_RUNTIME_uint32(local_catalog_push_min_cycle_interval_ms, 50,
    "Minimum interval between push cycles that master runs on its own, for floor requests and its "
    "timer. Requests that arrive within it share one cycle. Each cycle resolves the intents of "
    "every open catalog transaction, which costs a status round trip to their coordinators. Lock "
    "releases run their cycles regardless.");

DEFINE_RUNTIME_uint32(local_catalog_push_page_bytes, 1024 * 1024,
    "Size at which master seals a page of system catalog change records.");

DEFINE_RUNTIME_uint32(local_catalog_push_cache_mb, 64,
    "Bound on master's cache of serialized system catalog change pages. The oldest page is "
    "dropped first; a copy below the cache is sent pages built from the WAL.");

DEFINE_RUNTIME_uint32(local_catalog_push_max_in_flight_rpcs, 64,
    "Bound on push RPCs to tserver-local catalog copies outstanding at once, across all copies.");

DEFINE_RUNTIME_uint32(local_catalog_push_max_in_flight_bytes, 64 * 1024 * 1024,
    "Bound on bytes of system catalog change pages outstanding at once, across all copies.");

DEFINE_RUNTIME_uint32(local_catalog_push_release_reserved_rpcs, 16,
    "In-flight push RPCs that only copies a lock release is waiting on may use, so that a release "
    "is not queued behind a bulk catch-up of other copies.");

DEFINE_RUNTIME_uint32(local_catalog_release_inline_max_bytes, 4 * 1024 * 1024,
    "The most a lock release carries inline in system catalog change pages for one tserver. A "
    "tserver that needs more is pushed in pages, and its release waits at its gate meanwhile.");

DEFINE_RUNTIME_uint32(local_catalog_push_unknown_position_lookback_ops, 1000,
    "How many system catalog WAL operations below the head master's page cache starts with after "
    "master becomes leader. A copy whose position master does not know yet is pushed from the "
    "start of the cache and drops what it already holds.");

DEFINE_RUNTIME_uint32(local_catalog_push_rpc_timeout_ms, 30000,
    "Timeout of one push of system catalog change pages to a tserver.");

DEFINE_RUNTIME_uint32(sys_catalog_change_stream_idle_timeout_sec, 3600,
    "How long master keeps the position of a tserver's local catalog copy after that tserver last "
    "had a live YSQL lease and last reported. A tserver that is removed from the cluster would "
    "otherwise leave its entry behind for the life of the master process.");

DEFINE_test_flag(string, local_catalog_push_extra_targets, "",
    "Comma-separated host:port=N entries. Master treats N more push targets at each address as "
    "tservers with a live YSQL lease, with uuids host:port#0 to host:port#N-1. For load tests "
    "that stand in for many tservers with receivers in the test process.");

DEFINE_test_flag(string, local_catalog_push_block_target, "",
    "Master sends no push RPC to the tserver with this permanent uuid, and carries no pages to it "
    "on lock releases.");

METRIC_DEFINE_counter(server, local_catalog_push_cycles,
    "Local catalog push cycles", yb::MetricUnit::kOperations,
    "Number of push cycles master ran: each certified a safe time and built pages up to it.");

METRIC_DEFINE_counter(server, local_catalog_push_release_requests,
    "Local catalog push cycles requested by lock releases", yb::MetricUnit::kRequests,
    "Number of lock releases that asked for a push cycle. Releases that overlap share a cycle, so "
    "local_catalog_push_cycles can be lower.");

METRIC_DEFINE_counter(server, local_catalog_push_rpcs_sent,
    "Local catalog push RPCs sent", yb::MetricUnit::kRequests,
    "Number of push RPCs master sent to tserver-local catalog copies.");

METRIC_DEFINE_counter(server, local_catalog_push_bytes,
    "Local catalog push bytes", yb::MetricUnit::kBytes,
    "Bytes of system catalog change pages master sent in push RPCs.");

METRIC_DEFINE_counter(server, local_catalog_push_pages_built,
    "Local catalog pages built", yb::MetricUnit::kUnits,
    "Number of pages of system catalog change records master built into its cache.");

METRIC_DEFINE_counter(server, local_catalog_push_transient_pages_built,
    "Local catalog transient pages built", yb::MetricUnit::kUnits,
    "Number of pages master built from the WAL below its cache for copies that were behind it.");

METRIC_DEFINE_counter(server, local_catalog_floor_requests_received,
    "Local catalog floor requests received", yb::MetricUnit::kRequests,
    "Number of requests from tservers to push their local catalog copy up to a hybrid time.");

METRIC_DEFINE_counter(server, local_catalog_release_inline_pages,
    "Local catalog pages carried inline by lock releases", yb::MetricUnit::kUnits,
    "Number of pages lock releases carried inline to tservers.");

METRIC_DEFINE_counter(server, local_catalog_release_inline_skipped,
    "Local catalog lock releases that carried no pages", yb::MetricUnit::kRequests,
    "Number of release targets whose pages were not in the cache or exceeded "
    "local_catalog_release_inline_max_bytes, and were pushed in pages instead.");

METRIC_DEFINE_counter(server, local_catalog_push_failures,
    "Local catalog push failures", yb::MetricUnit::kRequests,
    "Number of push RPCs that failed and were retried with backoff.");

METRIC_DEFINE_gauge_uint64(server, local_catalog_push_in_flight_rpcs,
    "Local catalog push RPCs in flight", yb::MetricUnit::kRequests,
    "Push RPCs to tserver-local catalog copies outstanding now.");

METRIC_DEFINE_gauge_uint64(server, local_catalog_push_in_flight_rpcs_max,
    "Local catalog push RPCs in flight, maximum", yb::MetricUnit::kRequests,
    "The most push RPCs that were outstanding at once since master started.");

METRIC_DEFINE_gauge_uint64(server, local_catalog_push_in_flight_bytes_max,
    "Local catalog push bytes in flight, maximum", yb::MetricUnit::kBytes,
    "The most push bytes that were outstanding at once since master started.");

METRIC_DEFINE_gauge_uint64(server, local_catalog_push_cached_bytes,
    "Local catalog page cache bytes", yb::MetricUnit::kBytes,
    "Bytes of serialized system catalog change pages in master's cache.");

METRIC_DEFINE_event_stats(server, local_catalog_push_cycle_us,
    "Local catalog push cycle time", yb::MetricUnit::kMicroseconds,
    "Time one push cycle took: intent resolution plus building the new pages.");

namespace yb::master {

namespace {

constexpr auto kSchedulerIdleWait = 1s;
constexpr auto kMaxBackoff = 5s;
constexpr auto kMinBackoff = 100ms;
constexpr auto kCommitWaitStep = 1ms;
constexpr size_t kMaxTransientPages = 64;

OpId PbOpId(const OpIdPB& pb) { return OpId::FromPB(pb); }

struct ExtraTarget {
  std::string uuid;
  HostPort host_port;
};

std::vector<ExtraTarget> ParseExtraTargets(const std::string& spec) {
  std::vector<ExtraTarget> result;
  for (const auto& entry : StringSplit(spec, ',')) {
    const auto eq = entry.find('=');
    if (entry.empty() || eq == std::string::npos) {
      continue;
    }
    auto host_port = HostPort::FromString(entry.substr(0, eq), 0);
    if (!host_port.ok()) {
      LOG(WARNING) << "Ignoring extra push target " << entry << ": " << host_port.status();
      continue;
    }
    const auto count = std::stoi(entry.substr(eq + 1));
    for (int i = 0; i < count; ++i) {
      result.push_back(ExtraTarget{
          .uuid = Format("$0#$1", entry.substr(0, eq), i), .host_port = *host_port});
    }
  }
  return result;
}

}  // namespace

struct SysCatalogPusher::PushCall {
  std::string uuid;
  std::shared_ptr<tserver::TabletServerServiceProxy> proxy;
  tserver::PushSysCatalogChangesRequestPB req;
  tserver::PushSysCatalogChangesResponsePB resp;
  rpc::RpcController controller;
  size_t bytes = 0;
  OpId end;
  bool reseed = false;
};

SysCatalogPusher::SysCatalogPusher(
    Master& master, GetTabletPeerFunc get_tablet_peer, const MemTrackerPtr& parent_mem_tracker,
    const scoped_refptr<MetricEntity>& metric_entity)
    : master_(master),
      get_tablet_peer_(std::move(get_tablet_peer)),
      mem_tracker_(MemTracker::FindOrCreateTracker("SysCatalogPusher", parent_mem_tracker)),
      stream_metadata_(std::make_unique<cdc::StreamMetadata>()) {
  stream_metadata_->InitForSysCatalogChangeStream(xrepl::StreamId::GenerateRandom());
  cycles_ = METRIC_local_catalog_push_cycles.Instantiate(metric_entity);
  release_requests_ = METRIC_local_catalog_push_release_requests.Instantiate(metric_entity);
  rpcs_sent_ = METRIC_local_catalog_push_rpcs_sent.Instantiate(metric_entity);
  bytes_sent_ = METRIC_local_catalog_push_bytes.Instantiate(metric_entity);
  pages_built_ = METRIC_local_catalog_push_pages_built.Instantiate(metric_entity);
  transient_pages_built_ =
      METRIC_local_catalog_push_transient_pages_built.Instantiate(metric_entity);
  floor_requests_received_ =
      METRIC_local_catalog_floor_requests_received.Instantiate(metric_entity);
  release_inline_pages_ = METRIC_local_catalog_release_inline_pages.Instantiate(metric_entity);
  release_inline_skipped_ = METRIC_local_catalog_release_inline_skipped.Instantiate(metric_entity);
  push_failures_ = METRIC_local_catalog_push_failures.Instantiate(metric_entity);
  in_flight_rpcs_gauge_ = METRIC_local_catalog_push_in_flight_rpcs.Instantiate(metric_entity, 0);
  in_flight_rpcs_max_ = METRIC_local_catalog_push_in_flight_rpcs_max.Instantiate(metric_entity, 0);
  in_flight_bytes_max_ =
      METRIC_local_catalog_push_in_flight_bytes_max.Instantiate(metric_entity, 0);
  cached_bytes_gauge_ = METRIC_local_catalog_push_cached_bytes.Instantiate(metric_entity, 0);
  cycle_us_ = METRIC_local_catalog_push_cycle_us.Instantiate(metric_entity);
}

SysCatalogPusher::~SysCatalogPusher() {
  Shutdown();
}

Status SysCatalogPusher::Start() {
  {
    std::lock_guard lock(mutex_);
    next_timer_cycle_ = MonoTime::Now();
  }
  return Thread::Create(
      "master", "sys_catalog_pusher", &SysCatalogPusher::SchedulerLoop, this, &thread_);
}

void SysCatalogPusher::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
  }
  cond_.notify_all();
  cycle_cond_.notify_all();
  if (thread_) {
    WARN_NOT_OK(ThreadJoiner(thread_.get()).Join(), "Failed to join the system catalog pusher");
  }
  std::lock_guard lock(mutex_);
  mem_tracker_->Release(cached_bytes_);
  cached_bytes_ = 0;
  pages_.clear();
  transient_.clear();
}

Result<SysCatalogPusher::BuildResult> SysCatalogPusher::BuildPages(
    const tablet::TabletPeerPtr& peer, const OpId& after, int64_t to_index, HybridTime safe_time,
    bool wait_for_commit, size_t max_bytes, CoarseTimePoint deadline) {
  auto consensus = VERIFY_RESULT(peer->GetRaftConsensus());
  const size_t page_bytes = std::max<size_t>(FLAGS_local_catalog_push_page_bytes, 1);

  BuildResult result;
  result.built_through = after;

  bool crossed_restore = false;
  const OpId empty_op_id;
  const auto stream_id = stream_metadata_->GetStreamId();
  const TabletId tablet_id = kSysCatalogTabletId;
  const CoarseTimePoint& context_deadline = deadline;
  cdc::GetChangesResponsePB scratch;
  cdc::XClusterGetChangesContext context = {
      .stream_id = stream_id,
      .tablet_id = tablet_id,
      .from_op_id = empty_op_id,
      .tablet_peer = peer,
      // The system catalog tablet never splits, so a SPLIT_OP can only be a corrupt or
      // misdirected record; refusing it fails the build rather than shipping it.
      .update_on_split_op_func =
          [](const consensus::ReplicateMsg&) -> Status {
            return STATUS(
                IllegalState, "System catalog tablet cannot be split; refusing to ship a SPLIT_OP");
          },
      // A restore of master's system catalog rewrites catalog rows directly into master's RocksDB
      // while master applies the restore operation, so those rows never become WAL records and no
      // page can carry them. The restore operation gets a page of its own, which tells a copy below
      // it to fetch master's tablet again.
      .update_on_snapshot_op_func =
          [&crossed_restore](const consensus::ReplicateMsg& msg) -> Result<bool> {
            if (msg.snapshot_request().operation() !=
                tserver::TabletSnapshotOpRequestPB::RESTORE_SYS_CATALOG) {
              return false;
            }
            crossed_restore = true;
            return true;
          },
      .mem_tracker = mem_tracker_,
      .deadline = context_deadline,
      .stream_metadata = stream_metadata_.get(),
      .msgs_holder = nullptr,
      .resp = &scratch,
      .have_more_messages = nullptr,
      .last_readable_opid_index = nullptr,
  };

  auto current = std::make_shared<Page>();
  current->from = after;
  current->end = after;
  std::string buffer;
  size_t total_bytes = 0;

  const auto seal = [&](bool restore) {
    current->records = std::make_shared<const std::string>(std::move(buffer));
    current->ends_at_restore = restore;
    total_bytes += current->bytes();
    const auto end = current->end;
    result.pages.push_back(std::move(current));
    current = std::make_shared<Page>();
    current->from = end;
    current->end = end;
    buffer = std::string();
  };

  OpId position = after;
  while (position.index < to_index) {
    if (max_bytes != 0 && total_bytes >= max_bytes) {
      break;
    }
    auto read = VERIFY_RESULT(consensus->ReadCommittedMessages(
        position, to_index, page_bytes, deadline));
    if (read.messages.empty()) {
      if (!wait_for_commit) {
        break;
      }
      // The certified index is majority-replicated, and the leader marks it committed right
      // after, so this wait is short.
      if (CoarseMonoClock::Now() >= deadline) {
        return STATUS_FORMAT(
            TimedOut, "System catalog WAL index $0 was not committed in time; committed up to $1",
            to_index, consensus->GetCommittedAndMajorityReplicatedIndex().first);
      }
      std::this_thread::sleep_for(kCommitWaitStep);
      continue;
    }
    if (position.term == 0 && read.preceding_op.index == position.index &&
        current->from.index == position.index) {
      // The start was given by index alone; the WAL says which term it belongs to.
      current->from = read.preceding_op;
      current->end = read.preceding_op;
    }
    for (const auto& msg_ptr : read.messages) {
      const auto& msg = *msg_ptr;
      const auto op_id = OpId::FromPB(msg.id());
      position = op_id;
      if (msg.op_type() == consensus::OperationType::WRITE_OP &&
          msg.write().has_external_hybrid_time()) {
        // The producer drops these; a page covers them without a record.
        current->end = op_id;
        continue;
      }
      crossed_restore = false;
      const bool ends_batch = VERIFY_RESULT(cdc::PopulateXClusterRecords(context, msg));
      if (crossed_restore) {
        scratch.Clear();
        if (current->end.index > current->from.index) {
          seal(/* restore= */ false);
        }
        current->end = op_id;
        seal(/* restore= */ true);
        continue;
      }
      current->num_records += scratch.records_size();
      if (scratch.records_size() > 0) {
        scratch.AppendToString(&buffer);
        scratch.Clear();
      }
      current->end = op_id;
      if (ends_batch) {
        current->ends_with_metadata = true;
        seal(/* restore= */ false);
      } else if (buffer.size() >= page_bytes) {
        seal(/* restore= */ false);
      }
    }
  }
  if (current->end.index > current->from.index) {
    seal(/* restore= */ false);
  }
  result.built_through = position;
  if (safe_time.is_valid() && position.index >= to_index && !result.pages.empty() &&
      result.pages.back()->end.index == to_index) {
    std::const_pointer_cast<Page>(result.pages.back())->safe_time = safe_time;
  }
  return result;
}

Result<std::vector<SysCatalogPusher::PagePtr>> SysCatalogPusher::TEST_BuildPages(
    const OpId& after, int64_t to_index, HybridTime safe_time, CoarseTimePoint deadline) {
  auto peer = VERIFY_RESULT(get_tablet_peer_());
  std::lock_guard build_lock(build_mutex_);
  auto result = VERIFY_RESULT(BuildPages(
      peer, after, to_index, safe_time, /* wait_for_commit= */ true, /* max_bytes= */ 0,
      deadline));
  return result.pages;
}

void SysCatalogPusher::ResetForTermUnlocked(int64_t term) {
  const bool had_term = term_ != 0;
  if (had_term) {
    LOG(INFO) << "System catalog leader term changed from " << term_ << " to " << term
              << "; dropping " << pages_.size() << " cached pages and every copy's position";
  }
  term_ = term;
  pages_.clear();
  transient_.clear();
  mem_tracker_->Release(cached_bytes_);
  cached_bytes_ = 0;
  cached_bytes_gauge_->set_value(0);
  last_certified_ = Certified();
  if (!had_term) {
    return;
  }
  for (auto& [uuid, t] : targets_) {
    t.acked = OpId();
    t.known = false;
    t.last_safe_time = HybridTime::kInvalid;
  }
}

void SysCatalogPusher::AppendPagesUnlocked(std::vector<PagePtr> pages) {
  for (auto& page : pages) {
    if (!pages_.empty() && page->from.index != pages_.back()->end.index) {
      // Only a cycle appends, under build_mutex_, from the cache's end; a mismatch means the cache
      // was reset meanwhile and these pages belong to the old state.
      LOG(DFATAL) << "Dropping a page from " << page->from << " that does not continue the cache "
                  << "ending at " << pages_.back()->end;
      return;
    }
    cached_bytes_ += page->bytes();
    mem_tracker_->Consume(page->bytes());
    pages_built_->Increment();
    pages_.push_back(std::move(page));
  }
  EvictUnlocked();
}

void SysCatalogPusher::EvictUnlocked() {
  const size_t limit = static_cast<size_t>(FLAGS_local_catalog_push_cache_mb) * 1024 * 1024;
  // The last page is kept regardless: it carries the latest safe time, and an empty cache has no
  // end position to append the next cycle's pages to.
  while (pages_.size() > 1 && cached_bytes_ > limit) {
    const auto bytes = pages_.front()->bytes();
    cached_bytes_ -= bytes;
    mem_tracker_->Release(bytes);
    pages_.pop_front();
  }
  cached_bytes_gauge_->set_value(cached_bytes_);
}

Result<SysCatalogPusher::Certified> SysCatalogPusher::RunCycle(
    HybridTime floor, CoarseTimePoint deadline) {
  UniqueLock lock(mutex_);
  const auto ticket = ++cycles_requested_;
  if (floor.is_valid()) {
    pending_floor_ = pending_floor_.is_valid() ? std::max(pending_floor_, floor) : floor;
  }
  while (cycles_completed_ < ticket) {
    if (shutdown_) {
      return STATUS(ShutdownInProgress, "System catalog pusher is shutting down");
    }
    if (!cycle_running_) {
      cycle_running_ = true;
      const auto covers = cycles_requested_;
      const auto cycle_floor = pending_floor_;
      pending_floor_ = HybridTime::kInvalid;
      last_cycle_start_ = MonoTime::Now();
      lock.unlock();
      auto result = DoCycle(cycle_floor, deadline);
      lock.lock();
      cycle_running_ = false;
      cycles_completed_ = covers;
      last_cycle_status_ = result.ok() ? Status::OK() : result.status();
      cycle_cond_.notify_all();
      cond_.notify_all();
      continue;
    }
    if (cycle_cond_.wait_until(GetLockForCondition(lock), deadline) == std::cv_status::timeout) {
      return STATUS(TimedOut, "Timed out waiting for a system catalog push cycle");
    }
  }
  RETURN_NOT_OK(last_cycle_status_);
  return last_certified_;
}

Result<SysCatalogPusher::Certified> SysCatalogPusher::DoCycle(
    HybridTime floor, CoarseTimePoint deadline) {
  const auto start = MonoTime::Now();
  auto peer = VERIFY_RESULT(get_tablet_peer_());
  SCHECK(peer, NotFound, "System catalog tablet not found");
  auto consensus = VERIFY_RESULT(peer->GetRaftConsensus());
  const auto term = consensus->LeaderTerm();
  SCHECK_GT(term, 0, NotFound, "This master does not lead the system catalog tablet");

  // A floor never moves master's clock. A floor can be a tserver's MaxGlobalNow, its clock plus
  // the maximum skew; moving master's clock there and propagating it would let each such floor
  // ratchet the cluster's hybrid time further ahead of physical time. The scheduler runs cycles
  // until master's own clock has passed the floor instead.
  auto tablet = VERIFY_RESULT(peer->shared_tablet());
  const auto leader_safe_time = VERIFY_RESULT(peer->LeaderSafeTime());
  SCHECK(
      leader_safe_time.is_valid() && !leader_safe_time.is_special(), IllegalState,
      "The system catalog tablet's leader safe time is not valid");
  // Every catalog transaction that committed at or below the safe time is applied after this,
  // and one still running can no longer commit at or below it. Once per cycle, for every copy.
  if (auto* participant = tablet->transaction_participant()) {
    RETURN_NOT_OK(participant->ResolveIntents(leader_safe_time, deadline));
  }
  const auto certified_index = consensus->GetCommittedAndMajorityReplicatedIndex().second;

  std::lock_guard build_lock(build_mutex_);
  OpId start_after;
  {
    std::lock_guard lock(mutex_);
    if (term != term_) {
      ResetForTermUnlocked(term);
    }
    if (!pages_.empty()) {
      start_after = pages_.back()->end;
    } else {
      const auto lookback =
          static_cast<int64_t>(FLAGS_local_catalog_push_unknown_position_lookback_ops);
      start_after = OpId(0, std::max<int64_t>(certified_index - lookback, 0));
    }
  }

  BuildResult built;
  if (start_after.index < certified_index) {
    auto build = BuildPages(
        peer, start_after, certified_index, leader_safe_time, /* wait_for_commit= */ true,
        /* max_bytes= */ 0, deadline);
    if (!build.ok() && build.status().IsNotFound() && start_after.index > 0) {
      // The look-back reaches below the retained WAL; start at what is retained.
      build = BuildPages(
          peer, OpId(), certified_index, leader_safe_time, /* wait_for_commit= */ true,
          /* max_bytes= */ 0, deadline);
    }
    built = VERIFY_RESULT(std::move(build));
  } else {
    built.built_through = start_after;
  }

  std::lock_guard lock(mutex_);
  SCHECK_EQ(term, term_, IllegalState, "System catalog leader term changed during a push cycle");
  SCHECK_EQ(
      consensus->LeaderTerm(), term, IllegalState,
      "System catalog leader term changed during a push cycle");
  const bool had_pages = !built.pages.empty();
  AppendPagesUnlocked(std::move(built.pages));
  if (pages_.empty() || pages_.back()->end.index < certified_index) {
    // Nothing was read at all: an empty WAL range at the very start. The empty page stands for the
    // position, so the next cycle has an end to append to and the safe time has a carrier.
    auto page = std::make_shared<Page>();
    page->from = pages_.empty() ? built.built_through : pages_.back()->end;
    page->end = OpId(page->from.term, certified_index);
    if (page->end.index != page->from.index) {
      return STATUS_FORMAT(
          IllegalState, "Push cycle built pages through $0, below the certified index $1",
          pages_.empty() ? built.built_through : pages_.back()->end, certified_index);
    }
    page->records = std::make_shared<const std::string>();
    page->safe_time = leader_safe_time;
    AppendPagesUnlocked({std::move(page)});
  } else if (!had_pages || pages_.back()->safe_time < leader_safe_time) {
    // No WAL since the last cycle: the last page keeps its records and takes the new safe time,
    // which is valid for it because its end is the new certified index too.
    auto page = std::make_shared<Page>(*pages_.back());
    page->safe_time = leader_safe_time;
    pages_.back() = std::move(page);
  }
  last_certified_ = Certified{.safe_time = leader_safe_time, .index = certified_index};
  full_scan_ = true;
  wake_ = true;
  cycles_->Increment();
  cycle_us_->Increment((MonoTime::Now() - start).ToMicroseconds());
  VLOG(2) << "Push cycle certified " << leader_safe_time << " at index " << certified_index
          << "; cache holds " << pages_.size() << " pages, " << cached_bytes_ << " bytes";
  return last_certified_;
}

std::vector<SysCatalogPusher::PagePtr> SysCatalogPusher::CachedPagesAfterUnlocked(
    const OpId& after, size_t max_bytes) const {
  std::vector<PagePtr> result;
  if (pages_.empty() || after.index < pages_.front()->from.index) {
    return result;
  }
  auto it = std::upper_bound(
      pages_.begin(), pages_.end(), after.index,
      [](int64_t index, const PagePtr& page) { return index < page->end.index; });
  size_t bytes = 0;
  for (; it != pages_.end(); ++it) {
    if (!result.empty() && (bytes + (*it)->bytes() > max_bytes || (*it)->ends_at_restore)) {
      break;
    }
    result.push_back(*it);
    bytes += (*it)->bytes();
    if ((*it)->ends_at_restore) {
      break;
    }
  }
  return result;
}

SysCatalogPusher::Target& SysCatalogPusher::TargetUnlocked(const std::string& uuid) {
  auto& t = targets_[uuid];
  t.last_update = MonoTime::Now();
  return t;
}

void SysCatalogPusher::MergeHeartbeatUnlocked(Target* t, const TSDescriptorPtr& descriptor) {
  auto [op_id, reported_at] = descriptor->LocalCatalogAppliedOpId();
  if (op_id.empty() || !reported_at.Initialized() ||
      (t->heartbeat_merged_at.Initialized() && reported_at <= t->heartbeat_merged_at)) {
    return;
  }
  t->heartbeat_merged_at = reported_at;
  t->no_copy = false;
  if (!t->known || op_id.index > t->acked.index) {
    t->acked = op_id;
    t->known = true;
  }
}

void SysCatalogPusher::RequestPush(
    const std::string& uuid, const OpId& applied, HybridTime floor) {
  floor_requests_received_->Increment();
  {
    std::lock_guard lock(mutex_);
    auto& t = TargetUnlocked(uuid);
    t.no_copy = false;
    if (!applied.empty()) {
      t.acked = applied;
      t.known = true;
    }
    if (floor.is_valid() && (!t.floor.is_valid() || floor > t.floor)) {
      t.floor = floor;
    }
    // A request is the tserver's own word that it is waiting, so a backoff from earlier failures
    // no longer applies.
    t.next_attempt = MonoTime();
    VLOG(1) << "Floor request from " << uuid << " at " << applied << " for " << floor;
    full_scan_ = true;
    wake_ = true;
  }
  cond_.notify_all();
}

std::string SysCatalogPusher::SerializePush(
    const std::string& uuid, const std::vector<PagePtr>& pages) const {
  tserver::PushSysCatalogChangesRequestPB req;
  req.set_target_uuid(uuid);
  req.set_master_term(term_);
  for (const auto& page : pages) {
    auto* pb = req.add_pages();
    page->from.ToPB(pb->mutable_page_from());
    page->end.ToPB(pb->mutable_page_end());
    if (page->ends_at_restore) {
      pb->set_reseed_required(true);
      continue;
    }
    if (page->bytes() > 0) {
      pb->set_records(*page->records);
    }
    if (page->safe_time.is_valid()) {
      pb->set_safe_time(page->safe_time.ToUint64());
    }
  }
  return req.SerializeAsString();
}

SysCatalogPusher::ReleasePayloads SysCatalogPusher::PrepareRelease(
    const std::vector<std::string>& target_uuids, CoarseTimePoint deadline) {
  ReleasePayloads payloads;
  release_requests_->Increment();
  auto certified = RunCycle(HybridTime::kInvalid, deadline);
  if (!certified.ok()) {
    VLOG(1) << "Lock release carries no system catalog pages: " << certified.status();
    return payloads;
  }
  const auto inline_max = static_cast<size_t>(FLAGS_local_catalog_release_inline_max_bytes);
  {
    std::lock_guard lock(mutex_);
    if (pages_.empty()) {
      return payloads;
    }
    for (const auto& uuid : target_uuids) {
      if (PREDICT_FALSE(uuid == FLAGS_TEST_local_catalog_push_block_target)) {
        continue;
      }
      auto& t = TargetUnlocked(uuid);
      if (auto descriptor = master_.ts_manager()->LookupTSByUUID(uuid); descriptor.ok()) {
        MergeHeartbeatUnlocked(&t, *descriptor);
      }
      if (t.no_copy) {
        continue;
      }
      std::vector<PagePtr> pages;
      if (!t.known) {
        // Position unknown, as after a leader change: the scheduler pushes from the start of the
        // cache, and the copy's answer tells master where it is.
        t.release_priority = true;
        release_inline_skipped_->Increment();
        continue;
      } else if (t.acked.index >= pages_.back()->end.index) {
        if (t.last_safe_time.is_valid() && t.last_safe_time >= certified->safe_time) {
          continue;
        }
        pages.push_back(pages_.back());
      } else {
        pages = CachedPagesAfterUnlocked(t.acked, inline_max);
      }
      size_t bytes = 0;
      for (const auto& page : pages) {
        bytes += page->bytes();
      }
      const bool reaches_end =
          !pages.empty() && pages.back()->end.index >= pages_.back()->end.index;
      if (pages.empty() || !reaches_end || bytes > inline_max) {
        // Below the cache, too much for one release, or a restore in the way: the scheduler pushes
        // in pages, first to the targets a release waits on.
        t.release_priority = true;
        release_inline_skipped_->Increment();
        VLOG(1) << "Lock release carries no pages to " << uuid << " at " << t.acked
                << "; pushing in pages instead";
        continue;
      }
      ++t.releases_in_flight;
      release_inline_pages_->IncrementBy(pages.size());
      VLOG(1) << "Lock release carries " << pages.size() << " pages (" << bytes << " bytes) to "
              << uuid << " at " << t.acked << ", through " << pages.back()->end;
      payloads.emplace(uuid, SerializePush(uuid, pages));
    }
    full_scan_ = true;
    wake_ = true;
  }
  cond_.notify_all();
  return payloads;
}

void SysCatalogPusher::OnReleaseDone(
    const std::string& uuid, bool carried_pages, const tserver::ReleaseObjectLockResponsePB* resp) {
  {
    std::lock_guard lock(mutex_);
    auto it = targets_.find(uuid);
    if (it == targets_.end()) {
      return;
    }
    auto& t = it->second;
    if (carried_pages && t.releases_in_flight > 0) {
      --t.releases_in_flight;
    }
    if (resp && resp->has_local_catalog_applied_op_id()) {
      const auto applied = PbOpId(resp->local_catalog_applied_op_id());
      if (!t.known || applied.index > t.acked.index) {
        t.acked = applied;
        t.known = true;
      }
    }
    if (resp && resp->has_local_catalog_safe_time()) {
      const HybridTime safe_time(resp->local_catalog_safe_time());
      if (!t.last_safe_time.is_valid() || safe_time > t.last_safe_time) {
        t.last_safe_time = safe_time;
      }
    }
    if (!pages_.empty() && t.known && t.acked.index >= pages_.back()->end.index) {
      t.release_priority = false;
    }
    acked_.push_back(uuid);
    wake_ = true;
  }
  cond_.notify_all();
}

bool SysCatalogPusher::NeedsPagesUnlocked(const Target& t) const {
  if (pages_.empty()) {
    return false;
  }
  if (!t.known || t.acked.index < pages_.back()->end.index) {
    return true;
  }
  // At the end of the cache: only a newer safe time is worth a push.
  const auto& back = pages_.back();
  return back->safe_time.is_valid() &&
         (!t.last_safe_time.is_valid() || back->safe_time > t.last_safe_time);
}

bool SysCatalogPusher::NextPagesUnlocked(
    const Target& t, std::vector<PagePtr>* pages, OpId* transient_from) {
  const size_t page_bytes = std::max<size_t>(FLAGS_local_catalog_push_page_bytes, 1);
  pages->clear();
  if (!t.known) {
    *pages = CachedPagesAfterUnlocked(pages_.front()->from, page_bytes);
    return true;
  }
  if (t.acked.index >= pages_.back()->end.index) {
    pages->push_back(pages_.back());
    return true;
  }
  if (t.acked.index >= pages_.front()->from.index) {
    *pages = CachedPagesAfterUnlocked(t.acked, page_bytes);
    return true;
  }
  auto it = transient_.find(t.acked.index);
  if (it != transient_.end()) {
    pages->push_back(it->second);
    return true;
  }
  *transient_from = t.acked;
  return false;
}

Result<SysCatalogPusher::PagePtr> SysCatalogPusher::BuildTransientPage(
    const OpId& from, CoarseTimePoint deadline) {
  auto peer = VERIFY_RESULT(get_tablet_peer_());
  int64_t to_index;
  {
    std::lock_guard lock(mutex_);
    SCHECK(!pages_.empty(), IllegalState, "System catalog page cache is empty");
    to_index = pages_.front()->from.index;
  }
  if (from.index == 0) {
    // A copy with no position at all holds nothing master could continue; to the log reader index
    // 0 means its earliest entry, which is not where this copy is.
    auto page = std::make_shared<Page>();
    page->from = from;
    page->end = from;
    page->ends_at_restore = true;
    page->records = std::make_shared<const std::string>();
    return page;
  }
  std::lock_guard build_lock(build_mutex_);
  auto built = BuildPages(
      peer, from, to_index, HybridTime::kInvalid, /* wait_for_commit= */ false,
      std::max<size_t>(FLAGS_local_catalog_push_page_bytes, 1), deadline);
  if (!built.ok()) {
    if (cdc::CDCError::ValueFromStatus(built.status()) == cdc::CDCErrorPB::CHECKPOINT_TOO_OLD ||
        built.status().IsNotFound()) {
      // The WAL after this copy's position is gone: only a fresh copy of master's tablet can make
      // it current. A restore page is what tells it so.
      auto page = std::make_shared<Page>();
      page->from = from;
      page->end = from;
      page->ends_at_restore = true;
      page->records = std::make_shared<const std::string>();
      return page;
    }
    return built.status();
  }
  SCHECK(
      !built->pages.empty(), IllegalState,
      Format("No WAL could be read after $0 below the cache starting at $1", from, to_index));
  transient_pages_built_->Increment();
  return built->pages.front();
}

std::shared_ptr<SysCatalogPusher::PushCall> SysCatalogPusher::PrepareSend(
    const std::string& uuid, Target* t, const std::vector<PagePtr>& pages) {
  auto call = std::make_shared<PushCall>();
  call->uuid = uuid;
  call->proxy = t->proxy;
  call->req.set_target_uuid(uuid);
  call->req.set_master_term(term_);
  for (const auto& page : pages) {
    auto* pb = call->req.add_pages();
    page->from.ToPB(pb->mutable_page_from());
    page->end.ToPB(pb->mutable_page_end());
    if (page->ends_at_restore) {
      pb->set_reseed_required(true);
      call->reseed = true;
      break;
    }
    if (page->bytes() > 0) {
      pb->set_records(*page->records);
    }
    if (page->safe_time.is_valid()) {
      pb->set_safe_time(page->safe_time.ToUint64());
    }
    call->bytes += page->bytes();
    call->end = page->end;
  }
  call->controller.set_timeout(
      MonoDelta::FromMilliseconds(FLAGS_local_catalog_push_rpc_timeout_ms));
  t->in_flight = true;
  ++in_flight_rpcs_;
  in_flight_bytes_ += call->bytes;
  in_flight_rpcs_gauge_->set_value(in_flight_rpcs_);
  if (in_flight_rpcs_ > in_flight_rpcs_max_->value()) {
    in_flight_rpcs_max_->set_value(in_flight_rpcs_);
  }
  if (in_flight_bytes_ > in_flight_bytes_max_->value()) {
    in_flight_bytes_max_->set_value(in_flight_bytes_);
  }
  rpcs_sent_->Increment();
  bytes_sent_->IncrementBy(call->bytes);
  VLOG(2) << "Pushing " << pages.size() << " pages (" << call->bytes << " bytes) to " << uuid
          << " at " << t->acked << ", through " << pages.back()->end;
  return call;
}

void SysCatalogPusher::Send(const std::shared_ptr<PushCall>& call) {
  std::weak_ptr<SysCatalogPusher> weak_self = shared_from_this();
  call->proxy->PushSysCatalogChangesAsync(
      call->req, &call->resp, &call->controller, [weak_self, call] {
        if (auto self = weak_self.lock()) {
          self->OnPushDone(call->uuid, call);
        }
      });
}

void SysCatalogPusher::OnPushDone(const std::string& uuid, const std::shared_ptr<PushCall>& call) {
  {
    std::lock_guard lock(mutex_);
    --in_flight_rpcs_;
    in_flight_bytes_ -= call->bytes;
    in_flight_rpcs_gauge_->set_value(in_flight_rpcs_);
    auto it = targets_.find(uuid);
    if (it != targets_.end()) {
      auto& t = it->second;
      t.in_flight = false;
      t.last_update = MonoTime::Now();
      Status status = call->controller.status();
      if (status.ok() && call->resp.has_error()) {
        status = StatusFromPB(call->resp.error().status());
      }
      const auto acked_before = t.acked;
      const bool known_before = t.known;
      if (call->resp.has_applied_op_id()) {
        // The copy's own word on its position, after this push or where a gap was found.
        t.acked = PbOpId(call->resp.applied_op_id());
        t.known = true;
      }
      if (status.ok() && known_before &&
          (call->reseed || call->end.index > acked_before.index) &&
          t.acked.index <= acked_before.index) {
        // Pages chosen from this very position did not move the copy (a gap at the same position,
        // or a re-seed it has not started); sending them again at once would repeat the answer.
        status = STATUS_FORMAT(
            IllegalState, "Pages through $0 did not move the copy from $1", call->end,
            acked_before);
      }
      if (call->resp.has_safe_time()) {
        const HybridTime safe_time(call->resp.safe_time());
        if (!t.last_safe_time.is_valid() || safe_time > t.last_safe_time) {
          t.last_safe_time = safe_time;
        }
      }
      if (status.IsNotSupported()) {
        VLOG(1) << "Tserver " << uuid << " keeps no local catalog copy; not pushing to it until "
                << "its heartbeat reports a position";
        t.no_copy = true;
        t.release_priority = false;
        t.floor = HybridTime::kInvalid;
      } else if (!status.ok()) {
        push_failures_->Increment();
        ++t.consecutive_failures;
        const auto backoff = std::min<MonoDelta>(
            MonoDelta(kMinBackoff) * (1 << std::min(t.consecutive_failures - 1, 6)),
            MonoDelta(kMaxBackoff));
        t.next_attempt = MonoTime::Now() + backoff;
        VLOG(1) << "Push to " << uuid << " failed (" << t.consecutive_failures
                << " in a row), retrying in " << backoff << ": " << status;
      } else {
        t.consecutive_failures = 0;
        if (call->resp.gap()) {
          VLOG(1) << "Push to " << uuid << " found a gap; the copy is at " << t.acked;
        }
        if (t.floor.is_valid() && t.last_safe_time.is_valid() && t.last_safe_time >= t.floor) {
          t.floor = HybridTime::kInvalid;
        }
        if (!pages_.empty() && t.acked.index >= pages_.back()->end.index) {
          t.release_priority = false;
        }
      }
    }
    acked_.push_back(uuid);
    // A transient page nobody below the cache needs any more is dropped.
    for (auto tit = transient_.begin(); tit != transient_.end();) {
      bool needed = false;
      for (const auto& [_, target] : targets_) {
        if (target.known && target.acked.index == tit->first) {
          needed = true;
          break;
        }
      }
      tit = needed ? std::next(tit) : transient_.erase(tit);
    }
    wake_ = true;
  }
  cond_.notify_all();
}

void SysCatalogPusher::WakeScheduler() {
  {
    std::lock_guard lock(mutex_);
    wake_ = true;
  }
  cond_.notify_all();
}

bool SysCatalogPusher::EligibleUnlocked(const std::string& uuid, const Target& t, MonoTime now)
    const {
  if (!t.live || !t.proxy || t.no_copy ||
      PREDICT_FALSE(uuid == FLAGS_TEST_local_catalog_push_block_target)) {
    return false;
  }
  // A floor request while a release is in flight means the release's pages did not get the copy
  // there, and the copy is waiting at its gate for a push.
  if (t.in_flight || (t.releases_in_flight > 0 && !t.floor.is_valid())) {
    return false;
  }
  if (t.next_attempt.Initialized() && now < t.next_attempt) {
    return false;
  }
  return NeedsPagesUnlocked(t);
}

void SysCatalogPusher::RefreshTargetsUnlocked(const TSDescriptorVector& live) {
  const auto now = MonoTime::Now();
  for (auto& [uuid, t] : targets_) {
    t.live = false;
  }
  if (PREDICT_FALSE(!FLAGS_TEST_local_catalog_push_extra_targets.empty())) {
    if (extra_targets_spec_ != FLAGS_TEST_local_catalog_push_extra_targets) {
      extra_targets_spec_ = FLAGS_TEST_local_catalog_push_extra_targets;
      std::unordered_map<std::string, std::shared_ptr<tserver::TabletServerServiceProxy>> proxies;
      extra_targets_.clear();
      for (auto& extra : ParseExtraTargets(extra_targets_spec_)) {
        auto& proxy = proxies[extra.host_port.ToString()];
        if (!proxy) {
          proxy = std::make_shared<tserver::TabletServerServiceProxy>(
              &master_.proxy_cache(), extra.host_port);
        }
        extra_targets_.emplace_back(std::move(extra.uuid), proxy);
      }
    }
    for (const auto& [uuid, proxy] : extra_targets_) {
      auto& t = targets_[uuid];
      t.live = true;
      t.last_update = now;
      if (!t.proxy) {
        t.proxy = proxy;
      }
    }
  }
  for (const auto& descriptor : live) {
    auto& t = targets_[descriptor->permanent_uuid()];
    t.live = true;
    t.last_update = now;
    if (!t.proxy && !descriptor->GetProxy(&t.proxy).ok()) {
      t.proxy.reset();
    }
    MergeHeartbeatUnlocked(&t, descriptor);
  }
  // Entries of tservers that left the cluster go after the idle timeout.
  const auto idle_timeout =
      MonoDelta::FromSeconds(FLAGS_sys_catalog_change_stream_idle_timeout_sec);
  for (auto it = targets_.begin(); it != targets_.end();) {
    if (!it->second.live && !it->second.in_flight && it->second.releases_in_flight == 0 &&
        now - it->second.last_update > idle_timeout) {
      LOG(INFO) << "Dropping the local catalog position of tserver " << it->first
                << ", which has had no live lease for " << now - it->second.last_update;
      it = targets_.erase(it);
    } else {
      ++it;
    }
  }
}

void SysCatalogPusher::Drain() {
  bool full_scan;
  {
    std::lock_guard lock(mutex_);
    full_scan = full_scan_ || !last_full_scan_.Initialized() ||
                MonoTime::Now() - last_full_scan_ >= kSchedulerIdleWait;
    full_scan_ = false;
  }
  TSDescriptorVector live;
  if (full_scan) {
    auto* object_lock_info_manager = master_.catalog_manager_impl()->object_lock_info_manager();
    if (object_lock_info_manager) {
      live = object_lock_info_manager->GetAllTSDescriptorsWithALiveLease();
    }
  }

  for (int round = 0; round < 2; ++round) {
    std::vector<OpId> transient_needed;
    std::vector<std::shared_ptr<PushCall>> calls;
    {
      std::lock_guard lock(mutex_);
      if (pages_.empty()) {
        return;
      }
      auto peer = get_tablet_peer_();
      if (!peer.ok() || !*peer) {
        return;
      }
      auto consensus = (*peer)->GetRaftConsensus();
      if (!consensus.ok() || (*consensus)->LeaderTerm() != term_) {
        // Deposed: the next cycle, as leader again or never, resets the cache.
        return;
      }
      const auto now = MonoTime::Now();
      if (full_scan && round == 0) {
        last_full_scan_ = now;
        RefreshTargetsUnlocked(live);
        std::vector<std::pair<std::string, const Target*>> candidates;
        for (const auto& [uuid, t] : targets_) {
          if (EligibleUnlocked(uuid, t, now)) {
            candidates.emplace_back(uuid, &t);
          }
        }
        // Copies a request or a release waits on first, then the furthest behind first, so that
        // the transient pages built for stragglers are consumed and dropped early.
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
          const auto& ta = *a.second;
          const auto& tb = *b.second;
          const bool wa = ta.release_priority || ta.floor.is_valid();
          const bool wb = tb.release_priority || tb.floor.is_valid();
          if (wa != wb) {
            return wa;
          }
          return ta.acked.index < tb.acked.index;
        });
        waiting_.clear();
        for (const auto& [uuid, _] : candidates) {
          waiting_.push_back(uuid);
        }
      } else if (round == 0) {
        // A target whose push completed goes to the back of the queue; one a request or a
        // release waits on goes to the front.
        for (auto& uuid : acked_) {
          auto it = targets_.find(uuid);
          if (it == targets_.end() || !EligibleUnlocked(uuid, it->second, now)) {
            continue;
          }
          if (it->second.release_priority || it->second.floor.is_valid()) {
            waiting_.push_front(std::move(uuid));
          } else {
            waiting_.push_back(std::move(uuid));
          }
        }
      }
      acked_.clear();

      const size_t max_rpcs = std::max<uint32_t>(FLAGS_local_catalog_push_max_in_flight_rpcs, 1);
      const size_t reserved =
          std::min<size_t>(FLAGS_local_catalog_push_release_reserved_rpcs, max_rpcs - 1);
      const size_t max_bytes = FLAGS_local_catalog_push_max_in_flight_bytes;
      std::deque<std::string> deferred;
      while (!waiting_.empty()) {
        if (in_flight_rpcs_ >= max_rpcs || (in_flight_rpcs_ > 0 && in_flight_bytes_ >= max_bytes)) {
          break;
        }
        auto uuid = std::move(waiting_.front());
        waiting_.pop_front();
        auto it = targets_.find(uuid);
        if (it == targets_.end() || !EligibleUnlocked(uuid, it->second, now)) {
          continue;
        }
        auto& t = it->second;
        const bool priority = t.release_priority || t.floor.is_valid();
        if (!priority && in_flight_rpcs_ >= max_rpcs - reserved) {
          deferred.push_back(std::move(uuid));
          continue;
        }
        std::vector<PagePtr> pages;
        OpId transient_from;
        if (!NextPagesUnlocked(t, &pages, &transient_from)) {
          if (std::find(transient_needed.begin(), transient_needed.end(), transient_from) ==
              transient_needed.end()) {
            transient_needed.push_back(transient_from);
          }
          deferred.push_back(std::move(uuid));
          continue;
        }
        if (pages.empty()) {
          continue;
        }
        calls.push_back(PrepareSend(uuid, &t, pages));
      }
      for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
        waiting_.push_front(std::move(*it));
      }
    }
    for (const auto& call : calls) {
      Send(call);
    }
    if (transient_needed.empty()) {
      return;
    }
    // Built outside the lock; a target far behind does not hold up the others' pushes.
    const auto deadline = CoarseMonoClock::Now() +
                          MonoDelta::FromMilliseconds(FLAGS_local_catalog_push_rpc_timeout_ms);
    for (const auto& from : transient_needed) {
      auto page = BuildTransientPage(from, deadline);
      if (!page.ok()) {
        LOG(WARNING) << "Could not build a page after " << from << " for a copy below the cache: "
                     << page.status();
        continue;
      }
      std::lock_guard lock(mutex_);
      while (transient_.size() >= kMaxTransientPages) {
        transient_.erase(transient_.begin());
      }
      transient_[from.index] = std::move(*page);
    }
  }
}

void SysCatalogPusher::SchedulerLoop() {
  for (;;) {
    HybridTime floor;
    bool run_cycle = false;
    {
      UniqueLock lock(mutex_);
      if (shutdown_) {
        return;
      }
      if (!wake_) {
        auto wait_until = std::min(MonoTime::Now() + kSchedulerIdleWait, next_timer_cycle_);
        for (const auto& [uuid, t] : targets_) {
          if (t.next_attempt.Initialized() && t.live && !t.in_flight) {
            wait_until = std::min(wait_until, t.next_attempt);
          }
        }
        const auto wait = wait_until - MonoTime::Now();
        if (wait.ToNanoseconds() > 0) {
          cond_.wait_for(GetLockForCondition(lock), wait.ToSteadyDuration());
        }
        if (shutdown_) {
          return;
        }
      }
      wake_ = false;
      const auto now = MonoTime::Now();
      // A floor the last certified safe time does not cover needs a new cycle.
      for (const auto& [uuid, t] : targets_) {
        if (t.floor.is_valid() &&
            (!last_certified_.safe_time.is_valid() || t.floor > last_certified_.safe_time)) {
          floor = floor.is_valid() ? std::max(floor, t.floor) : t.floor;
        }
      }
      const auto min_interval =
          MonoDelta::FromMilliseconds(FLAGS_local_catalog_push_min_cycle_interval_ms);
      const bool throttled =
          last_cycle_start_.Initialized() && now - last_cycle_start_ < min_interval;
      if (floor.is_valid()) {
        if (throttled) {
          // Woken again when the interval is over, so the floor is not left waiting.
          next_timer_cycle_ = std::min(next_timer_cycle_, last_cycle_start_ + min_interval);
        } else {
          run_cycle = true;
        }
      }
      if (now >= next_timer_cycle_) {
        next_timer_cycle_ =
            now + MonoDelta::FromMilliseconds(std::max<uint32_t>(
                      FLAGS_local_catalog_push_interval_ms, 1));
        if (!run_cycle && !throttled) {
          // Nothing new in the WAL and nothing pending means nothing to do.
          auto peer = get_tablet_peer_();
          if (peer.ok() && *peer) {
            auto consensus = (*peer)->GetRaftConsensus();
            if (consensus.ok() && (*consensus)->LeaderTerm() > 0) {
              const auto committed = (*consensus)->GetCommittedAndMajorityReplicatedIndex().first;
              run_cycle = pages_.empty() || term_ != (*consensus)->LeaderTerm() ||
                          committed > pages_.back()->end.index;
            }
          }
        }
      }
    }
    if (run_cycle) {
      const auto deadline = CoarseMonoClock::Now() +
                            MonoDelta::FromMilliseconds(FLAGS_local_catalog_push_rpc_timeout_ms);
      auto result = RunCycle(floor, deadline);
      if (!result.ok()) {
        YB_LOG_EVERY_N_SECS(INFO, 10) << "System catalog push cycle failed: " << result.status();
      }
    }
    Drain();
  }
}

size_t SysCatalogPusher::TEST_NumCachedPages() const {
  std::lock_guard lock(mutex_);
  return pages_.size();
}

std::vector<SysCatalogPusher::PagePtr> SysCatalogPusher::TEST_CachedPages() const {
  std::lock_guard lock(mutex_);
  return std::vector<PagePtr>(pages_.begin(), pages_.end());
}

OpId SysCatalogPusher::TEST_TargetPosition(const std::string& uuid) const {
  std::lock_guard lock(mutex_);
  auto it = targets_.find(uuid);
  return it == targets_.end() ? OpId() : it->second.acked;
}

SysCatalogPusher::Certified SysCatalogPusher::TEST_LastCertified() const {
  std::lock_guard lock(mutex_);
  return last_certified_;
}

SysCatalogChangeService::SysCatalogChangeService(
    const scoped_refptr<MetricEntity>& metric_entity, std::shared_ptr<SysCatalogPusher> pusher)
    : cdc::SysCatalogChangeServiceIf(metric_entity), pusher_(std::move(pusher)) {}

void SysCatalogChangeService::RequestSysCatalogPush(
    const cdc::RequestSysCatalogPushRequestPB* req, cdc::RequestSysCatalogPushResponsePB* resp,
    rpc::RpcContext context) {
  if (req->requestor_uuid().empty()) {
    auto status = STATUS(InvalidArgument, "A push request must carry the tserver's uuid");
    StatusToPB(status, resp->mutable_error()->mutable_status());
    resp->mutable_error()->set_code(cdc::CDCErrorPB::INVALID_REQUEST);
  } else {
    pusher_->RequestPush(
        req->requestor_uuid(),
        req->has_applied_op_id() ? OpId::FromPB(req->applied_op_id()) : OpId(),
        req->has_floor() ? HybridTime(req->floor()) : HybridTime::kInvalid);
  }
  context.RespondSuccess();
}

}  // namespace yb::master
