// Copyright (c) YugaByte, Inc.
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

#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

#include "yb/util/metrics.h"
#include "yb/util/status.h"
#include "yb/util/test_macros.h"

#include "yb/master/catalog_manager_if.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"


#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"
#include "yb/yql/pgwrapper/pg_test_utils.h"

DECLARE_int32(history_cutoff_propagation_interval_ms);
DECLARE_int32(stream_compression_algo);
DECLARE_int32(timestamp_history_retention_interval_sec);
DECLARE_int32(timestamp_syscatalog_history_retention_interval_sec);
DECLARE_bool(ysql_use_relcache_file);

namespace yb::pgwrapper {
namespace {

class PgSnapshotTooOldTest : public PgMiniTestBase {
 protected:
  virtual void BeforePgProcessStart() {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_timestamp_history_retention_interval_sec) = 0;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_timestamp_syscatalog_history_retention_interval_sec) = 0;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_history_cutoff_propagation_interval_ms) = 1;
    FLAGS_ysql_use_relcache_file = false;
  }
};


} // namespace

// The test checks that multiple writes into single table with single tablet
// are performed with single RPC. Multiple scenarios are checked.
TEST_F(PgSnapshotTooOldTest, DefaultTablespaceGuc) {
  const std::string kDatabaseName = "testdb";
  auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
  auto sys_catalog_tablet = catalog_manager.sys_catalog()->tablet_peer()->tablet();




  PGConn conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  //ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  ASSERT_OK(conn.ExecuteFormat("create tablespace rf3 with "
    "(replica_placement='{\"num_replicas\":3, \"placement_blocks\":"
    "[{\"cloud\": \"cloud1\", \"region\":\"datacenter1\", \"zone\":\"rack1\",\"min_num_replicas\":3}]}');"));
  //ASSERT_OK(conn.ExecuteFormat("ALTER DATABASE yugabyte SET  client_encoding = 'LATIN1';"));
  // datestyle = 'DEFAULT'
  // default_text_search_config
  ASSERT_OK(conn.ExecuteFormat("ALTER DATABASE yugabyte SET  default_tablespace = 'rf3';"));
  while (env_->FileExists("/tmp/pause_test")) {
    sleep(1000);
    YB_LOG_EVERY_N_SECS(INFO, 10) << " Paused test ";
  }
  ASSERT_OK(sys_catalog_tablet->Flush(tablet::FlushMode::kSync));
  ASSERT_OK(sys_catalog_tablet->ForceManualRocksDBCompact());

  PGConn conn3 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn3.ExecuteFormat("CREATE DATABASE $0", "foo1"));
  //ASSERT_OK(conn3.ExecuteFormat("DROP DATABASE $0", kDatabaseName));

  ASSERT_OK(sys_catalog_tablet->Flush(tablet::FlushMode::kSync));
  ASSERT_OK(sys_catalog_tablet->ForceManualRocksDBCompact());

  sleep(1);

  PGConn conn2 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn2.ExecuteFormat("SET yb_debug_log_catcache_events=1;"));


}
} // namespace yb::pgwrapper
