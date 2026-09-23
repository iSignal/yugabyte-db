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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "yb/gutil/casts.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/monotime.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"

DECLARE_bool(pg_client_use_shared_memory);
DECLARE_bool(ysql_enable_read_request_caching);
DECLARE_int32(TEST_fetch_next_delay_ms);
DECLARE_string(TEST_fetch_next_delay_column);
DECLARE_string(ysql_pg_conf_csv);

using namespace std::literals;

namespace yb::pgwrapper {

namespace {

// Long enough that a backend which ignores the departed client is still parked on the stalled read
// when the test stops watching it.
constexpr auto kStall = 30s * kTimeMultiplier;
constexpr auto kReaction = 10s * kTimeMultiplier;
constexpr auto kClientConnectionCheckInterval = 500ms;
// libpq closes its socket and returns once this elapses.
constexpr auto kClientConnectTimeoutSec = 3;

constexpr auto kVictimUser = "preload_victim";

// Backends only show up in pg_stat_activity once startup completes, so look for the process title
// postgres sets as soon as it has read the startup packet: "postgres: <user> <db> <host> ...".
size_t CountBackendsOf(const std::string& user) {
  const auto prefix = Format("postgres: $0 ", user);
  size_t result = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    std::ifstream cmdline(entry.path() / "cmdline");
    std::string title(std::istreambuf_iterator<char>(cmdline), {});
    if (title.starts_with(prefix)) {
      ++result;
    }
  }
  return result;
}

}  // namespace

// The parameter picks the pggate transport: the shared memory exchange (backend blocks on its
// semaphore) or a plain RPC (backend blocks on a std::future).
class PgStartupClientDisconnectTest : public PgMiniTestBase,
                                      public ::testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pg_client_use_shared_memory) = GetParam();
    PgMiniTestBase::SetUp();
  }

  size_t NumTabletServers() override { return 1; }

  void BeforePgProcessStart() override {
    // Otherwise the tserver response cache answers the preload and there is nothing to stall.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_read_request_caching) = false;
  }

  // A client opens a connection whose catalog preload is stuck on the tserver, then gives up and
  // closes its socket. Returns once the client is gone and its backend was seen while stalled.
  void StartStalledConnectionAndLeave() {
    {
      auto conn = ASSERT_RESULT(Connect());
      ASSERT_OK(conn.ExecuteFormat("CREATE ROLE $0 LOGIN", kVictimUser));
    }

    // pg_authid is prefetched by every backend before authentication.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_column) = "rolcanlogin";
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_ms) =
        narrow_cast<int32_t>(MonoDelta(kStall).ToMilliseconds());

    const auto conn_str = Format(
        "host=$0 port=$1 user=$2 dbname=yugabyte connect_timeout=$3",
        pg_host_port().host(), pg_host_port().port(), kVictimUser, kClientConnectTimeoutSec);
    TestThreadHolder threads;
    auto client_status = CONNECTION_OK;
    threads.AddThreadFunctor([&conn_str, &client_status] {
      PGConnPtr client(PQconnectdb(conn_str.c_str()));
      client_status = PQstatus(client.get());
    });

    ASSERT_OK(WaitFor(
        [] { return CountBackendsOf(kVictimUser) > 0; }, kReaction, "victim backend to start"));
    threads.JoinAll();
    ASSERT_EQ(client_status, CONNECTION_BAD);
  }

  static void ResetStall() {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_ms) = 0;
  }
};

class PgStartupClientConnectionCheckTest : public PgStartupClientDisconnectTest {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_pg_conf_csv) = Format(
        "client_connection_check_interval=$0",
        MonoDelta(kClientConnectionCheckInterval).ToMilliseconds());
    PgStartupClientDisconnectTest::SetUp();
  }
};

// With client_connection_check_interval set, the backend notices the departed client while its
// preload is blocked in pggate and exits.
TEST_P(PgStartupClientConnectionCheckTest, ClientLeavesDuringStalledPreload) {
  auto reset_stall = ScopeExit([] { ResetStall(); });
  ASSERT_NO_FATALS(StartStalledConnectionAndLeave());
  ASSERT_OK(WaitFor(
      [] { return CountBackendsOf(kVictimUser) == 0; }, kReaction, "orphaned backend to exit"));
}

// A connected client is neither dropped while its connection is established nor afterwards, when
// the regular per-query check takes over the timer.
TEST_P(PgStartupClientConnectionCheckTest, ConnectedClientUnaffected) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.FetchFormat("SELECT pg_sleep($0)", 4 * MonoDelta(kClientConnectionCheckInterval)
      .ToSeconds()));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>("SELECT 1")), 1);
}

// As during query execution, client_connection_check_interval = 0 (the default) means the client
// is not checked: the backend waits out the stalled preload.
TEST_P(PgStartupClientDisconnectTest, NoCheckWhenIntervalIsZero) {
  auto reset_stall = ScopeExit([] { ResetStall(); });
  ASSERT_NO_FATALS(StartStalledConnectionAndLeave());
  SleepFor(kClientConnectionCheckInterval * 4);
  ASSERT_GT(CountBackendsOf(kVictimUser), 0);
}

std::string TransportName(const ::testing::TestParamInfo<bool>& info) {
  return info.param ? "SharedMem" : "Rpc";
}

INSTANTIATE_TEST_SUITE_P(
    , PgStartupClientDisconnectTest, ::testing::Bool(), TransportName);
INSTANTIATE_TEST_SUITE_P(
    , PgStartupClientConnectionCheckTest, ::testing::Bool(), TransportName);

}  // namespace yb::pgwrapper
