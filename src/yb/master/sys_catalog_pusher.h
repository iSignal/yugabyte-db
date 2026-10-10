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

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "yb/cdc/cdc_service.service.h"

#include "yb/common/hybrid_time.h"
#include "yb/common/opid.h"

#include "yb/gutil/thread_annotations.h"

#include "yb/master/master_fwd.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/tserver/tserver_fwd.h"

#include "yb/util/mem_tracker.h"
#include "yb/util/metrics_fwd.h"
#include "yb/util/monotime.h"
#include "yb/util/status_fwd.h"

namespace yb {

class Thread;

namespace cdc {
class StreamMetadata;
}  // namespace cdc

namespace tserver {
class PushSysCatalogChangesRequestPB;
class PushSysCatalogChangesResponsePB;
class ReleaseObjectLockResponsePB;
}  // namespace tserver

namespace master {

// Ships master's system catalog WAL to the tserver-local copies of the system catalog tablet.
//
// Master converts each range of WAL operations into change records once, keeps the serialized
// result as a page, and sends the same bytes to every tserver whose copy is below that range.
// Nobody pulls: master pushes pages after every DDL (inside the lock release when they fit), when a
// tserver asks for its copy to reach a hybrid time, and on a timer for catalog writes made outside
// DDL.
//
// A push cycle certifies one safe time S for the whole cluster: it resolves the intents of every
// open catalog transaction at the tablet's leader safe time S, then captures the
// majority-replicated index I_S. Every transaction that committed at or below S has its apply
// record at or before I_S, so the page that ends at I_S carries S, and a copy that has applied
// every page through it may publish S as its safe time C.
class SysCatalogPusher : public std::enable_shared_from_this<SysCatalogPusher> {
 public:
  using GetTabletPeerFunc = std::function<Result<tablet::TabletPeerPtr>()>;

  // Every WAL operation in (from.index, end.index], converted to change records. Immutable once
  // built, and shared by every RPC that carries it.
  struct Page {
    OpId from;
    OpId end;
    // Serialized cdc::GetChangesResponsePB holding only its records field. Concatenating the
    // records of consecutive pages is a valid serialization of their union.
    std::shared_ptr<const std::string> records;
    size_t num_records = 0;
    // Set only on a page that ends at a cycle's certified position.
    HybridTime safe_time;
    // The page is the restore operation of master's system catalog alone. A copy below it must be
    // fetched again from master.
    bool ends_at_restore = false;
    // The page's last record is a CHANGE_METADATA record.
    bool ends_with_metadata = false;

    size_t bytes() const { return records ? records->size() : 0; }
  };
  using PagePtr = std::shared_ptr<const Page>;

  // The safe time a cycle certified and the WAL index it is tied to.
  struct Certified {
    HybridTime safe_time;
    int64_t index = 0;
  };

  // Pages a lock release carries to each target inline, keyed by tserver uuid, as serialized
  // tserver::PushSysCatalogChangesRequestPB.
  using ReleasePayloads = std::unordered_map<std::string, std::string>;

  SysCatalogPusher(
      Master& master, GetTabletPeerFunc get_tablet_peer, const MemTrackerPtr& parent_mem_tracker,
      const scoped_refptr<MetricEntity>& metric_entity);
  ~SysCatalogPusher();

  Status Start();
  void Shutdown();

  // Runs a push cycle whose safe time is computed after this call started, and waits for it.
  // Calls that overlap share one cycle. The floor does not move master's clock: the cycle's safe
  // time may be below it, and the scheduler runs further cycles until it is not.
  Result<Certified> RunCycle(HybridTime floor, CoarseTimePoint deadline) EXCLUDES(mutex_);

  // A tserver asked for its copy to be complete up to floor. Records its position and wakes the
  // scheduler, which runs a cycle covering the floor and pushes the pages the copy lacks.
  void RequestPush(const std::string& uuid, const OpId& applied, HybridTime floor)
      EXCLUDES(mutex_);

  // Runs a cycle for a lock release that publishes catalog versions, and returns the pages each
  // target needs when they are in the cache and fit in local_catalog_release_inline_max_bytes.
  // Targets that need more are pushed in pages by the scheduler, with priority.
  ReleasePayloads PrepareRelease(
      const std::vector<std::string>& target_uuids, CoarseTimePoint deadline) EXCLUDES(mutex_);

  // The release to uuid has finished. carried_pages says whether it carried a payload from
  // PrepareRelease; resp is the target's last answer, when there is one.
  void OnReleaseDone(
      const std::string& uuid, bool carried_pages, const tserver::ReleaseObjectLockResponsePB* resp)
      EXCLUDES(mutex_);

  // For tests.
  size_t TEST_NumCachedPages() const EXCLUDES(mutex_);
  std::vector<PagePtr> TEST_CachedPages() const EXCLUDES(mutex_);
  Certified TEST_LastCertified() const EXCLUDES(mutex_);
  // The position master holds for the target, empty when unknown or when there is no entry.
  OpId TEST_TargetPosition(const std::string& uuid) const EXCLUDES(mutex_);
  // Builds pages for (after, to_index] directly from the WAL, without touching the cache.
  Result<std::vector<PagePtr>> TEST_BuildPages(
      const OpId& after, int64_t to_index, HybridTime safe_time, CoarseTimePoint deadline);

 private:
  struct Target {
    // The copy's applied position, as last told by the tserver itself (push or release answer,
    // floor request) or by its heartbeat. Meaningful only when known is set.
    OpId acked;
    bool known = false;
    // The heartbeat report already merged into acked, so an old report is not merged again after a
    // gap answer moved acked below it.
    MonoTime heartbeat_merged_at;
    HybridTime floor;
    HybridTime last_safe_time;
    bool live = false;
    // The tserver answered that it keeps no local copy. It gets no pushes until its heartbeat
    // reports a position.
    bool no_copy = false;
    bool in_flight = false;
    int releases_in_flight = 0;
    // A release is waiting on this target and its pages did not fit inline.
    bool release_priority = false;
    int consecutive_failures = 0;
    MonoTime next_attempt;
    MonoTime last_update;
    std::shared_ptr<tserver::TabletServerServiceProxy> proxy;
  };

  struct PushCall;

  struct BuildResult {
    std::vector<PagePtr> pages;
    // The last WAL operation the pages cover; equal to the starting position when nothing was
    // read.
    OpId built_through;
  };

  // Converts the committed operations in (after, to_index] into pages, sealing a page at
  // local_catalog_push_page_bytes, after a CHANGE_METADATA record, around a restore of the system
  // catalog, and at to_index. When wait_for_commit is set, waits until to_index is committed or the
  // deadline passes; otherwise stops at the committed index. Stops once max_bytes of records were
  // built, when max_bytes is not zero. The page that ends at to_index gets safe_time.
  Result<BuildResult> BuildPages(
      const tablet::TabletPeerPtr& peer, const OpId& after, int64_t to_index,
      HybridTime safe_time, bool wait_for_commit, size_t max_bytes, CoarseTimePoint deadline);

  Result<Certified> DoCycle(HybridTime floor, CoarseTimePoint deadline) EXCLUDES(mutex_);

  // Drops every page and every target position. Called when the system catalog tablet's leader
  // term changes: entries past a deposed leader's committed index may have been replaced.
  void ResetForTermUnlocked(int64_t term) REQUIRES(mutex_);

  void AppendPagesUnlocked(std::vector<PagePtr> pages) REQUIRES(mutex_);
  void EvictUnlocked() REQUIRES(mutex_);

  // Pages from the cache that a copy at position `after` needs to reach the cache's end, at most
  // max_bytes of them but at least one. Empty when the copy is below the cache or at its end.
  std::vector<PagePtr> CachedPagesAfterUnlocked(const OpId& after, size_t max_bytes) const
      REQUIRES(mutex_);

  // The next pages to push to t. Returns false when t needs a transient page that has not been
  // built yet, setting *transient_from to where it must start.
  bool NextPagesUnlocked(const Target& t, std::vector<PagePtr>* pages, OpId* transient_from)
      REQUIRES(mutex_);
  bool NeedsPagesUnlocked(const Target& t) const REQUIRES(mutex_);

  // Builds the page that starts at `from` and ends below the cache's first page, from the WAL.
  Result<PagePtr> BuildTransientPage(const OpId& from, CoarseTimePoint deadline)
      EXCLUDES(mutex_);

  Target& TargetUnlocked(const std::string& uuid) REQUIRES(mutex_);
  void MergeHeartbeatUnlocked(Target* t, const TSDescriptorPtr& descriptor) REQUIRES(mutex_);

  std::string SerializePush(const std::string& uuid, const std::vector<PagePtr>& pages) const
      REQUIRES(mutex_);

  bool EligibleUnlocked(const std::string& uuid, const Target& t, MonoTime now) const
      REQUIRES(mutex_);
  // Marks the targets with a live lease, and the test's extra targets, live; merges their
  // heartbeat positions; drops entries idle past sys_catalog_change_stream_idle_timeout_sec.
  void RefreshTargetsUnlocked(const TSDescriptorVector& live) REQUIRES(mutex_);

  void SchedulerLoop();
  // Pushes the next pages to every live target that needs them, within the in-flight bounds.
  void Drain() EXCLUDES(mutex_);
  // Accounts a push of pages to t as in flight and builds its request. The RPC itself is sent by
  // Send, after mutex_ is released, since its callback takes mutex_.
  std::shared_ptr<PushCall> PrepareSend(
      const std::string& uuid, Target* t, const std::vector<PagePtr>& pages) REQUIRES(mutex_);
  void Send(const std::shared_ptr<PushCall>& call) EXCLUDES(mutex_);
  void OnPushDone(const std::string& uuid, const std::shared_ptr<PushCall>& call)
      EXCLUDES(mutex_);
  void WakeScheduler() EXCLUDES(mutex_);

  Master& master_;
  const GetTabletPeerFunc get_tablet_peer_;
  const MemTrackerPtr mem_tracker_;
  std::unique_ptr<cdc::StreamMetadata> stream_metadata_;

  // Serializes the WAL reads of cycles and transient pages.
  std::mutex build_mutex_;

  mutable std::mutex mutex_;
  std::condition_variable cond_;

  int64_t term_ GUARDED_BY(mutex_) = 0;
  std::deque<PagePtr> pages_ GUARDED_BY(mutex_);
  size_t cached_bytes_ GUARDED_BY(mutex_) = 0;
  Certified last_certified_ GUARDED_BY(mutex_);
  // Pages built from the WAL below the cache for targets that are behind it, keyed by the index
  // they start after. Shared by targets at the same position.
  std::map<int64_t, PagePtr> transient_ GUARDED_BY(mutex_);
  std::unordered_map<std::string, Target> targets_ GUARDED_BY(mutex_);
  // Targets that need pages and wait for an in-flight slot, in the order they get one. Rebuilt by
  // a full scan of the targets, which runs when pages were added, on a request, and once a second;
  // a completed push only re-queues its own target, so that an acknowledgement costs the same
  // with ten targets as with ten thousand.
  std::deque<std::string> waiting_ GUARDED_BY(mutex_);
  std::vector<std::string> acked_ GUARDED_BY(mutex_);
  bool full_scan_ GUARDED_BY(mutex_) = true;
  MonoTime last_full_scan_ GUARDED_BY(mutex_);
  std::string extra_targets_spec_ GUARDED_BY(mutex_);
  std::vector<std::pair<std::string, std::shared_ptr<tserver::TabletServerServiceProxy>>>
      extra_targets_ GUARDED_BY(mutex_);
  size_t in_flight_rpcs_ GUARDED_BY(mutex_) = 0;
  size_t in_flight_bytes_ GUARDED_BY(mutex_) = 0;

  // Cycle coalescing: a call takes the next ticket and waits until a cycle that started after it
  // took the ticket has completed.
  uint64_t cycles_requested_ GUARDED_BY(mutex_) = 0;
  uint64_t cycles_completed_ GUARDED_BY(mutex_) = 0;
  bool cycle_running_ GUARDED_BY(mutex_) = false;
  HybridTime pending_floor_ GUARDED_BY(mutex_);
  Status last_cycle_status_ GUARDED_BY(mutex_);
  MonoTime last_cycle_start_ GUARDED_BY(mutex_);
  std::condition_variable cycle_cond_;

  bool wake_ GUARDED_BY(mutex_) = false;
  bool shutdown_ GUARDED_BY(mutex_) = false;
  MonoTime next_timer_cycle_ GUARDED_BY(mutex_);
  scoped_refptr<Thread> thread_;

  scoped_refptr<Counter> cycles_;
  scoped_refptr<Counter> release_requests_;
  scoped_refptr<Counter> rpcs_sent_;
  scoped_refptr<Counter> bytes_sent_;
  scoped_refptr<Counter> pages_built_;
  scoped_refptr<Counter> transient_pages_built_;
  scoped_refptr<Counter> floor_requests_received_;
  scoped_refptr<Counter> release_inline_pages_;
  scoped_refptr<Counter> release_inline_skipped_;
  scoped_refptr<Counter> push_failures_;
  scoped_refptr<AtomicGauge<uint64_t>> in_flight_rpcs_gauge_;
  scoped_refptr<AtomicGauge<uint64_t>> in_flight_rpcs_max_;
  scoped_refptr<AtomicGauge<uint64_t>> in_flight_bytes_max_;
  scoped_refptr<AtomicGauge<uint64_t>> cached_bytes_gauge_;
  scoped_refptr<EventStats> cycle_us_;
};

// The system catalog change service: tservers ask master to push to them through it.
class SysCatalogChangeService : public cdc::SysCatalogChangeServiceIf {
 public:
  SysCatalogChangeService(
      const scoped_refptr<MetricEntity>& metric_entity,
      std::shared_ptr<SysCatalogPusher> pusher);

  void RequestSysCatalogPush(
      const cdc::RequestSysCatalogPushRequestPB* req, cdc::RequestSysCatalogPushResponsePB* resp,
      rpc::RpcContext context) override;

 private:
  const std::shared_ptr<SysCatalogPusher> pusher_;
};

}  // namespace master
}  // namespace yb
