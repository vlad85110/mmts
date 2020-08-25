EXTENSION = multimaster
DATA = multimaster--1.0.sql
OBJS = src/multimaster.o src/dmq.o src/commit.o src/bytebuf.o src/bgwpool.o \
src/pglogical_output.o src/pglogical_proto.o src/pglogical_receiver.o \
src/pglogical_apply.o src/pglogical_hooks.o src/pglogical_config.o \
src/pglogical_relid_map.o src/ddd.o src/bkb.o src/spill.o src/state.o \
src/resolver.o src/ddl.o src/syncpoint.o src/global_tx.o
MODULE_big = multimaster

PG_CPPFLAGS += -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)

ifdef USE_PGXS
PG_CPPFLAGS += -I$(CURDIR)/src/include
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
PG_CPPFLAGS += -I$(top_srcdir)/$(subdir)/src/include
subdir = contrib/mmts
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

.PHONY: all

EXTRA_INSTALL=contrib/pg_pathman contrib/referee

all: multimaster.so

submake-regress:
	$(MAKE) -C $(top_builddir)/src/test/regress all
	$(MAKE) -C $(top_builddir)/src/test/regress tablespace-setup

# all .pl tests should pass now, but let's see what the buildfarm says
# ifndef MTM_ALL
# PROVE_TESTS ?=
# endif
PROVE_FLAGS += --timer
check: temp-install submake-regress
	$(prove_check)

start: temp-install
	rm -rf '$(CURDIR)'/tmp_check
	$(MKDIR_P) '$(CURDIR)'/tmp_check
	cd $(srcdir) && TESTDIR='$(CURDIR)' \
		$(with_temp_install) \
		PG_REGRESS='$(CURDIR)/$(top_builddir)/src/test/regress/pg_regress' \
		perl run.pl --action=start $(RUN_OPTS)

stop:
	cd $(srcdir) && TESTDIR='$(CURDIR)' \
		$(with_temp_install) \
		perl run.pl --action=stop $(RUN_OPTS)

run-pg-regress: submake-regress
	cd $(CURDIR)/$(top_builddir)/src/test/regress && \
	$(with_temp_install) \
	PGPORT='65432' \
	PGHOST='127.0.0.1' \
	PGUSER='$(USER)' \
	./pg_regress \
	--bindir='' \
	--use-existing \
	--schedule=$(abs_top_srcdir)/src/test/regress/parallel_schedule \
	--dlpath=$(CURDIR)/$(top_builddir)/src/test/regress \
	--inputdir=$(abs_top_srcdir)/src/test/regress

run-pathman-regress:
	cd $(CURDIR)/$(top_builddir)/src/test/regress && \
	$(with_temp_install) \
	PGPORT='65432' \
	PGHOST='127.0.0.1' \
	PGUSER='$(USER)' \
	./pg_regress \
	--bindir='' \
	--use-existing \
	--temp-config=$(abs_top_srcdir)/contrib/test_partition/pg_pathman.add \
	--inputdir=$(abs_top_srcdir)/contrib/test_partition/ \
	partition


# bgw-based partition spawning is not supported by mm, so I
# commenting out body of set_spawn_using_bgw() sql function before
# running that
run-pathman-regress-ext:
	cd $(CURDIR)/$(top_builddir)/src/test/regress && \
	$(with_temp_install) \
	PGPORT='65432' \
	PGHOST='127.0.0.1' \
	PGUSER='$(USER)' \
	./pg_regress \
	--bindir='' \
	--use-existing \
	--temp-config=$(abs_top_srcdir)/contrib/pg_pathman/conf.add \
	--inputdir=$(abs_top_srcdir)/contrib/pg_pathman/ \
	pathman_array_qual pathman_basic pathman_bgw pathman_calamity pathman_callbacks \
	pathman_column_type pathman_cte pathman_domains pathman_dropped_cols pathman_expressions \
	pathman_foreign_keys pathman_gaps pathman_inserts pathman_interval pathman_join_clause \
	pathman_lateral pathman_hashjoin pathman_mergejoin pathman_only pathman_param_upd_del \
	pathman_permissions pathman_rebuild_deletes pathman_rebuild_updates pathman_rowmarks \
	pathman_runtime_nodes pathman_subpartitions pathman_update_node pathman_update_triggers \
	pathman_upd_del pathman_utility_stmt pathman_views

pg-regress: | start run-pg-regress
pathman-regress: | start run-pathman-regress-ext stop
installcheck:
	$(prove_installcheck)
