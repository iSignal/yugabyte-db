# contrib/yb_xcluster_ddl_replication/Makefile

PGFILEDESC = "yb_xcluster_ddl_replication - YugaByte xCluster DDL replication"

EXTENSION = yb_xcluster_ddl_replication
DATA = yb_xcluster_ddl_replication--1.0.sql
SHLIB_LINK += -L$(YB_BUILD_ROOT)/lib -lyb_pggate

MODULE_big = yb_xcluster_ddl_replication
OBJS = \
	yb_xcluster_ddl_replication.o \
	extension_util.o \
	json_util.o \
	source_ddl_end_handler.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/yb_xcluster_ddl_replication
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
