/*-------------------------------------------------------------------------
 *
 * pg_pathcheck.c
 *	  Walk every Path in a finished query tree and flag freed / garbage nodes.
 *
 *	  Debug aid only.  Targets PostgreSQL 17 and 18.  Hooks
 *	  create_upper_paths_hook at UPPERREL_FINAL of the top query and runs
 *	  walk_top_root() inline; bogus or alias-recycled Paths are reported
 *	  with relation names and pathlist contents.  See walk_top_root(),
 *	  walk_path() and is_path_tag() for the per-function detail.
 *
 * Copyright (c) 2026 Andrei Lepikhov
 * Released under the MIT License; see LICENSE in the project root.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

#include "pathtags_generated.h"

#define PPC_NAME	"pg_pathcheck"
#define PPC_VERSION	"0.9.1"

/*
 * Common errhint() clause: every finding wants the triggering query in the
 * report.  debug_query_string is NULL for queries that arrive through paths
 * other than the protocol top level (function bodies, plancache, etc.), so
 * fall back to a placeholder.  Used inside ereport() argument lists.
 */
#define PPC_QUERY_HINT \
	errhint("query: %s", debug_query_string ? debug_query_string : "(null)")

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
	.name = PPC_NAME,
	.version = PPC_VERSION
);
#else
/* PG 17 predates PG_MODULE_MAGIC_EXT. */
PG_MODULE_MAGIC;
#endif

void		_PG_init(void);

/* Chained upstream hooks. */
static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/*
 * GUC: pg_pathcheck.elevel
 *		The elevel passed to ereport() when a corrupt Path is detected.
 *		LOG writes to the server log only; WARNING (default) also notifies
 *		the client and continues; ERROR aborts the statement; PANIC crashes
 *		the backend so you get a core dump for post-mortem.
 */
static int	ppc_elevel = WARNING;

static const struct config_enum_entry ppc_elevel_options[] = {
	{"log", LOG, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{"panic", PANIC, false},
	{NULL, 0, false}
};

/*
 * GUC: pg_pathcheck.stage_checks
 *		When on, the per-stage hooks (set_rel_pathlist_hook,
 *		set_join_pathlist_hook, and the pathlist check inside
 *		create_upper_paths_hook) walk the affected rels' pathlists on every
 *		firing.  Off by default: those checks multiply planner work by the
 *		number of base/join/upper stages and are only needed when narrowing
 *		down a bug already flagged by the end-of-planning walker.
 *
 *		Independently of this flag, see pg_pathcheck.end_walk for the
 *		end-of-planning walker.
 */
static bool ppc_stage_checks = false;

/*
 * GUC: pg_pathcheck.end_walk
 *		Master switch for the end-of-planning walk fired at UPPERREL_FINAL of
 *		the top query.  On by default; turn off to load the library purely for
 *		the per-stage tripwires (when stage_checks is on) or to silence the
 *		walker without unloading the library.
 */
static bool ppc_end_walk = true;

/* Forward declarations. */
static void ppc_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel, RelOptInfo *output_rel,
								   void *extra);
static void ppc_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
								  RelOptInfo *outerrel, RelOptInfo *innerrel,
								  JoinType jointype, JoinPathExtraData *extra);
static void ppc_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								 Index rti, RangeTblEntry *rte);
static void check_rel_pathlists(RelOptInfo *rel, const char *ctx,
								PlannerInfo *root);
static void check_rel_pathlist(List *paths, const char *listname,
							   RelOptInfo *rel, const char *ctx,
							   PlannerInfo *root);
static const char *upper_stage_name(UpperRelationKind stage);
static void walk_top_root(PlannerInfo *top_root, PlannerGlobal *glob);
static void walk_planner_info(HTAB *visited, PlannerInfo *root);
static void walk_rel(HTAB *visited, RelOptInfo *rel, PlannerInfo *root);
static void walk_pathlist(HTAB *visited, List *paths, const char *listname,
						  RelOptInfo *rel, PlannerInfo *root);
static void walk_path(HTAB *visited, Path *path, const char *source,
					  List *container, RelOptInfo *rel, PlannerInfo *root);
static void verify_path_parent(Path *path, RelOptInfo *expected,
							   const char *source, List *container,
							   PlannerInfo *root);
static bool mark_visited(HTAB *visited, void *ptr);
static bool is_path_tag(NodeTag tag);
static const char *tag_name(int tag);
static const char *format_relnames(RelOptInfo *rel, PlannerInfo *root);
static const char *format_pathlist(List *paths);


/*
 * _PG_init
 *		Register the GUCs and chain onto the planner hooks we need.
 */
void
_PG_init(void)
{
	DefineCustomEnumVariable(PPC_NAME ".elevel",
							 "elevel used when a corrupt Path is detected.",
							 "LOG writes to the server log only, WARNING "
							 "also notifies the client and continues, ERROR "
							 "aborts the statement, PANIC crashes for a "
							 "core dump.",
							 &ppc_elevel,
							 WARNING,
							 ppc_elevel_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable(PPC_NAME ".stage_checks",
							 "Run per-stage pathlist checks at base-rel, "
							 "join-rel and upper-rel hook boundaries.",
							 "Off by default.  Turn on only to narrow down a "
							 "finding already flagged at end of planning.",
							 &ppc_stage_checks,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable(PPC_NAME ".end_walk",
							 "Run the end-of-planning Path-tree walk at "
							 "UPPERREL_FINAL of the top query.",
							 "On by default.  Turn off to silence the walker "
							 "without unloading the library.",
							 &ppc_end_walk,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved(PPC_NAME);

	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = ppc_create_upper_paths;

	prev_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = ppc_set_join_pathlist;

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = ppc_set_rel_pathlist;
}


/*
 * ppc_set_join_pathlist
 *		Early dangling-pointer check, fired immediately after a join's
 *		pathlists have been populated.  Walks every entry of the two
 *		sibling rels' pathlist / partial_pathlist — i.e. the full outer
 *		and inner input-rel pathlists, not just the children referenced
 *		by the new join paths.  Any entry with a non-Path NodeTag, or a
 *		valid Path tag whose ->parent has drifted to another rel, is
 *		reported at pg_pathcheck.elevel.
 *
 *		This narrows the detection window from end-of-planning to
 *		end-of-this-join.  Set pg_pathcheck.elevel = 'error' to abort the
 *		statement on the first finding and pin the guilty query.
 */
static void
ppc_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					  RelOptInfo *outerrel, RelOptInfo *innerrel,
					  JoinType jointype, JoinPathExtraData *extra)
{
	if (prev_set_join_pathlist_hook)
		(*prev_set_join_pathlist_hook) (root, joinrel, outerrel, innerrel,
										jointype, extra);

	if (!ppc_stage_checks)
		return;

	Assert(joinrel != NULL && outerrel != NULL && innerrel != NULL);

	{
		char	   *outer_ctx = psprintf("outer side of join rel %s",
										 format_relnames(joinrel, root));
		char	   *inner_ctx = psprintf("inner side of join rel %s",
										 format_relnames(joinrel, root));

		check_rel_pathlists(outerrel, outer_ctx, root);
		check_rel_pathlists(innerrel, inner_ctx, root);
	}
}


/*
 * ppc_set_rel_pathlist
 *		Fires at the end of set_rel_pathlist() for each base rel.  Uses the
 *		same pathlist/partial_pathlist validator as the join hook — every
 *		entry must be a live Path whose ->parent is this rel.  Detection at
 *		this point localises a finding to a specific set_*_pathlist step.
 */
static void
ppc_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					 RangeTblEntry *rte)
{
	if (prev_set_rel_pathlist_hook)
		(*prev_set_rel_pathlist_hook) (root, rel, rti, rte);

	if (!ppc_stage_checks)
		return;

	Assert(rel != NULL);

	check_rel_pathlists(rel, "base rel", root);
}


/*
 * check_rel_pathlists
 *		Check both pathlist and partial_pathlist of one rel.  ctx is a short
 *		free-form string appended to every finding to describe the call site
 *		("base rel", "outer side of join rel {a,b}", etc.) so a developer
 *		can correlate findings with a specific planner step.
 */
static void
check_rel_pathlists(RelOptInfo *rel, const char *ctx, PlannerInfo *root)
{
	check_rel_pathlist(rel->pathlist, "pathlist", rel, ctx, root);
	check_rel_pathlist(rel->partial_pathlist, "partial_pathlist",
					   rel, ctx, root);
}


/*
 * check_rel_pathlist
 *		Validate every Path pointer in one list owned by rel.  Two failure
 *		modes:
 *		  (a) the entry's NodeTag is not a Path-family tag — the chunk has
 *		      been freed or overwritten (dangling pointer);
 *		  (b) the entry carries a valid Path tag but ->parent does not
 *		      match the owning rel — same-size-class aliasing, the slot
 *		      has been re-claimed by a Path of a different rel.
 */
static void
check_rel_pathlist(List *paths, const char *listname, RelOptInfo *rel,
				   const char *ctx, PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, paths)
	{
		Path	   *p = (Path *) lfirst(lc);
		NodeTag		tag;
		int			i = foreach_current_index(lc);

		if (p == NULL)
			continue;

		tag = nodeTag(p);
		if (!is_path_tag(tag))
		{
			ereport(ppc_elevel,
					errmsg(PPC_NAME ": dangling pointer at %s[%d], rel %s (%s)",
						   listname, i,
						   format_relnames(rel, root), ctx),
					errdetail("invalid NodeTag %s; %s contents: %s",
							  tag_name((int) tag), listname,
							  format_pathlist(paths)),
					PPC_QUERY_HINT);
			continue;
		}

		/*
		 * Upper rels legitimately hold paths whose ->parent is the input
		 * rel (apply_scanjoin_target_to_paths and similar), so the
		 * identity check only applies to base and join rels.
		 */
		if (!IS_UPPER_REL(rel) && p->parent != rel)
		{
			RelOptInfo *actual = p->parent;

			ereport(ppc_elevel,
					errmsg(PPC_NAME ": path parent mismatch at %s[%d], rel %s (%s)",
						   listname, i,
						   format_relnames(rel, root), ctx),
					errdetail("path %s claims rel %s",
							  tag_name((int) tag),
							  (actual != NULL && IsA(actual, RelOptInfo))
							  ? format_relnames(actual, root)
							  : "(garbage)"),
					PPC_QUERY_HINT);
		}
	}
}


/*
 * upper_stage_name
 *		Symbolic name for an UpperRelationKind value, for diagnostics.
 */
static const char *
upper_stage_name(UpperRelationKind stage)
{
	switch (stage)
	{
		case UPPERREL_SETOP:			return "UPPERREL_SETOP";
		case UPPERREL_PARTIAL_GROUP_AGG:	return "UPPERREL_PARTIAL_GROUP_AGG";
		case UPPERREL_GROUP_AGG:		return "UPPERREL_GROUP_AGG";
		case UPPERREL_WINDOW:			return "UPPERREL_WINDOW";
		case UPPERREL_PARTIAL_DISTINCT:	return "UPPERREL_PARTIAL_DISTINCT";
		case UPPERREL_DISTINCT:			return "UPPERREL_DISTINCT";
		case UPPERREL_ORDERED:			return "UPPERREL_ORDERED";
		case UPPERREL_FINAL:			return "UPPERREL_FINAL";
	}
	return "UPPERREL_?";
}


/*
 * ppc_create_upper_paths
 *		Dual duty:
 *		  1. Fire walk_top_root() at UPPERREL_FINAL of the top query.  The
 *		     walk runs before create_plan / setrefs.c, so Path-lifetime bugs
 *		     visible only after those stages are out of scope; base-, join-
 *		     and upper-rel Path generation is complete by this point.
 *		  2. Check the input and output rels' pathlist / partial_pathlist at
 *		     every upper-rel stage.  This narrows detection from "end of
 *		     planning" to "end of this upper-rel stage", so a finding at
 *		     (say) UPPERREL_ORDERED but not at UPPERREL_WINDOW pins the bug
 *		     to ordered-paths construction in grouping_planner.
 */
static void
ppc_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (prev_create_upper_paths_hook)
		(*prev_create_upper_paths_hook) (root, stage, input_rel, output_rel,
										 extra);

	Assert(root != NULL && IsA(root, PlannerInfo));

	if (ppc_end_walk &&
		root->parent_root == NULL && stage == UPPERREL_FINAL)
	{
		Assert(root->glob != NULL);
		walk_top_root(root, root->glob);
	}

	if (ppc_stage_checks)
	{
		const char *sname = upper_stage_name(stage);
		char	   *in_ctx = psprintf("create_upper_paths input, stage %s", sname);
		char	   *out_ctx = psprintf("create_upper_paths output, stage %s", sname);

		if (input_rel != NULL)
			check_rel_pathlists(input_rel, in_ctx, root);
		if (output_rel != NULL)
			check_rel_pathlists(output_rel, out_ctx, root);
	}
}


/*
 * walk_top_root
 *		End-of-planning walker body.  Visits every RelOptInfo reachable from
 *		top_root, recurses into glob->subroots, and sweeps glob->subpaths for
 *		dangling pointers.
 *
 *		The dedup HTAB is created on the stack of this function and threaded
 *		through the recursion as a parameter.  Keeping it off file-scope makes
 *		the lifetime visible at every call site and structurally rules out
 *		re-entrance surprises (nested planner() calls during outer planning);
 *		each walk has its own private dedup state.
 */
static void
walk_top_root(PlannerInfo *top_root, PlannerGlobal *glob)
{
	HASHCTL		ctl = {0};
	HTAB	   *visited;

	Assert(top_root != NULL && IsA(top_root, PlannerInfo));
	Assert(glob != NULL && IsA(glob, PlannerGlobal));
	Assert(glob == top_root->glob);

	ctl.keysize = sizeof(void *);
	ctl.entrysize = sizeof(void *);
	ctl.hcxt = CurrentMemoryContext;

	/*
	 * Allocate the dedup HTAB in CurrentMemoryContext, which at hook-fire
	 * time is the planner's per-query context (typically MessageContext for
	 * a top-level query, or whatever context the caller of standard_planner
	 * set up for a nested call).  hash_destroy() below releases it on the
	 * normal return path; on ereport(ERROR) the context owner reclaims it
	 * when the transaction or message context is reset, so we never leak.
	 */
	visited = hash_create(PPC_NAME " visited", 64, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	walk_planner_info(visited, top_root);

	/* Subplan PlannerInfos and their backing top-level Paths. */
	foreach_node(PlannerInfo, root, glob->subroots)
		walk_planner_info(visited, root);

	/*
	 * glob->subpaths holds the top-level Path produced for each SubPlan;
	 * not every entry is reachable from a per-rel pathlist, so walk this
	 * flat list directly to be sure we cover them.  mark_visited() dedups
	 * any path already seen via the subroot recursion above.
	 */
	walk_pathlist(visited, glob->subpaths, "glob->subpaths", NULL, top_root);

	hash_destroy(visited);
}


/*
 * walk_planner_info
 *		Visit every RelOptInfo on this root and recurse into per-RTE
 *		subquery PlannerInfos via RelOptInfo->subroot.  The PlannerGlobal
 *		subroots list is walked separately by the caller (walk_top_root).
 */
static void
walk_planner_info(HTAB *visited, PlannerInfo *root)
{
	int			i;

	if (root == NULL || !mark_visited(visited, root))
		return;

	Assert(IsA(root, PlannerInfo));
	check_stack_depth();

	/* Upper rels: one List per UpperRelationKind. */
	for (i = 0; i <= UPPERREL_FINAL; i++)
	{
		foreach_node(RelOptInfo, rel, root->upper_rels[i])
			walk_rel(visited, rel, root);
	}

	/* Base rels and appendrel children. */
	Assert(root->simple_rel_array_size >= 0);
	Assert(root->simple_rel_array_size == 0 ||
		   root->simple_rel_array != NULL);
	for (i = 1; i < root->simple_rel_array_size; i++)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel == NULL)
			continue;
		walk_rel(visited, rel, root);
		/* Recurse into subquery PlannerInfos. */
		if (rel->subroot != NULL)
		{
			Assert(IsA(rel->subroot, PlannerInfo));
			walk_planner_info(visited, rel->subroot);
		}
	}

	/* Join rels collected during dynamic programming. */
	foreach_node(RelOptInfo, rel, root->join_rel_list)
		walk_rel(visited, rel, root);

	/* Non-recursive term of a recursive CTE, if any. */
	walk_path(visited, root->non_recursive_path, "non_recursive_path", NULL,
			  NULL, root);
}


/*
 * walk_rel
 *		Visit every Path slot on a RelOptInfo, and recurse into parallel
 *		RelOptInfos that hang off it (part_rels[]).  Upward links
 *		(parent / top_parent) are intentionally not followed: those rels are
 *		reached via simple_rel_array or join_rel_list anyway.
 */
static void
walk_rel(HTAB *visited, RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	if (rel == NULL || !mark_visited(visited, rel))
		return;

	Assert(IsA(rel, RelOptInfo));

	/*
	 * Parent-match check for every Path directly attached to this rel.
	 * If a slot has been reused by a Path built for another rel
	 * (same-size-class aliasing that survives the NodeTag check), the
	 * reused Path's ->parent points to its real owner, not to us.
	 *
	 * Skip upper rels: their pathlists legitimately carry paths whose
	 * ->parent is the input rel (see apply_scanjoin_target_to_paths and
	 * similar).  The invariant only holds for base and join rels.
	 */
	if (!IS_UPPER_REL(rel))
	{
		foreach(lc, rel->pathlist)
			verify_path_parent(lfirst(lc), rel, "pathlist",
							   rel->pathlist, root);
		foreach(lc, rel->partial_pathlist)
			verify_path_parent(lfirst(lc), rel, "partial_pathlist",
							   rel->partial_pathlist, root);
		foreach(lc, rel->cheapest_parameterized_paths)
			verify_path_parent(lfirst(lc), rel, "cheapest_parameterized_paths",
							   rel->cheapest_parameterized_paths, root);
		verify_path_parent(rel->cheapest_startup_path, rel,
						   "cheapest_startup_path", NULL, root);
		verify_path_parent(rel->cheapest_total_path, rel,
						   "cheapest_total_path", NULL, root);
		verify_path_parent(rel->cheapest_unique_path, rel,
						   "cheapest_unique_path", NULL, root);
	}

	walk_pathlist(visited, rel->pathlist, "pathlist", rel, root);
	walk_pathlist(visited, rel->partial_pathlist, "partial_pathlist", rel, root);
	walk_pathlist(visited, rel->cheapest_parameterized_paths,
				  "cheapest_parameterized_paths", rel, root);
	walk_path(visited, rel->cheapest_startup_path, "cheapest_startup_path",
			  NULL, rel, root);
	walk_path(visited, rel->cheapest_total_path, "cheapest_total_path", NULL,
			  rel, root);
	walk_path(visited, rel->cheapest_unique_path, "cheapest_unique_path", NULL,
			  rel, root);

	/*
	 * Partition children: each one is also reachable via the upper_rels /
	 * simple_rel_array traversal above, so this descent is normally
	 * redundant.  Kept as a paranoid defence: if the main traversal misses
	 * a partition rel because of structural drift, walking part_rels[]
	 * gives a second chance to spot the corruption.
	 */
	if (rel->part_rels != NULL)
	{
		int	i;

		Assert(rel->nparts >= 0);
		for (i = 0; i < rel->nparts; i++)
			if (rel->part_rels[i] != NULL)
			{
				Assert(IsA(rel->part_rels[i], RelOptInfo));
				walk_rel(visited, rel->part_rels[i], root);
			}
	}
}


/*
 * verify_path_parent
 *		Confirm that a Path found directly on rel's own pathlist-family
 *		fields actually claims rel as its parent.  A mismatch catches the
 *		aliasing case that escapes the NodeTag check: the memory chunk was
 *		freed and re-allocated as a different Path (possibly belonging to
 *		another rel) within the same planning session.
 *
 *		Called only from walk_rel, never from the sub-path recursion —
 *		inside compound Path nodes the parent legitimately varies (e.g.,
 *		JoinPath.outerjoinpath belongs to the child rel, not the join rel).
 */
static void
verify_path_parent(Path *path, RelOptInfo *expected, const char *source,
				   List *container, PlannerInfo *root)
{
	RelOptInfo *actual;

	/*
	 * Skip NULL and paths with an invalid NodeTag: walk_path already
	 * reports those, and ->parent on a bogus chunk is not readable.
	 */
	if (path == NULL || !is_path_tag(nodeTag(path)))
		return;

	actual = path->parent;
	if (actual == expected)
		return;

	/*
	 * The mismatched ->parent may itself be freed memory; validate its
	 * NodeTag before reading ->reloptkind via IS_UPPER_REL().
	 */
	if (actual == NULL || !IsA(actual, RelOptInfo))
	{
		ereport(ppc_elevel,
				errmsg(PPC_NAME ": path has non-RelOptInfo parent in %s, target rel %s",
					   source, format_relnames(expected, root)),
				PPC_QUERY_HINT);
		return;
	}

	/*
	 * Upper rels legitimately carry paths whose ->parent is the input rel
	 * (see apply_scanjoin_target_to_paths and friends).  We filter these out
	 * here rather than in walk_rel so that a garbage ->parent is still caught
	 * by the IsA check above.
	 */
	if (IS_UPPER_REL(actual))
		return;

	/*
	 * Classic same-size-class alias: the slot was reused by another rel's
	 * path.  Name both rels by their contributing base relations.
	 */
	ereport(ppc_elevel,
			errmsg(PPC_NAME ": path parent mismatch in %s, target rel %s",
				   source, format_relnames(expected, root)),
			container != NULL
			? errdetail("path %s claims rel %s, path signature: rows: %.0lf, scost: %.2lf, tcost: %.2lf; %s contents: %s",
						tag_name(path->type),
						actual->relids != NULL ? nodeToString(actual->relids) : "UPPER_REL",
						path->rows, path->startup_cost, path->total_cost,
						source, format_pathlist(container))
			: errdetail("path %s claims rel %s, path signature: rows: %.0lf, scost: %.2lf, tcost: %.2lf",
						tag_name(path->type),
						actual->relids != NULL ? nodeToString(actual->relids) : "UPPER_REL",
						path->rows, path->startup_cost, path->total_cost),
			PPC_QUERY_HINT);
}


/*
 * walk_pathlist
 *		Visit each Path in a List, tagging every element with the list's name.
 */
static void
walk_pathlist(HTAB *visited, List *paths, const char *listname,
			  RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, paths)
		walk_path(visited, (Path *) lfirst(lc), listname, paths, rel, root);
}


/*
 * walk_path
 *		Validate a Path's NodeTag and descend into every embedded sub-Path.
 *
 *		The NodeTag check is the main "is this memory still a Path" probe.
 *		With CLOBBER_FREED_MEMORY the tag becomes 0x7F7F7F7F which fails
 *		is_path_tag; with a real freed-and-reused chunk the odds of landing
 *		on a valid Path tag are vanishingly low.
 *
 *		source names the field or list that contains this Path (for diagnostics).
 *		container, when non-NULL, is the List whose full contents are dumped
 *		in the errdetail when corruption is detected.
 *
 *		Layout note: a back-branch rename of a walked field will break the
 *		build here — fix the access knowingly, do not just rename the cast.
 *		A back-branch *addition* of a new Path * field on an already-walked
 *		struct compiles cleanly and silently narrows coverage; only an audit
 *		of the case against pathnodes.h will catch that.
 *
 *		Audited for completeness against PG 17.9 and PG 18.3.  When bumping
 *		the targeted PG version pair, re-audit by grepping pathnodes.h for
 *		"Path[[:space:]]*\*" and confirming each hit is reached either from
 *		a case below, from walk_rel(), or from walk_planner_info().
 */
static void
walk_path(HTAB *visited, Path *path, const char *source, List *container,
		  RelOptInfo *rel, PlannerInfo *root)
{
	NodeTag		tag;

	if (path == NULL)
		return;

	check_stack_depth();

	tag = nodeTag(path);
	if (!is_path_tag(tag))
	{
		ereport(ppc_elevel,
				errmsg(PPC_NAME ": invalid NodeTag %s in %s, rel %s",
					   tag_name((int) tag), source,
					   format_relnames(rel, root)),
				container != NULL
				? errdetail("%s contents: %s",
							source, format_pathlist(container))
				: 0,
				PPC_QUERY_HINT);
		return;
	}

	if (!mark_visited(visited, path))
		return;

	switch (tag)
	{
		case T_Path:
		case T_IndexPath:
		case T_TidPath:
		case T_TidRangePath:
		case T_GroupResultPath:
			/* No sub-Paths. */
			break;

		case T_BitmapHeapPath:
			walk_path(visited, ((BitmapHeapPath *) path)->bitmapqual,
					  "BitmapHeapPath.bitmapqual", NULL, rel, root);
			break;
		case T_BitmapAndPath:
			walk_pathlist(visited, ((BitmapAndPath *) path)->bitmapquals,
						  "BitmapAndPath.bitmapquals", rel, root);
			break;
		case T_BitmapOrPath:
			walk_pathlist(visited, ((BitmapOrPath *) path)->bitmapquals,
						  "BitmapOrPath.bitmapquals", rel, root);
			break;

		case T_SubqueryScanPath:
			walk_path(visited, ((SubqueryScanPath *) path)->subpath,
					  "SubqueryScanPath.subpath", NULL, rel, root);
			break;

		case T_ForeignPath:
			walk_path(visited, ((ForeignPath *) path)->fdw_outerpath,
					  "ForeignPath.fdw_outerpath", NULL, rel, root);
			break;

		case T_CustomPath:
			walk_pathlist(visited, ((CustomPath *) path)->custom_paths,
						  "CustomPath.custom_paths", rel, root);
			break;

		case T_AppendPath:
			walk_pathlist(visited, ((AppendPath *) path)->subpaths,
						  "AppendPath.subpaths", rel, root);
			break;
		case T_MergeAppendPath:
			walk_pathlist(visited, ((MergeAppendPath *) path)->subpaths,
						  "MergeAppendPath.subpaths", rel, root);
			break;

		case T_MaterialPath:
			walk_path(visited, ((MaterialPath *) path)->subpath,
					  "MaterialPath.subpath", NULL, rel, root);
			break;
		case T_MemoizePath:
			walk_path(visited, ((MemoizePath *) path)->subpath,
					  "MemoizePath.subpath", NULL, rel, root);
			break;
		case T_GatherPath:
			walk_path(visited, ((GatherPath *) path)->subpath,
					  "GatherPath.subpath", NULL, rel, root);
			break;
		case T_GatherMergePath:
			walk_path(visited, ((GatherMergePath *) path)->subpath,
					  "GatherMergePath.subpath", NULL, rel, root);
			break;

		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			walk_path(visited, ((JoinPath *) path)->outerjoinpath,
					  "JoinPath.outerjoinpath", NULL, rel, root);
			walk_path(visited, ((JoinPath *) path)->innerjoinpath,
					  "JoinPath.innerjoinpath", NULL, rel, root);
			break;

		case T_ProjectionPath:
			walk_path(visited, ((ProjectionPath *) path)->subpath,
					  "ProjectionPath.subpath", NULL, rel, root);
			break;
		case T_ProjectSetPath:
			walk_path(visited, ((ProjectSetPath *) path)->subpath,
					  "ProjectSetPath.subpath", NULL, rel, root);
			break;
		case T_SortPath:
			walk_path(visited, ((SortPath *) path)->subpath,
					  "SortPath.subpath", NULL, rel, root);
			break;
		case T_IncrementalSortPath:
			walk_path(visited, ((IncrementalSortPath *) path)->spath.subpath,
					  "IncrementalSortPath.subpath", NULL, rel, root);
			break;
		case T_GroupPath:
			walk_path(visited, ((GroupPath *) path)->subpath,
					  "GroupPath.subpath", NULL, rel, root);
			break;
		case T_UniquePath:
			walk_path(visited, ((UniquePath *) path)->subpath,
					  "UniquePath.subpath", NULL, rel, root);
			break;
		case T_UpperUniquePath:
			/* PG 17/18 DISTINCT-stage uniquification; UniquePath is the IN-form. */
			walk_path(visited, ((UpperUniquePath *) path)->subpath,
					  "UpperUniquePath.subpath", NULL, rel, root);
			break;
		case T_AggPath:
			walk_path(visited, ((AggPath *) path)->subpath,
					  "AggPath.subpath", NULL, rel, root);
			break;
		case T_GroupingSetsPath:
			walk_path(visited, ((GroupingSetsPath *) path)->subpath,
					  "GroupingSetsPath.subpath", NULL, rel, root);
			break;
		case T_MinMaxAggPath:
			{
				/*
				 * MinMaxAggPath has no top-level Path *, but each
				 * MinMaxAggInfo on mmaggregates carries an access path
				 * (the index-scan-with-LIMIT-1 sub-plan that materialises
				 * one MIN/MAX value) and the PlannerInfo subroot used to
				 * plan it.  Neither is reachable from glob->subroots
				 * (preprocess_minmax_aggregates does not register subroots
				 * there), so we walk them explicitly here.  walk_planner_info
				 * is idempotent via the visited HTAB, so we can call it
				 * defensively without worrying about double-traversal.
				 */
				MinMaxAggPath *mmap = (MinMaxAggPath *) path;
				ListCell   *lc;

				foreach(lc, mmap->mmaggregates)
				{
					MinMaxAggInfo *info = lfirst_node(MinMaxAggInfo, lc);

					walk_path(visited, info->path, "MinMaxAggInfo.path",
							  NULL, rel, root);
					if (info->subroot != NULL)
						walk_planner_info(visited, info->subroot);
				}
			}
			break;
		case T_WindowAggPath:
			walk_path(visited, ((WindowAggPath *) path)->subpath,
					  "WindowAggPath.subpath", NULL, rel, root);
			break;

		case T_SetOpPath:
#if PG_VERSION_NUM >= 180000
			walk_path(visited, ((SetOpPath *) path)->leftpath,
					  "SetOpPath.leftpath", NULL, rel, root);
			walk_path(visited, ((SetOpPath *) path)->rightpath,
					  "SetOpPath.rightpath", NULL, rel, root);
#else
			/* PG 17 had a single subpath; PG 18 split it into left/right. */
			walk_path(visited, ((SetOpPath *) path)->subpath,
					  "SetOpPath.subpath", NULL, rel, root);
#endif
			break;
		case T_RecursiveUnionPath:
			/* leftpath = non-recursive term, rightpath = recursive term. */
			walk_path(visited, ((RecursiveUnionPath *) path)->leftpath,
					  "RecursiveUnionPath.leftpath", NULL, rel, root);
			walk_path(visited, ((RecursiveUnionPath *) path)->rightpath,
					  "RecursiveUnionPath.rightpath", NULL, rel, root);
			break;

		case T_LockRowsPath:
			walk_path(visited, ((LockRowsPath *) path)->subpath,
					  "LockRowsPath.subpath", NULL, rel, root);
			break;
		case T_ModifyTablePath:
			walk_path(visited, ((ModifyTablePath *) path)->subpath,
					  "ModifyTablePath.subpath", NULL, rel, root);
			break;
		case T_LimitPath:
			walk_path(visited, ((LimitPath *) path)->subpath,
					  "LimitPath.subpath", NULL, rel, root);
			break;

		default:
			/*
			 * Reachable on PG 17 or 18 only if a back-branch added a Path
			 * subtype (so PATH_TAG_LIST grew) but this switch was not
			 * taught about it.  Loud Assert in cassert builds; in
			 * production we miss the new subtype's sub-paths.
			 */
			Assert(false);
			break;
	}
}


/*
 * Lookup table: NodeTag value → symbolic name.  Generated at build time
 * from src/backend/nodes/nodetags.h by a sed rule in the Makefile.
 * Entries for unused slots are NULL.  The array size auto-tracks the
 * highest designated-initializer index emitted by the generator, so new
 * upstream tags don't silently overflow.
 */
static const char * const nodetag_names[] = {
#include "nodetag_names.h"
};

/*
 * tag_name
 *		Return the symbolic name for a NodeTag value, or "UNDEF(nnn)" when
 *		the value falls outside the known range.  The UNDEF string is
 *		allocated in the current memory context on each call; the caller
 *		typically consumes it immediately inside an ereport.
 */
static const char *
tag_name(int tag)
{
	if (tag >= 0 && tag < (int) lengthof(nodetag_names) &&
		nodetag_names[tag] != NULL)
		return nodetag_names[tag];

	return psprintf("UNDEF(%d)", tag);
}


/*
 * format_relnames
 *		Build a human-readable string from the owning RelOptInfo's relids,
 *		resolving each member through root->simple_rte_array[i]->eref.
 *		Returns a palloc'd string like "{t1, t2}" or "(unknown)" when
 *		the rel or root is not available.
 *
 *		Defensive against the very thing this extension hunts for: we are
 *		typically called from inside an ereport() that fired *because*
 *		something was corrupt, so rel->relids may itself be a freed or
 *		garbage Bitmapset.  Validate the NodeTag before iterating.
 */
static const char *
format_relnames(RelOptInfo *rel, PlannerInfo *root)
{
	StringInfoData buf;
	int			x;
	bool		first = true;

	if (rel == NULL || rel->relids == NULL || root == NULL)
		return "(unknown)";
	if (!IsA(rel->relids, Bitmapset))
		return "(invalid relids)";

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');

	x = -1;
	while ((x = bms_next_member(rel->relids, x)) >= 0)
	{
		RangeTblEntry *rte;

		if (!first)
			appendStringInfoString(&buf, ", ");
		first = false;

		if (x < root->simple_rel_array_size &&
			root->simple_rte_array != NULL &&
			(rte = root->simple_rte_array[x]) != NULL &&
			rte->eref != NULL)
		{
			appendStringInfoString(&buf, rte->eref->aliasname);
		}
		else
		{
			appendStringInfo(&buf, "rel#%d", x);
		}
	}

	appendStringInfoChar(&buf, '}');
	return buf.data;
}


/*
 * format_pathlist
 *		Dump the contents of a List of Path pointers: address and NodeTag
 *		for each element.  Used in errdetail when corruption is detected.
 */
static const char *
format_pathlist(List *paths)
{
	StringInfoData buf;
	ListCell   *lc;
	int			i = 0;

	if (paths == NIL)
		return "(empty)";

	initStringInfo(&buf);

	foreach(lc, paths)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (i > 0)
			appendStringInfoString(&buf, "; ");

		if (p == NULL)
		{
			appendStringInfo(&buf, "[%d] NULL", i);
		}
		else
		{
			NodeTag		t = nodeTag(p);

			if (is_path_tag(t))
				appendStringInfo(&buf, "[%d] %s", i, tag_name((int) t));
			else
				appendStringInfo(&buf, "[%d] %s INVALID", i,
								 tag_name((int) t));
		}
		i++;
	}

	return buf.data;
}


/*
 * mark_visited
 *		Insert ptr into the visited set; true on first visit, false otherwise.
 */
static bool
mark_visited(HTAB *visited, void *ptr)
{
	bool	found;

	Assert(visited != NULL);
	Assert(ptr != NULL);

	(void) hash_search(visited, &ptr, HASH_ENTER, &found);
	return !found;
}


/*
 * is_path_tag
 *		True if tag names a Path descendant.  The case list is expanded from
 *		PATH_TAG_LIST, which gen_pathtags.pl derives from pathnodes.h at
 *		build time — so the whitelist cannot go stale.  Keeping it as a
 *		switch (rather than a range test) preserves the freed-memory-tag
 *		safety property: a random integer that happens to fall into the
 *		numeric range of Path tags but was never minted by core still
 *		falls through to the default arm.
 */
static bool
is_path_tag(NodeTag tag)
{
	switch (tag)
	{
#define PPC_CASE(t)		case t:
		PATH_TAG_LIST(PPC_CASE)
#undef PPC_CASE
			return true;
		default:
			return false;
	}
}
