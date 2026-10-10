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

#include "yb/gutil/walltime.h"

#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"

#include "yb/integration-tests/external_mini_cluster.h"

#include "yb/master/sys_catalog_constants.h"

#include "yb/rpc/rpc_controller.h"

#include "yb/tserver/local_catalog_replica.h"
#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/metrics.h"
#include "yb/util/result.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_thread_holder.h"

#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"

METRIC_DECLARE_entity(server);
METRIC_DECLARE_counter(local_catalog_reads_served);
METRIC_DECLARE_counter(local_catalog_reads_waited_for_version);
METRIC_DECLARE_counter(local_catalog_reads_waited_for_own_writes);
METRIC_DECLARE_counter(local_catalog_reads_to_master_in_ddl);
METRIC_DECLARE_counter(local_catalog_reads_to_master_not_serving);
METRIC_DECLARE_counter(local_catalog_apply_failures);
METRIC_DECLARE_counter(local_catalog_reseeds);
METRIC_DECLARE_counter(local_catalog_pushes_received);
METRIC_DECLARE_counter(local_catalog_pushes_applied);
METRIC_DECLARE_counter(local_catalog_pushes_refused);
METRIC_DECLARE_counter(local_catalog_floor_requests_sent);
METRIC_DECLARE_counter(local_catalog_push_cycles);
METRIC_DECLARE_counter(local_catalog_push_release_requests);
METRIC_DECLARE_counter(local_catalog_push_rpcs_sent);
METRIC_DECLARE_counter(local_catalog_push_transient_pages_built);
METRIC_DECLARE_counter(local_catalog_floor_requests_received);
METRIC_DECLARE_counter(local_catalog_release_inline_pages);
METRIC_DECLARE_counter(local_catalog_release_inline_skipped);
METRIC_DECLARE_gauge_uint64(local_catalog_push_in_flight_rpcs_max);
METRIC_DECLARE_gauge_uint64(local_catalog_push_in_flight_bytes_max);
METRIC_DECLARE_gauge_uint32(local_catalog_serving_state);
METRIC_DECLARE_gauge_uint64(local_catalog_safe_time_micros);
METRIC_DECLARE_event_stats(local_catalog_apply_latency_us);
METRIC_DECLARE_event_stats(local_catalog_gate_wait_us);
METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerService_Read);
METRIC_DECLARE_histogram(handler_latency_yb_master_MasterAdmin_IsInitDbDone);

using namespace std::literals;

namespace yb::pgwrapper {

namespace {

// LocalCatalogServingState::kServing. The gauge carries the enum's numeric value; a test in
// another process cannot see the enum itself.
constexpr uint32_t kServingStateServing = 1;
constexpr uint32_t kServingStateDisabledLease = 2;

constexpr auto kServingTimeout = 180s;

}  // namespace

class PgLocalCatalogTest : public LibPqTestBase {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    // The design targets one mode only: per-database catalog versions with invalidation messages,
    // object locks serializing DDL against catalog readers, and DDL running inside the user's
    // transaction.
    options->extra_tserver_flags.push_back("--enable_local_tserver_catalog=true");
    options->extra_master_flags.push_back("--enable_local_tserver_catalog=true");
    for (auto* flags : {&options->extra_master_flags, &options->extra_tserver_flags}) {
      flags->push_back("--ysql_yb_enable_invalidation_messages=true");
      flags->push_back("--enable_object_locking_for_table_locks=true");
      flags->push_back("--allowed_preview_flags_csv=ysql_enable_concurrent_ddl");
      flags->push_back("--ysql_enable_concurrent_ddl=true");
      flags->push_back("--ysql_yb_ddl_transaction_block_enabled=true");
      // Needed by the rolled-back-subtransaction test: without it PG refuses to interleave a
      // SAVEPOINT with a DDL in one transaction block.
      flags->push_back("--ysql_yb_enable_ddl_savepoint_support=true");
      // Auto-analyze runs its own DDLs and bumps catalog versions, which would make every counter
      // assertion below racy.
      flags->push_back("--ysql_enable_auto_analyze=false");
      // Level 1 across the feature's modules, so a failure can be read out of the daemon logs.
      // The applier's level 3 is deliberately not on here: it logs one line per applied row, which
      // ran to 170k lines and 540k in the suite log, and slowed the timing-sensitive tests enough
      // to make the lease-loss test flake. Use it on a single test or a live cluster instead.
      flags->push_back(
          "--vmodule=local_catalog_applier=1,local_catalog_replica=1,local_catalog_read=1,"
          "pg_client_session=1,ts_local_lock_manager=1,heartbeater=1,object_lock_info_manager=1,"
          "sys_catalog_pusher=2,ysql_lease_manager=1");
    }
  }

  int GetNumTabletServers() const override { return 3; }

  ExternalTabletServer& TServer(size_t idx) { return *cluster_->tablet_server(idx); }

  Result<int64_t> Counter(size_t idx, const MetricPrototype& proto) {
    return TServer(idx).GetMetric<int64_t>(
        &METRIC_ENTITY_server, /* entity_id= */ nullptr, &proto, "value");
  }

  Result<int64_t> EventStatsCount(size_t idx, const MetricPrototype& proto) {
    return TServer(idx).GetMetric<int64_t>(
        &METRIC_ENTITY_server, /* entity_id= */ nullptr, &proto, "total_count");
  }

  Status WaitForServing(size_t idx) {
    return LoggedWaitFor(
        [this, idx]() -> Result<bool> {
          auto state = Counter(idx, METRIC_local_catalog_serving_state);
          return state.ok() && *state == kServingStateServing;
        },
        kServingTimeout, Format("Local catalog copy on tserver $0 to serve", idx));
  }

  Status WaitForAllServing() {
    for (int i = 0; i < GetNumTabletServers(); ++i) {
      RETURN_NOT_OK(WaitForServing(i));
    }
    return Status::OK();
  }

  // Master answers catalog reads through its tserver Read service, so this count is the number of
  // catalog read round trips the cluster has made to master.
  Result<int64_t> MasterCatalogReadCount() {
    return cluster_->master()->GetMetric<int64_t>(
        &METRIC_ENTITY_server, /* entity_id= */ nullptr,
        &METRIC_handler_latency_yb_tserver_TabletServerService_Read, "total_count");
  }

  // How many times master has answered IsInitDbDone. The applier asks only when it has no copy on
  // disk, so this count staying still across a restart is what proves a reused copy needs nothing
  // from master.
  Result<int64_t> MasterInitDbDoneCount() {
    return cluster_->master()->GetMetric<int64_t>(
        &METRIC_ENTITY_server, /* entity_id= */ nullptr,
        &METRIC_handler_latency_yb_master_MasterAdmin_IsInitDbDone, "total_count");
  }

  Result<PGConn> ConnectTo(size_t idx) { return ConnectToTs(TServer(idx)); }

  Result<PGConn> ConnectTo(size_t idx, const std::string& db_name) {
    return ConnectToTsForDB(TServer(idx), db_name);
  }

  // Every counter that says where a catalog read went, as one line. Reported alongside a failed
  // expectation so that "not served locally" says which branch took the read instead.
  std::string RoutingCounters(size_t idx) {
    const auto get = [this, idx](const MetricPrototype& proto) -> std::string {
      auto value = Counter(idx, proto);
      return value.ok() ? std::to_string(*value) : value.status().ToString();
    };
    return Format(
        "ts $0: served=$1 waited_for_version=$2 to_master_in_ddl=$3 to_master_not_serving=$4 "
        "serving_state=$5 safe_time_micros=$6",
        idx, get(METRIC_local_catalog_reads_served),
        get(METRIC_local_catalog_reads_waited_for_version),
        get(METRIC_local_catalog_reads_to_master_in_ddl),
        get(METRIC_local_catalog_reads_to_master_not_serving),
        get(METRIC_local_catalog_serving_state), get(METRIC_local_catalog_safe_time_micros));
  }

  // Waits until the copy on tserver idx holds a state at or above the given wall clock reading.
  // The gauge carries C's physical microseconds, and on a single-host cluster the tserver clocks
  // and this process read the same wall clock, so the two are comparable. Master moves C only
  // when it pushes, so the wait reads the catalog at an explicit time past micros on that
  // tserver, which asks master for a push up to it.
  Status WaitForSafeTimePast(size_t idx, int64_t micros, const std::string& what) {
    return LoggedWaitFor(
        [this, idx, micros]() -> Result<bool> {
          auto value = Counter(idx, METRIC_local_catalog_safe_time_micros);
          if (value.ok() && *value > micros) {
            return true;
          }
          if (GetCurrentTimeMicros() > micros + 1) {
            auto conn = VERIFY_RESULT(ConnectTo(idx));
            RETURN_NOT_OK(conn.ExecuteFormat("SET yb_read_time = $0", micros + 1));
            WARN_NOT_OK(
                ResultToStatus(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_am")),
                "Catalog read at an explicit time failed");
          }
          return false;
        },
        kServingTimeout,
        Format("Local catalog copy on tserver $0 to hold state past $1 ($2)", idx, micros, what));
  }

  // The leader master's counters: it is the one that pushes, and a step-down moves them to
  // another process.
  Result<int64_t> MasterCounter(const MetricPrototype& proto) {
    return cluster_->GetLeaderMaster()->GetMetric<int64_t>(
        &METRIC_ENTITY_server, /* entity_id= */ nullptr, &proto, "value");
  }

  Status SetOnAllTServers(const std::string& flag, const std::string& value) {
    for (int i = 0; i < GetNumTabletServers(); ++i) {
      RETURN_NOT_OK(cluster_->SetFlag(&TServer(i), flag, value));
    }
    return Status::OK();
  }

  Result<int64_t> CatalogVersion(PGConn* conn) {
    return conn->FetchRow<int64_t>(
        "SELECT current_version FROM pg_catalog.pg_yb_catalog_version WHERE db_oid = "
        "(SELECT oid FROM pg_database WHERE datname = current_database())");
  }

  // Runs func and returns how much the named counter grew on tserver idx while it ran.
  Result<int64_t> CounterDelta(
      size_t idx, const MetricPrototype& proto, const std::function<Status()>& func) {
    const auto before = VERIFY_RESULT(Counter(idx, proto));
    RETURN_NOT_OK(func());
    return VERIFY_RESULT(Counter(idx, proto)) - before;
  }
};

// Phase 7 test 12: the copy exists and serves on every tserver of a fresh cluster, and the apply
// histogram shows a page was actually pushed and applied rather than the state
// having been assumed.
TEST_F(PgLocalCatalogTest, BootstrapReachesServing) {
  ASSERT_OK(WaitForAllServing());
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    ASSERT_GT(ASSERT_RESULT(EventStatsCount(i, METRIC_local_catalog_apply_latency_us)), 0);
    ASSERT_GT(ASSERT_RESULT(Counter(i, METRIC_local_catalog_safe_time_micros)), 0);
    ASSERT_EQ(ASSERT_RESULT(Counter(i, METRIC_local_catalog_reseeds)), 0);
    ASSERT_EQ(ASSERT_RESULT(Counter(i, METRIC_local_catalog_apply_failures)), 0);
  }
}

// Phase 2's check that A[db] matches master, expressed without reaching into the tserver: the
// version rows the copy holds must be the rows master holds. The same query is answered from the
// copy and then, with the copy out of the read path, by master, and the two must agree.
TEST_F(PgLocalCatalogTest, CatalogVersionRowsMatchMaster) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE bump_the_version (k INT PRIMARY KEY)"));
  }
  const std::string kQuery =
      "SELECT db_oid, current_version, last_breaking_version FROM pg_yb_catalog_version "
      "ORDER BY db_oid";

  std::string from_copy;
  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_served, [this, &kQuery, &from_copy]() -> Status {
        auto conn = VERIFY_RESULT(ConnectTo(0));
        from_copy = VERIFY_RESULT(conn.FetchAllAsString(kQuery));
        return Status::OK();
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0);

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto from_master = ASSERT_RESULT(conn.FetchAllAsString(kQuery));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));

  ASSERT_EQ(from_copy, from_master);
}

// Phase 7 test 1: a backend whose catcache is cold reads a copy that is already current, and its
// misses are answered locally.
TEST_F(PgLocalCatalogTest, ColdBackendServedLocally) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'one')"));
  }
  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_served, [this]() -> Status {
        auto conn = VERIFY_RESULT(ConnectTo(0));
        auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1"));
        SCHECK_EQ(value, "one", IllegalState, "Wrong row read through the local catalog copy");
        return Status::OK();
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
}

// The point of the feature, measured: the same cold-backend query costs master catalog read round
// trips when the copy is out of the read path and materially fewer when it is serving. Phase 7
// test 1 asks for "served counter increments, master catalog read count unchanged"; an exact zero
// is not assertable here because a fresh connection's own startup work -- authentication and the
// preload path, which YBCIsSysTablePrefetchingStarted keeps on the legacy catalog session and
// therefore off the copy -- reaches master regardless.
TEST_F(PgLocalCatalogTest, MasterCatalogReadsDropWhenServing) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'one')"));
  }

  // The query is run on a fresh connection each time so that its catalog lookups are misses.
  const auto measure = [this]() -> Result<int64_t> {
    const auto before = VERIFY_RESULT(MasterCatalogReadCount());
    auto conn = VERIFY_RESULT(ConnectTo(0));
    for (int i = 0; i < 5; ++i) {
      RETURN_NOT_OK(conn.Execute("DISCARD ALL"));
      auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1"));
      SCHECK_EQ(value, "one", IllegalState, "Wrong row read");
    }
    return VERIFY_RESULT(MasterCatalogReadCount()) - before;
  };

  const auto with_copy = ASSERT_RESULT(measure());

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  const auto without_copy = ASSERT_RESULT(measure());
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));

  LOG(INFO) << "Master catalog reads: with the copy serving " << with_copy
            << ", with it out of the read path " << without_copy;
  ASSERT_LT(with_copy, without_copy)
      << "Serving from the copy did not reduce master catalog reads. " << RoutingCounters(0);
}

// The core requirement, measured the way a remote region feels it: a fresh backend must not pay
// the master round trip for its catalog reads. TEST_local_catalog_master_read_delay_ms stands in
// for that round trip by sleeping before every catalog request this tserver sends to master, on
// both catalog paths, and not at all on a request the copy answered. With the copy serving, the
// time a fresh connection plus one query costs must therefore barely move when the delay is
// turned on.
TEST_F(PgLocalCatalogTest, FreshBackendDoesNotPayMasterCatalogLatency) {
  constexpr auto kDelayMs = 100;
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'one')"));
  }
  ASSERT_OK(WaitForAllServing());

  // A fresh connection and one query on it, which is the whole of what a remote client waits for.
  const auto measure = [this]() -> Result<MonoDelta> {
    const auto start = MonoTime::Now();
    auto conn = VERIFY_RESULT(ConnectTo(0));
    auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1"));
    SCHECK_EQ(value, "one", IllegalState, "Wrong row read");
    return MonoTime::Now() - start;
  };

  // Warm the cluster so that the first measurement is not paying one-time work.
  ASSERT_RESULT(measure());
  const auto without_delay = ASSERT_RESULT(measure());

  ASSERT_OK(cluster_->SetFlag(
      &TServer(0), "TEST_local_catalog_master_read_delay_ms", std::to_string(kDelayMs)));
  const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
  const auto with_delay = ASSERT_RESULT(measure());
  const auto served = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)) - served_before;
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_master_read_delay_ms", "0"));

  const auto added = with_delay - without_delay;
  const auto added_ms = added.ToMilliseconds();
  LOG(INFO) << "Fresh connection and one query: " << without_delay.ToMilliseconds()
            << " ms with no injected delay, " << with_delay.ToMilliseconds() << " ms with "
            << kDelayMs << " ms per master catalog request; added " << added_ms << " ms, which is "
            << (static_cast<double>(added_ms) / kDelayMs) << " master catalog requests. The copy "
            << "answered " << served << " requests in the same window. " << RoutingCounters(0);

  ASSERT_GT(served, 0) << "The copy answered nothing, so this measures the wrong thing. "
                       << RoutingCounters(0);
  // Not one whole round trip: with the copy serving, a fresh backend's catalog work must not
  // reach master at all. Half the injected delay is the allowance for measurement noise.
  ASSERT_LT(added_ms, kDelayMs / 2)
      << "A fresh backend paid " << (static_cast<double>(added_ms) / kDelayMs) << " master catalog "
      << "round trips while the copy was serving; the copy answered " << served << ". "
      << RoutingCounters(0);
}

// Phase 7 test 2: a DDL on one tserver is visible to a backend on another as soon as the DDL
// returns, and that backend's catalog misses are answered from its own copy. This is the exposure
// gate: the DDL's client is not told success until every tserver has acknowledged the lock
// release, and that acknowledgement waits for C to pass the time master read the new versions at.
TEST_F(PgLocalCatalogTest, CrossTserverDdlVisibleImmediately) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  // Warm B so the read below misses on the new table only.
  ASSERT_OK(conn_b.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));

  ASSERT_OK(conn_a.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_OK(conn_a.Execute("INSERT INTO t VALUES (7, 'seven')"));

  auto delta = ASSERT_RESULT(CounterDelta(
      1, METRIC_local_catalog_reads_served, [&conn_b]() -> Status {
        auto value = VERIFY_RESULT(conn_b.FetchRow<std::string>("SELECT v FROM t WHERE k = 7"));
        SCHECK_EQ(value, "seven", IllegalState, "Wrong row read after a cross-tserver DDL");
        return Status::OK();
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
}

// Phase 7 test 3. The session that ran the DDL reads its own catalog changes from its own
// tserver's copy. The version wait is in force for it -- the request carries the version this
// backend advanced at its own commit -- but it does not normally have anything to wait for,
// because the exposure gate runs on every tserver including this one and the release handler there
// is synchronous, so C had already passed the time master read the new versions at before the
// commit returned. The wait is therefore asserted to be available and correct rather than to fire;
// what it guards is the paths the gate does not cover, a version delivered by heartbeat while the
// copy is behind and a release acknowledged while the copy was not serving.
TEST_F(PgLocalCatalogTest, OwnTserverDdlServedLocally) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));
  const auto waited_before =
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_version));

  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_served, [&conn]() -> Status {
        RETURN_NOT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
        RETURN_NOT_OK(conn.Execute("INSERT INTO t VALUES (1, 'a')"));
        auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1"));
        SCHECK_EQ(value, "a", IllegalState, "Wrong row read after the session's own DDL");
        return Status::OK();
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
  ASSERT_GE(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_version)),
            waited_before);
}

// The version wait is not dead code, and this is the hole it closes. A version published while
// the copy was not serving was never gated, so when the copy starts serving again its C can be
// below the time master read that version at, and a backend already at that version would read a
// copy that lacks its rows.
//
// Pausing the applier alone cannot set this up: the release gate would then hold the DDL's commit,
// which is what GateHoldsDdlWhileApplierIsPaused demonstrates. Serving is disabled as well, which
// makes the gate publish the version immediately, and only then re-enabled with the applier still
// paused.
TEST_F(PgLocalCatalogTest, ReadWaitsForAVersionPublishedWhileNotServing) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));
  }

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_pause_applier", "true"));

  // The gate on tserver 0 publishes without waiting because its copy is not serving, so this
  // commits even though that copy is frozen and behind.
  {
    auto ddl_conn = ASSERT_RESULT(ConnectTo(1));
    ASSERT_OK(ddl_conn.Execute("CREATE TABLE raises_the_version (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(ddl_conn.Execute("INSERT INTO raises_the_version VALUES (1, 'v')"));
  }

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));
  const auto waited_before =
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_version));

  TestThreadHolder threads;
  std::atomic<bool> read_returned{false};
  Status read_status;
  std::string value;
  threads.AddThreadFunctor([this, &read_returned, &read_status, &value] {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    auto result = conn.FetchRow<std::string>("SELECT v FROM raises_the_version WHERE k = 1");
    read_status = ResultToStatus(result);
    if (result.ok()) {
      value = *result;
    }
    read_returned = true;
  });

  ASSERT_OK(LoggedWaitFor(
      [this, waited_before]() -> Result<bool> {
        auto waited = Counter(0, METRIC_local_catalog_reads_waited_for_version);
        return waited.ok() && *waited > waited_before;
      },
      120s, "A catalog read to wait for a version the copy has not applied"));
  ASSERT_FALSE(read_returned.load()) << "Read answered from a copy that lacks the version";

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_pause_applier", "false"));
  threads.JoinAll();
  ASSERT_OK(read_status);
  ASSERT_EQ(value, "v");
}

// A catalog miss inside a transaction block that has already written is served from the copy,
// even though the session's read point is by then the attached transaction's own read point. What
// makes that safe is the read point history: a catalog snapshot has its own read time serial
// number, and the switch to it saves the transaction's data read time away before the redirect
// writes C onto the object.
TEST_F(PgLocalCatalogTest, CatalogMissInTransactionBlockAfterAWrite) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE written (k INT PRIMARY KEY)"));
    ASSERT_OK(setup.Execute("CREATE TABLE cold (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO cold VALUES (1, 'c')"));
  }

  for (const auto* isolation : {"REPEATABLE READ", "READ COMMITTED"}) {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
    ASSERT_OK(conn.ExecuteFormat("BEGIN ISOLATION LEVEL $0", isolation));
    ASSERT_OK(conn.Execute("INSERT INTO written VALUES (1)"));
    ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM cold WHERE k = 1")), "c")
        << isolation;
    ASSERT_OK(conn.Execute("COMMIT"));
    ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
        << isolation << ": " << RoutingCounters(0);
    ASSERT_OK(conn.Execute("DELETE FROM written"));
  }
}

// The data snapshot of a REPEATABLE READ transaction survives a catalog read served from the copy.
// The redirect writes C onto the session read point, which with a transaction attached is the
// transaction's own read point object, so this is the test that would catch that write escaping:
// C is far below the transaction's read time, so a leak would make rows committed after the
// transaction started visible to it, or would fail the read outright.
TEST_F(PgLocalCatalogTest, RepeatableReadDataSnapshotSurvivesLocalCatalogRead) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE t (k INT PRIMARY KEY)"));
    ASSERT_OK(setup.Execute("INSERT INTO t SELECT generate_series(1, 10)"));
    ASSERT_OK(setup.Execute("CREATE TABLE w (k INT PRIMARY KEY)"));
    ASSERT_OK(setup.Execute("CREATE TABLE cold (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO cold VALUES (1, 'c')"));
  }

  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn_a.Execute("BEGIN ISOLATION LEVEL REPEATABLE READ"));
  const auto first_count = ASSERT_RESULT(conn_a.FetchRow<int64_t>("SELECT count(*) FROM t"));
  ASSERT_EQ(first_count, 10);

  // Committed after A fixed its data read time, so A must not see it until it commits.
  {
    auto conn_b = ASSERT_RESULT(ConnectTo(1));
    ASSERT_OK(conn_b.Execute("INSERT INTO t VALUES (11)"));
  }

  // The write attaches a distributed transaction, so from here the session read point is the
  // transaction's.
  ASSERT_OK(conn_a.Execute("INSERT INTO w VALUES (1)"));

  const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
  ASSERT_EQ(ASSERT_RESULT(conn_a.FetchRow<std::string>("SELECT v FROM cold WHERE k = 1")), "c");
  ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
      << RoutingCounters(0);

  ASSERT_EQ(ASSERT_RESULT(conn_a.FetchRow<int64_t>("SELECT count(*) FROM t")), first_count)
      << "The catalog read moved the transaction's data snapshot. " << RoutingCounters(0);
  ASSERT_OK(conn_a.Execute("COMMIT"));

  ASSERT_EQ(ASSERT_RESULT(conn_a.FetchRow<int64_t>("SELECT count(*) FROM t")), first_count + 1);
}

// Phase 7 test 5: after a DDL in a transaction block, every catalog read of that block goes to
// master, because the block's own uncommitted catalog rows exist only there.
TEST_F(PgLocalCatalogTest, InDdlTransactionBlockReadsMaster) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));

  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_to_master_in_ddl, [&conn]() -> Status {
        RETURN_NOT_OK(conn.Execute("BEGIN"));
        RETURN_NOT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
        RETURN_NOT_OK(conn.Execute("INSERT INTO t VALUES (3, 'three')"));
        auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 3"));
        SCHECK_EQ(value, "three", IllegalState, "Wrong row read inside the DDL transaction block");
        return conn.Execute("COMMIT");
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 3")), "three");
}

// Phase 7 test 6: a temporary relation's DDL increments no catalog version, so the per-operation
// version check cannot see it. The session's own-writes floor is what makes its next read wait for
// the copy to hold those rows.
//
// `DISCARD PLANS` is not enough to prove that: it drops the plan cache, not the relcache, so the
// select can be answered from catcache entries the CREATE itself populated. A second no-increment
// DDL on the same relation invalidates its relcache entry, which forces the rows to be read back.
TEST_F(PgLocalCatalogTest, TemporaryRelation) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TEMP TABLE tt (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO tt VALUES (1, 'x')"));

  const auto waited_before =
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_own_writes));
  ASSERT_OK(conn.Execute("ALTER TABLE tt ADD COLUMN c INT DEFAULT 7"));
  ASSERT_OK(conn.Execute("DISCARD PLANS"));

  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM tt WHERE k = 1")), "x");
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>("SELECT c FROM tt WHERE k = 1")), 7);
  ASSERT_GT(ASSERT_RESULT(conn.FetchRow<int64_t>(
                "SELECT count(*) FROM pg_attribute WHERE attrelid = 'tt'::regclass")),
            0);
  ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_own_writes)),
            waited_before)
      << "The own-writes floor did not fire for a DDL that incremented no catalog version: "
      << RoutingCounters(0);
}

// A DDL that does increment a catalog version must not pay the own-writes floor: the exposure gate
// has already pushed C past that version's read time, so the session's next statement has nothing
// to wait for.
TEST_F(PgLocalCatalogTest, VersionIncrementingDdlDoesNotPayTheOwnWritesFloor) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));

  const auto waited_before =
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_own_writes));
  ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'a')"));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1")), "a");
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_own_writes)),
            waited_before)
      << RoutingCounters(0);
}

// Phase 7 tests 4 and 9, rewritten around what PG actually does here. A repeatable-read
// transaction does not hold one catalog snapshot across statements: accepting invalidation
// messages drops the catalog snapshot, so a table another session creates mid-transaction does
// become visible to a later `SELECT count(*) FROM pg_class` in the same transaction. The property
// the copy owes is therefore not stability across statements but agreement with master, so the
// same sequence is run twice, once answered from the copy and once with the copy taken out of the
// read path, and the two must give the same answers.
TEST_F(PgLocalCatalogTest, UserCatalogReadMatchesMaster) {
  ASSERT_OK(WaitForAllServing());

  // (before, after) counts of pg_class around a table created by another session.
  using BeforeAfter = std::pair<int64_t, int64_t>;
  const auto run_sequence = [this](const std::string& suffix) -> Result<BeforeAfter> {
    auto conn_a = VERIFY_RESULT(ConnectTo(0));
    auto conn_b = VERIFY_RESULT(ConnectTo(1));
    RETURN_NOT_OK(conn_a.Execute("BEGIN ISOLATION LEVEL REPEATABLE READ"));
    auto before = VERIFY_RESULT(conn_a.FetchRow<int64_t>("SELECT count(*) FROM pg_class"));
    RETURN_NOT_OK(conn_b.ExecuteFormat("CREATE TABLE added_$0 (k INT PRIMARY KEY)", suffix));
    auto after = VERIFY_RESULT(conn_a.FetchRow<int64_t>("SELECT count(*) FROM pg_class"));
    RETURN_NOT_OK(conn_a.Execute("COMMIT"));
    return BeforeAfter{before, after};
  };

  int64_t served_delta = 0;
  BeforeAfter local{};
  {
    const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
    local = ASSERT_RESULT(run_sequence("local"));
    served_delta = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)) - served_before;
  }
  ASSERT_GT(served_delta, 0) << RoutingCounters(0);

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  auto from_master = ASSERT_RESULT(run_sequence("master"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));

  // Each sequence adds one table and its primary key index to pg_class, so the second run starts
  // two rows higher. Compare the deltas the sequence observed, not the absolute counts.
  ASSERT_EQ(local.second - local.first, from_master.second - from_master.first);
  ASSERT_EQ(from_master.first - local.first, local.second - local.first);
}

// Phase 7 test 7: catalog scans PG issues internally while building a relcache entry, here the
// check constraint's pg_constraint scan, are answered from the copy and the constraint is
// enforced on a tserver that never ran the DDL.
TEST_F(PgLocalCatalogTest, InternalCatalogReadsCheckConstraint) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn_b.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));
  ASSERT_OK(conn_a.Execute("CREATE TABLE c (k INT PRIMARY KEY, v INT CHECK (v > 10))"));

  ASSERT_OK(conn_b.Execute("INSERT INTO c VALUES (1, 20)"));
  auto status = conn_b.Execute("INSERT INTO c VALUES (2, 5)");
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "violates check constraint");
}

// A rolled back subtransaction's catalog rows must not become visible in the copy. The apply
// record carries the aborted subtransaction set, and the apply drops those intents.
TEST_F(PgLocalCatalogTest, RolledBackSubtransactionIsNotVisible) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_OK(conn.Execute("CREATE TABLE kept (k INT PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("SAVEPOINT s"));
  ASSERT_OK(conn.Execute("CREATE TABLE rolled_back (k INT PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("ROLLBACK TO s"));
  ASSERT_OK(conn.Execute("COMMIT"));

  auto other = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(other.Execute("INSERT INTO kept VALUES (1)"));
  auto status = other.Execute("INSERT INTO rolled_back VALUES (1)");
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "does not exist");
}

// CREATE DATABASE adds a whole set of cotables to master's system catalog tablet through
// schema-change records. The copy must mirror them, or a connection to the new database on another
// tserver cannot read its own catalog.
TEST_F(PgLocalCatalogTest, NewDatabaseCotablesReachTheCopy) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE DATABASE fresh_db"));
  }
  auto conn = ASSERT_RESULT(ConnectTo(1, "fresh_db"));
  ASSERT_OK(conn.Execute("CREATE TABLE in_new_db (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO in_new_db VALUES (1, 'z')"));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM in_new_db WHERE k = 1")), "z");
}

// CREATE INDEX updates pg_class.relhasindex of the indexed table in place, outside the DDL's own
// transaction. That update is an ordinary write in master's WAL, so the copy must show the index
// to a planner on another tserver.
TEST_F(PgLocalCatalogTest, NewIndexIsUsedByAnotherTserver) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn_a.Execute("CREATE TABLE t (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn_a.Execute("INSERT INTO t SELECT i, i FROM generate_series(1, 500) i"));
  ASSERT_OK(conn_b.FetchRow<int64_t>("SELECT count(*) FROM t"));

  ASSERT_OK(conn_a.Execute("CREATE INDEX t_v_idx ON t (v)"));
  auto plan = ASSERT_RESULT(conn_b.FetchAllAsString("EXPLAIN SELECT v FROM t WHERE v = 42"));
  ASSERT_STR_CONTAINS(plan, "Index");
}

// The same, in the session that created the index: its next statement must plan against the new
// index, which requires its own tserver's copy to hold the index rows before the read is answered.
TEST_F(PgLocalCatalogTest, NewIndexIsUsedBySameSession) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute("INSERT INTO t SELECT i, i FROM generate_series(1, 500) i"));
  ASSERT_OK(conn.Execute("CREATE INDEX t_v_idx ON t (v)"));
  auto plan = ASSERT_RESULT(conn.FetchAllAsString("EXPLAIN SELECT v FROM t WHERE v = 42"));
  ASSERT_STR_CONTAINS(plan, "Index");
}

// A shared catalog's rows reach every database's view of the copy: CREATE ROLE on one tserver must
// be usable for a privilege check on another.
TEST_F(PgLocalCatalogTest, SharedCatalogReachesOtherTservers) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn_a.Execute("CREATE TABLE t (k INT PRIMARY KEY)"));
  ASSERT_OK(conn_a.Execute("CREATE ROLE reader_role"));
  ASSERT_OK(conn_b.Execute("GRANT SELECT ON t TO reader_role"));
  ASSERT_TRUE(ASSERT_RESULT(conn_b.FetchRow<bool>(
      "SELECT has_table_privilege('reader_role', 't', 'SELECT')")));
}

// A view's rewrite rules live in pg_rewrite, which the copy holds like any other catalog table.
TEST_F(PgLocalCatalogTest, ViewDefinedOnAnotherTserver) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn_a.Execute("CREATE TABLE t (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn_a.Execute("INSERT INTO t VALUES (1, 10), (2, 20)"));
  ASSERT_OK(conn_a.Execute("CREATE VIEW v AS SELECT k FROM t WHERE v > 15"));
  auto rows = ASSERT_RESULT(conn_b.FetchRows<int32_t>("SELECT k FROM v ORDER BY k"));
  ASSERT_EQ(rows, (decltype(rows){2}));
}

// A user-defined type resolved at parse time comes from pg_type, and ADD COLUMN with a wide
// default stores that default in pg_attribute's attmissingval, a value the copy must carry byte
// for byte.
TEST_F(PgLocalCatalogTest, TypeAndMissingValueAcrossTservers) {
  ASSERT_OK(WaitForAllServing());
  auto conn_a = ASSERT_RESULT(ConnectTo(0));
  auto conn_b = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn_a.Execute("CREATE TYPE color AS ENUM ('red', 'green')"));
  ASSERT_OK(conn_a.Execute("CREATE TABLE t (k INT PRIMARY KEY, c color)"));
  ASSERT_OK(conn_b.Execute("INSERT INTO t VALUES (1, 'green')"));

  ASSERT_OK(conn_a.Execute(
      "ALTER TABLE t ADD COLUMN note TEXT DEFAULT repeat('abcdefghij', 40)"));
  auto note = ASSERT_RESULT(conn_b.FetchRow<std::string>("SELECT note FROM t WHERE k = 1"));
  ASSERT_EQ(note.size(), 400);
}

// Phase 7 test 15: pushes that fail to apply are counted and leave C where it was; the copy keeps
// serving reads at that C, and master's retries move it once the failures stop. A catalog read at
// an explicit time above C is what asks master to push; with the copy failing, that read gives up
// after local_catalog_serve_wait_timeout_ms and reads master.
TEST_F(PgLocalCatalogTest, PushFailuresDoNotMoveSafeTime) {
  ASSERT_OK(WaitForAllServing());
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "local_catalog_serve_wait_timeout_ms", "2000"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_fail_push", "true"));
  const auto failures_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_apply_failures));
  const auto safe_time_frozen = ASSERT_RESULT(Counter(0, METRIC_local_catalog_safe_time_micros));

  const auto read_above_c = [this]() -> Status {
    auto conn = VERIFY_RESULT(ConnectTo(0));
    RETURN_NOT_OK(conn.ExecuteFormat("SET yb_read_time = $0", GetCurrentTimeMicros()));
    return ResultToStatus(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_am"));
  };
  ASSERT_OK(read_above_c());
  ASSERT_OK(LoggedWaitFor(
      [this, failures_before]() -> Result<bool> {
        return VERIFY_RESULT(Counter(0, METRIC_local_catalog_apply_failures)) >
               failures_before + 1;
      },
      60s, "Failed pushes to be counted"));
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_safe_time_micros)), safe_time_frozen);
  ASSERT_EQ(
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_serving_state)), kServingStateServing);

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_fail_push", "false"));
  ASSERT_OK(LoggedWaitFor(
      [this, safe_time_frozen]() -> Result<bool> {
        auto now = Counter(0, METRIC_local_catalog_safe_time_micros);
        return now.ok() && *now > safe_time_frozen;
      },
      60s, "C to advance again after pushes succeed"));
}

// Phase 4's gate and phase 7 test 16: while the applier cannot advance C, a DDL's commit cannot
// return, because the lock release it waits on cannot publish the new catalog versions until the
// copy holds them.
TEST_F(PgLocalCatalogTest, GateHoldsDdlWhileApplierIsPaused) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));
  }
  const auto gate_waits_before =
      ASSERT_RESULT(EventStatsCount(0, METRIC_local_catalog_gate_wait_us));

  ASSERT_OK(SetOnAllTServers("TEST_local_catalog_pause_applier", "true"));

  TestThreadHolder threads;
  std::atomic<bool> ddl_returned{false};
  Status ddl_status;
  threads.AddThreadFunctor([this, &ddl_returned, &ddl_status] {
    auto ddl_conn = ASSERT_RESULT(ConnectTo(0));
    ddl_status = ddl_conn.Execute("CREATE TABLE gated (k INT PRIMARY KEY)");
    ddl_returned = true;
  });

  // The DDL must still be in flight. Two seconds is far longer than the push it would otherwise
  // take.
  SleepFor(2s * kTimeMultiplier);
  ASSERT_FALSE(ddl_returned.load()) << "DDL returned while every local catalog copy was frozen";

  ASSERT_OK(SetOnAllTServers("TEST_local_catalog_pause_applier", "false"));
  threads.JoinAll();
  ASSERT_OK(ddl_status);
  ASSERT_GT(
      ASSERT_RESULT(EventStatsCount(0, METRIC_local_catalog_gate_wait_us)), gate_waits_before);
}

// Phase 7 test 13: a restarted tserver reopens the copy it already has and resumes from the
// position it persisted, without fetching a new copy.
TEST_F(PgLocalCatalogTest, RestartResumesWithoutReseed) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'before restart')"));
  }
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reseeds)), 0);

  TServer(0).Shutdown();
  ASSERT_OK(TServer(0).Restart());
  ASSERT_OK(WaitForServing(0));
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reseeds)), 0);

  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM t WHERE k = 1")), "before restart");

  // A DDL after the restart still reaches this tserver's copy.
  ASSERT_OK(conn.Execute("CREATE TABLE after_restart (k INT PRIMARY KEY)"));
  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_served, [this]() -> Status {
        auto fresh = VERIFY_RESULT(ConnectTo(0));
        return ResultToStatus(fresh.FetchRow<int64_t>("SELECT count(*) FROM after_restart"));
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
}

// Serving is a precondition for the redirect, not an assumption: with it turned off the same
// queries are answered by master and stay correct.
TEST_F(PgLocalCatalogTest, ServingDisabledRoutesToMaster) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 'y')"));
  }

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_to_master_not_serving, [this]() -> Status {
        auto fresh = VERIFY_RESULT(ConnectTo(0));
        auto value = VERIFY_RESULT(fresh.FetchRow<std::string>("SELECT v FROM t WHERE k = 1"));
        SCHECK_EQ(value, "y", IllegalState, "Wrong row read with local serving disabled");
        return Status::OK();
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));
}

// Phase 7 test 14: master no longer retains the WAL the copy needs, so the copy cannot catch up
// incrementally and only a fresh copy of master's tablet makes it current again. The answer is
// injected rather than produced by shrinking master's retention, because doing that far enough to
// lose a stopped copy's position also disturbs every other tablet in the cluster; the injected
// answer takes the same re-seed branch a reseed_required page takes.
TEST_F(PgLocalCatalogTest, CheckpointTooOldReseeds) {
  ASSERT_OK(WaitForAllServing());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE before_reseed (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO before_reseed VALUES (1, 'kept')"));
  }
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reseeds)), 0);

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_push_reseed", "true"));
  // The next pages master sends tserver 0 are treated as a re-seed answer; a DDL elsewhere sends
  // some.
  {
    auto conn = ASSERT_RESULT(ConnectTo(1));
    ASSERT_OK(conn.Execute("CREATE TABLE triggers_a_push (k INT PRIMARY KEY)"));
  }
  ASSERT_OK(LoggedWaitFor(
      [this]() -> Result<bool> {
        auto reseeds = Counter(0, METRIC_local_catalog_reseeds);
        return reseeds.ok() && *reseeds > 0;
      },
      120s, "The copy to be re-seeded after CHECKPOINT_TOO_OLD"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_push_reseed", "false"));

  ASSERT_OK(WaitForServing(0));
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM before_reseed WHERE k = 1")),
      "kept");

  // The fresh copy is current, not merely present: a DDL after the re-seed is served from it.
  ASSERT_OK(conn.Execute("CREATE TABLE after_reseed (k INT PRIMARY KEY)"));
  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_served, [this]() -> Status {
        auto fresh = VERIFY_RESULT(ConnectTo(0));
        return ResultToStatus(fresh.FetchRow<int64_t>("SELECT count(*) FROM after_reseed"));
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
}

// A DDL transaction that is open while a copy is re-seeded has written catalog rows that the copy
// receives through the remote bootstrap, as intents in its own tablet, and not as records from the
// applier. Only the transaction's later writes and its APPLY record arrive in pushed pages. The
// DDL's rows must still be readable on that tserver once the DDL has committed. Disabled: the
// copy does not apply those intents yet (notes/todos.md T3(b)).
TEST_F(PgLocalCatalogTest, DISABLED_DdlOpenAcrossReseedIsVisibleAfterCommit) {
  ASSERT_OK(WaitForAllServing());
  auto ddl_conn = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(ddl_conn.Execute("BEGIN"));
  ASSERT_OK(ddl_conn.Execute("CREATE TABLE open_across_reseed (k INT PRIMARY KEY, v TEXT)"));

  const auto reseeds_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reseeds));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_push_reseed", "true"));
  ASSERT_OK(LoggedWaitFor(
      [this, reseeds_before]() -> Result<bool> {
        auto reseeds = Counter(0, METRIC_local_catalog_reseeds);
        return reseeds.ok() && *reseeds > reseeds_before;
      },
      120s, "The copy to be re-seeded after CHECKPOINT_TOO_OLD"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_push_reseed", "false"));
  ASSERT_OK(WaitForServing(0));

  ASSERT_OK(ddl_conn.Execute("INSERT INTO open_across_reseed VALUES (1, 'one')"));
  ASSERT_OK(ddl_conn.Execute("COMMIT"));
  // The coordinator forgets the transaction once master has applied it, and then answers a status
  // request for it with ABORTED.
  SleepFor(5s * kTimeMultiplier);

  auto from_copy = ASSERT_RESULT(ConnectTo(0));
  auto copy_result = from_copy.FetchRow<std::string>("SELECT v FROM open_across_reseed");
  const auto copy_counters = RoutingCounters(0);

  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  auto from_master = ASSERT_RESULT(ConnectTo(0));
  auto master_result = from_master.FetchRow<std::string>("SELECT v FROM open_across_reseed");
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));

  ASSERT_EQ(ASSERT_RESULT(std::move(master_result)), "one");
  ASSERT_TRUE(copy_result.ok()) << copy_result.status() << "; " << copy_counters;
  ASSERT_EQ(*copy_result, "one") << copy_counters;
}

// A session that writes catalog rows outside a DDL, the way a YSQL upgrade's migrations do, can
// hold uncommitted catalog rows that exist only on master, so its catalog reads go to master.
TEST_F(PgLocalCatalogTest, NonDdlSysTableSessionReadsMaster) {
  ASSERT_OK(WaitForAllServing());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (k INT PRIMARY KEY)"));

  auto delta = ASSERT_RESULT(CounterDelta(
      0, METRIC_local_catalog_reads_to_master_in_ddl, [&conn]() -> Status {
        RETURN_NOT_OK(conn.Execute("SET yb_non_ddl_txn_for_sys_tables_allowed = ON"));
        RETURN_NOT_OK(ResultToStatus(
            conn.FetchRow<int64_t>("SELECT count(*) FROM pg_class WHERE relname = 'warmup'")));
        return conn.Execute("SET yb_non_ddl_txn_for_sys_tables_allowed = OFF");
      }));
  ASSERT_GT(delta, 0) << RoutingCounters(0) << "; " << RoutingCounters(1);
}

// Phase 7 test 17: a DDL transaction block that is left open must not hold up another session's
// DDL, nor the catalog reads that follow it. Master certifies a safe time by resolving intents
// (TransactionParticipant::ResolveIntents), and a transaction that is still open is reported
// PENDING and dropped from the set once the status tablet's clock passes the resolve time
// (transaction_participant.cc:1377-1379), so the copy's C advances past the second DDL's commit
// while the first block is still open. If that were not so, the exposure gate would make every
// DDL wait for every open DDL block anywhere in the cluster.
class PgLocalCatalogOpenBlockTest : public PgLocalCatalogTest {
 protected:
  // A ceiling far above a few push round trips and far below the idle period the open block would
  // impose if it did serialize the second DDL.
  static constexpr auto kSecondDdlCeiling = 30s;

  void RunOpenBlockDoesNotBlock(size_t x_ts, size_t y_ts, int ddls_in_x, const std::string& tag) {
    constexpr size_t kReaderTs = 2;
    auto x = ASSERT_RESULT(ConnectTo(x_ts));
    ASSERT_OK(x.Execute("BEGIN"));
    for (int i = 0; i < ddls_in_x; ++i) {
      ASSERT_OK(x.ExecuteFormat("CREATE TABLE a_$0_$1 (k INT PRIMARY KEY, v TEXT)", tag, i));
      ASSERT_OK(x.ExecuteFormat("INSERT INTO a_$0_$1 VALUES (1, 'a')", tag, i));
    }

    // X now holds its catalog writes uncommitted and issues nothing further.
    auto y = ASSERT_RESULT(ConnectTo(y_ts));
    const auto y_start = MonoTime::Now();
    ASSERT_OK(y.ExecuteFormat("CREATE TABLE b_$0 (k INT PRIMARY KEY, v TEXT)", tag));
    ASSERT_OK(y.ExecuteFormat("INSERT INTO b_$0 VALUES (1, 'b')", tag));
    const auto y_elapsed = MonoTime::Now() - y_start;
    ASSERT_LT(y_elapsed, kSecondDdlCeiling)
        << tag << ": the second DDL waited for the open block. " << RoutingCounters(y_ts);

    // C must pass the second DDL's commit on every tserver while X is still open.
    const auto after_b_micros = GetCurrentTimeMicros();
    for (int i = 0; i < GetNumTabletServers(); ++i) {
      ASSERT_OK(WaitForSafeTimePast(i, after_b_micros, tag));
    }
    ASSERT_EQ(ASSERT_RESULT(x.FetchRow<int32_t>("SELECT 1")), 1)
        << tag << ": the open block did not survive the second DDL";

    // A cold backend elsewhere reads the second DDL's table from its own copy.
    {
      auto cold = ASSERT_RESULT(ConnectTo(kReaderTs));
      const auto served_before =
          ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served));
      ASSERT_EQ(
          ASSERT_RESULT(
              cold.FetchRow<std::string>(Format("SELECT v FROM b_$0 WHERE k = 1", tag))),
          "b");
      ASSERT_GT(ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served)), served_before)
          << tag << ": " << RoutingCounters(kReaderTs);
    }

    ASSERT_OK(x.Execute("COMMIT"));

    // And after the block commits, its own tables are readable from a copy too.
    {
      auto cold = ASSERT_RESULT(ConnectTo(kReaderTs));
      const auto served_before =
          ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served));
      ASSERT_EQ(
          ASSERT_RESULT(
              cold.FetchRow<std::string>(Format("SELECT v FROM a_$0_0 WHERE k = 1", tag))),
          "a");
      ASSERT_GT(ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served)), served_before)
          << tag << ": " << RoutingCounters(kReaderTs);
    }
  }
};

TEST_F(PgLocalCatalogOpenBlockTest, OpenDdlBlockDoesNotBlockAnotherTserversDdl) {
  ASSERT_OK(WaitForAllServing());
  RunOpenBlockDoesNotBlock(/* x_ts= */ 0, /* y_ts= */ 1, /* ddls_in_x= */ 1, "cross");
}

TEST_F(PgLocalCatalogOpenBlockTest, OpenDdlBlockDoesNotBlockADdlOnTheSameTserver) {
  ASSERT_OK(WaitForAllServing());
  RunOpenBlockDoesNotBlock(/* x_ts= */ 0, /* y_ts= */ 0, /* ddls_in_x= */ 1, "same");
}

TEST_F(PgLocalCatalogOpenBlockTest, TwoOpenDdlsDoNotBlockAnotherTserversDdl) {
  ASSERT_OK(WaitForAllServing());
  RunOpenBlockDoesNotBlock(/* x_ts= */ 0, /* y_ts= */ 1, /* ddls_in_x= */ 2, "two");
}

// Phase 7 test 18: two DDLs issued at the same moment from two tservers, on different tables, ten
// times over. Both must return, both tables must be readable from a third tserver's copy, and
// every copy must end at the same catalog version as master.
TEST_F(PgLocalCatalogTest, ConcurrentDdlsFromTwoTserversConverge) {
  ASSERT_OK(WaitForAllServing());
  constexpr int kIterations = 10;
  constexpr size_t kReaderTs = 2;

  for (int i = 0; i < kIterations; ++i) {
    auto conn_p = ASSERT_RESULT(ConnectTo(0));
    auto conn_q = ASSERT_RESULT(ConnectTo(1));
    Status status_p, status_q;
    {
      TestThreadHolder threads;
      threads.AddThreadFunctor([&conn_p, &status_p, i] {
        status_p = conn_p.ExecuteFormat("CREATE TABLE p_$0 (k INT PRIMARY KEY, v TEXT)", i);
        if (status_p.ok()) {
          status_p = conn_p.ExecuteFormat("INSERT INTO p_$0 VALUES (1, 'p')", i);
        }
      });
      threads.AddThreadFunctor([&conn_q, &status_q, i] {
        status_q = conn_q.ExecuteFormat("CREATE TABLE q_$0 (k INT PRIMARY KEY, v TEXT)", i);
        if (status_q.ok()) {
          status_q = conn_q.ExecuteFormat("INSERT INTO q_$0 VALUES (1, 'q')", i);
        }
      });
      threads.JoinAll();
    }
    ASSERT_TRUE(status_p.ok()) << "iteration " << i << ": " << status_p;
    ASSERT_TRUE(status_q.ok()) << "iteration " << i << ": " << status_q;

    auto reader = ASSERT_RESULT(ConnectTo(kReaderTs));
    const auto served_before =
        ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served));
    ASSERT_EQ(
        ASSERT_RESULT(reader.FetchRow<std::string>(Format("SELECT v FROM p_$0 WHERE k = 1", i))),
        "p");
    ASSERT_EQ(
        ASSERT_RESULT(reader.FetchRow<std::string>(Format("SELECT v FROM q_$0 WHERE k = 1", i))),
        "q");
    ASSERT_GT(ASSERT_RESULT(Counter(kReaderTs, METRIC_local_catalog_reads_served)), served_before)
        << "iteration " << i << ": " << RoutingCounters(kReaderTs);
  }

  // Master's own value, read with this tserver's copy out of the read path.
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  int64_t master_version = 0;
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    master_version = ASSERT_RESULT(CatalogVersion(&conn));
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));

  // Every copy converges on it.
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    auto conn = ASSERT_RESULT(ConnectTo(i));
    ASSERT_OK(LoggedWaitFor(
        [this, &conn, master_version]() -> Result<bool> {
          auto version = CatalogVersion(&conn);
          return version.ok() && *version == master_version;
        },
        kServingTimeout,
        Format("Copy on tserver $0 to reach catalog version $1", i, master_version)));
  }
}

// Phase 7 test 19: an explicit read time decides where the read goes. At or below C the copy can
// answer at exactly that time; above C it cannot, because reading at C instead would answer a
// different question; below the copy's history retention it cannot either, because the versions
// that old are gone.
TEST_F(PgLocalCatalogTest, ExplicitReadTimeRouting) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE rt (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO rt VALUES (1, 'r')"));
  }
  // A margin so that the read time used below is unambiguously after the table's creation.
  const auto after_setup_micros = GetCurrentTimeMicros() + 2000000;
  ASSERT_OK(WaitForSafeTimePast(0, after_setup_micros, "setup"));

  const auto c_micros = ASSERT_RESULT(Counter(0, METRIC_local_catalog_safe_time_micros));

  // At or below C: served from the copy at that time.
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
    ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", c_micros - 1000));
    ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM rt WHERE k = 1")), "r");
    ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
        << "A read time below C was not served locally. " << RoutingCounters(0);
  }

  const auto read_at = [this](int64_t micros) -> Result<bool> {
    auto conn = VERIFY_RESULT(ConnectTo(0));
    RETURN_NOT_OK(conn.ExecuteFormat("SET yb_read_time = $0", micros));
    const auto result = conn.FetchRow<std::string>("SELECT v FROM rt WHERE k = 1");
    LOG(INFO) << "Read at " << micros << ": "
              << (result.ok() ? "ok" : result.status().ToString());
    return result.ok();
  };

  // Above C: the copy asks master for a push up to the read time and serves once it holds it.
  {
    const auto floors_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_floor_requests_sent));
    const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
    const auto to_master_before =
        ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving));
    ASSERT_TRUE(ASSERT_RESULT(read_at(GetCurrentTimeMicros())));
    ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_floor_requests_sent)), floors_before);
    ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
        << "A read time above C was not served after the push. " << RoutingCounters(0);
    ASSERT_EQ(
        ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving)),
        to_master_before)
        << RoutingCounters(0);
  }

  // Above C with the copy unable to apply: master answers once the wait runs out.
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "local_catalog_serve_wait_timeout_ms", "1000"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_pause_applier", "true"));
  {
    const auto to_master_before =
        ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving));
    ASSERT_TRUE(ASSERT_RESULT(read_at(GetCurrentTimeMicros())));
    ASSERT_GT(
        ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving)),
        to_master_before)
        << "A read time above C was not routed to master. " << RoutingCounters(0);
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_pause_applier", "false"));
}

// A tserver shut down while a push RPC waits for its paused apply thread completes the push and
// stops cleanly, without destroying the applier under the waiting RPC thread.
TEST_F(PgLocalCatalogTest, ShutdownWhilePushWaits) {
  ASSERT_OK(WaitForAllServing());
  // Not serving, so the lock release of the DDL below does not wait for the paused copy.
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_pause_applier", "true"));
  const auto received = ASSERT_RESULT(Counter(0, METRIC_local_catalog_pushes_received));
  auto conn = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn.Execute("CREATE TABLE shutdown_push (a INT)"));
  ASSERT_OK(LoggedWaitFor(
      [&]() -> Result<bool> {
        return VERIFY_RESULT(Counter(0, METRIC_local_catalog_pushes_received)) > received;
      },
      30s, "A push to reach the paused tserver"));
  const auto start = CoarseMonoClock::Now();
  TServer(0).Shutdown(SafeShutdown::kTrue, RequireExitCode0::kTrue);
  ASSERT_LT(CoarseMonoClock::Now() - start, 10s);
}

// The copy retains system catalog history for
// timestamp_syscatalog_history_retention_interval_sec, the interval master applies to this same
// data, and not for the user tablet interval timestamp_history_retention_interval_sec. This
// fixture shrinks the user tablet interval to a few seconds and reads at a time older than it,
// which the copy must still answer.
//
// The test's power is one-sided. It holds under the system catalog interval whatever else
// happens, because that interval bounds the cutoff directly. Under the user tablet interval it
// would only fail once a flush or a compaction folded the shorter interval into the committed
// cutoff, and an external cluster test cannot force either on this tablet: the copy is invisible
// to master, so there is no admin path to it.
class PgLocalCatalogShortRetentionTest : public PgLocalCatalogTest {
 protected:
  static constexpr auto kUserTabletRetention = 5;

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgLocalCatalogTest::UpdateMiniClusterOptions(options);
    options->extra_tserver_flags.push_back(
        Format("--timestamp_history_retention_interval_sec=$0", kUserTabletRetention));
  }
};

TEST_F(PgLocalCatalogShortRetentionTest, ReadOlderThanUserTabletRetentionIsServedLocally) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE rt (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO rt VALUES (1, 'r')"));
  }
  // The read time below has to be after the table exists and before the user tablet interval, so
  // enough time has to pass for both to hold at once.
  const auto read_micros = GetCurrentTimeMicros();
  SleepFor(MonoDelta::FromSeconds(3 * kUserTabletRetention));

  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
  ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", read_micros));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM rt WHERE k = 1")), "r");
  ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
      << "A read older than the user tablet retention was not served from the copy. "
      << RoutingCounters(0);
}

// A read time below the copy's own history cutoff is the one case the redirect declines for
// retention rather than for state, and it must leave the copy for master, which keeps four hours
// of the same history. Both retention intervals are shortened and the memtable is made small,
// because the cutoff the reader gate consults advances only when a flush or a compaction calls
// TabletRetentionPolicy::GetRetentionDirective, and an external cluster test has no admin path to
// this tablet with which to force one.
class PgLocalCatalogTightCutoffTest : public PgLocalCatalogTest {
 protected:
  static constexpr auto kRetentionSec = 5;

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgLocalCatalogTest::UpdateMiniClusterOptions(options);
    options->extra_tserver_flags.push_back(
        Format("--timestamp_history_retention_interval_sec=$0", kRetentionSec));
    options->extra_tserver_flags.push_back(
        Format("--timestamp_syscatalog_history_retention_interval_sec=$0", kRetentionSec));
    options->extra_tserver_flags.push_back("--memstore_size_mb=1");
    // The cutoff the reader gate consults only moves when a flush calls GetRetentionDirective, and
    // waiting for a 1 MB memtable to fill from catalog writes alone made this test depend on how
    // much churn the DDLs below happen to produce. 64 KB is reached by a handful of them.
    options->extra_tserver_flags.push_back("--db_write_buffer_size=65536");
  }
};

TEST_F(PgLocalCatalogTightCutoffTest, ReadBelowTheCopysCutoffGoesToMaster) {
  ASSERT_OK(WaitForAllServing());
  // Only catalog tables are read below, because the user tablets on this tserver keep the same
  // few seconds of history and would reject the read on their own.
  const auto read_micros = GetCurrentTimeMicros();

  // Catalog writes, so that the copy's memtable passes memstore_size_mb and the flush that
  // follows advances its committed history cutoff past the read time taken above.
  {
    auto churn = ASSERT_RESULT(ConnectTo(1));
    for (int i = 0; i < 60; ++i) {
      ASSERT_OK(churn.ExecuteFormat("CREATE TABLE churn_$0 (k INT PRIMARY KEY, v TEXT)", i));
      ASSERT_OK(churn.ExecuteFormat("DROP TABLE churn_$0", i));
    }
  }
  SleepFor(MonoDelta::FromSeconds(4 * kRetentionSec));

  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto to_master_before =
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving));
  ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", read_micros));
  const auto relations = ASSERT_RESULT(conn.FetchRow<int64_t>(
      "SELECT count(*) FROM pg_catalog.pg_class WHERE relnamespace = 'pg_catalog'::regnamespace"));
  ASSERT_GT(relations, 0);
  ASSERT_GT(
      ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_to_master_not_serving)),
      to_master_before)
      << "A read below the copy's cutoff was not routed to master. " << RoutingCounters(0);
}

// The copy must never reach master's tablet report. Master has no record of its tablet id and
// never can: master's own tablet ids come from GenerateObjectId, which is a v4 UUID rendered as
// hex, so the id used here is outside the space master can generate. A reported id master does not
// know is classified as an orphaned replica, and with the orphan deletion check turned off master
// would order the reporting tserver to delete it. The report is built from tablet_map_, so the
// guard is that the copy is not in that map, which this test observes through ListTablets: the
// handler enumerates tablet_map_ (tablet_service.cc:3292-3296).
TEST_F(PgLocalCatalogTest, LocalCatalogCopyIsNotInTheReportableTabletSet) {
  ASSERT_OK(WaitForAllServing());
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    auto tablets =
        ASSERT_RESULT(cluster_->ListTablets(&TServer(i), /* user_tablets_only= */ false));
    for (const auto& entry : tablets.status_and_schema()) {
      ASSERT_NE(entry.tablet_status().tablet_id(), kLocalCatalogTabletId)
          << "The local catalog copy is in tserver " << i << "'s tablet map, so it would be "
          << "offered to master in the tablet report";
    }
  }
}

// The other half of the same concern, from master's side: with the orphan deletion check disabled,
// a full tablet report must still leave the copy alone. A tserver sends a full report when it
// re-registers after a restart.
TEST_F(PgLocalCatalogTest, FullReportWithOrphanDeletionCheckOffLeavesTheCopyAlone) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE orphan_probe (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO orphan_probe VALUES (1, 'p')"));
  }
  for (size_t i = 0; i < cluster_->num_masters(); ++i) {
    ASSERT_OK(cluster_->SetFlag(
        cluster_->master(i), "master_enable_deletion_check_for_orphaned_tablets", "false"));
  }

  TServer(0).Shutdown();
  ASSERT_OK(TServer(0).Restart());
  ASSERT_OK(WaitForServing(0));
  // Several heartbeats, so that the full report is processed and any delete it provoked would have
  // arrived.
  SleepFor(5s * kTimeMultiplier);

  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_serving_state)), kServingStateServing)
      << RoutingCounters(0);
  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM orphan_probe WHERE k = 1")), "p");
  ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
      << "The copy stopped serving after a full tablet report. " << RoutingCounters(0);
}

// A measurement, not an invariant: how long the first catalog-missing query costs on a freshly
// restarted tserver, with the copy in the read path and with it out. The numbers are logged rather
// than asserted, because a latency bound would be flaky on shared hardware. What the breakdown
// separates is the copy's own first-touch cost (cold block cache on the copied SSTs, plus any
// version wait) from the cost every cold backend pays regardless (authentication, the relcache
// build, and the catalog preload, all of which stay on the legacy catalog session and go to
// master).
TEST_F(PgLocalCatalogTest, FirstQueryLatencyAfterRestart) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE first_query (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO first_query VALUES (1, 'f')"));
  }

  // Returns how long a fresh backend takes to answer one query that misses the catalog cache, and
  // how much of that the copy served.
  const auto measure = [this](const char* what, bool serve_locally) -> Status {
    TServer(0).Shutdown();
    RETURN_NOT_OK(TServer(0).Restart());
    RETURN_NOT_OK(WaitForServing(0));
    // After the restart, because a restart discards runtime flag changes.
    RETURN_NOT_OK(cluster_->SetFlag(
        &TServer(0), "TEST_local_catalog_disable_serving", serve_locally ? "false" : "true"));

    const auto served_before = VERIFY_RESULT(Counter(0, METRIC_local_catalog_reads_served));
    const auto waited_before =
        VERIFY_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_version));
    const auto connect_start = MonoTime::Now();
    auto conn = VERIFY_RESULT(ConnectTo(0));
    const auto connected = MonoTime::Now();
    auto value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM first_query WHERE k = 1"));
    const auto queried = MonoTime::Now();
    SCHECK_EQ(value, "f", IllegalState, "Wrong row read");

    // The same query again on the same backend, now that its caches are warm.
    const auto warm_start = MonoTime::Now();
    value = VERIFY_RESULT(conn.FetchRow<std::string>("SELECT v FROM first_query WHERE k = 1"));
    const auto warm_done = MonoTime::Now();

    LOG(INFO) << what << ": connect " << (connected - connect_start) << ", first query "
              << (queried - connected) << ", same query warm " << (warm_done - warm_start)
              << ", reads served locally "
              << VERIFY_RESULT(Counter(0, METRIC_local_catalog_reads_served)) - served_before
              << ", version waits "
              << VERIFY_RESULT(Counter(0, METRIC_local_catalog_reads_waited_for_version)) -
                     waited_before
              << ", gate waits "
              << VERIFY_RESULT(EventStatsCount(0, METRIC_local_catalog_gate_wait_us));
    return Status::OK();
  };

  ASSERT_OK(measure("copy serving", /* serve_locally= */ true));
  ASSERT_OK(measure("copy out of the read path", /* serve_locally= */ false));
}

// A copy already on disk is reopened without a round trip to master and without waiting out the
// creation retry interval. The fixture sets that interval to a minute, so a regression that applies
// it before the first attempt cannot pass: serving would resume a minute later rather than at once.
class PgLocalCatalogSlowRetryTest : public PgLocalCatalogTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgLocalCatalogTest::UpdateMiniClusterOptions(options);
    options->extra_tserver_flags.push_back("--local_catalog_bootstrap_retry_delay_ms=60000");
  }
};

TEST_F(PgLocalCatalogSlowRetryTest, RestartReusesTheCopyWithoutAskingMaster) {
  ASSERT_OK(WaitForAllServing());
  {
    auto setup = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(setup.Execute("CREATE TABLE reuse (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(setup.Execute("INSERT INTO reuse VALUES (1, 'r')"));
  }
  const auto initdb_checks_before = ASSERT_RESULT(MasterInitDbDoneCount());

  TServer(0).Shutdown();
  ASSERT_OK(TServer(0).Restart());
  const auto restarted_at = MonoTime::Now();
  ASSERT_OK(WaitForServing(0));
  const auto serving_after = MonoTime::Now() - restarted_at;
  LOG(INFO) << "Serving resumed " << serving_after << " after the restart returned";

  ASSERT_EQ(ASSERT_RESULT(MasterInitDbDoneCount()), initdb_checks_before)
      << "The reopened copy asked master whether initdb was done";
  ASSERT_EQ(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reseeds)), 0);
  ASSERT_LT(serving_after, 20s) << "The reopened copy waited out the creation retry interval";

  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto served_before = ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM reuse WHERE k = 1")), "r");
  ASSERT_GT(ASSERT_RESULT(Counter(0, METRIC_local_catalog_reads_served)), served_before)
      << RoutingCounters(0);
}


// Plan section 9.3: lock releases carry pages, master pushes the rest, nobody pulls.
class PgLocalCatalogPushTest : public PgLocalCatalogTest {
 protected:
  // Master learns each copy's position from the first release's answers; until then it would
  // carry its whole look-back to every target.
  Status WarmUp() {
    RETURN_NOT_OK(WaitForAllServing());
    auto conn = VERIFY_RESULT(ConnectTo(0));
    RETURN_NOT_OK(conn.Execute("CREATE TABLE IF NOT EXISTS push_warmup (k INT PRIMARY KEY)"));
    return conn.Execute("DROP TABLE push_warmup");
  }

  Status ReadOn(size_t idx, const std::string& table, const std::string& expected) {
    auto conn = VERIFY_RESULT(ConnectTo(idx));
    auto value = VERIFY_RESULT(conn.FetchRow<std::string>(
        Format("SELECT v FROM $0 WHERE k = 1", table)));
    SCHECK_EQ(value, expected, IllegalState, Format("Wrong row in $0 on tserver $1", table, idx));
    return Status::OK();
  }
};

TEST_F(PgLocalCatalogPushTest, ReleaseCarriesOnePageInline) {
  ASSERT_OK(WarmUp());
  const auto inline_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_pages));
  const auto rpcs_before = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent));
  std::vector<int64_t> applied_before(GetNumTabletServers());
  std::vector<int64_t> floors_before(GetNumTabletServers());
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    applied_before[i] = ASSERT_RESULT(Counter(i, METRIC_local_catalog_pushes_applied));
    floors_before[i] = ASSERT_RESULT(Counter(i, METRIC_local_catalog_floor_requests_sent));
  }
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE inline_page (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO inline_page VALUES (1, 'i')"));
  }
  ASSERT_GE(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_pages)) - inline_before,
      GetNumTabletServers());
  for (int i = 1; i < GetNumTabletServers(); ++i) {
    ASSERT_GT(ASSERT_RESULT(Counter(i, METRIC_local_catalog_pushes_applied)), applied_before[i])
        << RoutingCounters(i);
    ASSERT_EQ(ASSERT_RESULT(Counter(i, METRIC_local_catalog_floor_requests_sent)), floors_before[i])
        << "tserver " << i << " had to ask for a push";
  }
  LOG(INFO) << "Push RPCs beside the release: "
            << ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent)) - rpcs_before;

  const auto served_before = ASSERT_RESULT(Counter(2, METRIC_local_catalog_reads_served));
  const auto master_reads_before = ASSERT_RESULT(MasterCatalogReadCount());
  ASSERT_OK(ReadOn(2, "inline_page", "i"));
  ASSERT_GT(ASSERT_RESULT(Counter(2, METRIC_local_catalog_reads_served)), served_before);
  ASSERT_EQ(ASSERT_RESULT(MasterCatalogReadCount()), master_reads_before) << RoutingCounters(2);
}

TEST_F(PgLocalCatalogPushTest, CreateDatabaseIsPagedNotInline) {
  ASSERT_OK(WarmUp());
  const auto skipped_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped));
  const auto rpcs_before = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent));
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE DATABASE paged_db"));
  }
  ASSERT_GE(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped)) - skipped_before,
      2);
  // 11.6 MB of records in 1 MB pages, to two tservers at least.
  ASSERT_GE(ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent)) - rpcs_before, 10);
  ASSERT_LE(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_in_flight_bytes_max)),
      64 * 1024 * 1024);

  const auto served_before = ASSERT_RESULT(Counter(2, METRIC_local_catalog_reads_served));
  const auto master_reads_before = ASSERT_RESULT(MasterCatalogReadCount());
  auto conn = ASSERT_RESULT(ConnectTo(2, "paged_db"));
  ASSERT_OK(conn.Execute("CREATE TABLE in_paged_db (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_GT(ASSERT_RESULT(Counter(2, METRIC_local_catalog_reads_served)), served_before);
  LOG(INFO) << "Master catalog reads for the new database's first connection and DDL: "
            << ASSERT_RESULT(MasterCatalogReadCount()) - master_reads_before;
}

TEST_F(PgLocalCatalogPushTest, SmallInlineCapPagesEveryDdl) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "local_catalog_release_inline_max_bytes", "1024"));
  const auto skipped_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped));
  constexpr int kDdls = 10;
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    for (int i = 0; i < kDdls; ++i) {
      ASSERT_OK(conn.ExecuteFormat("CREATE TABLE capped_$0 (k INT PRIMARY KEY, v TEXT)", i));
    }
    ASSERT_OK(conn.Execute("INSERT INTO capped_9 VALUES (1, 'c')"));
  }
  ASSERT_GE(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped)) - skipped_before,
      kDdls * GetNumTabletServers());
  ASSERT_OK(ReadOn(2, "capped_9", "c"));
}

// A copy that cannot apply holds every DDL at its gate; once it can, master's pushes bring it to
// the head without any pull.
TEST_F(PgLocalCatalogPushTest, StragglerIsPushedNotPolled) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(&TServer(2), "TEST_local_catalog_pause_applier", "true"));
  Status ddl_status;
  std::atomic<bool> ddl_done{false};
  TestThreadHolder threads;
  threads.AddThreadFunctor([this, &ddl_status, &ddl_done] {
    auto conn = ConnectTo(0);
    if (!conn.ok()) {
      ddl_status = conn.status();
      ddl_done = true;
      return;
    }
    for (int i = 0; ddl_status.ok() && i < 3; ++i) {
      ddl_status = conn->ExecuteFormat("CREATE TABLE straggled_$0 (k INT PRIMARY KEY, v TEXT)", i);
    }
    if (ddl_status.ok()) {
      ddl_status = conn->Execute("INSERT INTO straggled_2 VALUES (1, 's')");
    }
    ddl_done = true;
  });
  SleepFor(3s * kTimeMultiplier);
  ASSERT_FALSE(ddl_done.load()) << "A DDL completed while a serving copy could not apply";
  ASSERT_OK(cluster_->SetFlag(&TServer(2), "TEST_local_catalog_pause_applier", "false"));
  threads.JoinAll();
  ASSERT_OK(ddl_status);
  ASSERT_OK(ReadOn(2, "straggled_2", "s"));
  ASSERT_OK(ReadOn(1, "straggled_2", "s"));
}

// Pages a release carried are lost on the way; the gate's floor request makes master push them.
TEST_F(PgLocalCatalogPushTest, LostPushIsRepairedNextCycle) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_drop_pushed_batch", "true"));
  const auto floors_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent));
  const auto refused_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_pushes_refused));
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE lost_push (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO lost_push VALUES (1, 'l')"));
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_drop_pushed_batch", "false"));
  ASSERT_GT(ASSERT_RESULT(Counter(1, METRIC_local_catalog_pushes_refused)), refused_before);
  ASSERT_GT(ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent)), floors_before);
  ASSERT_OK(ReadOn(1, "lost_push", "l"));
}

// Master believes a copy is ahead of where it is, which is what a restart from an old checkpoint
// looks like. The release's pages start above the copy and are answered with a gap; master pushes
// from the position the copy gives.
TEST_F(PgLocalCatalogPushTest, GapFromStaleReportIsRepushed) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      &TServer(1), "TEST_local_catalog_report_applied_op_id_ahead", "20"));
  // Long enough for the overstated report to reach master.
  SleepFor(3s * kTimeMultiplier);
  const auto refused_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_pushes_refused));
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE gapped (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO gapped VALUES (1, 'g')"));
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_report_applied_op_id_ahead", "0"));
  LOG(INFO) << "Refused pages on tserver 1: "
            << ASSERT_RESULT(Counter(1, METRIC_local_catalog_pushes_refused)) - refused_before;
  ASSERT_OK(ReadOn(1, "gapped", "g"));
}

TEST_F(PgLocalCatalogPushTest, ConcurrentDdlsShareCycles) {
  ASSERT_OK(WarmUp());
  const auto cycles_before = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_cycles));
  const auto releases_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_release_requests));
  const auto floors_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_floor_requests_received));
  constexpr int kSessions = 4;
  constexpr int kDdls = 10;
  std::vector<Status> statuses(kSessions);
  {
    TestThreadHolder threads;
    for (int s = 0; s < kSessions; ++s) {
      threads.AddThreadFunctor([this, s, &statuses] {
        auto conn = ConnectTo(s % GetNumTabletServers());
        if (!conn.ok()) {
          statuses[s] = conn.status();
          return;
        }
        for (int i = 0; i < kDdls && statuses[s].ok(); ++i) {
          statuses[s] =
              conn->ExecuteFormat("CREATE TABLE shared_$0_$1 (k INT PRIMARY KEY, v TEXT)", s, i);
        }
      });
    }
  }
  for (const auto& status : statuses) {
    ASSERT_OK(status);
  }
  const auto cycles =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_cycles)) - cycles_before;
  const auto releases =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_release_requests)) - releases_before;
  const auto floors =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_floor_requests_received)) - floors_before;
  LOG(INFO) << "Push cycles for " << kSessions * kDdls << " DDLs: " << cycles << ", releases "
            << releases << ", floor requests " << floors;
  // Releases share cycles; floor requests add at most one cycle each.
  ASSERT_LE(cycles, releases + floors);

  // Every copy ends where master is.
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "true"));
  int64_t master_version = 0;
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    master_version = ASSERT_RESULT(CatalogVersion(&conn));
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(0), "TEST_local_catalog_disable_serving", "false"));
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    auto conn = ASSERT_RESULT(ConnectTo(i));
    ASSERT_EQ(ASSERT_RESULT(CatalogVersion(&conn)), master_version) << "tserver " << i;
  }
}

// The handoff outlives the caller: the release handler gives up on its deadline while the apply
// is still running.
TEST_F(PgLocalCatalogPushTest, ApplyOutlivingItsCallerDoesNotCrash) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      &TServer(1), "TEST_local_catalog_pushed_batch_apply_delay_ms", "40000"));
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE outlived (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO outlived VALUES (1, 'o')"));
  }
  ASSERT_OK(cluster_->SetFlag(
      &TServer(1), "TEST_local_catalog_pushed_batch_apply_delay_ms", "0"));
  ASSERT_OK(WaitForServing(1));
  ASSERT_OK(ReadOn(1, "outlived", "o"));
  cluster_->AssertNoCrashes();
}

class PgLocalCatalogPushMultiMasterTest : public PgLocalCatalogPushTest {
 protected:
  int GetNumMasters() const override { return 3; }
};

// A new master leader knows no copy's position: its first release pushes from the start of its
// cache, and the copies drop what they already hold.
TEST_F(PgLocalCatalogPushMultiMasterTest, NewMasterLeaderPushesFromLookback) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->StepDownMasterLeaderAndWaitForNewLeader());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE after_stepdown (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO after_stepdown VALUES (1, 'a')"));
  }
  ASSERT_OK(ReadOn(2, "after_stepdown", "a"));
  for (int i = 0; i < GetNumTabletServers(); ++i) {
    ASSERT_EQ(ASSERT_RESULT(Counter(i, METRIC_local_catalog_reseeds)), 0);
  }
}

// Plan section 9.5.
TEST_F(PgLocalCatalogPushTest, TempTableFloorRequestsOneCycle) {
  ASSERT_OK(WarmUp());
  auto conn = ASSERT_RESULT(ConnectTo(1));
  const auto floors_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent));
  ASSERT_OK(conn.Execute("CREATE TEMP TABLE tt (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO tt VALUES (1, 't')"));
  const auto served_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_served));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM tt WHERE k = 1")), "t");
  // The temporary relation's rows reach the copy with the next push cycle: the session's own-write
  // wait asks for one unless an earlier push already carried them.
  ASSERT_LE(ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent)) - floors_before, 1);
  ASSERT_GT(ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_served)), served_before)
      << RoutingCounters(1);
}

// Nothing pulls, and with nothing written master has nothing to push.
TEST_F(PgLocalCatalogPushTest, IdleClusterSendsNoPushes) {
  ASSERT_OK(WarmUp());
  // The DDL's own after-effects (master's DDL verification writes) settle first.
  SleepFor(5s);
  const auto rpcs_before = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent));
  const auto cycles_before = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_cycles));
  SleepFor(15s);
  const auto rpcs = ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_rpcs_sent)) - rpcs_before;
  const auto cycles =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_cycles)) - cycles_before;
  LOG(INFO) << "Over 15 s idle: " << rpcs << " push RPCs, " << cycles << " cycles";
  ASSERT_EQ(rpcs, 0);
  ASSERT_EQ(cycles, 0);
}

TEST_F(PgLocalCatalogPushTest, FallbackKnobZeroUsesMasterAtOnce) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "local_catalog_serve_wait_timeout_ms", "0"));
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "true"));
  auto conn = ASSERT_RESULT(ConnectTo(1));
  const auto to_master_before =
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving));
  ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", GetCurrentTimeMicros()));
  const auto start = MonoTime::Now();
  ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_am"));
  ASSERT_LT(MonoTime::Now() - start, 5s);
  ASSERT_GT(
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving)),
      to_master_before);
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "false"));
}

// Plan section 9.2, through the push RPC of a real tserver: hand-built pages with no records, so
// that only the queue's position rules decide the outcome.
class PgLocalCatalogApplierTest : public PgLocalCatalogPushTest {
 protected:
  Result<OpIdPB> SysCatalogCommittedOpId() {
    auto proxy = cluster_->GetLeaderMasterProxy<consensus::ConsensusServiceProxy>();
    consensus::GetLastOpIdRequestPB req;
    consensus::GetLastOpIdResponsePB resp;
    req.set_dest_uuid(cluster_->GetLeaderMaster()->uuid());
    req.set_tablet_id(master::kSysCatalogTabletId);
    req.set_opid_type(consensus::COMMITTED_OPID);
    rpc::RpcController controller;
    controller.set_timeout(10s);
    RETURN_NOT_OK(proxy.GetLastOpId(req, &resp, &controller));
    SCHECK(!resp.has_error(), IllegalState, resp.error().ShortDebugString());
    return resp.opid();
  }

  // One push of record-less pages (from, end] to tserver idx.
  Result<tserver::PushSysCatalogChangesResponsePB> Push(
      size_t idx, const std::vector<std::pair<int64_t, int64_t>>& ranges, int64_t term,
      const std::string& uuid = "") {
    auto proxy = cluster_->GetTServerProxy<tserver::TabletServerServiceProxy>(idx);
    tserver::PushSysCatalogChangesRequestPB req;
    req.set_target_uuid(uuid.empty() ? TServer(idx).uuid() : uuid);
    req.set_master_term(term);
    for (const auto& [from, end] : ranges) {
      auto* page = req.add_pages();
      page->mutable_page_from()->set_term(term);
      page->mutable_page_from()->set_index(from);
      page->mutable_page_end()->set_term(term);
      page->mutable_page_end()->set_index(end);
    }
    tserver::PushSysCatalogChangesResponsePB resp;
    rpc::RpcController controller;
    controller.set_timeout(30s);
    RETURN_NOT_OK(proxy.PushSysCatalogChanges(req, &resp, &controller));
    return resp;
  }

  // The copy's applied position, learned from a push that starts far above it.
  Result<int64_t> AppliedIndex(size_t idx, int64_t term) {
    auto resp = VERIFY_RESULT(Push(idx, {{1LL << 40, (1LL << 40) + 1}}, term));
    SCHECK(resp.gap(), IllegalState, "A page far above the copy was not answered with a gap");
    return resp.applied_op_id().index();
  }
};

TEST_F(PgLocalCatalogApplierTest, GapIsAnsweredNotApplied) {
  ASSERT_OK(WarmUp());
  const auto term = ASSERT_RESULT(SysCatalogCommittedOpId()).term();
  const auto applied = ASSERT_RESULT(AppliedIndex(1, term));
  auto resp = ASSERT_RESULT(Push(1, {{applied + 10, applied + 11}}, term));
  ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
  ASSERT_TRUE(resp.gap());
  ASSERT_EQ(resp.applied_op_id().index(), applied);
}

TEST_F(PgLocalCatalogApplierTest, OverlapIsFiltered) {
  ASSERT_OK(WarmUp());
  const auto term = ASSERT_RESULT(SysCatalogCommittedOpId()).term();
  const auto applied = ASSERT_RESULT(AppliedIndex(1, term));
  const auto safe_time_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_safe_time_micros));
  auto resp = ASSERT_RESULT(Push(1, {{applied - 3, applied - 1}, {applied - 1, applied}}, term));
  ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
  ASSERT_FALSE(resp.gap());
  ASSERT_EQ(resp.applied_op_id().index(), applied);
  ASSERT_EQ(ASSERT_RESULT(Counter(1, METRIC_local_catalog_safe_time_micros)), safe_time_before);
}

TEST_F(PgLocalCatalogApplierTest, StaleTermAndWrongTargetAreRefused) {
  ASSERT_OK(WarmUp());
  const auto term = ASSERT_RESULT(SysCatalogCommittedOpId()).term();
  const auto applied = ASSERT_RESULT(AppliedIndex(1, term));
  auto stale = ASSERT_RESULT(Push(1, {{applied, applied}}, term - 1));
  ASSERT_TRUE(stale.has_error()) << stale.ShortDebugString();
  auto wrong = ASSERT_RESULT(Push(1, {{applied, applied}}, term, "not-this-tserver"));
  ASSERT_TRUE(wrong.has_error()) << wrong.ShortDebugString();
}

// Two pushes queued out of order are applied in the order of their starting positions. Applied in
// arrival order, the second would start above the copy and be answered with a gap.
TEST_F(PgLocalCatalogApplierTest, QueueOrdersByPageFrom) {
  ASSERT_OK(WarmUp());
  const auto term = ASSERT_RESULT(SysCatalogCommittedOpId()).term();
  const auto applied = ASSERT_RESULT(AppliedIndex(2, term));
  ASSERT_OK(cluster_->SetFlag(&TServer(2), "TEST_local_catalog_pause_applier", "true"));
  Result<tserver::PushSysCatalogChangesResponsePB> later{STATUS(Uninitialized, "")};
  Result<tserver::PushSysCatalogChangesResponsePB> earlier{STATUS(Uninitialized, "")};
  {
    TestThreadHolder threads;
    threads.AddThreadFunctor([this, &later, applied, term] {
      later = Push(2, {{applied + 1, applied + 2}}, term);
    });
    SleepFor(500ms);
    threads.AddThreadFunctor([this, &earlier, applied, term] {
      earlier = Push(2, {{applied, applied + 1}}, term);
    });
    SleepFor(500ms);
    ASSERT_OK(cluster_->SetFlag(&TServer(2), "TEST_local_catalog_pause_applier", "false"));
  }
  ASSERT_OK(earlier);
  ASSERT_OK(later);
  ASSERT_FALSE(earlier->gap()) << earlier->ShortDebugString();
  ASSERT_FALSE(later->gap()) << later->ShortDebugString();
  ASSERT_EQ(later->applied_op_id().index(), applied + 2);
}

// Plan section 9.3: a DDL whose records span pages, under the inline cap, is carried whole.
TEST_F(PgLocalCatalogPushTest, DropSchemaCascadeIsInlineMultiPage) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "local_catalog_push_page_bytes", "65536"));
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE SCHEMA cascaded"));
  for (int i = 0; i != 30; ++i) {
    ASSERT_OK(conn.ExecuteFormat("CREATE TABLE cascaded.t_$0 (k INT PRIMARY KEY, v TEXT)", i));
  }
  const auto inline_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_pages));
  const auto skipped_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped));
  ASSERT_OK(conn.Execute("DROP SCHEMA cascaded CASCADE"));
  const auto inline_pages =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_pages)) - inline_before;
  LOG(INFO) << "Pages carried inline by the DROP SCHEMA release: " << inline_pages;
  ASSERT_EQ(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_release_inline_skipped)), skipped_before);
  ASSERT_GE(inline_pages, 2 * GetNumTabletServers());
  auto reader = ASSERT_RESULT(ConnectTo(2));
  ASSERT_EQ(ASSERT_RESULT(reader.FetchRow<int64_t>(
      "SELECT count(*) FROM pg_namespace WHERE nspname = 'cascaded'")), 0);
}

// Plan section 9.5: a catalog read that went to master moves the session's floor, so the
// session's next read from the copy is at or after what it saw on master.
TEST_F(PgLocalCatalogPushTest, MasterSnapshotTimeIsFloorForNextLocalRead) {
  ASSERT_OK(WarmUp());
  auto conn = ASSERT_RESULT(ConnectTo(1));
  ASSERT_OK(conn.Execute("CREATE TABLE floor_t (k INT PRIMARY KEY, v TEXT)"));
  // A read time above C with the copy unable to move goes to master.
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "local_catalog_serve_wait_timeout_ms", "500"));
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "true"));
  const auto to_master_before =
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving));
  ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", GetCurrentTimeMicros()));
  ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_am"));
  ASSERT_OK(conn.Execute("RESET yb_read_time"));
  ASSERT_GT(
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving)),
      to_master_before);
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "false"));
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "local_catalog_serve_wait_timeout_ms", "300000"));
  const auto master_read_micros = GetCurrentTimeMicros();
  const auto floors_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent));
  const auto served_before = ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_served));
  auto fresh = ASSERT_RESULT(ConnectTo(1));
  ASSERT_RESULT(fresh.FetchRow<int64_t>("SELECT count(*) FROM floor_t"));
  ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_class"));
  ASSERT_GT(ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_served)), served_before);
  ASSERT_GT(ASSERT_RESULT(Counter(1, METRIC_local_catalog_floor_requests_sent)), floors_before);
  ASSERT_GE(ASSERT_RESULT(Counter(1, METRIC_local_catalog_safe_time_micros)), master_read_micros);
}

// With no wait bound, a statement waits for the copy instead of going to master.
TEST_F(PgLocalCatalogPushTest, FallbackKnobNeverHoldsTheStatement) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "local_catalog_serve_wait_timeout_ms", "-1"));
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "true"));
  auto conn = ASSERT_RESULT(ConnectTo(1));
  const auto to_master_before =
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving));
  ASSERT_OK(conn.ExecuteFormat("SET yb_read_time = $0", GetCurrentTimeMicros()));
  std::atomic<bool> done{false};
  TestThreadHolder threads;
  threads.AddThreadFunctor([&conn, &done] {
    ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_catalog.pg_am"));
    done = true;
  });
  SleepFor(3s);
  ASSERT_FALSE(done.load()) << "The statement did not wait for the copy";
  ASSERT_OK(cluster_->SetFlag(&TServer(1), "TEST_local_catalog_pause_applier", "false"));
  threads.JoinAll();
  ASSERT_EQ(
      ASSERT_RESULT(Counter(1, METRIC_local_catalog_reads_to_master_not_serving)),
      to_master_before);
}

// An apply that outlasts master's push RPC timeout is retried by master; the retried page is
// filtered as overlap and the copy ends where master is.
TEST_F(PgLocalCatalogPushTest, StalledApplyTimesOutAndRecovers) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "local_catalog_push_rpc_timeout_ms", "2000"));
  ASSERT_OK(cluster_->SetFlag(
      &TServer(1), "TEST_local_catalog_pushed_batch_apply_delay_ms", "4000"));
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE stalled (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO stalled VALUES (1, 's')"));
  }
  ASSERT_OK(cluster_->SetFlag(
      &TServer(1), "TEST_local_catalog_pushed_batch_apply_delay_ms", "0"));
  ASSERT_OK(ReadOn(1, "stalled", "s"));
  ASSERT_OK(ReadOn(2, "stalled", "s"));
  cluster_->AssertNoCrashes();
}

// A catalog write master makes outside any DDL reaches the copies on the timer cycle.
class PgLocalCatalogPushTimerTest : public PgLocalCatalogPushTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgLocalCatalogPushTest::UpdateMiniClusterOptions(options);
    options->extra_master_flags.push_back("--local_catalog_push_interval_ms=2000");
  }
};

TEST_F(PgLocalCatalogPushTimerTest, TimerCycleCarriesWritesOutsideDdl) {
  ASSERT_OK(WarmUp());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  ASSERT_OK(conn.Execute("CREATE TABLE timer_t (k INT PRIMARY KEY)"));
  // A catalog write outside DDL: no release carries it and no version moves.
  ASSERT_OK(conn.Execute("SET yb_non_ddl_txn_for_sys_tables_allowed = true"));
  ASSERT_OK(conn.Execute(
      "UPDATE pg_catalog.pg_class SET relhasindex = relhasindex WHERE relname = 'timer_t'"));
  const auto written_micros = GetCurrentTimeMicros();
  const auto floors_before = ASSERT_RESULT(Counter(2, METRIC_local_catalog_floor_requests_sent));
  ASSERT_OK(LoggedWaitFor(
      [this, written_micros]() -> Result<bool> {
        return VERIFY_RESULT(Counter(2, METRIC_local_catalog_safe_time_micros)) > written_micros;
      },
      30s, "C on tserver 2 to pass the write without a DDL or a floor request"));
  ASSERT_EQ(ASSERT_RESULT(Counter(2, METRIC_local_catalog_floor_requests_sent)), floors_before);
}

// Plan section 9.4: a tserver loses its YSQL lease, DDLs complete without it, and it regains the
// lease. PG starts only once the copy holds everything up to the new lease's bound.
class PgLocalCatalogRejoinTest : public PgLocalCatalogPushTest {
 protected:
  static constexpr size_t kJoiner = 2;

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgLocalCatalogPushTest::UpdateMiniClusterOptions(options);
    options->extra_master_flags.push_back("--master_ysql_operation_lease_ttl_ms=5000");
  }

  Status LoseLease() {
    RETURN_NOT_OK(cluster_->SetFlag(
        &TServer(kJoiner), "TEST_tserver_enable_ysql_lease_refresh", "false"));
    return LoggedWaitFor(
        [this]() -> Result<bool> {
          return VERIFY_RESULT(Counter(kJoiner, METRIC_local_catalog_serving_state)) ==
                 kServingStateDisabledLease;
        },
        60s, "The joiner to lose its lease");
  }

  Status RegainLease() {
    return cluster_->SetFlag(&TServer(kJoiner), "TEST_tserver_enable_ysql_lease_refresh", "true");
  }

  // PG on the joiner is restarted after the copy serves, so connections fail for a moment.
  Result<PGConn> ConnectToJoiner() {
    std::optional<PGConn> conn;
    RETURN_NOT_OK(LoggedWaitFor(
        [this, &conn]() -> Result<bool> {
          auto result = ConnectTo(kJoiner);
          if (!result.ok()) {
            return false;
          }
          if (!result->FetchRow<int32_t>("SELECT 1").ok()) {
            return false;
          }
          conn.emplace(std::move(*result));
          return true;
        },
        120s, "PG on the joiner to accept connections"));
    return std::move(*conn);
  }

  Result<int64_t> JoinerMasterReads() {
    int64_t total = 0;
    for (const auto* proto :
         {&METRIC_local_catalog_reads_to_master_not_serving,
          &METRIC_local_catalog_reads_to_master_in_ddl}) {
      total += VERIFY_RESULT(Counter(kJoiner, *proto));
    }
    return total;
  }
};

TEST_F(PgLocalCatalogRejoinTest, RejoinCatchesUpFromCache) {
  ASSERT_OK(WarmUp());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE rejoin_t (k INT PRIMARY KEY, a INT, v TEXT)"));
    ASSERT_OK(conn.Execute("CREATE INDEX rejoin_i ON rejoin_t (a)"));
    ASSERT_OK(conn.Execute("INSERT INTO rejoin_t VALUES (1, 1, 'r')"));
  }
  ASSERT_OK(LoseLease());
  // No index build while the joiner has no lease: a backfill connects to the PG of every tserver
  // that holds a tablet of the table, and the joiner's PG is stopped.
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("ALTER TABLE rejoin_t ADD COLUMN w TEXT DEFAULT 'added'"));
    ASSERT_OK(conn.Execute("CREATE VIEW rejoin_v AS SELECT k, v, w FROM rejoin_t"));
  }
  const auto floors_before =
      ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_floor_requests_sent));
  ASSERT_OK(RegainLease());
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
  const auto master_reads_before = ASSERT_RESULT(JoinerMasterReads());
  const auto plan = ASSERT_RESULT(conn.FetchAllAsString(
      "EXPLAIN (COSTS OFF) SELECT v FROM rejoin_t WHERE a = 1"));
  ASSERT_STR_CONTAINS(plan, "rejoin_i");
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<std::string>("SELECT w FROM rejoin_v WHERE k = 1")), "added");
  ASSERT_EQ(ASSERT_RESULT(JoinerMasterReads()), master_reads_before) << RoutingCounters(kJoiner);
  ASSERT_GT(ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_floor_requests_sent)),
            floors_before);
}

TEST_F(PgLocalCatalogRejoinTest, RejoinWithNoDdlFlipsOnEmptyCycle) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(LoseLease());
  ASSERT_OK(RegainLease());
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
}

TEST_F(PgLocalCatalogRejoinTest, RejoinBelowCacheReadsWal) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(cluster_->GetLeaderMaster(), "local_catalog_push_cache_mb", "1"));
  ASSERT_OK(LoseLease());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    for (int i = 0; i < 100; ++i) {
      ASSERT_OK(conn.ExecuteFormat("CREATE TABLE below_cache_$0 (k INT PRIMARY KEY, v TEXT)", i));
    }
    ASSERT_OK(conn.Execute("INSERT INTO below_cache_99 VALUES (1, 'b')"));
  }
  const auto transient_before =
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_transient_pages_built));
  ASSERT_OK(RegainLease());
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM below_cache_99 WHERE k = 1")), "b");
  ASSERT_GT(
      ASSERT_RESULT(MasterCounter(METRIC_local_catalog_push_transient_pages_built)),
      transient_before);
}

TEST_F(PgLocalCatalogRejoinTest, RejoinPgStartHoldTimesOut) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(
      &TServer(kJoiner), "local_catalog_serve_wait_timeout_ms", "3000"));
  ASSERT_OK(LoseLease());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE held (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO held VALUES (1, 'h')"));
  }
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "TEST_local_catalog_push_block_target",
      TServer(kJoiner).uuid()));
  ASSERT_OK(RegainLease());
  {
    auto conn = ASSERT_RESULT(ConnectToJoiner());
    ASSERT_NE(
        ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_serving_state)),
        kServingStateServing);
    const auto master_reads_before = ASSERT_RESULT(JoinerMasterReads());
    ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM held WHERE k = 1")), "h");
    ASSERT_GT(ASSERT_RESULT(JoinerMasterReads()), master_reads_before);
  }
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "TEST_local_catalog_push_block_target", ""));
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectTo(kJoiner));
  const auto served_before = ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_reads_served));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM held WHERE k = 1")), "h");
  ASSERT_GT(ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_reads_served)), served_before);
}

// The joiner's copy is below what master retains, so master tells it to fetch its copy again, and
// it serves once the fresh copy has a page whose safe time passes the lease bound. The retention
// loss is injected (TEST_local_catalog_push_reseed) as in CheckpointTooOldReseeds.
TEST_F(PgLocalCatalogRejoinTest, RejoinBelowRetentionReseeds) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(LoseLease());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE reseeded (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO reseeded VALUES (1, 'r')"));
  }
  ASSERT_OK(cluster_->SetFlag(&TServer(kJoiner), "TEST_local_catalog_push_reseed", "true"));
  ASSERT_OK(RegainLease());
  ASSERT_OK(LoggedWaitFor(
      [this]() -> Result<bool> {
        return VERIFY_RESULT(Counter(kJoiner, METRIC_local_catalog_reseeds)) > 0;
      },
      120s, "The joiner to re-seed"));
  ASSERT_OK(cluster_->SetFlag(&TServer(kJoiner), "TEST_local_catalog_push_reseed", "false"));
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
  const auto master_reads_before = ASSERT_RESULT(JoinerMasterReads());
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT v FROM reseeded WHERE k = 1")), "r");
  ASSERT_EQ(ASSERT_RESULT(JoinerMasterReads()), master_reads_before) << RoutingCounters(kJoiner);
}

// With no bound on the hold, PG does not start until the copy serves.
TEST_F(PgLocalCatalogRejoinTest, RejoinPgStartHoldNever) {
  ASSERT_OK(WarmUp());
  ASSERT_OK(cluster_->SetFlag(&TServer(kJoiner), "local_catalog_serve_wait_timeout_ms", "-1"));
  ASSERT_OK(LoseLease());
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "TEST_local_catalog_push_block_target",
      TServer(kJoiner).uuid()));
  ASSERT_OK(RegainLease());
  const auto start = MonoTime::Now();
  while (MonoTime::Now() - start < 5s) {
    auto conn = ConnectTo(kJoiner);
    ASSERT_FALSE(conn.ok() && conn->FetchRow<int32_t>("SELECT 1").ok())
        << "PG on the joiner accepted a connection before its copy served";
    SleepFor(500ms);
  }
  ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "TEST_local_catalog_push_block_target", ""));
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
  const auto served_before = ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_reads_served));
  ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_class"));
  ASSERT_GT(ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_reads_served)), served_before);
}

// Two tservers lose and regain their leases while DDLs run on the third; the DDLs are not held by
// the joiners' catch-up.
TEST_F(PgLocalCatalogRejoinTest, RejoinWaveDoesNotHoldDdl) {
  ASSERT_OK(WarmUp());
  auto conn = ASSERT_RESULT(ConnectTo(0));
  const auto run_ddls = [&conn](const std::string& tag, int count) -> Result<MonoDelta> {
    std::vector<MonoDelta> latencies;
    for (int i = 0; i != count; ++i) {
      const auto start = MonoTime::Now();
      RETURN_NOT_OK(conn.ExecuteFormat("CREATE TABLE wave_$0_$1 (k INT PRIMARY KEY)", tag, i));
      latencies.push_back(MonoTime::Now() - start);
    }
    std::sort(latencies.begin(), latencies.end());
    return latencies[latencies.size() / 2];
  };
  const auto baseline = ASSERT_RESULT(run_ddls("base", 10));
  for (size_t ts : {1, 2}) {
    ASSERT_OK(cluster_->SetFlag(&TServer(ts), "TEST_tserver_enable_ysql_lease_refresh", "false"));
  }
  ASSERT_OK(LoggedWaitFor(
      [this]() -> Result<bool> {
        return VERIFY_RESULT(Counter(1, METRIC_local_catalog_serving_state)) ==
                   kServingStateDisabledLease &&
               VERIFY_RESULT(Counter(2, METRIC_local_catalog_serving_state)) ==
                   kServingStateDisabledLease;
      },
      60s, "Both joiners to lose their leases"));
  ASSERT_RESULT(run_ddls("gap", 10));
  for (size_t ts : {1, 2}) {
    ASSERT_OK(cluster_->SetFlag(&TServer(ts), "TEST_tserver_enable_ysql_lease_refresh", "true"));
  }
  const auto during = ASSERT_RESULT(run_ddls("rejoin", 10));
  LOG(INFO) << "DDL p50: baseline " << baseline << ", during the rejoin " << during;
  ASSERT_LT(during, baseline * 3);
  for (size_t ts : {1, 2}) {
    ASSERT_OK(WaitForServing(ts));
  }
  auto reader = ASSERT_RESULT(ConnectTo(2));
  ASSERT_RESULT(reader.FetchRow<int64_t>("SELECT count(*) FROM wave_rejoin_9"));
}

// A DDL that completes while the joiner catches up is acknowledged by the joiner at once, and the
// joiner does not serve until its copy holds that DDL's version.
TEST_F(PgLocalCatalogRejoinTest, ReleaseDuringCatchupGatesTheFlip) {
  ASSERT_OK(WarmUp());
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    ASSERT_OK(conn.Execute("CREATE TABLE flip (k INT PRIMARY KEY, v TEXT)"));
    ASSERT_OK(conn.Execute("INSERT INTO flip VALUES (1, 'before')"));
  }
  ASSERT_OK(LoseLease());
  ASSERT_OK(cluster_->SetFlag(&TServer(kJoiner), "TEST_local_catalog_pause_applier", "true"));
  ASSERT_OK(RegainLease());
  // The lease is back once master targets the joiner's releases again; a DDL meanwhile must not
  // wait for the paused copy.
  SleepFor(5s * kTimeMultiplier);
  {
    auto conn = ASSERT_RESULT(ConnectTo(0));
    const auto start = MonoTime::Now();
    ASSERT_OK(conn.Execute("ALTER TABLE flip ADD COLUMN w TEXT DEFAULT 'after'"));
    ASSERT_LT(MonoTime::Now() - start, 20s);
  }
  SleepFor(2s);
  ASSERT_NE(
      ASSERT_RESULT(Counter(kJoiner, METRIC_local_catalog_serving_state)), kServingStateServing);
  ASSERT_OK(cluster_->SetFlag(&TServer(kJoiner), "TEST_local_catalog_pause_applier", "false"));
  ASSERT_OK(WaitForServing(kJoiner));
  auto conn = ASSERT_RESULT(ConnectToJoiner());
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SELECT w FROM flip WHERE k = 1")), "after");
}

}  // namespace yb::pgwrapper
