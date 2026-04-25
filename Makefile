# contrib/pg_pathcheck/Makefile

MODULE_big = pg_pathcheck
OBJS = \
	$(WIN32RES) \
	pg_pathcheck.o
PGFILEDESC = "pg_pathcheck - validate planner Path trees for freed memory"

REGRESS = pg_pathcheck
REGRESS_OPTS = --temp-config=$(srcdir)/pg_pathcheck.conf

EXTRA_CLEAN = nodetag_names.h pathtags_generated.h pg_pathcheck-*.zip

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
# them as PATH_TAG_LIST(X).  pg_pathcheck.c consumes this header to build
# is_path_tag(), so the recognised-tag set automatically tracks the
# pathnodes.h that is actually installed at build time.  No manual list to
# maintain.
pathtags_generated.h: $(PATHNODES_H) gen_pathtags.pl
	$(PERL) $(srcdir)/gen_pathtags.pl $< $@

pg_pathcheck.o: nodetag_names.h pathtags_generated.h

# ---------------------------------------------------------------------------
# PGXN distribution archive.
#
# Builds pg_pathcheck-<version>-pg17-18.zip containing every git-tracked
# file in the working tree.  The version comes from META.json (single
# source of truth); the -pg17-18 suffix marks this as the back-branch
# build to disambiguate it from the master-targeted distribution.
#
# Run from a git checkout: `make dist` (or `make dist-pgxn`).
#
# Note for PGXN submission: this Makefile names the archive file with the
# -pg17-18 suffix, but META.json's name/version fields are unchanged
# (pg_pathcheck / 0.9.1).  If you intend to publish two parallel releases
# on PGXN — one from master, one from this branch — you will likely want
# to either rename the distribution to pg_pathcheck-pg17-18 in META.json
# or version-suffix it (0.9.1+pg17-18 build metadata).  Decide at upload
# time; the archive filename produced here does not constrain that choice.
# ---------------------------------------------------------------------------

DISTVERSION := $(shell awk -F'"' '/"version":/ {print $$4; exit}' $(srcdir)/META.json)
DISTNAME := pg_pathcheck-$(DISTVERSION)-pg17-18

.PHONY: dist dist-pgxn
dist dist-pgxn: $(DISTNAME).zip

$(DISTNAME).zip: $(srcdir)/META.json
	@if ! git -C $(srcdir) rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		echo "ERROR: $@ requires a git checkout (uses git archive)"; \
		exit 1; \
	fi
	@if [ -z "$(DISTVERSION)" ]; then \
		echo "ERROR: could not extract version from META.json"; \
		exit 1; \
	fi
	@# `git archive HEAD` excludes uncommitted changes — including a
	@# freshly edited META.json, which would silently produce a release
	@# archive missing its own metadata.  `git stash create` produces a
	@# transient commit object that includes the working-tree state and
	@# returns its SHA; falls back to HEAD on a clean tree.  No
	@# user-visible stash entry is created.
	@rev=$$(git -C $(srcdir) stash create 2>/dev/null); \
	rev=$${rev:-HEAD}; \
	echo "==> Archiving from $$rev"; \
	git -C $(srcdir) archive --worktree-attributes --format=zip \
		--prefix=$(DISTNAME)/ -o $(CURDIR)/$@ "$$rev"
	@echo "==> Built $@ ($$(wc -c < $@) bytes)"
