# contrib/pg_pathcheck/Makefile

MODULE_big = pg_pathcheck
OBJS = \
	$(WIN32RES) \
	pg_pathcheck.o
PGFILEDESC = "pg_pathcheck - validate planner Path trees for freed memory"

EXTRA_CLEAN = nodetag_names.h

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
NODETAGS_H = $(shell $(PG_CONFIG) --includedir-server)/nodes/nodetags.h
else
subdir = contrib/pg_pathcheck
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
NODETAGS_H = $(top_srcdir)/src/backend/nodes/nodetags.h
endif

# Generate a tag-value-to-name lookup table from the server's nodetags.h.
nodetag_names.h: $(NODETAGS_H)
	sed -n 's/^[[:space:]]*\(T_[A-Za-z_]*\) = \([0-9]*\),.*/\t[\2] = "\1",/p' $< > $@

pg_pathcheck.o: nodetag_names.h
