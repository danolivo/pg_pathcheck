# contrib/pg_pathcheck/Makefile

MODULE_big = pg_pathcheck
OBJS = \
	$(WIN32RES) \
	pg_pathcheck.o
PGFILEDESC = "pg_pathcheck - validate planner Path trees for freed memory"

EXTRA_CLEAN = nodetag_names.h pathtags_generated.h pg_pathcheck-*.zip make.log

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

# Convenience target: after the developer has audited walk_path() against
# the upstream pathnodes.h change, this rewrites pg_pathcheck.c's
# PPC_WALK_PATH_EXPECTED_HASHES block in place with the current hashes.
# The audit itself is not automated -- running this without auditing
# walk_path() defeats the entire guard.
.PHONY: bless-path-hashes
bless-path-hashes: pathtags_generated.h
	@echo "==> Refreshing PPC_WALK_PATH_EXPECTED_HASHES from pathtags_generated.h"
	@echo "    Did you audit walk_path() for the layout change?  If not, stop now."
	$(PERL) $(srcdir)/bless_path_hashes.pl $(srcdir)/pg_pathcheck.c pathtags_generated.h

# ---------------------------------------------------------------------------
# PGXN distribution archive.
#
# Builds pg_pathcheck-<version>.zip containing every git-tracked file in
# the working tree.  The version comes from META.json (single source of
# truth).  This is the master-targeted distribution; the parallel pg17-18
# back-branch build adds a -pg17-18 archive suffix to disambiguate.
#
# Run from a git checkout: `make dist` (or `make dist-pgxn`).
# ---------------------------------------------------------------------------

DISTVERSION := $(shell awk -F'"' '/"version":/ {print $$4; exit}' $(srcdir)/META.json)
DISTNAME := pg_pathcheck-$(DISTVERSION)

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
