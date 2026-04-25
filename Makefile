# contrib/pg_pathcheck/Makefile

MODULE_big = pg_pathcheck
OBJS = \
	$(WIN32RES) \
	pg_pathcheck.o
PGFILEDESC = "pg_pathcheck - validate planner Path trees for freed memory"

REGRESS = pg_pathcheck
REGRESS_OPTS = --temp-config=$(srcdir)/pg_pathcheck.conf

EXTRA_CLEAN = nodetag_names.h pathtags_generated.h

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
NODETAGS_H = $(shell $(PG_CONFIG) --includedir-server)/nodes/nodetags.h
PATHNODES_H = $(shell $(PG_CONFIG) --includedir-server)/nodes/pathnodes.h
else
subdir = contrib/pg_pathcheck
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
NODETAGS_H = $(top_srcdir)/src/backend/nodes/nodetags.h
PATHNODES_H = $(top_srcdir)/src/include/nodes/pathnodes.h
endif

# Generate a tag-value-to-name lookup table from the server's nodetags.h.
nodetag_names.h: $(NODETAGS_H)
	sed -n 's/^[[:space:]]*\(T_[A-Za-z_]*\) = \([0-9]*\),.*/\t[\2] = "\1",/p' $< > $@

# Derive the set of concrete Path subtype NodeTags from pathnodes.h and emit
# a structural hash per subtype.  pg_pathcheck.c consumes this header to
# build is_path_tag() and to compile-time-check that walk_path()'s per-tag
# dispatcher and blessed hash set stay in sync with upstream.  If pathnodes.h
# gains, loses or edits a Path subtype, the build fails at a StaticAssertDecl
# in pg_pathcheck.c until the developer audits walk_path() and blesses the
# new hash(es).
pathtags_generated.h: $(PATHNODES_H) gen_pathtags.pl
	$(PERL) $(srcdir)/gen_pathtags.pl $< $@

pg_pathcheck.o: nodetag_names.h pathtags_generated.h
