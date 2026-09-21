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

#include <chrono>
#include <string>

#include "yb/gutil/casts.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/monotime.h"
#include "yb/util/result.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"

DECLARE_bool(ysql_enable_read_request_caching);
DECLARE_int32(TEST_fetch_next_delay_ms);
DECLARE_string(TEST_fetch_next_delay_column);
DECLARE_string(ysql_pg_conf_csv);
DECLARE_uint64(TEST_delay_before_get_locks_status_ms);

using namespace std::literals;

namespace yb::pgwrapper {

namespace {

// How long a stalled RPC stays stalled. It only has to outlast the reaction window below, so that a
// backend which ignores interrupts is still parked when the assertions run. The RPC deadline
// (ysql_client_read_write_timeout_ms, 10 minutes) is what bounds such a backend today.
constexpr auto kStall = 30s;
// How long the test is willing to wait for a backend that honors interrupts.
constexpr auto kReaction = 10s;

constexpr auto kStalledQuery = "SELECT * FROM pg_locks";

Result<bool> BackendIsAlive(PGConn* conn, int pid) {
  return VERIFY_RESULT(conn->FetchRow<int64_t>(
      Format("SELECT COUNT(*) FROM pg_stat_activity WHERE pid = $0", pid))) > 0;
}

Result<bool> BackendIsActive(PGConn* conn, int pid) {
  return VERIFY_RESULT(conn->FetchRow<int64_t>(Format(
      "SELECT COUNT(*) FROM pg_stat_activity WHERE pid = $0 AND state = 'active'", pid))) > 0;
}

}  // namespace

class PgRpcInterruptTest : public PgMiniTestBase {
 protected:
  size_t NumTabletServers() override { return 1; }

  // Makes the GetLockStatus RPC behind "SELECT * FROM pg_locks" hang on the tserver.
  static void StallPgLocks() {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_delay_before_get_locks_status_ms) =
        MonoDelta(kStall).ToMilliseconds() * kTimeMultiplier;
  }
};

// A query cancel that arrives while the backend is parked in a synchronous RPC must take effect
// right away rather than after the RPC completes.
TEST_F(PgRpcInterruptTest, CancelBackendDuringStalledRpc) {
  auto observer = ASSERT_RESULT(Connect());
  auto victim = ASSERT_RESULT(Connect());
  const auto victim_pid = victim.BackendPID();

  StallPgLocks();

  TestThreadHolder threads;
  Status query_status;
  threads.AddThreadFunctor([&victim, &query_status] {
    query_status = ResultToStatus(victim.Fetch(kStalledQuery));
  });

  ASSERT_OK(WaitFor(
      [&observer, victim_pid] { return BackendIsActive(&observer, victim_pid); },
      kReaction, "victim query to start"));

  const auto start = MonoTime::Now();
  ASSERT_OK(observer.FetchRow<bool>(Format("SELECT pg_cancel_backend($0)", victim_pid)));
  threads.JoinAll();
  const auto elapsed = MonoTime::Now() - start;

  ASSERT_NOK(query_status);
  ASSERT_STR_CONTAINS(query_status.ToString(), "canceling statement due to user request");
  ASSERT_LT(elapsed, kReaction);
  // A cancel must not take the backend down with it.
  ASSERT_TRUE(ASSERT_RESULT(BackendIsAlive(&observer, victim_pid)));
}

// A client that disappears while the backend is parked in a synchronous RPC must be noticed within
// client_connection_check_interval instead of leaving an orphan backend behind for the rest of the
// RPC deadline.
TEST_F(PgRpcInterruptTest, ClientDisconnectDuringStalledRpc) {
  auto observer = ASSERT_RESULT(Connect());

  const auto conn_str = Format(
      "host=$0 port=$1 user=$2 dbname=yugabyte "
      "options='-c client_connection_check_interval=1s'",
      pg_host_port().host(), pg_host_port().port(), PGConnSettings::kDefaultUser);
  PGConnPtr victim(PQconnectdb(conn_str.c_str()));
  ASSERT_EQ(PQstatus(victim.get()), CONNECTION_OK) << PQerrorMessage(victim.get());
  const auto victim_pid = PQbackendPID(victim.get());

  StallPgLocks();

  ASSERT_EQ(PQsendQuery(victim.get(), kStalledQuery), 1) << PQerrorMessage(victim.get());
  ASSERT_OK(WaitFor(
      [&observer, victim_pid] { return BackendIsActive(&observer, victim_pid); },
      kReaction, "victim query to start"));

  // Drop the client end of the connection without reading the response.
  victim.reset();

  ASSERT_OK(WaitFor(
      [&observer, victim_pid]() -> Result<bool> {
        return !VERIFY_RESULT(BackendIsAlive(&observer, victim_pid));
      },
      kReaction, "orphaned backend to exit"));
}

// Connection establishment prefetches catalog data before any of the timers a backend normally
// relies on are armed, so a stalled preload used to hold the client for the full RPC deadline.
class PgStartupTimeoutTest : public PgRpcInterruptTest {
 protected:
  void BeforePgProcessStart() override {
    // Preloads must reach the master, otherwise the tserver response cache answers them and there
    // is nothing to stall.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_read_request_caching) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_pg_conf_csv) = "authentication_timeout=5";
  }
};

TEST_F(PgStartupTimeoutTest, StalledCatalogPreload) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_column) = "relname";
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_ms) =
      narrow_cast<int32_t>(MonoDelta(kStall).ToMilliseconds() * kTimeMultiplier);

  const auto conn_str = Format(
      "host=$0 port=$1 user=$2 dbname=yugabyte",
      pg_host_port().host(), pg_host_port().port(), PGConnSettings::kDefaultUser);

  const auto start = MonoTime::Now();
  PGConnPtr conn(PQconnectdb(conn_str.c_str()));
  const auto elapsed = MonoTime::Now() - start;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_ms) = 0;

  ASSERT_EQ(PQstatus(conn.get()), CONNECTION_BAD);
  LOG(INFO) << "Connection attempt failed after " << elapsed << ": " << PQerrorMessage(conn.get());
  ASSERT_LT(elapsed, kReaction);
}

}  // namespace yb::pgwrapper
