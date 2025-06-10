
## Introduction to catalog cache

Many common postgres operations, like parsing a query, planning etc require looking up entries in postgres system catalog tables like pg\_class, pg\_operator, pg\_statistic, pg\_attribute to for [postgres metadata](https://gist.githubusercontent.com/iSignal/5b6f8480d9d8900ec6ebb777b9111248/raw/8be2e81f20ba8c1eab020cb188720dea72ca6a77/96%2520catalog%2520cache%2520misses%2520for%2520a%2520query) for the columns, operators etc. Each postgres backend (process) caches such metadata for performance reasons. In YugabyteDB, misses on these caches need to be loaded from the yb-master leader, so **initial queries on that backend can be slow until these caches are warm, especially if the leader yb-master is in a different region**. 

## Effect of DDLs on catalog caches

On most schema changes (DDLs), these caches are completely discarded on running backends and are then refreshed from either the yb-master leader or an intermediate response cache on the local yb-tserver. This refresh causes a latency hit on running queries while they wait for this process to complete. There is also a memory increase because the cache is now preloaded with all rows of these catalog tables (as opposed to just the actively used entries that it had before). 

## Key knobs

There is a tradeoff between the latency of loading these entries from a remote yb-master and the memory cost of prepopulating them ahead of time. YugabyteDB has a set of knobs that allow customizing this tradeoff to control the preloading entries into postgres caches. Here are some potential situations where one would want to tweak these knobs.

### Scenarios

1. **Initial queries on a new connection are slow.** The effect on slow initial queries during cache warmup may be particularly significant on multi-region clusters (where the master leader is far away from the postgres backend) or when the client does not have a steady pool of connections that are being reused.   
   1. To confirm that catalog caching is the cause of this, see the section [Confirming that catalog cache misses are a root cause of latency](#confirming-that-catalog-cache-misses-are-a-root-cause-of-latency)  
   2. See [Connection pooling](#connection-pooling) and [Preload additional system tables](#preload-additional-system-tables) for possible solutions.  
2. **High CPU load on yb-master leader**. If the client does not have a steady pool of connections, the resulting connection churn may cause load on the yb-master leader as these caches are warmed up repeatedly on each new postgres connection.  
   1. To confirm that catalog caching is the cause of this, see the section [Confirming that catalog cache misses are a root cause of latency](#confirming-that-catalog-cache-misses-are-a-root-cause-of-latency).  
   2. See [Connection pooling](#connection-pooling), [Tserver response cache](#tserver-response-cache) and [Preload additional system tables](#preload-additional-system-tables) for possible solutions.  
3. On the flip side, preloading of caches after a DDL change may cause **memory spikes on postgres backends or out of memory (OOM) events**.   
   1. To confirm that catalog caching is the cause of this, correlate the time when DDLs were run ([Write RPCs on yb-master](../../../../launch-and-manage/monitor-and-alert/metrics/ybmaster/#:~:text=handler_latency_yb_tserver_TabletServerService_Write)) to the time of the OOM event or a spike in [postgres RSS metrics](../../../../preview/yugabyte-platform/alerts-monitoring/anywhere-metrics/#per-process).  
   2. See [Minimal catalog cache preloading](#minimal-catalog-cache-preloading) for a possible solution.

### Details of knobs

| Knob | What does it do? | Effect | How to use it? |
| :---- | :---- | :---- | :---- |
| Connection pooling | When there is significant connection churn, the warm up of catalog caches on each new connection can cause high initial client latency and CPU load on the yb-master leader process.  Connection pooling allows better reuse of connections across different queries. | Reduces connection churn, so more queries should land on a backend with a warm cache. | It is highly recommended to set up connection pooling for YugabyteDB by exploring the following approaches. [Built-in connection pooling](../../../../explore/going-beyond-sql/connection-mgr-ysql/) on YugabyteDB server (Early Access in 2024.2). [Client-side connection pooling.](../../../../../drivers-orms/smart-drivers/#connection-pooling) [Intermediate connection pooling](https://www.yugabyte.com/blog/database-connection-management/) through tools like pgbouncer/odyssey.    |
| Minimal catalog cache preloading | After a DDL change, only a small subset of the catalog cache entries are preloaded. See [Effect of DDL changes on catalog caches](#effect-of-ddls-on-catalog-caches) to compare with the default behavior.  | This reduces the memory spike that results but increases the warm up time for queries after a DDL change. | Set the yb-tserver gflag \--ysql\_minimal\_catalog\_caches\_preload=true |
| Preload additional system tables | All catalog cache entries corresponding to specific pg catalog tables are preloaded (both on regular postgres backend startup and after a DDL change)  | Decreases warm up time for these caches and hence, decreases the latency impact of initial queries on new connections. Causes more memory consumption on all backends, irrespective of a DDL change. | See [Identifying the specific tables to be preloaded](#identify-the-specific-tables-to-be-preloaded) for how to identify the catalog tables to be preloaded.  Set the yb-tserver flag  \--ysql\_catalog\_preload\_additional\_tables=true to preload caches for the following tables pg\_am,pg\_amproc,pg\_cast,pg\_inherits,pg\_policy,pg\_proc,pg\_tablespace,pg\_trigger Set the yb-tserver flag \--ysql\_catalog\_preload\_additional\_table\_list=\<list of pg tables\>, to populate caches for these tables in addition to the default list. For example \--ysql\_catalog\_preload\_additional\_table\_list=pg\_operator,pg\_amop,pg\_cast,pg\_aggregate.   |
| Tserver response cache | Enables an intermediate cache on the yb-tserver for certain Postgres \-\> yb-master RPCs It is enabled by default in YB releases \>= 2024.1 | Reduces CPU load on yb-master leader. | Set the yb-tserver flag `--ysql_enable_read_request_caching=true` |

## Using the knobs 

### Confirming that catalog cache misses are a root cause of latency

To confirm that catalog cache misses are a cause, use these techniques

1. Run [EXPLAIN (ANALYZE, DIST) \<query\>](../../../../explore/query-1-performance/explain-analyze/#:~:text=Index%20Writes.-,Catalog%20Read%20Requests,-%3A%20Number%20of%20requests) on the 1st query on a new connection shows a high number of Catalog Reads. A subsequent run of the same EXPLAIN (ANALYZE, DIST) typically shows a drop in the number of Catalog Reads.  
2. YBA/YBM metrics dashboards show a [high number](https://docs.yugabyte.com/images/yp/metrics114.png) of [Catalog Cache Misses](../../../../../yugabyte-platform/alerts-monitoring/anywhere-metrics/#ysql-ops-and-latency:~:text=on%20other%20metrics.-,Catalog%20Cache%20Misses,-During%20YSQL%20query). There should be a [corresponding rate of increase of yb-master Read RPCs](../../../../launch-and-manage/monitor-and-alert/metrics/ybmaster/#:~:text=handler_latency_yb_tserver_TabletServerService_Read).  
3. Troubleshooting framework shows the anomaly **Excessive Catalog Reads.**  
4. [Manually collect logs](#manually-collecting-logs-for-catalog-reads).


### Identify the specific tables to be preloaded

From the [Catalog Cache Misses](../../../../yugabyte-platform/alerts-monitoring/anywhere-metrics/#ysql-ops-and-latency:~:text=on%20other%20metrics.-,Catalog%20Cache%20Misses,-During%20YSQL%20query) metrics dashboard, identify the Postgres catalog table names that are causing the highest misses. You can do this manually or by using the “Outlier Tables” view on the dashboard. Once the top N catalog tables are identified, add them one by one to the gflag until the first conn latency is acceptable to the gflag `--ysql_catalog_preload_additional_table_list`. It might be sufficient to just set `--ysql_catalog_preload_additional_tables=true` [as appropriate](#key-knobs). 

If there are still a significant number of misses to these tables after preloading them, [manually collect logs](#manually-collecting-logs-for-catalog-reads) and share them to Yugabyte Support.


### Manually collecting logs for catalog reads

If the catalog reads can be traced to a specific query, set the following GUCs and run [EXPLAIN (ANALYZE, DIST) \<query\>](../../../../explore/query-1-performance/explain-analyze/#:~:text=Distributed%20Storage%20Counters)

```

SET yb_debug_log_catcache_events = 1;
SET yb_debug_report_error_stacktrace = 1;
SET client_min_messages = LOG;
```

Collect the output from this session.

If no specific query can be identified, set the gflag [`--ysql_pg_conf_csv`](https://docs.yugabyte.com/preview/reference/configuration/all-flags-yb-tserver/#ysql-pg-conf-csv) to include  `yb_debug_log_catcache_events=1` for a short duration and collect the postgres log file (doing so may affect performance significantly).
