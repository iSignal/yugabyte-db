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

#include <string>
#include <vector>

#include "yb/cdc/cdc_producer.h"
#include "yb/cdc/xrepl_stream_metadata.h"

#include "yb/consensus/log.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/replicate_msgs_holder.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/sys_catalog-test_base.h"
#include "yb/master/sys_catalog_constants.h"
#include "yb/master/sys_catalog_pusher.h"

#include "yb/rpc/connection_context.h"
#include "yb/rpc/local_call.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_context.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/service_pool.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/tserver.pb.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/flags.h"
#include "yb/util/test_macros.h"

DECLARE_bool(enable_local_tserver_catalog);
DECLARE_bool(enable_object_locking_for_table_locks);
DECLARE_bool(ysql_enable_concurrent_ddl);
DECLARE_string(allowed_preview_flags_csv);
DECLARE_uint32(local_catalog_push_page_bytes);
DECLARE_uint32(local_catalog_push_cache_mb);
DECLARE_uint32(local_catalog_push_min_cycle_interval_ms);
DECLARE_uint32(local_catalog_push_interval_ms);
DECLARE_uint32(local_catalog_push_max_in_flight_rpcs);
DECLARE_uint32(local_catalog_push_release_reserved_rpcs);
DECLARE_uint32(local_catalog_push_unknown_position_lookback_ops);
DECLARE_string(TEST_local_catalog_push_extra_targets);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_int32(log_min_segments_to_retain);
DECLARE_int32(log_segment_size_mb);
DECLARE_int32(log_cache_size_limit_mb);
DECLARE_int32(global_log_cache_size_limit_mb);

METRIC_DECLARE_entity(server);
METRIC_DECLARE_counter(local_catalog_push_cycles);

namespace yb::master {

namespace {

// Push receivers in the test process, standing in for tservers' local catalog copies. Each push is
// answered by `on_push`, which by default advances the target to the end of the pages it got.
class FakeReceivers {
 public:
  using Req = tserver::PushSysCatalogChangesRequestPB;
  using Resp = tserver::PushSysCatalogChangesResponsePB;

  struct Target {
    OpId applied;
    int pushes = 0;
    std::vector<int64_t> first_page_from;
    bool reseed = false;
  };

  Status Start() {
    metric_entity_ = METRIC_ENTITY_server.Instantiate(&metric_registry_, "fake-receivers");
    messenger_ = VERIFY_RESULT(rpc::MessengerBuilder("fake-receivers")
                                   .set_num_reactors(2)
                                   .set_metric_entity(metric_entity_)
                                   .Build());
    Endpoint bound;
    RETURN_NOT_OK(messenger_->ListenAddress(
        rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>(), Endpoint(),
        &bound));
    host_port_ = HostPort::FromBoundEndpoint(bound);
    thread_pool_ = std::make_shared<rpc::ThreadPool>(
        rpc::ThreadPoolOptions{.name = "fake-receivers", .max_workers = 64});
    auto service = std::make_unique<Service>(this);
    const auto name = service->service_name();
    service_pool_ = make_scoped_refptr<rpc::ServicePool>(
        /* max_tasks= */ 10000, [pool = thread_pool_](auto) { return pool; },
        &messenger_->scheduler(), std::move(service), messenger_->metric_entity());
    RETURN_NOT_OK(messenger_->RegisterService(name, service_pool_));
    return messenger_->StartAcceptor();
  }

  void Shutdown() {
    if (messenger_) {
      messenger_->ShutdownAcceptor();
      messenger_->UnregisterAllServices();
      service_pool_->Shutdown();
      messenger_->Shutdown();
      thread_pool_->Shutdown();
      messenger_.reset();
    }
  }

  ~FakeReceivers() { Shutdown(); }

  std::string Flag(size_t count) const { return Format("$0=$1", host_port_.ToString(), count); }
  std::string Uuid(size_t i) const { return Format("$0#$1", host_port_.ToString(), i); }

  void SetStart(const OpId& start) {
    std::lock_guard lock(mutex_);
    start_ = start;
  }

  // Called with the lock held; returns true when it answered the push itself.
  std::function<bool(const std::string&, Target*, const Req&, Resp*)> on_push;
  std::atomic<int> delay_ms{0};

  Target Get(const std::string& uuid) {
    std::lock_guard lock(mutex_);
    auto it = targets_.find(uuid);
    return it == targets_.end() ? Target() : it->second;
  }

  int max_in_flight() const { return max_in_flight_; }
  int max_in_flight_per_target() const { return max_in_flight_per_target_; }

 private:
  class Service : public rpc::ServiceIf {
   public:
    explicit Service(FakeReceivers* owner)
        : owner_(owner), method_("yb.tserver.TabletServerService", "PushSysCatalogChanges") {}
    void FillEndpoints(const rpc::RpcServicePtr& service, rpc::RpcEndpointMap* map) override {
      map->emplace(method_.serialized_body(), std::make_pair(service, 0UL));
    }
    void Handle(rpc::InboundCallPtr call) override {
      rpc::HandleCall<rpc::RpcCallPBParamsImpl<Req, Resp>>(
          std::move(call), [this](const Req* req, Resp* resp, rpc::RpcContext context) {
            owner_->HandlePush(*req, resp);
            context.RespondSuccess();
          });
    }
    std::string service_name() const override { return "yb.tserver.TabletServerService"; }
    std::string metric_name() const override { return service_name(); }

   private:
    FakeReceivers* const owner_;
    const rpc::RemoteMethod method_;
  };

  void HandlePush(const Req& req, Resp* resp) {
    {
      std::lock_guard lock(mutex_);
      max_in_flight_ = std::max(max_in_flight_, ++in_flight_);
      max_in_flight_per_target_ =
          std::max(max_in_flight_per_target_, ++in_flight_per_target_[req.target_uuid()]);
    }
    if (delay_ms.load() > 0) {
      std::this_thread::sleep_for(delay_ms.load() * 1ms);
    }
    std::lock_guard lock(mutex_);
    --in_flight_;
    --in_flight_per_target_[req.target_uuid()];
    auto [it, inserted] = targets_.try_emplace(req.target_uuid());
    auto& t = it->second;
    if (inserted) {
      t.applied = start_;
    }
    ++t.pushes;
    if (!req.pages().empty()) {
      t.first_page_from.push_back(req.pages(0).page_from().index());
    }
    if (!on_push || !on_push(req.target_uuid(), &t, req, resp)) {
      for (const auto& page : req.pages()) {
        if (page.page_from().index() > t.applied.index) {
          resp->set_gap(true);
          break;
        }
        if (page.reseed_required()) {
          t.reseed = true;
          break;
        }
        if (page.page_end().index() > t.applied.index) {
          t.applied = OpId::FromPB(page.page_end());
        }
        if (page.has_safe_time()) {
          resp->set_safe_time(page.safe_time());
        }
      }
    }
    t.applied.ToPB(resp->mutable_applied_op_id());
  }

  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  std::unique_ptr<rpc::Messenger> messenger_;
  std::shared_ptr<rpc::ThreadPool> thread_pool_;
  scoped_refptr<rpc::ServicePool> service_pool_;
  HostPort host_port_;
  std::mutex mutex_;
  OpId start_;
  std::unordered_map<std::string, Target> targets_;
  int in_flight_ = 0;
  int max_in_flight_ = 0;
  std::unordered_map<std::string, int> in_flight_per_target_;
  int max_in_flight_per_target_ = 0;
};

}  // namespace

class SysCatalogPusherTest : public SysCatalogTest {
 protected:
  void TearDown() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_local_catalog_push_extra_targets) = "";
    receivers_.Shutdown();
    SysCatalogTest::TearDown();
  }

  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_allowed_preview_flags_csv) = "ysql_enable_concurrent_ddl";
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_locking_for_table_locks) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_concurrent_ddl) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_local_tserver_catalog) = true;
    // Only the cycles a test runs, so that page boundaries are the test's own.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_interval_ms) = 3600 * 1000;
    SysCatalogTest::SetUp();
    pusher_ = master_->sys_catalog_pusher();
    ASSERT_NE(pusher_, nullptr);
  }

  Result<tablet::TabletPeerPtr> Peer() {
    return master_->catalog_manager_impl()->tablet_peer();
  }

  Result<OpId> CommittedOpId() {
    auto consensus = VERIFY_RESULT(VERIFY_RESULT(Peer())->GetRaftConsensus());
    return OpId(consensus->LeaderTerm(), consensus->GetCommittedAndMajorityReplicatedIndex().first);
  }

  // Each upsert is one WAL operation that produces one record.
  Status WriteTables(int count, const std::string& prefix) {
    auto epoch = LeaderEpoch(kLeaderTerm, sys_catalog_->pitr_count());
    for (int i = 0; i < count; ++i) {
      auto table = CreateUncommittedTable(Format("$0_$1", prefix, i));
      RETURN_NOT_OK(sys_catalog_->Upsert(epoch, table));
      table->mutable_metadata()->CommitMutation();
    }
    return Status::OK();
  }

  // The records the per-call producer returns for (from, to], fetched the way the pull service
  // fetched them.
  Result<std::vector<std::string>> ProducerRecords(OpId from, int64_t to) {
    auto peer = VERIFY_RESULT(Peer());
    cdc::StreamMetadata stream;
    stream.InitForSysCatalogChangeStream(xrepl::StreamId::GenerateRandom());
    const auto stream_id = stream.GetStreamId();
    const TabletId tablet_id = kSysCatalogTabletId;
    const auto deadline = CoarseMonoClock::Now() + 30s;
    std::shared_ptr<MemTracker> mem_tracker;
    std::vector<std::string> records;
    while (from.index < to) {
      cdc::GetChangesResponsePB resp;
      consensus::ReplicateMsgsHolder holder;
      consensus::HaveMoreMessages have_more{false};
      int64_t last_readable = 0;
      cdc::XClusterGetChangesContext context = {
          .stream_id = stream_id,
          .tablet_id = tablet_id,
          .from_op_id = from,
          .tablet_peer = peer,
          .update_on_split_op_func = [](const consensus::ReplicateMsg&) { return Status::OK(); },
          .update_on_snapshot_op_func = {},
          .mem_tracker = mem_tracker,
          .deadline = deadline,
          .stream_metadata = &stream,
          .msgs_holder = &holder,
          .resp = &resp,
          .have_more_messages = &have_more,
          .last_readable_opid_index = &last_readable,
      };
      RETURN_NOT_OK(cdc::GetChangesForXCluster(context));
      for (const auto& record : resp.records()) {
        if (OpId::FromPB(record.op_id()).index <= to) {
          records.push_back(record.SerializeAsString());
        }
      }
      const auto next = OpId::FromPB(resp.checkpoint().op_id());
      SCHECK_GT(next.index, from.index, IllegalState, "The producer made no progress");
      from = next;
    }
    return records;
  }

  static Result<std::vector<std::string>> PageRecords(
      const std::vector<SysCatalogPusher::PagePtr>& pages) {
    std::vector<std::string> records;
    for (const auto& page : pages) {
      cdc::GetChangesResponsePB changes;
      SCHECK(changes.ParseFromString(*page->records), Corruption, "Page does not parse");
      for (const auto& record : changes.records()) {
        records.push_back(record.SerializeAsString());
      }
    }
    return records;
  }

  Result<int64_t> Cycles() {
    return METRIC_local_catalog_push_cycles.Instantiate(master_->metric_entity())->value();
  }

  // Every receiver at the end of the cache, as far as master knows.
  Status WaitForAllAtEnd(size_t count, MonoDelta timeout) {
    return WaitFor(
        [this, count]() {
          const auto end = pusher_->TEST_LastCertified().index;
          for (size_t i = 0; i != count; ++i) {
            if (pusher_->TEST_TargetPosition(receivers_.Uuid(i)).index < end) {
              return false;
            }
          }
          return true;
        },
        timeout, "Every receiver at the end of master's cache");
  }

  SysCatalogPusher* pusher_ = nullptr;
  FakeReceivers receivers_;
};

// For the same WAL range, the pages hold exactly the records the per-call producer returns.
TEST_F(SysCatalogPusherTest, PageBytesEqualProducerRecords) {
  const auto start = ASSERT_RESULT(CommittedOpId());
  ASSERT_OK(WriteTables(20, "equal"));
  const auto end = ASSERT_RESULT(CommittedOpId());
  auto pages = ASSERT_RESULT(pusher_->TEST_BuildPages(
      start, end.index, HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  ASSERT_FALSE(pages.empty());
  ASSERT_EQ(pages.front()->from.index, start.index);
  ASSERT_EQ(pages.back()->end.index, end.index);
  const auto page_records = ASSERT_RESULT(PageRecords(pages));
  const auto producer_records = ASSERT_RESULT(ProducerRecords(start, end.index));
  ASSERT_EQ(page_records.size(), producer_records.size());
  ASSERT_GE(page_records.size(), 20);
  for (size_t i = 0; i < page_records.size(); ++i) {
    ASSERT_EQ(page_records[i], producer_records[i]) << "record " << i;
  }
}

// Pages are contiguous, sealed at the byte cap, and only the page that ends at the certified
// position carries the safe time.
TEST_F(SysCatalogPusherTest, PagesSealAtCapAndCycleEnd) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_page_bytes) = 256;
  const auto start = ASSERT_RESULT(CommittedOpId());
  ASSERT_OK(WriteTables(20, "cap"));
  const auto end = ASSERT_RESULT(CommittedOpId());
  const HybridTime safe_time(HybridTime::FromMicros(1000).ToUint64());
  auto pages = ASSERT_RESULT(pusher_->TEST_BuildPages(
      start, end.index, safe_time, CoarseMonoClock::Now() + 30s));
  ASSERT_GT(pages.size(), 5);
  for (size_t i = 0; i < pages.size(); ++i) {
    if (i > 0) {
      ASSERT_EQ(pages[i]->from.index, pages[i - 1]->end.index) << "page " << i;
    }
    ASSERT_GT(pages[i]->end.index, pages[i]->from.index) << "page " << i;
    // A page is sealed by the operation that takes it to the cap, so it exceeds the cap by at most
    // that operation's records.
    if (i + 1 < pages.size()) {
      ASSERT_GE(pages[i]->bytes(), 256) << "page " << i;
      ASSERT_FALSE(pages[i]->safe_time.is_valid()) << "page " << i;
    }
  }
  ASSERT_EQ(pages.back()->end.index, end.index);
  ASSERT_EQ(pages.back()->safe_time, safe_time);
}

// A cycle with no new WAL keeps the last page and gives it the newer safe time.
TEST_F(SysCatalogPusherTest, EmptyCycleCarriesSafeTime) {
  ASSERT_OK(WriteTables(3, "empty"));
  const auto first = ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid,
                                                     CoarseMonoClock::Now() + 30s));
  const auto pages_after_first = pusher_->TEST_NumCachedPages();
  const auto second = ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid,
                                                      CoarseMonoClock::Now() + 30s));
  ASSERT_GE(second.index, first.index);
  ASSERT_GT(second.safe_time, first.safe_time);
  if (second.index == first.index) {
    ASSERT_EQ(pusher_->TEST_NumCachedPages(), pages_after_first);
  }
  auto pages = pusher_->TEST_CachedPages();
  ASSERT_EQ(pages.back()->end.index, second.index);
  ASSERT_EQ(pages.back()->safe_time, second.safe_time);
}

// A floor ahead of master's clock is reached by later cycles, not by moving the clock.
TEST_F(SysCatalogPusherTest, FloorIsReachedWithoutMovingTheClock) {
  const auto floor = master_->clock()->Now().AddMilliseconds(300);
  pusher_->RequestPush("floor-only", OpId(), floor);
  const auto first = ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid,
                                                     CoarseMonoClock::Now() + 30s));
  ASSERT_LT(first.safe_time, floor);
  ASSERT_OK(WaitFor(
      [this, floor]() { return pusher_->TEST_LastCertified().safe_time >= floor; },
      10s, "A cycle at or above the floor"));
  // The clock was not moved past physical time: its physical part stays within a few ms of now.
  const auto now = master_->clock()->Now();
  ASSERT_LE(now.GetPhysicalValueMicros(), GetCurrentTimeMicros() + 50000);
}

// Concurrent requests share cycles.
TEST_F(SysCatalogPusherTest, FloorRequestsCoalesce) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_min_cycle_interval_ms) = 1000;
  ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  const auto before = ASSERT_RESULT(Cycles());
  const auto floor = master_->clock()->Now();
  for (int i = 0; i < 100; ++i) {
    pusher_->RequestPush(Format("ts-$0", i % 10), OpId(), floor);
  }
  ASSERT_OK(WaitFor(
      [this, floor]() { return pusher_->TEST_LastCertified().safe_time >= floor; },
      10s, "A cycle covering the floor"));
  SleepFor(2s);
  const auto cycles = ASSERT_RESULT(Cycles()) - before;
  LOG(INFO) << "Cycles for 100 floor requests: " << cycles;
  ASSERT_LE(cycles, 3);
}

// With no room in the cache, only the last page stays, whatever any copy's position.
TEST_F(SysCatalogPusherTest, EvictionByBytesOnly) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_page_bytes) = 256;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_cache_mb) = 0;
  ASSERT_OK(WriteTables(10, "evict"));
  const auto certified = ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid,
                                                         CoarseMonoClock::Now() + 30s));
  ASSERT_EQ(pusher_->TEST_NumCachedPages(), 1);
  ASSERT_EQ(pusher_->TEST_CachedPages().back()->end.index, certified.index);
}

// Many targets far behind are served in waves: never more RPCs in flight than the bound, and never
// two to one target at once.
TEST_F(SysCatalogPusherTest, InFlightBoundsHold) {
  constexpr size_t kTargets = 200;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_page_bytes) = 512;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_max_in_flight_rpcs) = 8;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_release_reserved_rpcs) = 2;
  const auto start = ASSERT_RESULT(CommittedOpId());
  ASSERT_OK(WriteTables(30, "bounds"));
  ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  ASSERT_OK(receivers_.Start());
  receivers_.SetStart(start);
  receivers_.delay_ms = 5;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_local_catalog_push_extra_targets) =
      receivers_.Flag(kTargets);
  pusher_->RequestPush("wake", OpId(), HybridTime::kInvalid);
  ASSERT_OK(WaitForAllAtEnd(kTargets, 120s));
  LOG(INFO) << "Max in flight: " << receivers_.max_in_flight() << ", per target "
            << receivers_.max_in_flight_per_target();
  ASSERT_LE(receivers_.max_in_flight(), 8);
  ASSERT_EQ(receivers_.max_in_flight_per_target(), 1);
}

// A gap answer at X moves master's position for the target to X, and the next push starts there.
TEST_F(SysCatalogPusherTest, GapAnswerRewinds) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_page_bytes) = 256;
  const auto start = ASSERT_RESULT(CommittedOpId());
  ASSERT_OK(WriteTables(20, "gap"));
  ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  ASSERT_OK(receivers_.Start());
  const OpId gap_at(start.term, start.index + 3);
  receivers_.SetStart(gap_at);
  std::atomic<bool> answered_gap{false};
  receivers_.on_push = [&answered_gap, gap_at](
      const std::string&, FakeReceivers::Target* t, const FakeReceivers::Req& req,
      FakeReceivers::Resp* resp) {
    if (answered_gap.exchange(true)) {
      return false;
    }
    // The first push is answered with a gap whatever it carries.
    resp->set_gap(true);
    t->applied = gap_at;
    return true;
  };
  // Master believes the target is further along than it is.
  pusher_->RequestPush(
      receivers_.Uuid(0), OpId(start.term, start.index + 15), HybridTime::kInvalid);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_local_catalog_push_extra_targets) = receivers_.Flag(1);
  pusher_->RequestPush("wake", OpId(), HybridTime::kInvalid);
  ASSERT_OK(WaitForAllAtEnd(1, 60s));
  const auto t = receivers_.Get(receivers_.Uuid(0));
  ASSERT_GE(t.first_page_from.size(), 2);
  // The first push started at the page holding the position master believed, above the gap; the
  // second at or below where the copy said it was.
  ASSERT_GT(t.first_page_from[0], gap_at.index);
  ASSERT_LE(t.first_page_from[0], start.index + 15);
  ASSERT_LE(t.first_page_from[1], gap_at.index);
}

// A new term of the system catalog tablet empties the cache and forgets every target's position;
// the next cycle rebuilds the cache from the look-back.
TEST_F(SysCatalogPusherTest, TermChangeClearsCache) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_unknown_position_lookback_ops) = 5;
  ASSERT_OK(WriteTables(20, "term"));
  ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  pusher_->RequestPush("known", ASSERT_RESULT(CommittedOpId()), HybridTime::kInvalid);
  ASSERT_FALSE(pusher_->TEST_TargetPosition("known").empty());
  const auto pages_before = pusher_->TEST_CachedPages();

  auto consensus = ASSERT_RESULT(ASSERT_RESULT(Peer())->GetRaftConsensus());
  ASSERT_OK(consensus->EmulateElection());
  ASSERT_OK(WaitFor(
      [&consensus]() { return consensus->LeaderTerm() > 0; }, 30s, "Leadership in the new term"));
  const auto certified = ASSERT_RESULT(
      pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  ASSERT_TRUE(pusher_->TEST_TargetPosition("known").empty());
  const auto pages_after = pusher_->TEST_CachedPages();
  ASSERT_FALSE(pages_after.empty());
  ASSERT_GE(pages_after.front()->from.index, certified.index - 5);
  // Rebuilt, not kept: none of the old page objects is in the new cache.
  for (const auto& page : pages_after) {
    ASSERT_TRUE(std::find(pages_before.begin(), pages_before.end(), page) == pages_before.end());
  }
}

class SysCatalogPusherShortRetentionTest : public SysCatalogPusherTest {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_log_segment_size_mb) = 1;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_log_min_seconds_to_retain) = 0;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_log_min_segments_to_retain) = 1;
    // The log cache keeps system catalog operations until memory forces them out, and a position
    // still in it is retained as far as a copy is concerned.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_log_cache_size_limit_mb) = 0;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_global_log_cache_size_limit_mb) = 1;
    SysCatalogPusherTest::SetUp();
  }
};

// A target whose position is below the retained WAL is told to re-seed and gets no records.
TEST_F(SysCatalogPusherShortRetentionTest, BelowRetentionIsReseed) {
  // Small pages and no room in the cache: only the last page stays, well above position 1.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_page_bytes) = 256;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_local_catalog_push_cache_mb) = 0;
  auto peer = ASSERT_RESULT(Peer());
  // Enough WAL to fill a few 1 MB segments, flushed, so that the first ones can be collected.
  for (int round = 0; round != 20 && peer->log()->GetMinReplicateIndex() <= 2; ++round) {
    ASSERT_OK(WriteTables(300, Format("retention_$0", round)));
    ASSERT_OK(ASSERT_RESULT(peer->shared_tablet())->Flush(
        tablet::FlushMode::kSync, rocksdb::FlushReason::kSysCatalogFlush));
    ASSERT_OK(peer->RunLogGC(/* rollover= */ true));
    LOG(INFO) << "Round " << round << ": log holds " << peer->log()->GetMinReplicateIndex()
              << " to " << peer->log()->GetLatestEntryOpId() << ", xrepl retention index "
              << peer->log()->GetXReplMinReplicatedIndex() << ", log cache bytes "
              << ASSERT_RESULT(peer->GetRaftConsensus())->LogCacheSize();
  }
  ASSERT_GT(peer->log()->GetMinReplicateIndex(), 2);
  ASSERT_RESULT(pusher_->RunCycle(HybridTime::kInvalid, CoarseMonoClock::Now() + 30s));
  ASSERT_OK(receivers_.Start());
  receivers_.SetStart(OpId(1, 1));
  pusher_->RequestPush(receivers_.Uuid(0), OpId(1, 1), HybridTime::kInvalid);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_local_catalog_push_extra_targets) = receivers_.Flag(1);
  pusher_->RequestPush("wake", OpId(), HybridTime::kInvalid);
  ASSERT_OK(WaitFor(
      [this]() { return receivers_.Get(receivers_.Uuid(0)).reseed; }, 60s,
      "The target below the retained WAL to be told to re-seed"));
}

}  // namespace yb::master
