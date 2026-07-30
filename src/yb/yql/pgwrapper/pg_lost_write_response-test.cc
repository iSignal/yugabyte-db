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

#include <string>

#include "yb/integration-tests/external_mini_cluster.h"

#include "yb/util/status.h"
#include "yb/util/test_macros.h"

#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"

namespace yb::pgwrapper {

// A write can be replicated and applied and still have its response lost -- the leader is killed
// after the apply, or the RPC is simply dropped. The client then retries the very same request.
// Since a YSQL primary-key insert is a conditional write ("insert if absent"), the retry must not
// be rejected by the row that its own first attempt wrote.
//
// TEST_num_write_responses_to_fail_after_apply fires from WriteQueryCompletionCallback, i.e. after
// the write has been applied, so there is no race here: by the time the client retries, the row is
// guaranteed to already exist.
//
// The same tablet write path serves both cases: user tables via TabletServiceImpl::Write on a
// tserver, and YSQL system catalog (DDL) writes via MasterTabletServiceImpl::Write, which
// delegates to it. Hence one flag, armed on either the tserver or the master.
class PgLostWriteResponseTest : public LibPqTestBase {
 public:
  int GetNumMasters() const override { return 1; }
  int GetNumTabletServers() const override { return 1; }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    LibPqTestBase::UpdateMiniClusterOptions(options);
    // Single replica so the armed daemon is unambiguously the one serving the write.
    options->replication_factor = 1;
  }

 protected:
  static constexpr auto kFlag = "TEST_num_write_responses_to_fail_after_apply";

  // Arms the injection and returns a checker that fails the test unless it actually fired.
  // Without this, anything that quietly consumes the count would leave the test passing
  // vacuously.
  void Arm(ExternalDaemon* daemon) { ASSERT_OK(cluster_->SetFlag(daemon, kFlag, "1")); }

  void AssertFired(ExternalDaemon* daemon) {
    const auto remaining = ASSERT_RESULT(cluster_->GetFlag(daemon, kFlag));
    ASSERT_EQ(remaining, "0") << "injection never fired; the test proved nothing";
  }
};

// A user-table INSERT whose response is lost after apply must succeed on retry, and must leave
// exactly one row. A "duplicate key value violates unique constraint" here would mean the retry
// was rejected by its own first attempt.
TEST_F(PgLostWriteResponseTest, UserTableInsert) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (k int PRIMARY KEY, v int)"));
  // Warm up caches so the armed write is the INSERT itself.
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 1)"));

  ASSERT_NO_FATALS(Arm(cluster_->tablet_server(0)));
  const auto status = conn.Execute("INSERT INTO t VALUES (2, 2)");
  LOG(INFO) << "INSERT with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->tablet_server(0)));

  ASSERT_OK(status);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM t WHERE k = 2")), 1);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM t")), 2);
}

// An INSERT into a table carrying a secondary index cannot use the single-shard fast path -- the
// base row and the index entry must be updated atomically, so it runs as a distributed
// transaction, like every catalog write. If the transactional-ness is what decides which error
// escapes, this should behave like the catalog cases rather than like UserTableInsert.
TEST_F(PgLostWriteResponseTest, UserTableInsertWithSecondaryIndex) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE ti (k int PRIMARY KEY, v int)"));
  ASSERT_OK(conn.Execute("CREATE INDEX ti_v_idx ON ti (v)"));
  ASSERT_OK(conn.Execute("INSERT INTO ti VALUES (1, 1)"));

  ASSERT_NO_FATALS(Arm(cluster_->tablet_server(0)));
  const auto status = conn.Execute("INSERT INTO ti VALUES (2, 2)");
  LOG(INFO) << "indexed INSERT with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->tablet_server(0)));

  ASSERT_OK(status);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM ti WHERE k = 2")), 1);
}

// The first write of an explicit transaction block, on a table without secondary indexes, is a
// transactional write that may be sent before the transaction's read point is finalized -- read
// time selection can be deferred to the tablet server serving the first operation
// (kPickReadTimeOnDocDB). If the write request carries no read time, a lost response puts it in
// the same position as the fast path despite being transactional.
TEST_F(PgLostWriteResponseTest, UserTableInsertFirstInTxnBlock) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE tb (k int PRIMARY KEY, v int)"));
  ASSERT_OK(conn.Execute("INSERT INTO tb VALUES (1, 1)"));

  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_NO_FATALS(Arm(cluster_->tablet_server(0)));
  const auto status = conn.Execute("INSERT INTO tb VALUES (2, 2)");
  LOG(INFO) << "first-in-txn INSERT with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->tablet_server(0)));

  ASSERT_OK(status);
  ASSERT_OK(conn.Execute("COMMIT"));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM tb WHERE k = 2")), 1);
}

// The CREATE TABLE AS data load is issued as non-transactional fast-path writes (the new table
// is not visible to other transactions), which carry no read time. A lost response on one of
// them therefore fails the whole statement the same way UserTableInsert does: the retry is
// detected as a duplicate, but without a read time the dedup verdict is returned as a raw
// AlreadyPresent error instead of success.
TEST_F(PgLostWriteResponseTest, UserTableCtasDataWrite) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (a int PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("INSERT INTO warmup VALUES (1)"));

  ASSERT_NO_FATALS(Arm(cluster_->tablet_server(0)));
  const auto status = conn.Execute(
      "CREATE TABLE ctas AS SELECT i AS k, i AS v FROM generate_series(1, 1000) i");
  LOG(INFO) << "CTAS with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->tablet_server(0)));

  ASSERT_OK(status);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM ctas")), 1000);
}

// Same thing for a system catalog write. CREATE TABLE inserts into pg_class, pg_type,
// pg_attribute and pg_depend, all of which have a primary key, so a re-executed catalog write
// surfaces as a duplicate key on one of them -- the DB-22042 signature.
TEST_F(PgLostWriteResponseTest, CatalogDdl) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (a int PRIMARY KEY)"));

  ASSERT_NO_FATALS(Arm(cluster_->master(0)));
  const auto status = conn.Execute("CREATE TABLE ct (a int PRIMARY KEY, b text, c numeric)");
  LOG(INFO) << "CREATE TABLE with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->master(0)));

  ASSERT_OK(status);
  ASSERT_EQ(
      ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) FROM pg_class WHERE relname = 'ct'")),
      1);
  ASSERT_OK(conn.Execute("INSERT INTO ct VALUES (1, 'a', 1.5)"));
}

// CREATE EXTENSION is what the original report hit: it performs hundreds of catalog writes in one
// statement, so it is far likelier to have one of them lose its response.
TEST_F(PgLostWriteResponseTest, CatalogCreateExtension) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE warmup (a int PRIMARY KEY)"));

  ASSERT_NO_FATALS(Arm(cluster_->master(0)));
  const auto status = conn.Execute("CREATE EXTENSION IF NOT EXISTS vector");
  LOG(INFO) << "CREATE EXTENSION with lost response returned: " << status;
  ASSERT_NO_FATALS(AssertFired(cluster_->master(0)));

  ASSERT_OK(status);
  ASSERT_EQ(
      ASSERT_RESULT(
          conn.FetchRow<int64_t>("SELECT count(*) FROM pg_extension WHERE extname = 'vector'")),
      1);
}

}  // namespace yb::pgwrapper
