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

#include <optional>
#include <string>

#include "yb/master/master.h"
#include "yb/master/mini_master.h"

#include "yb/util/metrics.h"
#include "yb/util/result.h"
#include "yb/util/status_format.h"
#include "yb/util/test_macros.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"
#include "yb/yql/pgwrapper/pg_test_utils.h"

METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerService_Read);

DECLARE_bool(ysql_catalog_preload_additional_tables);
DECLARE_string(ysql_catalog_preload_additional_table_list);
DECLARE_string(ysql_catalog_preload_exclude_schemas);
DECLARE_bool(ysql_enable_auto_analyze);
DECLARE_bool(ysql_enable_relcache_init_optimization);

using namespace std::literals;

namespace yb::pgwrapper {
namespace {

constexpr auto kDbName = "exclusion_db";

struct CacheMemoryUsage {
  int64_t cache_context_bytes;
  int64_t backend_pss_kb;
};

// Sums used bytes of CacheMemoryContext and all of its descendants. total_bytes is not used
// because CacheMemoryContext grows in blocks of up to 8MB. pg_get_backend_memory_contexts() emits
// contexts in depth-first pre-order, so the subtree is the run of rows after CacheMemoryContext
// with a deeper level.
Result<int64_t> CacheMemoryContextBytes(PGConn& conn) {
  const auto rows = VERIFY_RESULT((conn.FetchRows<std::string, int32_t, int64_t>(
      "SELECT name, level, used_bytes FROM pg_get_backend_memory_contexts()")));
  std::optional<int32_t> cache_level;
  int64_t total = 0;
  for (const auto& [name, level, bytes] : rows) {
    if (cache_level) {
      if (level <= *cache_level) {
        break;
      }
      total += bytes;
    } else if (name == "CacheMemoryContext") {
      cache_level = level;
      total = bytes;
    }
  }
  SCHECK(cache_level, NotFound, "CacheMemoryContext not found");
  return total;
}

} // namespace

// Preloads enough catalogs that, without exclusion, planning a simple query on a relation misses
// no catalog cache. pg_operator and pg_amop are not in the ysql_catalog_preload_additional_tables
// default list, and they are not relation-scoped, so they must stay fully loaded under exclusion.
class PgCatalogPreloadExclusionTest : public PgMiniTestBase {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_catalog_preload_additional_tables) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_catalog_preload_additional_table_list) =
        "pg_operator,pg_amop";
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_auto_analyze) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_relcache_init_optimization) = false;
    PgMiniTestBase::SetUp();
    master_reads_.emplace(*cluster_->mini_master()->master()->metric_entity(),
                          METRIC_handler_latency_yb_tserver_TabletServerService_Read);
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 WITH COLOCATION = true", kDbName));
    conn = ASSERT_RESULT(ConnectToDB(kDbName));
    db_oid_ = ASSERT_RESULT(conn.FetchRow<PGOid>(
        "SELECT oid FROM pg_database WHERE datname = current_database()"));
  }

  size_t NumTabletServers() override { return 1; }

  Status SetExcludeSchemasFlag(const std::string& value) {
    LOG(INFO) << "ysql_catalog_preload_exclude_schemas=" << value;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_catalog_preload_exclude_schemas) = value;
    return RestartPostgres();
  }

  Result<Oid> SchemaOid(PGConn& conn, const std::string& schema) {
    return conn.FetchRow<PGOid>(Format("SELECT '$0'::regnamespace::oid", schema));
  }

  Result<PGConn> ConnectAs(const std::string& user) {
    auto settings = MakeConnSettings(kDbName);
    settings.user = user;
    return PGConnBuilder(settings).Connect();
  }

  // Master (sys catalog) read RPCs issued while running the query on an established connection.
  // The handler latency histogram is updated after the response is sent, so let it settle before
  // taking the final reading.
  Result<size_t> MasterReads(PGConn& conn, const std::string& query) {
    return master_reads_->Delta([&conn, &query]() -> Status {
      RETURN_NOT_OK(conn.Fetch(query));
      SleepFor(200ms);
      return Status::OK();
    });
  }

  Oid db_oid_ = kInvalidOid;
  std::optional<SingleMetricWatcher> master_reads_;
};

class PgCatalogPreloadExclusionMissTest : public PgCatalogPreloadExclusionTest {
 protected:
  void SetUp() override {
    PgCatalogPreloadExclusionTest::SetUp();
    auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
    ASSERT_OK(conn.Execute("CREATE SCHEMA incl"));
    ASSERT_OK(conn.Execute("CREATE SCHEMA excl"));
    for (const auto* table : {"incl.t", "excl.t"}) {
      ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", table));
      ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (v)", table));
      ASSERT_OK(conn.ExecuteFormat(
          "INSERT INTO $0 SELECT i, i * 10 FROM generate_series(1, 10) i", table));
    }
    // Relations in the kept schema that depend on relations in the excluded one.
    ASSERT_OK(conn.Execute(
        "CREATE TABLE incl.child (k INT PRIMARY KEY, p INT REFERENCES excl.t (k))"));
    ASSERT_OK(conn.Execute("CREATE TABLE incl.parted (k INT, v INT) PARTITION BY RANGE (k)"));
    ASSERT_OK(conn.Execute(
        "CREATE TABLE incl.parted_p0 PARTITION OF incl.parted FOR VALUES FROM (0) TO (100)"));
    ASSERT_OK(conn.Execute(
        "CREATE TABLE excl.parted_p1 PARTITION OF incl.parted FOR VALUES FROM (100) TO (200)"));
    excl_oid_ = ASSERT_RESULT(SchemaOid(conn, "excl"));
  }

  Oid excl_oid_ = kInvalidOid;
};

TEST_F(PgCatalogPreloadExclusionMissTest, ExcludedSchemaMissesGoToMaster) {
  constexpr auto kInclQuery = "SELECT v FROM incl.t WHERE k = 3";
  constexpr auto kExclQuery = "SELECT v FROM excl.t WHERE k = 3";

  // Planning a relation's first query reads pg_statistic_ext, which is never cached, so each
  // relation's first-use reads are compared with the same query without exclusion.
  size_t incl_base, excl_base;
  {
    auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
    incl_base = ASSERT_RESULT(MasterReads(conn, kInclQuery));
    excl_base = ASSERT_RESULT(MasterReads(conn, kExclQuery));
  }

  ASSERT_OK(SetExcludeSchemasFlag(Format("postgres@$0:$1", db_oid_, excl_oid_)));

  auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
  const auto incl_reads = ASSERT_RESULT(MasterReads(conn, kInclQuery));
  const auto excl_reads = ASSERT_RESULT(MasterReads(conn, kExclQuery));
  LOG(INFO) << "First-use master reads without exclusion: incl.t=" << incl_base
            << " excl.t=" << excl_base << "; with excl excluded: incl.t=" << incl_reads
            << " excl.t=" << excl_reads;
  ASSERT_EQ(incl_reads, incl_base);
  ASSERT_GT(excl_reads, excl_base)
      << "excluded relation should have been loaded from master on first use";
  ASSERT_EQ(ASSERT_RESULT(MasterReads(conn, kExclQuery)), 0)
      << "excluded relation should be cached after first use";
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>(kInclQuery)), 30);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>(kExclQuery)), 30);
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int64_t>(
      "SELECT count(*) FROM excl.t WHERE v >= 50")), 6);

  auto fk_conn = ASSERT_RESULT(ConnectToDB(kDbName));
  ASSERT_OK(fk_conn.Execute("INSERT INTO incl.child VALUES (1, 1)"));
  auto status = fk_conn.Execute("INSERT INTO incl.child VALUES (2, 999)");
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "violates foreign key constraint");

  auto part_conn = ASSERT_RESULT(ConnectToDB(kDbName));
  ASSERT_OK(part_conn.Execute("INSERT INTO incl.parted VALUES (5, 50), (150, 1500)"));
  ASSERT_EQ(ASSERT_RESULT(part_conn.FetchRow<int32_t>(
      "SELECT v FROM incl.parted WHERE k = 150")), 1500);
  ASSERT_EQ(ASSERT_RESULT(part_conn.FetchRow<int64_t>(
      "SELECT count(*) FROM excl.parted_p1")), 1);
  ASSERT_EQ(ASSERT_RESULT(part_conn.FetchRow<int64_t>(
      "SELECT count(*) FROM incl.parted")), 2);
}

TEST_F(PgCatalogPreloadExclusionMissTest, ExclusionMatchesRoleAndDatabase) {
  constexpr auto kExclQuery = "SELECT v FROM excl.t WHERE k = 3";
  {
    auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
    ASSERT_OK(conn.Execute("CREATE ROLE other_role SUPERUSER LOGIN"));
  }

  // Entries for another role, another database, a system schema, and malformed entries must
  // leave postgres unaffected.
  ASSERT_OK(SetExcludeSchemasFlag(Format(
      "other_role@$0:$1;postgres@$2:$1;postgres@$0:11;garbage;postgres@x:$1;postgres@$0:1x",
      db_oid_, excl_oid_, db_oid_ + 1)));

  auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
  const auto postgres_reads = ASSERT_RESULT(MasterReads(conn, kExclQuery));

  auto other_conn = ASSERT_RESULT(ConnectAs("other_role"));
  const auto other_reads = ASSERT_RESULT(MasterReads(other_conn, kExclQuery));
  LOG(INFO) << "First-use master reads of excl.t: postgres=" << postgres_reads
            << " other_role=" << other_reads;
  ASSERT_GT(other_reads, postgres_reads);
  ASSERT_EQ(ASSERT_RESULT(other_conn.FetchRow<int32_t>(kExclQuery)), 30);
}

// Compares the catalog cache footprint of a fresh backend with nothing, one of two equally sized
// schemas, and both schemas excluded.
TEST_F(PgCatalogPreloadExclusionTest, CacheMemoryShrinksWithExclusion) {
  const int kNumTables = RegularBuildVsSanitizers(100, 10);
  constexpr int kNumColumns = 20;
  const std::vector<std::string> kSchemas = {"s1", "s2"};

  std::string columns;
  for (int c = 1; c <= kNumColumns; ++c) {
    columns += Format("$0c$1 INT", c == 1 ? "" : ", ", c);
  }

  std::vector<Oid> schema_oids;
  {
    auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
    for (const auto& schema : kSchemas) {
      ASSERT_OK(conn.ExecuteFormat("CREATE SCHEMA $0", schema));
      for (int t = 0; t < kNumTables; ++t) {
        ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0.t$1 ($2)", schema, t, columns));
        ASSERT_OK(conn.ExecuteFormat(
            "CREATE INDEX NONCONCURRENTLY ON $0.t$1 (c1)", schema, t));
        ASSERT_OK(conn.ExecuteFormat(
            "CREATE INDEX NONCONCURRENTLY ON $0.t$1 (c2, c3)", schema, t));
      }
      schema_oids.push_back(ASSERT_RESULT(SchemaOid(conn, schema)));
    }
  }

  auto measure = [this](const std::string& flag) -> Result<CacheMemoryUsage> {
    RETURN_NOT_OK(SetExcludeSchemasFlag(flag));
    auto conn = VERIFY_RESULT(ConnectToDB(kDbName));
    RETURN_NOT_OK(conn.Fetch("SELECT 1"));
    const auto pid = VERIFY_RESULT(conn.FetchRow<int32_t>("SELECT pg_backend_pid()"));
    CacheMemoryUsage usage {
      .cache_context_bytes = VERIFY_RESULT(CacheMemoryContextBytes(conn)),
      .backend_pss_kb = VERIFY_RESULT(
          ProcFileValue(Format("/proc/$0/smaps_rollup", pid), "Pss:")),
    };
    LOG(INFO) << "exclude_schemas='" << flag << "': CacheMemoryContext subtree used = "
              << usage.cache_context_bytes << " bytes, backend PSS = "
              << usage.backend_pss_kb << " kB";
    return usage;
  };

  const auto none = ASSERT_RESULT(measure(""));
  const auto one = ASSERT_RESULT(measure(
      Format("postgres@$0:$1", db_oid_, schema_oids[0])));
  const auto both = ASSERT_RESULT(measure(
      Format("postgres@$0:$1,$2", db_oid_, schema_oids[0], schema_oids[1])));

  const auto one_saved = none.cache_context_bytes - one.cache_context_bytes;
  const auto both_saved = none.cache_context_bytes - both.cache_context_bytes;
  LOG(INFO) << "CacheMemoryContext used bytes saved: one schema = " << one_saved
            << " bytes, both schemas = " << both_saved << " bytes; "
            << "backend PSS saved: one schema = " << none.backend_pss_kb - one.backend_pss_kb
            << " kB, both schemas = " << none.backend_pss_kb - both.backend_pss_kb << " kB";

  ASSERT_LT(one.cache_context_bytes, none.cache_context_bytes);
  ASSERT_LT(both.cache_context_bytes, one.cache_context_bytes);
  // The schemas are identical, so excluding the second one should save about as much again.
  ASSERT_GT(both_saved, one_saved * 3 / 2);
  ASSERT_LT(both_saved, one_saved * 5 / 2);
}

} // namespace yb::pgwrapper
