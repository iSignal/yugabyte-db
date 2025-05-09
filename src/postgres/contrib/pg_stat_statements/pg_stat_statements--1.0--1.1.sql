/* contrib/pg_stat_statements/pg_stat_statements--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.1'" to load this file. \quit

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_statements DROP VIEW pg_stat_statements;
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements();

/* Then we can drop them */
DROP VIEW pg_stat_statements;
DROP FUNCTION pg_stat_statements();

/* Now redefine */
CREATE FUNCTION pg_stat_statements(
    OUT userid oid,
    OUT dbid oid,
    OUT query text,
    OUT calls int8,
    OUT total_time float8,
    OUT rows int8,
    OUT shared_blks_hit int8,
    OUT shared_blks_read int8,
    OUT shared_blks_dirtied int8,
    OUT shared_blks_written int8,
    OUT local_blks_hit int8,
    OUT local_blks_read int8,
    OUT local_blks_dirtied int8,
    OUT local_blks_written int8,
    OUT temp_blks_read int8,
    OUT temp_blks_written int8,
    OUT blk_read_time float8,
    OUT blk_write_time float8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE VIEW pg_stat_statements AS
  SELECT * FROM pg_stat_statements();

GRANT SELECT ON pg_stat_statements TO PUBLIC;

/***

CREATE TABLE yb_stat_statements(
    userid oid,
    dbid oid,
    queryid bigint,
    query text,
    calls int8,
    total_time float8,
    min_time float8,
    max_time float8,
    mean_time float8,
    stddev_time float8,
    rows int8,
    shared_blks_hit int8,
    shared_blks_read int8,
    shared_blks_dirtied int8,
    shared_blks_written int8,
    local_blks_hit int8,
    local_blks_read int8,
    local_blks_dirtied int8,
    local_blks_written int8,
    temp_blks_read int8,
    temp_blks_written int8,
    blk_read_time float8,
    blk_write_time float8,
    yb_latency_histogram jsonb
);

**/