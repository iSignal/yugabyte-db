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

// Load test for master's push of its system catalog WAL to the tserver-local catalog copies.
// Receivers in the test process stand in for N tservers: master pushes to them as it pushes to
// real copies, while DDLs run on the real tservers. The test reports master CPU per cycle and per
// MB pushed, the in-flight occupancy, cycle latency, and the status RPCs that intent resolution
// costs the coordinators. Parameters are the flags below; the numbers are meaningful in release
// builds only.

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <google/protobuf/empty.pb.h>

#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_service.proxy.h"

#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"

#include "yb/integration-tests/external_mini_cluster.h"

#include "yb/master/sys_catalog_constants.h"

#include "yb/rpc/connection_context.h"
#include "yb/rpc/local_call.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/proxy.h"
#include "yb/rpc/rpc_context.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/service_pool.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/tserver/tserver.pb.h"

#include "yb/gutil/strings/join.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/curl_util.h"
#include "yb/util/flags.h"
#include "yb/util/json_document.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/result.h"
#include "yb/util/status_format.h"
#include "yb/util/string_util.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_thread_holder.h"

#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"

DEFINE_NON_RUNTIME_uint32(local_catalog_load_real_tservers, 1,
    "Number of real tservers in the cluster.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_ddl_sessions, 4,
    "Number of sessions that run DDL in a loop for the whole test.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_lag_ddls, 50,
    "Number of DDLs FixedLag runs between recording the streams' position and starting them.");
DEFINE_NON_RUNTIME_int32(local_catalog_load_master_log_cache_mb, 0,
    "When positive, master's log_cache_size_limit_mb, so that pages for old positions are built "
    "from the WAL on disk.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_baseline_sec, 10,
    "Length of the calibration's baseline phase.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_duration_sec, 30,
    "Length of the measured phase.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_queue_sample_ms, 10,
    "Interval at which the system catalog change service's queue length is sampled on master.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_client_messengers, 8,
    "Number of messengers serving the push receivers, each with one reactor and its own address; "
    "receiver i is served by messenger i modulo this number.");
DEFINE_NON_RUNTIME_string(local_catalog_load_extra_master_flags, "",
    "Whitespace-separated flags added to master's command line.");
DEFINE_NON_RUNTIME_double(local_catalog_load_max_foreign_cores, 0.25,
    "A phase is flagged when processes outside the test's process tree used more CPU cores than "
    "this on average.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_push_targets, 100,
    "Number of push receivers in the test process, each standing in for one tserver's local "
    "catalog copy in the push scenarios.");
DEFINE_NON_RUNTIME_uint32(local_catalog_load_joiners, 100,
    "Number of push receivers that start far behind in JoinerWave. They start below master's "
    "page cache only when the lag DDLs exceed it: pass --local_catalog_load_extra_master_flags "
    "with a small --local_catalog_push_cache_mb or "
    "--local_catalog_push_unknown_position_lookback_ops to measure pages built from the WAL.");
DEFINE_NON_RUNTIME_double(local_catalog_load_max_host_busy_fraction, 0.8,
    "A phase is flagged when the host's CPUs were busy for more than this fraction of the phase, "
    "counting the test's own processes.");

METRIC_DECLARE_entity(server);

using namespace std::literals;

namespace yb::pgwrapper {

namespace {

const char* const kFloorRequestHandlerMetric =
    "handler_latency_yb_cdc_SysCatalogChangeService_RequestSysCatalogPush";
const char* const kChangeServiceQueueMetric = "rpcs_in_queue_yb_cdc_SysCatalogChangeService";
const char* const kLeaseHandlerMetric =
    "handler_latency_yb_master_MasterYsqlLease_RefreshYsqlLease";
const char* const kHeartbeatHandlerMetric =
    "handler_latency_yb_master_MasterHeartbeat_TSHeartbeat";
const char* const kQueueOverflowMetric = "rpcs_queue_overflow";
const char* const kIncomingQueueTimeMetric = "rpc_incoming_queue_time";
const char* const kTxnStatusHandlerMetric =
    "handler_latency_yb_tserver_TabletServerService_GetTransactionStatus";
const char* const kLogCacheDiskReadsMetric = "log_cache_disk_reads";
const char* const kTserverFloorRequestsMetric = "local_catalog_floor_requests_sent";
const char* const kTserverGateWaitMetric = "local_catalog_gate_wait_us";
const char* const kTserverWaitedForVersionMetric = "local_catalog_reads_waited_for_version";
const char* const kTserverWaitedForOwnWritesMetric = "local_catalog_reads_waited_for_own_writes";
const char* const kLogCacheNumOpsMetric = "log_cache_num_ops";
const char* const kLogCacheSizeMetric = "log_cache_size";

// One /jsonmetricz entry. Reading a histogram through /jsonmetricz resets its percentiles, min
// and max, but not total_count and total_sum; every field is therefore taken from one fetch.
struct MetricSnapshot {
  int64_t value = 0;
  int64_t total_count = 0;
  int64_t total_sum = 0;
  int64_t max = 0;
  int64_t p95 = 0;
  int64_t p99 = 0;
  int64_t p99_9 = 0;
};

Result<MetricSnapshot> FetchMetric(
    const ExternalDaemon& daemon, const std::string& name,
    const std::string& entity_type = "server",
    const std::string& entity_id = "") {
  const auto url =
      Format("http://$0/jsonmetricz?metrics=$1", daemon.bound_http_hostport().ToString(), name);
  EasyCurl curl;
  faststring dst;
  RETURN_NOT_OK(curl.FetchURL(url, &dst));
  JsonDocument doc;
  auto root = VERIFY_RESULT(doc.Parse(dst.ToString()));
  for (const auto& entity : VERIFY_RESULT(root.GetArray())) {
    if (VERIFY_RESULT(entity["type"].GetString()) != entity_type ||
        (!entity_id.empty() && VERIFY_RESULT(entity["id"].GetString()) != entity_id)) {
      continue;
    }
    for (const auto& metric : VERIFY_RESULT(entity["metrics"].GetArray())) {
      if (VERIFY_RESULT(metric["name"].GetString()) != name) {
        continue;
      }
      MetricSnapshot result;
      const auto get = [&metric](const char* field) -> int64_t {
        auto value = metric[field].GetInt64();
        return value.ok() ? *value : 0;
      };
      result.value = get("value");
      result.total_count = get("total_count");
      result.total_sum = get("total_sum");
      result.max = get("max");
      result.p95 = get("percentile_95");
      result.p99 = get("percentile_99");
      result.p99_9 = get("percentile_99_9");
      return result;
    }
  }
  return STATUS_FORMAT(NotFound, "Metric $0 not found on $1", name, daemon.id());
}

// utime + stime of a process or thread, in clock ticks, and the thread's name.
struct ProcStat {
  std::string comm;
  pid_t ppid = 0;
  int64_t ticks = 0;
  // cutime + cstime: CPU of children the process has already reaped.
  int64_t reaped_children_ticks = 0;
};

Result<ProcStat> ReadProcStat(const std::string& path) {
  std::ifstream in(path);
  SCHECK(in.good(), IOError, Format("Cannot open $0", path));
  std::string line;
  std::getline(in, line);
  const auto open = line.find('(');
  const auto close = line.rfind(')');
  SCHECK(open != std::string::npos && close != std::string::npos, IllegalState,
         Format("Malformed $0: $1", path, line));
  ProcStat result;
  result.comm = line.substr(open + 1, close - open - 1);
  std::istringstream rest(line.substr(close + 2));
  std::vector<std::string> fields;
  for (std::string field; rest >> field;) {
    fields.push_back(field);
  }
  // Fields after the name start at field 3 (state): ppid is field 4, utime and stime are 14 and
  // 15, cutime and cstime are 16 and 17.
  SCHECK_GE(fields.size(), 15U, IllegalState, Format("Short $0: $1", path, line));
  result.ppid = std::stoi(fields[1]);
  result.ticks = std::stoll(fields[11]) + std::stoll(fields[12]);
  result.reaped_children_ticks = std::stoll(fields[13]) + std::stoll(fields[14]);
  return result;
}

// Thread names end in a sequence number (rpc_tp_Master-high-pri_12); dropping the trailing
// digits and separators groups a pool's threads together.
std::string ThreadGroup(const std::string& comm) {
  auto end = comm.find_last_not_of("0123456789-_");
  return end == std::string::npos ? comm : comm.substr(0, end + 1);
}

Result<std::map<std::string, int64_t>> ThreadGroupTicks(pid_t pid) {
  std::map<std::string, int64_t> result;
  const auto task_dir = Format("/proc/$0/task", pid);
  std::vector<std::string> children;
  RETURN_NOT_OK(Env::Default()->GetChildren(task_dir, &children));
  for (const auto& tid : children) {
    if (tid == "." || tid == "..") {
      continue;
    }
    auto stat = ReadProcStat(Format("$0/$1/stat", task_dir, tid));
    if (!stat.ok()) {
      // The thread exited between the listing and the read.
      continue;
    }
    result[ThreadGroup(stat->comm)] += stat->ticks;
  }
  return result;
}

// Jiffies summed over all CPUs of the host, from the /proc/stat "cpu" line (user nice system idle
// iowait irq softirq steal). The kernel charges interrupt and softirq time, which includes
// loopback network processing done for the test, to no process, so it is kept apart from the
// time that processes use.
struct HostTicks {
  int64_t process = 0;
  int64_t interrupt = 0;
};

Result<HostTicks> HostBusyTicks() {
  std::ifstream in("/proc/stat");
  std::string label;
  in >> label;
  SCHECK_EQ(label, "cpu", IllegalState, "Unexpected /proc/stat");
  int64_t fields[8];
  for (auto& field : fields) {
    in >> field;
  }
  SCHECK(in.good(), IllegalState, "Short /proc/stat");
  return HostTicks{
      .process = fields[0] + fields[1] + fields[2] + fields[7],
      .interrupt = fields[5] + fields[6]};
}

// CPU of this process and every process descended from it (the cluster's daemons and their
// postgres processes), including descendants that have exited and been reaped.
Result<int64_t> ProcessTreeTicks() {
  std::map<pid_t, ProcStat> stats;
  std::vector<std::string> entries;
  RETURN_NOT_OK(Env::Default()->GetChildren("/proc", &entries));
  for (const auto& entry : entries) {
    if (entry.empty() || !std::all_of(entry.begin(), entry.end(), ::isdigit)) {
      continue;
    }
    auto stat = ReadProcStat(Format("/proc/$0/stat", entry));
    if (stat.ok()) {
      stats.emplace(std::stoi(entry), *stat);
    }
  }
  std::multimap<pid_t, pid_t> children;
  for (const auto& [pid, stat] : stats) {
    children.emplace(stat.ppid, pid);
  }
  int64_t total = 0;
  std::vector<pid_t> pending{getpid()};
  while (!pending.empty()) {
    const auto pid = pending.back();
    pending.pop_back();
    if (auto it = stats.find(pid); it != stats.end()) {
      total += it->second.ticks + it->second.reaped_children_ticks;
    }
    for (auto [it, end] = children.equal_range(pid); it != end; ++it) {
      pending.push_back(it->second);
    }
  }
  return total;
}

struct HostSample {
  CoarseTimePoint at;
  HostTicks busy;
  int64_t tree_ticks = 0;
};

Result<HostSample> SampleHost() {
  HostSample result;
  result.at = CoarseMonoClock::Now();
  result.busy = VERIFY_RESULT(HostBusyTicks());
  result.tree_ticks = VERIFY_RESULT(ProcessTreeTicks());
  return result;
}

struct HostLoad {
  double busy_cores = 0;
  double interrupt_cores = 0;
  double foreign_cores = 0;
  double busy_fraction = 0;
  std::string flag;
};

HostLoad ComputeHostLoad(const HostSample& begin, const HostSample& end) {
  static const auto ticks_per_sec = sysconf(_SC_CLK_TCK);
  static const auto num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  HostLoad result;
  const double seconds = ToSeconds(end.at - begin.at);
  if (seconds <= 0) {
    return result;
  }
  const auto cores = [&](int64_t ticks) {
    return static_cast<double>(ticks) / ticks_per_sec / seconds;
  };
  result.interrupt_cores = cores(end.busy.interrupt - begin.busy.interrupt);
  const double process_cores = cores(end.busy.process - begin.busy.process);
  result.busy_cores = process_cores + result.interrupt_cores;
  result.foreign_cores = std::max(0.0, process_cores - cores(end.tree_ticks - begin.tree_ticks));
  result.busy_fraction = result.busy_cores / num_cpus;
  if (result.foreign_cores > FLAGS_local_catalog_load_max_foreign_cores) {
    result.flag += Format(
        "HOST UNDER LOAD: processes outside the test used $0 cores (limit $1). ",
        result.foreign_cores, FLAGS_local_catalog_load_max_foreign_cores);
  }
  if (result.busy_fraction > FLAGS_local_catalog_load_max_host_busy_fraction) {
    result.flag += Format(
        "HOST SATURATED: $0 of $1 CPUs busy (limit $2 of them). ", result.busy_cores, num_cpus,
        FLAGS_local_catalog_load_max_host_busy_fraction);
  }
  return result;
}

struct Percentiles {
  size_t count = 0;
  int64_t p50 = 0;
  int64_t p99 = 0;
  int64_t p99_9 = 0;
  int64_t max = 0;
};

Percentiles ComputePercentiles(std::vector<int64_t> values) {
  Percentiles result;
  result.count = values.size();
  if (values.empty()) {
    return result;
  }
  std::sort(values.begin(), values.end());
  const auto at = [&values](double fraction) {
    return values[std::min(values.size() - 1, static_cast<size_t>(fraction * values.size()))];
  };
  result.p50 = at(0.5);
  result.p99 = at(0.99);
  result.p99_9 = at(0.999);
  result.max = values.back();
  return result;
}

}  // namespace

class PgLocalCatalogLoadTest : public LibPqTestBase {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_tserver_flags.push_back(
        Format("--enable_local_tserver_catalog=$0", TserverCopy()));
    options->extra_master_flags.push_back("--enable_local_tserver_catalog=true");
    for (auto* flags : {&options->extra_master_flags, &options->extra_tserver_flags}) {
      flags->push_back("--ysql_yb_enable_invalidation_messages=true");
      flags->push_back("--enable_object_locking_for_table_locks=true");
      flags->push_back("--allowed_preview_flags_csv=ysql_enable_concurrent_ddl");
      flags->push_back("--ysql_enable_concurrent_ddl=true");
      flags->push_back("--ysql_yb_ddl_transaction_block_enabled=true");
      flags->push_back("--ysql_enable_auto_analyze=false");
    }
    options->replication_factor = std::min(3, GetNumTabletServers());
    options->extra_master_flags.push_back("--log_min_seconds_to_retain=7200");
    for (const auto& flag : StringSplit(FLAGS_local_catalog_load_extra_master_flags, ' ')) {
      if (!flag.empty()) {
        options->extra_master_flags.push_back(flag);
      }
    }
    if (FLAGS_local_catalog_load_master_log_cache_mb > 0) {
      options->extra_master_flags.push_back(
          Format("--log_cache_size_limit_mb=$0", FLAGS_local_catalog_load_master_log_cache_mb));
    }
  }

  int GetNumTabletServers() const override {
    return FLAGS_local_catalog_load_real_tservers;
  }

  // Whether the real tservers keep a local catalog copy, and so receive pushes themselves.
  virtual bool TserverCopy() const { return false; }

  ExternalMaster& Master() { return *cluster_->GetLeaderMaster(); }

  // The last committed position of master's system catalog tablet.
  Result<OpIdPB> SysCatalogCommittedOpId() {
    auto proxy = cluster_->GetLeaderMasterProxy<consensus::ConsensusServiceProxy>();
    consensus::GetLastOpIdRequestPB req;
    consensus::GetLastOpIdResponsePB resp;
    req.set_dest_uuid(Master().uuid());
    req.set_tablet_id(master::kSysCatalogTabletId);
    req.set_opid_type(consensus::COMMITTED_OPID);
    rpc::RpcController controller;
    controller.set_timeout(10s);
    RETURN_NOT_OK(proxy.GetLastOpId(req, &resp, &controller));
    SCHECK(!resp.has_error(), IllegalState, resp.error().ShortDebugString());
    return resp.opid();
  }

  // DDL sessions running create, alter and drop on their own tables until stopped. Latencies are
  // recorded per phase.
  void StartDdlLoad(TestThreadHolder* threads) {
    for (uint32_t w = 0; w != FLAGS_local_catalog_load_ddl_sessions; ++w) {
      threads->AddThreadFunctor([this, w, &stop = threads->stop_flag()] {
        auto conn = ASSERT_RESULT(ConnectToTs(*cluster_->tablet_server(w % GetNumTabletServers())));
        for (int64_t k = 0; !stop.load(); ++k) {
          const auto table = Format("load_$0_$1", w, k);
          for (const auto& stmt :
               {Format("CREATE TABLE $0 (a INT)", table),
                Format("ALTER TABLE $0 ADD COLUMN b INT", table),
                Format("DROP TABLE $0", table)}) {
            const auto start = CoarseMonoClock::Now();
            auto status = conn.Execute(stmt);
            RecordDdl(ToMicroseconds(CoarseMonoClock::Now() - start), status);
          }
        }
      });
    }
  }

  void RecordDdl(int64_t latency_us, const Status& status) {
    std::lock_guard lock(ddl_mutex_);
    auto& phase = ddl_phases_[current_phase_];
    if (status.ok()) {
      phase.latency_us.push_back(latency_us);
    } else {
      ++phase.errors;
      if (phase.first_error.empty()) {
        phase.first_error = status.ToString();
      }
    }
  }

  struct DdlPhase {
    std::vector<int64_t> latency_us;
    int64_t errors = 0;
    std::string first_error;
  };

  // Master-side counters at a phase boundary.
  struct MasterSample {
    CoarseTimePoint at;
    int64_t cpu_ticks = 0;
    std::map<std::string, int64_t> thread_ticks;
    MetricSnapshot floor_request_handler;
    MetricSnapshot lease_handler;
    MetricSnapshot heartbeat_handler;
    MetricSnapshot queue_overflow;
    MetricSnapshot incoming_queue_time;
    int64_t test_process_ticks = 0;
    MetricSnapshot log_cache_disk_reads;
    MetricSnapshot log_cache_num_ops;
    MetricSnapshot log_cache_size;
    int64_t txn_status_rpcs = 0;
    HostSample host;
    // Per real tserver with a copy: its floor requests and the waits that send them.
    struct Tserver {
      int64_t floor_requests = 0;
      int64_t gate_waits = 0;
      int64_t gate_wait_us = 0;
      int64_t waited_for_version = 0;
      int64_t waited_for_own_writes = 0;
    };
    std::vector<Tserver> tservers;
  };

  Result<MasterSample> SampleMaster() {
    MasterSample result;
    result.host = VERIFY_RESULT(SampleHost());
    result.at = CoarseMonoClock::Now();
    auto& master = Master();
    result.cpu_ticks = VERIFY_RESULT(ReadProcStat(Format("/proc/$0/stat", master.pid()))).ticks;
    result.thread_ticks = VERIFY_RESULT(ThreadGroupTicks(master.pid()));
    result.floor_request_handler = VERIFY_RESULT(FetchMetric(master, kFloorRequestHandlerMetric));
    result.lease_handler = VERIFY_RESULT(FetchMetric(master, kLeaseHandlerMetric));
    result.heartbeat_handler = VERIFY_RESULT(FetchMetric(master, kHeartbeatHandlerMetric));
    result.queue_overflow = VERIFY_RESULT(FetchMetric(master, kQueueOverflowMetric));
    result.incoming_queue_time = VERIFY_RESULT(FetchMetric(master, kIncomingQueueTimeMetric));
    result.test_process_ticks = VERIFY_RESULT(ReadProcStat("/proc/self/stat")).ticks;
    for (auto [name, snapshot] :
         {std::pair{kLogCacheDiskReadsMetric, &result.log_cache_disk_reads},
          std::pair{kLogCacheNumOpsMetric, &result.log_cache_num_ops},
          std::pair{kLogCacheSizeMetric, &result.log_cache_size}}) {
      *snapshot = VERIFY_RESULT(FetchMetric(master, name, "tablet", master::kSysCatalogTabletId));
    }
    if (TserverCopy()) {
      for (size_t i = 0; i != cluster_->num_tablet_servers(); ++i) {
        const auto& ts = *cluster_->tablet_server(i);
        MasterSample::Tserver sample;
        sample.floor_requests = VERIFY_RESULT(FetchMetric(ts, kTserverFloorRequestsMetric)).value;
        const auto gate = VERIFY_RESULT(FetchMetric(ts, kTserverGateWaitMetric));
        sample.gate_waits = gate.total_count;
        sample.gate_wait_us = gate.total_sum;
        sample.waited_for_version =
            VERIFY_RESULT(FetchMetric(ts, kTserverWaitedForVersionMetric)).value;
        sample.waited_for_own_writes =
            VERIFY_RESULT(FetchMetric(ts, kTserverWaitedForOwnWritesMetric)).value;
        result.tservers.push_back(sample);
      }
    }
    for (auto* daemon : cluster_->daemons()) {
      auto count = FetchMetric(*daemon, kTxnStatusHandlerMetric);
      if (count.ok()) {
        result.txn_status_rpcs += count->total_count;
      }
    }
    return result;
  }

  struct QueueStats {
    int64_t samples = 0;
    int64_t sum = 0;
    int64_t max = 0;
    int64_t failures = 0;
  };

  // Samples the queue length of master's floor request service until stopped.
  void StartQueueSampler(TestThreadHolder* threads, QueueStats* stats) {
    threads->AddThreadFunctor([this, stats, &stop = threads->stop_flag()] {
      auto& master = Master();
      while (!stop.load()) {
        auto sample = FetchMetric(master, kChangeServiceQueueMetric);
        if (sample.ok()) {
          ++stats->samples;
          stats->sum += sample->value;
          stats->max = std::max(stats->max, sample->value);
        } else {
          ++stats->failures;
        }
        std::this_thread::sleep_for(FLAGS_local_catalog_load_queue_sample_ms * 1ms);
      }
    });
  }

  void SetPhase(size_t phase) {
    std::lock_guard lock(ddl_mutex_);
    current_phase_ = phase;
  }

  // One phase: the DDL load runs throughout.
  struct PhaseResult {
    std::string name;
    MasterSample begin;
    MasterSample end;
    QueueStats queue;
    DdlPhase ddl;
  };

  Result<PhaseResult> RunPhase(
      const std::string& name, size_t phase_index, CoarseDuration duration) {
    PhaseResult result;
    result.name = name;
    SetPhase(phase_index);
    // Resets the percentiles of master's histograms, so that the end sample covers this phase.
    result.begin = VERIFY_RESULT(SampleMaster());
    {
      TestThreadHolder sampler;
      StartQueueSampler(&sampler, &result.queue);
      std::this_thread::sleep_for(duration);
      sampler.Stop();
    }
    result.end = VERIFY_RESULT(SampleMaster());
    std::lock_guard lock(ddl_mutex_);
    result.ddl = ddl_phases_[phase_index];
    return result;
  }

  void Report(const std::vector<PhaseResult>& phases) {
    const auto ticks_per_sec = sysconf(_SC_CLK_TCK);
    std::ofstream csv(GetTestPath("local_catalog_load.csv"));
    csv << "phase,real_tservers,seconds,master_cores,floor_requests_per_sec,queue_mean,queue_max,"
           "queue_overflow,incoming_queue_mean_us,lease_p99_us,heartbeat_p99_us,"
           "txn_status_rpcs_per_sec,ddls,ddl_p50_us,ddl_p99_us,ddl_errors,"
           "log_cache_disk_reads_per_sec,log_cache_num_ops,log_cache_bytes,host_busy_cores,"
           "foreign_cores,host_flag\n";
    for (const auto& phase : phases) {
      const auto& b = phase.begin;
      const auto& e = phase.end;
      const double seconds = ToSeconds(e.at - b.at);
      const double cpu_sec = static_cast<double>(e.cpu_ticks - b.cpu_ticks) / ticks_per_sec;
      const auto floor_requests =
          e.floor_request_handler.total_count - b.floor_request_handler.total_count;
      const auto queued = e.incoming_queue_time.total_count - b.incoming_queue_time.total_count;
      const auto queued_sum = e.incoming_queue_time.total_sum - b.incoming_queue_time.total_sum;
      const auto ddl = ComputePercentiles(phase.ddl.latency_us);
      const auto per = [](double num, double den) { return den > 0 ? num / den : 0.0; };

      const auto host = ComputeHostLoad(b.host, e.host);
      LOG(INFO) << "==== Phase " << phase.name << ": " << seconds << " s, real tservers "
                << GetNumTabletServers();
      LOG(INFO) << "  host: " << host.busy_cores << " of " << sysconf(_SC_NPROCESSORS_ONLN)
                << " CPUs busy (" << host.interrupt_cores << " in interrupts), "
                << host.foreign_cores << " cores used by processes outside the test";
      if (!host.flag.empty()) {
        LOG(WARNING) << "  " << host.flag << "Phase " << phase.name
                     << " measurements are not reliable.";
        flagged_phases_.push_back(phase.name + ": " + host.flag);
      }
      LOG(INFO) << "  test process CPU (DDL sessions, samplers): "
                << per(static_cast<double>(e.test_process_ticks - b.test_process_ticks) /
                           ticks_per_sec, seconds)
                << " cores";
      LOG(INFO) << "  master CPU: " << per(cpu_sec, seconds) << " cores; floor requests/s "
                << per(floor_requests, seconds);
      LOG(INFO) << "  change service queue: mean " << per(phase.queue.sum, phase.queue.samples)
                << ", max " << phase.queue.max << " over " << phase.queue.samples << " samples ("
                << phase.queue.failures << " failed); overflow "
                << e.queue_overflow.value - b.queue_overflow.value
                << "; server-wide queue wait mean " << per(queued_sum, queued) << " us";
      LOG(INFO) << "  lease handler p99 " << e.lease_handler.p99 << " us max "
                << e.lease_handler.max << " us; heartbeat handler p99 "
                << e.heartbeat_handler.p99 << " us max " << e.heartbeat_handler.max
                << " us; txn status RPCs/s " << per(e.txn_status_rpcs - b.txn_status_rpcs, seconds);
      LOG(INFO) << "  log cache: disk reads/s "
                << per(e.log_cache_disk_reads.value - b.log_cache_disk_reads.value, seconds)
                << " ops; ops held " << b.log_cache_num_ops.value << " -> "
                << e.log_cache_num_ops.value << "; bytes held " << b.log_cache_size.value << " -> "
                << e.log_cache_size.value;
      LOG(INFO) << "  DDL: " << ddl.count << " ok, p50 " << ddl.p50 << " us, p99 " << ddl.p99
                << " us, errors " << phase.ddl.errors
                << (phase.ddl.first_error.empty() ? "" : " first: ") << phase.ddl.first_error;
      for (size_t i = 0; i != std::min(b.tservers.size(), e.tservers.size()); ++i) {
        const auto& tb = b.tservers[i];
        const auto& te = e.tservers[i];
        const auto gate_waits = te.gate_waits - tb.gate_waits;
        LOG(INFO) << "  tserver " << i << ": floor requests/s "
                  << per(te.floor_requests - tb.floor_requests, seconds) << "; gate waits/s "
                  << per(gate_waits, seconds) << ", mean "
                  << per(te.gate_wait_us - tb.gate_wait_us, gate_waits)
                  << " us, gate wait time per second " << per(te.gate_wait_us - tb.gate_wait_us,
                                                              seconds * 1e6)
                  << "; reads waited for version " << te.waited_for_version - tb.waited_for_version
                  << ", for own writes " << te.waited_for_own_writes - tb.waited_for_own_writes;
      }
      std::vector<std::pair<int64_t, std::string>> threads;
      for (const auto& [group, ticks] : e.thread_ticks) {
        auto it = b.thread_ticks.find(group);
        threads.emplace_back(ticks - (it == b.thread_ticks.end() ? 0 : it->second), group);
      }
      std::sort(threads.rbegin(), threads.rend());
      for (size_t i = 0; i != std::min<size_t>(threads.size(), 8); ++i) {
        LOG(INFO) << "  master threads " << threads[i].second << ": "
                  << per(static_cast<double>(threads[i].first) / ticks_per_sec, seconds)
                  << " cores";
      }

      csv << phase.name << "," << GetNumTabletServers() << "," << seconds << ","
          << per(cpu_sec, seconds) << "," << per(floor_requests, seconds) << ","
          << per(phase.queue.sum, phase.queue.samples) << "," << phase.queue.max << ","
          << e.queue_overflow.value - b.queue_overflow.value << "," << per(queued_sum, queued)
          << "," << e.lease_handler.p99 << "," << e.heartbeat_handler.p99 << ","
          << per(e.txn_status_rpcs - b.txn_status_rpcs, seconds) << "," << ddl.count << ","
          << ddl.p50 << "," << ddl.p99 << "," << phase.ddl.errors << ","
          << per(e.log_cache_disk_reads.value - b.log_cache_disk_reads.value, seconds) << ","
          << e.log_cache_num_ops.value << "," << e.log_cache_size.value << ","
          << host.busy_cores << "," << host.foreign_cores << ",\"" << host.flag << "\"\n";
    }
    LOG(INFO) << "CSV written to " << GetTestPath("local_catalog_load.csv");
    for (const auto& flagged : flagged_phases_) {
      LOG(WARNING) << "FLAGGED " << flagged;
    }
  }

  // A baseline phase and a measured phase, both with the DDL load and the real tservers' copies.
  void RunScenario(const std::string& name) {
    {
      const auto begin = ASSERT_RESULT(SampleHost());
      std::this_thread::sleep_for(3s);
      const auto host = ComputeHostLoad(begin, ASSERT_RESULT(SampleHost()));
      LOG(INFO) << "Host before the scenario: " << host.busy_cores << " CPUs busy, "
                << host.foreign_cores << " cores outside the test";
      if (!host.flag.empty()) {
        LOG(WARNING) << "Before the scenario: " << host.flag;
        flagged_phases_.push_back("before the scenario: " + host.flag);
      }
    }
    TestThreadHolder ddl_threads;
    StartDdlLoad(&ddl_threads);
    std::vector<PhaseResult> phases;
    phases.push_back(ASSERT_RESULT(
        RunPhase("baseline", 0, FLAGS_local_catalog_load_baseline_sec * 1s)));
    phases.push_back(ASSERT_RESULT(
        RunPhase(name, 1, FLAGS_local_catalog_load_duration_sec * 1s)));
    ddl_threads.Stop();
    Report(phases);
  }

  std::mutex ddl_mutex_;
  size_t current_phase_ GUARDED_BY(ddl_mutex_) = 0;
  std::array<DdlPhase, 2> ddl_phases_ GUARDED_BY(ddl_mutex_);
  std::vector<std::string> flagged_phases_;
};

class PgLocalCatalogLoadCalibrationTest : public PgLocalCatalogLoadTest {
 protected:
  bool TserverCopy() const override { return true; }
};

// Calibration: only the real tservers' copies, under the DDL load of PushSteady. Run with
// --local_catalog_load_real_tservers=N and compare the per-DDL numbers with PushSteady run with
// --local_catalog_load_push_targets=N.
TEST_F(PgLocalCatalogLoadCalibrationTest, YB_DISABLE_TEST(RealTserversOnly)) {
  RunScenario("real_tservers_only");
}

// Push receivers in the test process. Master pushes to them as it would to tservers' local catalog
// copies (TEST_local_catalog_push_extra_targets); each answers with the position it reached, and
// discards the records. With advance off, a receiver never moves from its start position, so
// master re-sends the same pages to it for as long as the test runs.
class FakePushReceivers {
 public:
  struct Stats {
    int64_t pushes = 0;
    int64_t pages = 0;
    int64_t bytes = 0;
    int64_t gaps = 0;
    int64_t record_bytes = 0;
    size_t caught_up = 0;
    Percentiles caught_up_ms;
  };

  FakePushReceivers(size_t num_targets, size_t num_servers, bool advance)
      : num_targets_(num_targets), num_servers_(std::max<size_t>(num_servers, 1)),
        advance_(advance),
        metric_entity_(METRIC_ENTITY_server.Instantiate(&metric_registry_, "push-receivers")) {}

  ~FakePushReceivers() { Shutdown(); }

  Status Start(const OpId& start, int64_t head_index) {
    start_ = start;
    head_index_ = head_index;
    started_at_ = CoarseMonoClock::Now();
    for (size_t i = 0; i != num_servers_; ++i) {
      auto server = std::make_unique<Server>();
      server->messenger = VERIFY_RESULT(rpc::MessengerBuilder(Format("push-receiver-$0", i))
                                            .set_num_reactors(1)
                                            .set_metric_entity(metric_entity_)
                                            .Build());
      Endpoint bound;
      RETURN_NOT_OK(server->messenger->ListenAddress(
          rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>(),
          Endpoint(), &bound));
      server->host_port = HostPort::FromBoundEndpoint(bound);
      server->thread_pool = std::make_shared<rpc::ThreadPool>(rpc::ThreadPoolOptions{
          .name = Format("push-receiver-$0", i), .max_workers = 2});
      auto service = std::make_unique<Service>(this);
      const auto service_name = service->service_name();
      server->service_pool = make_scoped_refptr<rpc::ServicePool>(
          /* max_tasks= */ 10000, [pool = server->thread_pool](auto) { return pool; },
          &server->messenger->scheduler(), std::move(service), server->messenger->metric_entity());
      RETURN_NOT_OK(server->messenger->RegisterService(service_name, server->service_pool));
      RETURN_NOT_OK(server->messenger->StartAcceptor());
      servers_.push_back(std::move(server));
    }
    return Status::OK();
  }

  // The value of TEST_local_catalog_push_extra_targets that makes master push to these receivers.
  std::string TargetsFlag() const {
    std::vector<std::string> entries;
    for (size_t i = 0; i != servers_.size(); ++i) {
      const auto count = num_targets_ / num_servers_ + (i < num_targets_ % num_servers_ ? 1 : 0);
      entries.push_back(Format("$0=$1", servers_[i]->host_port.ToString(), count));
    }
    return JoinStrings(entries, ",");
  }

  // The uuid master knows the i-th receiver of the first address by.
  std::string TargetUuid(size_t i) const {
    return Format("$0#$1", servers_.front()->host_port.ToString(), i);
  }

  // Moves the head the receivers must reach and restarts the catch-up clock at `since`.
  void SetHead(int64_t head_index, CoarseTimePoint since) {
    std::lock_guard lock(mutex_);
    head_index_ = head_index;
    started_at_ = since;
    for (auto& [_, target] : targets_) {
      target.caught_up_ms = target.applied.index >= head_index_ ? 0 : -1;
    }
  }

  // Makes Stats::record_bytes count the serialized size of the records above the start position.
  void CountRecordBytesAboveStart() { count_record_bytes_ = true; }

  Stats Summarize() {
    std::lock_guard lock(mutex_);
    Stats result = stats_;
    std::vector<int64_t> caught_up;
    for (const auto& [_, target] : targets_) {
      if (target.caught_up_ms >= 0) {
        caught_up.push_back(target.caught_up_ms);
      }
    }
    result.caught_up = caught_up.size();
    result.caught_up_ms = ComputePercentiles(std::move(caught_up));
    return result;
  }

  void Shutdown() {
    for (auto& server : servers_) {
      server->messenger->ShutdownAcceptor();
      server->messenger->UnregisterAllServices();
      server->service_pool->Shutdown();
      server->messenger->Shutdown();
      server->thread_pool->Shutdown();
    }
    servers_.clear();
  }

 private:
  struct Server {
    std::unique_ptr<rpc::Messenger> messenger;
    HostPort host_port;
    std::shared_ptr<rpc::ThreadPool> thread_pool;
    scoped_refptr<rpc::ServicePool> service_pool;
  };

  struct Target {
    OpId applied;
    int64_t caught_up_ms = -1;
  };

  class Service : public rpc::ServiceIf {
   public:
    explicit Service(FakePushReceivers* owner)
        : owner_(owner), method_("yb.tserver.TabletServerService", "PushSysCatalogChanges") {}

    void FillEndpoints(const rpc::RpcServicePtr& service, rpc::RpcEndpointMap* map) override {
      map->emplace(method_.serialized_body(), std::make_pair(service, 0UL));
    }

    void Handle(rpc::InboundCallPtr call) override {
      rpc::HandleCall<rpc::RpcCallPBParamsImpl<
          tserver::PushSysCatalogChangesRequestPB, tserver::PushSysCatalogChangesResponsePB>>(
          std::move(call),
          [this](const tserver::PushSysCatalogChangesRequestPB* req,
                 tserver::PushSysCatalogChangesResponsePB* resp, rpc::RpcContext context) {
            owner_->HandlePush(*req, resp);
            context.RespondSuccess();
          });
    }

    std::string service_name() const override { return "yb.tserver.TabletServerService"; }
    std::string metric_name() const override { return service_name(); }

   private:
    FakePushReceivers* const owner_;
    const rpc::RemoteMethod method_;
  };

  void HandlePush(
      const tserver::PushSysCatalogChangesRequestPB& req,
      tserver::PushSysCatalogChangesResponsePB* resp) {
    std::lock_guard lock(mutex_);
    auto [it, inserted] = targets_.try_emplace(req.target_uuid());
    auto& target = it->second;
    if (inserted) {
      target.applied = start_;
    }
    ++stats_.pushes;
    for (const auto& page : req.pages()) {
      ++stats_.pages;
      stats_.bytes += page.records().size();
      if (count_record_bytes_ && !page.records().empty()) {
        cdc::GetChangesResponsePB changes;
        if (changes.ParseFromString(page.records())) {
          for (const auto& record : changes.records()) {
            if (record.op_id().index() > start_.index) {
              stats_.record_bytes += record.ByteSizeLong();
            }
          }
        }
      }
      if (page.page_from().index() > target.applied.index) {
        ++stats_.gaps;
        resp->set_gap(true);
        break;
      }
      if (advance_ && page.page_end().index() > target.applied.index) {
        target.applied = OpId::FromPB(page.page_end());
      }
      if (page.has_safe_time()) {
        resp->set_safe_time(page.safe_time());
      }
    }
    if (target.caught_up_ms < 0 && target.applied.index >= head_index_) {
      target.caught_up_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          CoarseMonoClock::Now() - started_at_).count();
    }
    target.applied.ToPB(resp->mutable_applied_op_id());
  }

  const size_t num_targets_;
  const size_t num_servers_;
  const bool advance_;
  bool count_record_bytes_ = false;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  std::vector<std::unique_ptr<Server>> servers_;
  std::mutex mutex_;
  OpId start_;
  int64_t head_index_ = 0;
  CoarseTimePoint started_at_;
  std::unordered_map<std::string, Target> targets_;
  Stats stats_;
};

// Plan section 9.7: master pushes to N receivers in the test process.
class PgLocalCatalogPushLoadTest : public PgLocalCatalogLoadTest {
 protected:
  struct PushSample {
    CoarseTimePoint at;
    int64_t cpu_ticks = 0;
    std::map<std::string, int64_t> thread_ticks;
    int64_t txn_status_rpcs = 0;
    std::map<std::string, MetricSnapshot> metrics;
  };

  static constexpr const char* kPushMetrics[] = {
      "local_catalog_push_cycles", "local_catalog_push_rpcs_sent", "local_catalog_push_bytes",
      "local_catalog_push_pages_built", "local_catalog_push_transient_pages_built",
      "local_catalog_push_failures", "local_catalog_push_in_flight_rpcs_max",
      "local_catalog_push_in_flight_bytes_max", "local_catalog_push_cycle_us",
      "local_catalog_floor_requests_received", "local_catalog_release_inline_pages",
      "local_catalog_release_inline_skipped"};

  Result<PushSample> SamplePush() {
    PushSample result;
    result.at = CoarseMonoClock::Now();
    auto& master = Master();
    result.cpu_ticks = VERIFY_RESULT(ReadProcStat(Format("/proc/$0/stat", master.pid()))).ticks;
    result.thread_ticks = VERIFY_RESULT(ThreadGroupTicks(master.pid()));
    for (const auto* name : kPushMetrics) {
      result.metrics[name] = VERIFY_RESULT(FetchMetric(master, name));
    }
    for (auto* daemon : cluster_->daemons()) {
      auto count = FetchMetric(*daemon, kTxnStatusHandlerMetric);
      if (count.ok()) {
        result.txn_status_rpcs += count->total_count;
      }
    }
    return result;
  }

  void ReportPush(
      const std::string& name, const PushSample& b, const PushSample& e,
      const FakePushReceivers::Stats& receivers, size_t num_targets) {
    const auto ticks_per_sec = sysconf(_SC_CLK_TCK);
    const double seconds = ToSeconds(e.at - b.at);
    const double cpu_sec = static_cast<double>(e.cpu_ticks - b.cpu_ticks) / ticks_per_sec;
    const auto delta = [&b, &e](const char* metric) {
      const auto& end = e.metrics.at(metric);
      const auto& begin = b.metrics.at(metric);
      return end.total_count > 0 ? end.total_count - begin.total_count : end.value - begin.value;
    };
    const auto per = [](double num, double den) { return den > 0 ? num / den : 0.0; };
    const auto cycles = delta("local_catalog_push_cycles");
    const auto bytes = delta("local_catalog_push_bytes");
    std::lock_guard lock(ddl_mutex_);
    const auto ddl = ComputePercentiles(ddl_phases_[1].latency_us);
    LOG(INFO) << "==== Push scenario " << name << ": " << seconds << " s, " << num_targets
              << " receivers";
    LOG(INFO) << "  master CPU: " << per(cpu_sec, seconds) << " cores; " << cpu_sec << " CPU-s; "
              << per(cpu_sec * 1e6, cycles) << " us per cycle; " << per(cpu_sec * 1e3, bytes / 1e6)
              << " ms per MB pushed";
    LOG(INFO) << "  cycles " << cycles << " (" << per(cycles, seconds) << "/s), mean "
              << per(e.metrics.at("local_catalog_push_cycle_us").total_sum -
                         b.metrics.at("local_catalog_push_cycle_us").total_sum,
                     cycles)
              << " us; pages built " << delta("local_catalog_push_pages_built")
              << ", transient " << delta("local_catalog_push_transient_pages_built");
    LOG(INFO) << "  push RPCs " << delta("local_catalog_push_rpcs_sent") << ", bytes " << bytes
              << " (" << per(bytes, seconds * 1e6) << " MB/s), failures "
              << delta("local_catalog_push_failures") << "; in flight max "
              << e.metrics.at("local_catalog_push_in_flight_rpcs_max").value << " RPCs, "
              << e.metrics.at("local_catalog_push_in_flight_bytes_max").value << " bytes";
    LOG(INFO) << "  releases: inline pages " << delta("local_catalog_release_inline_pages")
              << ", skipped " << delta("local_catalog_release_inline_skipped")
              << "; floor requests " << delta("local_catalog_floor_requests_received")
              << "; coordinator status RPCs/s " << per(e.txn_status_rpcs - b.txn_status_rpcs,
                                                        seconds);
    LOG(INFO) << "  receivers: pushes " << receivers.pushes << ", pages " << receivers.pages
              << ", bytes " << receivers.bytes << ", gaps " << receivers.gaps << "; caught up "
              << receivers.caught_up << "/" << num_targets << ", p50 "
              << receivers.caught_up_ms.p50 << " ms, max " << receivers.caught_up_ms.max << " ms";
    LOG(INFO) << "  DDL: " << ddl.count << " ok, p50 " << ddl.p50 << " us, p99 " << ddl.p99
              << " us, errors " << ddl_phases_[1].errors;
    std::vector<std::pair<int64_t, std::string>> threads;
    for (const auto& [group, ticks] : e.thread_ticks) {
      auto it = b.thread_ticks.find(group);
      threads.emplace_back(ticks - (it == b.thread_ticks.end() ? 0 : it->second), group);
    }
    std::sort(threads.rbegin(), threads.rend());
    for (size_t i = 0; i != std::min<size_t>(threads.size(), 6); ++i) {
      LOG(INFO) << "  master threads " << threads[i].second << ": "
                << per(static_cast<double>(threads[i].first) / ticks_per_sec, seconds)
                << " cores";
    }
  }

  struct PushScenario {
    std::string name;
    size_t targets = 0;
    bool ddl_load = false;
    // DDLs run before the receivers start, so that they start this far behind.
    uint32_t lag_ddls = 0;
    bool advance = true;
    // Statements run once, after the receivers started, instead of the DDL loop.
    std::vector<std::string> once = {};
  };

  // Has master run a push cycle now, as a tserver's floor request does, so that its cache ends at
  // the committed index and the receivers have a head they can reach.
  Status CertifyNow() {
    auto proxy = cluster_->GetLeaderMasterProxy<cdc::SysCatalogChangeServiceProxy>();
    cdc::RequestSysCatalogPushRequestPB req;
    cdc::RequestSysCatalogPushResponsePB resp;
    req.set_requestor_uuid("load-test");
    req.set_floor(HybridTime::FromMicros(GetCurrentTimeMicros()).ToUint64());
    rpc::RpcController controller;
    controller.set_timeout(10s);
    RETURN_NOT_OK(proxy.RequestSysCatalogPush(req, &resp, &controller));
    std::this_thread::sleep_for(500ms);
    return Status::OK();
  }

  void RunPushScenario(const PushScenario& scenario) {
    auto start = ASSERT_RESULT(SysCatalogCommittedOpId());
    if (scenario.lag_ddls) {
      auto conn = ASSERT_RESULT(Connect());
      for (uint32_t i = 0; i != scenario.lag_ddls; ++i) {
        ASSERT_OK(conn.ExecuteFormat("CREATE TABLE lag_$0 (a INT)", i));
      }
    }
    ASSERT_OK(CertifyNow());
    if (!scenario.lag_ddls) {
      start = ASSERT_RESULT(SysCatalogCommittedOpId());
    }
    auto head = ASSERT_RESULT(SysCatalogCommittedOpId());

    FakePushReceivers receivers(
        scenario.targets, std::max(1U, FLAGS_local_catalog_load_client_messengers),
        scenario.advance);
    ASSERT_OK(receivers.Start(OpId::FromPB(start), head.index()));
    SetPhase(1);
    const auto begin = ASSERT_RESULT(SamplePush());
    ASSERT_OK(cluster_->SetFlag(
        &Master(), "TEST_local_catalog_push_extra_targets", receivers.TargetsFlag()));
    {
      TestThreadHolder ddl_threads;
      if (scenario.ddl_load) {
        StartDdlLoad(&ddl_threads);
      }
      if (!scenario.once.empty()) {
        auto conn = ASSERT_RESULT(Connect());
        for (const auto& stmt : scenario.once) {
          const auto stmt_start = CoarseMonoClock::Now();
          auto status = conn.Execute(stmt);
          RecordDdl(ToMicroseconds(CoarseMonoClock::Now() - stmt_start), status);
          ASSERT_OK(status);
        }
        const auto returned = CoarseMonoClock::Now();
        receivers.SetHead(ASSERT_RESULT(SysCatalogCommittedOpId()).index(), returned);
        ASSERT_OK(CertifyNow());
      }
      std::this_thread::sleep_for(FLAGS_local_catalog_load_duration_sec * 1s);
      ddl_threads.Stop();
    }
    const auto end = ASSERT_RESULT(SamplePush());
    ASSERT_OK(cluster_->SetFlag(&Master(), "TEST_local_catalog_push_extra_targets", ""));
    ReportPush(scenario.name, begin, end, receivers.Summarize(), scenario.targets);
    receivers.Shutdown();
  }
};

TEST_F(PgLocalCatalogPushLoadTest, YB_DISABLE_TEST(PushSteady)) {
  RunPushScenario({.name = "push_steady", .targets = FLAGS_local_catalog_load_push_targets,
                   .ddl_load = true});
}

TEST_F(PgLocalCatalogPushLoadTest, YB_DISABLE_TEST(PushFixedLag)) {
  RunPushScenario({.name = "push_fixed_lag", .targets = FLAGS_local_catalog_load_push_targets,
                   .lag_ddls = FLAGS_local_catalog_load_lag_ddls, .advance = false});
}

TEST_F(PgLocalCatalogPushLoadTest, YB_DISABLE_TEST(PushJoinerWave)) {
  RunPushScenario({.name = "push_joiner_wave", .targets = FLAGS_local_catalog_load_joiners,
                   .lag_ddls = FLAGS_local_catalog_load_lag_ddls});
}

TEST_F(PgLocalCatalogPushLoadTest, YB_DISABLE_TEST(PushIdle)) {
  RunPushScenario({.name = "push_idle", .targets = FLAGS_local_catalog_load_push_targets});
}

TEST_F(PgLocalCatalogPushLoadTest, YB_DISABLE_TEST(CreateDatabaseFanout)) {
  RunPushScenario({.name = "create_database_fanout",
                   .targets = FLAGS_local_catalog_load_push_targets,
                   .once = {"CREATE DATABASE fanout_db"}});
}

class PgLocalCatalogDdlWalSizeTest : public PgLocalCatalogLoadTest {
 protected:
  struct WalSample {
    int64_t op_index = 0;
    int64_t wal_bytes = 0;
    int64_t catalog_version = 0;
  };

  Result<WalSample> SampleWal(PGConn* conn) {
    WalSample result;
    result.op_index = VERIFY_RESULT(SysCatalogCommittedOpId()).index();
    result.wal_bytes = VERIFY_RESULT(
        FetchMetric(Master(), "log_bytes_logged", "tablet", master::kSysCatalogTabletId)).value;
    result.catalog_version = VERIFY_RESULT(conn->FetchRow<int64_t>(
        "SELECT current_version FROM pg_catalog.pg_yb_catalog_version WHERE db_oid = "
        "(SELECT oid FROM pg_database WHERE datname = current_database())"));
    return result;
  }

  // Runs one push receiver from `from` until it holds `to_index`, asking master for a push the
  // way a tserver waiting on its copy does, and returns the serialized size of the records above
  // `from` and the number of pages master pushed: what one tserver needs for the statements.
  Result<std::pair<int64_t, int64_t>> PushedBytes(const OpIdPB& from, int64_t to_index) {
    FakePushReceivers receiver(/* num_targets= */ 1, /* num_servers= */ 1, /* advance= */ true);
    receiver.CountRecordBytesAboveStart();
    RETURN_NOT_OK(receiver.Start(OpId::FromPB(from), to_index));
    auto proxy = cluster_->GetLeaderMasterProxy<cdc::SysCatalogChangeServiceProxy>();
    {
      cdc::RequestSysCatalogPushRequestPB req;
      cdc::RequestSysCatalogPushResponsePB resp;
      req.set_requestor_uuid(receiver.TargetUuid(0));
      *req.mutable_applied_op_id() = from;
      rpc::RpcController controller;
      controller.set_timeout(10s);
      RETURN_NOT_OK(proxy.RequestSysCatalogPush(req, &resp, &controller));
    }
    RETURN_NOT_OK(cluster_->SetFlag(
        &Master(), "TEST_local_catalog_push_extra_targets", receiver.TargetsFlag()));
    auto status = LoggedWaitFor(
        [&]() -> Result<bool> {
          if (receiver.Summarize().caught_up == 1) {
            return true;
          }
          cdc::RequestSysCatalogPushRequestPB req;
          cdc::RequestSysCatalogPushResponsePB resp;
          req.set_requestor_uuid(receiver.TargetUuid(0));
          req.set_floor(HybridTime::FromMicros(GetCurrentTimeMicros()).ToUint64());
          rpc::RpcController controller;
          controller.set_timeout(10s);
          RETURN_NOT_OK(proxy.RequestSysCatalogPush(req, &resp, &controller));
          return false;
        },
        120s, "The push receiver to catch up");
    RETURN_NOT_OK(cluster_->SetFlag(&Master(), "TEST_local_catalog_push_extra_targets", ""));
    RETURN_NOT_OK(status);
    const auto stats = receiver.Summarize();
    receiver.Shutdown();
    return std::make_pair(stats.record_bytes, stats.pages);
  }

  // Runs statements on conn and reports what master's system catalog WAL gained, including the
  // writes master makes in the 2 s after the last statement (DDL verification).
  void Measure(PGConn* conn, const std::string& name, const std::vector<std::string>& stmts) {
    const auto start_op = ASSERT_RESULT(SysCatalogCommittedOpId());
    const auto before = ASSERT_RESULT(SampleWal(conn));
    const auto started = CoarseMonoClock::Now();
    std::string failure;
    for (const auto& stmt : stmts) {
      auto status = conn->Execute(stmt);
      if (!status.ok()) {
        failure = Format(" | FAILED at '$0': $1", stmt, status.ToString());
        WARN_NOT_OK(conn->Execute("ROLLBACK"), "Rollback failed");
        break;
      }
    }
    const auto elapsed_ms = ToMilliseconds(CoarseMonoClock::Now() - started);
    std::this_thread::sleep_for(2s);
    const auto after = ASSERT_RESULT(SampleWal(conn));
    const auto [bytes, pages] = ASSERT_RESULT(PushedBytes(start_op, after.op_index));
    LOG(INFO) << "DDL WAL SIZE | " << name << " | ops " << after.op_index - before.op_index
              << " | wal_bytes " << after.wal_bytes - before.wal_bytes << " | record_bytes "
              << bytes << " | pages " << pages << " | catalog_version +"
              << after.catalog_version - before.catalog_version << " | ddl_ms " << elapsed_ms
              << failure;
  }

};

// WAL written to master's system catalog tablet by common DDL shapes, and what master pushes to one
// copy for it. The real tserver's copy is off, so master pushes to nothing else.
TEST_F(PgLocalCatalogDdlWalSizeTest, YB_DISABLE_TEST(DdlWalSize)) {
  auto conn = ASSERT_RESULT(Connect());
  const auto repeat = [](int n, const std::function<std::string(int)>& f) {
    std::vector<std::string> result;
    for (int i = 0; i != n; ++i) {
      result.push_back(f(i));
    }
    return result;
  };
  const auto wrap = [](std::vector<std::string> stmts) {
    stmts.insert(stmts.begin(), "BEGIN");
    stmts.push_back("COMMIT");
    return stmts;
  };

  Measure(&conn, "idle 2 s (control)", {});
  Measure(&conn, "CREATE TABLE t (a int, b text)", {"CREATE TABLE t (a INT, b TEXT)"});
  Measure(&conn, "ALTER TABLE t ADD COLUMN c int", {"ALTER TABLE t ADD COLUMN c INT"});
  Measure(&conn, "CREATE INDEX ON t (b)", {"CREATE INDEX ON t (b)"});
  Measure(&conn, "DROP TABLE t", {"DROP TABLE t"});

  ASSERT_OK(conn.Execute("CREATE SCHEMA s"));
  Measure(&conn, "100 x CREATE TABLE s.t_i (a int, b text), one txn each",
          repeat(100, [](int i) { return Format("CREATE TABLE s.t_$0 (a INT, b TEXT)", i); }));
  for (int i = 0; i != 100; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO s.t_$0 SELECT g, g::text FROM generate_series(1, 100) g", i));
  }
  Measure(&conn, "ANALYZE (database, 100 tables x 100 rows)", {"ANALYZE"});
  Measure(&conn, "BEGIN; ANALYZE; COMMIT (100 tables)", wrap({"ANALYZE"}));
  Measure(&conn, "DROP SCHEMA s CASCADE (100 tables)", {"DROP SCHEMA s CASCADE"});

  ASSERT_OK(conn.Execute("CREATE TABLE p (a INT, b TEXT) PARTITION BY RANGE (a)"));
  for (int i = 0; i != 100; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
        "CREATE TABLE p_$0 PARTITION OF p FOR VALUES FROM ($1) TO ($2)", i, i * 100,
        (i + 1) * 100));
  }
  Measure(&conn, "ALTER TABLE p ADD COLUMN c int (100 partitions)",
          {"ALTER TABLE p ADD COLUMN c INT"});

  Measure(&conn, "BEGIN; 20 x CREATE TABLE; COMMIT",
          wrap(repeat(20, [](int i) { return Format("CREATE TABLE m_$0 (a INT, b TEXT)", i); })));
  Measure(&conn, "BEGIN; 20 x ALTER TABLE ADD COLUMN; COMMIT",
          wrap(repeat(20, [](int i) { return Format("ALTER TABLE m_$0 ADD COLUMN c INT", i); })));
  std::vector<std::string> mixed;
  for (int i = 0; i != 5; ++i) {
    mixed.push_back(Format("CREATE TABLE n_$0 (a INT, b TEXT)", i));
    mixed.push_back(Format("ALTER TABLE m_$0 ADD COLUMN d INT", i));
    mixed.push_back(Format("CREATE INDEX ON m_$0 (b)", i + 5));
    mixed.push_back(Format("DROP TABLE m_$0", i + 10));
  }
  Measure(&conn, "BEGIN; 20 mixed (5 CREATE, 5 ADD COLUMN, 5 CREATE INDEX, 5 DROP); COMMIT",
          wrap(mixed));
}

TEST_F(PgLocalCatalogDdlWalSizeTest, YB_DISABLE_TEST(DdlWalSizeDatabase)) {
  auto conn = ASSERT_RESULT(Connect());
  Measure(&conn, "idle 2 s (control)", {});
  Measure(&conn, "CREATE DATABASE d1", {"CREATE DATABASE d1"});
  Measure(&conn, "DROP DATABASE d1", {"DROP DATABASE d1"});
}

}  // namespace yb::pgwrapper
