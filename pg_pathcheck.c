/*-------------------------------------------------------------------------
 *
 * pg_pathcheck.c
 *	  Walk every Path in a finished query tree and flag freed / garbage nodes.
 *
 *	  Debug aid only.  When loaded, planner_shutdown_hook traverses the top
 *	  PlannerInfo and every reachable subroot (RelOptInfo->subroot,
 *	  PlannerGlobal->subroots), visiting pathlist, partial_pathlist,
 *	  cheapest_*_path, non_recursive_path, and every sub-Path field of every
 *	  compound Path type.  Each visited Path is checked for a valid NodeTag;
 *	  a bogus tag (e.g. the 0x7F bytes from CLOBBER_FREED_MEMORY, or a node
 *	  allocated in a freed slot after pfree) is reported as a WARNING together
 *	  with the relation names resolved from the owning RelOptInfo's relids
 *	  and the full contents of the containing pathlist.
 *
 *	  planner_shutdown_hook is not handed the top PlannerInfo directly, so we
 *	  stash it from create_upper_paths_hook into PlannerGlobal->extension_state.
 *	  That slot lives in the planner's per-query context and is reclaimed on
 *	  both normal exit and elog(ERROR), so nothing to clean up ourselves.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "optimizer/extendplan.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "pathtags_generated.h"

#define PPC_NAME	"pg_pathcheck"
#define PPC_VERSION	"0.9"

PG_MODULE_MAGIC_EXT(
	.name = PPC_NAME,
	.version = PPC_VERSION
);

void		_PG_init(void);

/* Chained upstream hooks. */
static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static planner_shutdown_hook_type prev_planner_shutdown_hook = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/* Lazily resolved ID for our extension_state slot on PlannerGlobal. */
static int	ppc_ext_id = -1;

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
 *		The end-of-planning walker (planner_shutdown_hook) and the root
 *		stashing inside create_upper_paths_hook remain active regardless of
 *		this flag.
 */
static bool ppc_stage_checks = false;

/*
 * Visited-pointer hash, rebuilt per top-level walk. Deduplication tool.
 * Prevents exponential blow-up when the same sub-path is reachable from
 * multiple parents (AppendPath children, cheapest_*_path aliases, etc.).
 */
static HTAB *visited = NULL;

/* Forward declarations. */
static void ppc_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel, RelOptInfo *output_rel,
								   void *extra);
static void ppc_planner_shutdown(PlannerGlobal *glob, Query *parse,
								 const char *query_string, PlannedStmt *pstmt);
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
static void walk_planner_info(PlannerInfo *root);
static void walk_rel(RelOptInfo *rel, PlannerInfo *root);
static void walk_pathlist(List *paths, const char *listname,
						  RelOptInfo *rel, PlannerInfo *root);
static void walk_path(Path *path, const char *source, List *container,
					   RelOptInfo *rel, PlannerInfo *root);
static void verify_path_parent(Path *path, RelOptInfo *expected,
							   const char *source, List *container,
							   PlannerInfo *root);
static bool mark_visited(void *ptr);
static bool is_path_tag(NodeTag tag);
static const char *tag_name(int tag);
static const char *format_relnames(RelOptInfo *rel, PlannerInfo *root);
static const char *format_pathlist(List *paths);


/*
 * _PG_init
 *		Chain onto the two planner hooks we need.
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

	MarkGUCPrefixReserved(PPC_NAME);

	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = ppc_create_upper_paths;

	prev_planner_shutdown_hook = planner_shutdown_hook;
	planner_shutdown_hook = ppc_planner_shutdown;

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
					errhint("query: %s",
							debug_query_string ? debug_query_string : "(null)"));
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
					errhint("query: %s",
							debug_query_string ? debug_query_string : "(null)"));
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
 *		  1. Capture the top-level PlannerInfo at UPPERREL_FINAL, since the
 *		     planner-shutdown hook is not handed the top root directly.
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

	/* (1) stash top root so planner_shutdown_hook can find it */
	if (root->parent_root == NULL && stage == UPPERREL_FINAL)
	{
		if (ppc_ext_id < 0)
			ppc_ext_id = GetPlannerExtensionId(PPC_NAME);
		SetPlannerGlobalExtensionState(root->glob, ppc_ext_id, root);
	}

	/* (2) per-stage pathlist check on input and output rels (opt-in) */
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
 * ppc_planner_shutdown
 *		Walk all Paths reachable from the top root and from PlannerGlobal.
 */
static void
ppc_planner_shutdown(PlannerGlobal *glob, Query *parse,
					 const char *query_string, PlannedStmt *pstmt)
{
	PlannerInfo *top_root;

	if (ppc_ext_id >= 0 &&
		(top_root = GetPlannerGlobalExtensionState(glob, ppc_ext_id)) != NULL)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(void *);
		ctl.entrysize = sizeof(void *);
		ctl.hcxt = CurrentMemoryContext;

		/*
		 * Do not care about previous value of the pointer. It might stay
		 * initialized in case of previous internal error. But memory already
		 * freed because of transactional memory context.
		 */
		visited = hash_create(PPC_NAME " visited", 1024, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		walk_planner_info(top_root);

		/* Subplan PlannerInfos and their backing top-level Paths. */
		foreach_node(PlannerInfo, root, glob->subroots)
			walk_planner_info(root);

		/*
		 * Deliberately duplicated crawler - just to find potential
		 * low-probability discrepancies or dangled pointers in this list itself
		 */
		walk_pathlist(glob->subpaths, "glob->subpaths", NULL, top_root);

		hash_destroy(visited);
		visited = NULL;
	}

	if (prev_planner_shutdown_hook)
		(*prev_planner_shutdown_hook) (glob, parse, query_string, pstmt);
}


/*
 * walk_planner_info
 *		Visit every RelOptInfo on this root and recurse into its subroots.
 */
static void
walk_planner_info(PlannerInfo *root)
{
	int			i;

	if (root == NULL || !mark_visited(root))
		return;

	check_stack_depth();

	/* Upper rels: one List per UpperRelationKind. */
	for (i = 0; i <= UPPERREL_FINAL; i++)
	{
		foreach_node(RelOptInfo, rel, root->upper_rels[i])
			walk_rel(rel, root);
	}

	/* Base rels and appendrel children. */
	Assert(root->simple_rel_array_size == 0 ||
		   root->simple_rel_array != NULL);
	for (i = 1; i < root->simple_rel_array_size; i++)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel == NULL)
			continue;
		walk_rel(rel, root);
		/* Recurse into subquery PlannerInfos. */
		if (rel->subroot != NULL)
			walk_planner_info(rel->subroot);
	}

	/* Join rels collected during dynamic programming. */
	foreach_node(RelOptInfo, rel, root->join_rel_list)
		walk_rel(rel, root);

	/* Non-recursive term of a recursive CTE, if any. */
	walk_path(root->non_recursive_path, "non_recursive_path", NULL,
			  NULL, root);
}


/*
 * walk_rel
 *		Visit every Path slot on a RelOptInfo, and recurse into parallel
 *		RelOptInfos that hang off it (unique_rel, grouped_rel, part_rels[]).
 *		Upward links (parent / top_parent) are intentionally not followed:
 *		those rels are reached via simple_rel_array or join_rel_list anyway.
 */
static void
walk_rel(RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	if (rel == NULL || !mark_visited(rel))
		return;

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
	}

	walk_pathlist(rel->pathlist, "pathlist", rel, root);
	walk_pathlist(rel->partial_pathlist, "partial_pathlist", rel, root);
	walk_pathlist(rel->cheapest_parameterized_paths,
				  "cheapest_parameterized_paths", rel, root);
	walk_path(rel->cheapest_startup_path, "cheapest_startup_path", NULL,
			  rel, root);
	walk_path(rel->cheapest_total_path, "cheapest_total_path", NULL,
			  rel, root);

	/*
	 * Recurse into special RelOptInfos in case their paths are washed out of
	 * the main pathlist.
	 */
	if (rel->unique_rel != NULL)
		walk_rel(rel->unique_rel, root);
	if (rel->grouped_rel != NULL)
		walk_rel(rel->grouped_rel, root);

	/* Purely redundant. Just to be paranoid. */
	if (rel->part_rels != NULL)
	{
		int	i;

		for (i = 0; i < rel->nparts; i++)
			if (rel->part_rels[i] != NULL)
				walk_rel(rel->part_rels[i], root);
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
	 * ->parent doesn't match.  As the memory is reused it might happen we see
	 * a sort of garbage here, so validate the pointer before dereferencing it
	 * via IS_UPPER_REL (which reads ->reloptkind).
	 */
	if (actual == NULL || !IsA(actual, RelOptInfo))
	{
		ereport(ppc_elevel,
				errmsg(PPC_NAME ": path has non-RelOptInfo parent in %s, target rel %s",
					   source, format_relnames(expected, root)),
				errhint("query: %s",
						debug_query_string ? debug_query_string : "(null)"));
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
			errhint("query: %s",
					debug_query_string ? debug_query_string : "(null)"));
}


/*
 * walk_pathlist
 *		Visit each Path in a List, tagging every element with the list's name.
 */
static void
walk_pathlist(List *paths, const char *listname,
			  RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, paths)
		walk_path((Path *) lfirst(lc), listname, paths, rel, root);
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
 *		Layout safety net: every Path subtype dereferenced below is guarded
 *		by a structural-hash entry in PPC_WALK_PATH_EXPECTED_HASHES further
 *		down in this file.  If you are here because a field name no longer
 *		compiles, *do not* paper over it by renaming the access — read the
 *		mirror block's header comment first: the hash-mismatch diagnostic
 *		will tell you which struct moved and why you are being forced to
 *		look.
 */
static void
walk_path(Path *path, const char *source, List *container,
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
				errhint("query: %s",
						debug_query_string ? debug_query_string : "(null)"));
		return;
	}

	if (!mark_visited(path))
		return;

	/*
	 * Dive into path tree. It is necessary (most of the time redundant) step
	 * that we need to pass because single operation, represented by specific
	 * RelOptInfo might be implemented by a complex path tree.
	 */
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
			walk_path(((BitmapHeapPath *) path)->bitmapqual,
					  "BitmapHeapPath.bitmapqual", NULL, rel, root);
			break;
		case T_BitmapAndPath:
			walk_pathlist(((BitmapAndPath *) path)->bitmapquals,
						  "BitmapAndPath.bitmapquals", rel, root);
			break;
		case T_BitmapOrPath:
			walk_pathlist(((BitmapOrPath *) path)->bitmapquals,
						  "BitmapOrPath.bitmapquals", rel, root);
			break;

		case T_SubqueryScanPath:
			walk_path(((SubqueryScanPath *) path)->subpath,
					  "SubqueryScanPath.subpath", NULL, rel, root);
			break;

		case T_ForeignPath:
			walk_path(((ForeignPath *) path)->fdw_outerpath,
					  "ForeignPath.fdw_outerpath", NULL, rel, root);
			break;

		case T_CustomPath:
			walk_pathlist(((CustomPath *) path)->custom_paths,
						  "CustomPath.custom_paths", rel, root);
			break;

		case T_AppendPath:
			walk_pathlist(((AppendPath *) path)->subpaths,
						  "AppendPath.subpaths", rel, root);
			break;
		case T_MergeAppendPath:
			walk_pathlist(((MergeAppendPath *) path)->subpaths,
						  "MergeAppendPath.subpaths", rel, root);
			break;

		case T_MaterialPath:
			walk_path(((MaterialPath *) path)->subpath,
					  "MaterialPath.subpath", NULL, rel, root);
			break;
		case T_MemoizePath:
			walk_path(((MemoizePath *) path)->subpath,
					  "MemoizePath.subpath", NULL, rel, root);
			break;
		case T_GatherPath:
			walk_path(((GatherPath *) path)->subpath,
					  "GatherPath.subpath", NULL, rel, root);
			break;
		case T_GatherMergePath:
			walk_path(((GatherMergePath *) path)->subpath,
					  "GatherMergePath.subpath", NULL, rel, root);
			break;

		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			walk_path(((JoinPath *) path)->outerjoinpath,
					  "JoinPath.outerjoinpath", NULL, rel, root);
			walk_path(((JoinPath *) path)->innerjoinpath,
					  "JoinPath.innerjoinpath", NULL, rel, root);
			break;

		case T_ProjectionPath:
			walk_path(((ProjectionPath *) path)->subpath,
					  "ProjectionPath.subpath", NULL, rel, root);
			break;
		case T_ProjectSetPath:
			walk_path(((ProjectSetPath *) path)->subpath,
					  "ProjectSetPath.subpath", NULL, rel, root);
			break;
		case T_SortPath:
			walk_path(((SortPath *) path)->subpath,
					  "SortPath.subpath", NULL, rel, root);
			break;
		case T_IncrementalSortPath:
			walk_path(((IncrementalSortPath *) path)->spath.subpath,
					  "IncrementalSortPath.subpath", NULL, rel, root);
			break;
		case T_GroupPath:
			walk_path(((GroupPath *) path)->subpath,
					  "GroupPath.subpath", NULL, rel, root);
			break;
		case T_UniquePath:
			walk_path(((UniquePath *) path)->subpath,
					  "UniquePath.subpath", NULL, rel, root);
			break;
		case T_AggPath:
			walk_path(((AggPath *) path)->subpath,
					  "AggPath.subpath", NULL, rel, root);
			break;
		case T_GroupingSetsPath:
			walk_path(((GroupingSetsPath *) path)->subpath,
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

					walk_path(info->path, "MinMaxAggInfo.path",
							  NULL, rel, root);
					if (info->subroot != NULL)
						walk_planner_info(info->subroot);
				}
			}
			break;
		case T_WindowAggPath:
			walk_path(((WindowAggPath *) path)->subpath,
					  "WindowAggPath.subpath", NULL, rel, root);
			break;

		case T_SetOpPath:
			walk_path(((SetOpPath *) path)->leftpath,
					  "SetOpPath.leftpath", NULL, rel, root);
			walk_path(((SetOpPath *) path)->rightpath,
					  "SetOpPath.rightpath", NULL, rel, root);
			break;
		case T_RecursiveUnionPath:
			walk_path(((RecursiveUnionPath *) path)->leftpath,
					  "RecursiveUnionPath.leftpath", NULL, rel, root);
			walk_path(((RecursiveUnionPath *) path)->rightpath,
					  "RecursiveUnionPath.rightpath", NULL, rel, root);
			break;

		case T_LockRowsPath:
			walk_path(((LockRowsPath *) path)->subpath,
					  "LockRowsPath.subpath", NULL, rel, root);
			break;
		case T_ModifyTablePath:
			walk_path(((ModifyTablePath *) path)->subpath,
					  "ModifyTablePath.subpath", NULL, rel, root);
			break;
		case T_LimitPath:
			walk_path(((LimitPath *) path)->subpath,
					  "LimitPath.subpath", NULL, rel, root);
			break;

		default:
			/* is_path_tag accepted it, so this can't happen. */
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
mark_visited(void *ptr)
{
	bool	found;

	Assert(visited != NULL);
	Assert(ptr != NULL);

	(void) hash_search(visited, &ptr, HASH_ENTER, &found);
	return !found;
}


/*
 * PPC_WALK_PATH_EXPECTED_HASHES
 *		Single hand-maintained source of truth for the set of Path subtypes
 *		walk_path() knows how to descend into, together with each subtype's
 *		expected structural hash.  Drives two compile-time checks:
 *
 *		  - The count of entries here must equal the count in PATH_TAG_LIST
 *			(generated from pathnodes.h).  Catches any addition or removal
 *			of a Path subtype in core: the build fails with a message
 *			pointing here.
 *
 *		  - Each entry's expected hash must equal PPC_PATH_HASH_T_<Subtype>
 *			from pathtags_generated.h.  Catches any layout edit inside an
 *			existing Path struct (field add/remove/rename/retype/reorder),
 *			naming the specific subtype that drifted.
 *
 *		When the build breaks:
 *
 *		  1. If the count mismatches: a subtype was added or removed in
 *			 pathnodes.h.  Teach walk_path() how to descend into the new
 *			 subtype (or let it fall through if it has no sub-Paths), then
 *			 add or remove the corresponding entry below.
 *
 *		  2. If a per-tag hash mismatches: the named subtype's body changed.
 *			 Diff pathnodes.h against the version the hash was blessed
 *			 against, audit walk_path()'s case for that subtype, and run
 *			 `make bless-path-hashes` to refresh this list.  (The make
 *			 target is a convenience; the audit of walk_path() is not
 *			 automated and is mandatory.)
 *
 *		All concrete subtypes are listed — even the ones walk_path() treats
 *		as leaves (T_Path, T_IndexPath, T_TidPath, T_TidRangePath,
 *		T_GroupResultPath) — because "no sub-Paths to descend" is itself a
 *		layout claim that can rot if core grows a new Path * field in one
 *		of them.
 *
 *		Limitation: each hash covers only its subtype's own body, not the
 *		bodies of embedded parent structs.  walk_path() reaches into
 *		embedded parents at exactly one point -- IncrementalSortPath's
 *		spath.subpath -- and that access is protected by the SortPath
 *		entry below.  If future walker code dereferences a non-Path struct
 *		embedded in a Path (e.g. ((Foo *) path)->non_path.field), layout
 *		changes in that non-Path struct will not trip this guard and must
 *		be handled separately.
 *
 *		See contrib/pg_pathcheck/README.md section "Bumping PostgreSQL"
 *		for the end-to-end workflow.
 */
#define PPC_WALK_PATH_EXPECTED_HASHES(X) \
	X(T_Path,                0x09ee69a9e5a8f23bULL) \
	X(T_IndexPath,           0xd9eab6b3c997d515ULL) \
	X(T_BitmapHeapPath,      0x75a7b181f72c352dULL) \
	X(T_BitmapAndPath,       0xccc395f9829cc5eeULL) \
	X(T_BitmapOrPath,        0xccc395f9829cc5eeULL) \
	X(T_TidPath,             0x689cb40f04282c9cULL) \
	X(T_TidRangePath,        0x49cc3bbec71064f4ULL) \
	X(T_SubqueryScanPath,    0x90f989f41b57b041ULL) \
	X(T_ForeignPath,         0xf6d2f824716c5cd8ULL) \
	X(T_CustomPath,          0x349311c5592f0b5bULL) \
	X(T_AppendPath,          0xb56a41497d3eb372ULL) \
	X(T_MergeAppendPath,     0xfce0f29acc68fbbcULL) \
	X(T_GroupResultPath,     0x84701fcd7617cc00ULL) \
	X(T_MaterialPath,        0x90f989f41b57b041ULL) \
	X(T_MemoizePath,         0x3dd2cdd46b1c4006ULL) \
	X(T_GatherPath,          0x50237ef02554d33dULL) \
	X(T_GatherMergePath,     0x5468656d4d359ad1ULL) \
	X(T_NestPath,            0xc9d7126d080e587eULL) \
	X(T_MergePath,           0x18d4e85c337a4499ULL) \
	X(T_HashPath,            0x7fabf0c425c0856cULL) \
	X(T_ProjectionPath,      0xe9172dd4d292d671ULL) \
	X(T_ProjectSetPath,      0x90f989f41b57b041ULL) \
	X(T_SortPath,            0x90f989f41b57b041ULL) \
	X(T_IncrementalSortPath, 0x4c69aafa5f126723ULL) \
	X(T_GroupPath,           0xd3ffe008ac1ac2fcULL) \
	X(T_UniquePath,          0x5654a3410d707ef9ULL) \
	X(T_AggPath,             0x179cb625db854a97ULL) \
	X(T_GroupingSetsPath,    0x62c65256f41bdcb6ULL) \
	X(T_MinMaxAggPath,       0xda76e318ee433a2fULL) \
	X(T_WindowAggPath,       0xccabff582235b106ULL) \
	X(T_SetOpPath,           0xd849dc901753a051ULL) \
	X(T_RecursiveUnionPath,  0x46f3b9fc9f2321f6ULL) \
	X(T_LockRowsPath,        0x094815119f154b2dULL) \
	X(T_ModifyTablePath,     0x1ea4932e8ac9b889ULL) \
	X(T_LimitPath,           0x4fd0995222ca414eULL)

/*
 * Count-parity check: the number of subtypes we expect walk_path() to
 * handle must equal the number actually present in PATH_TAG_LIST.  This
 * is what catches additions and removals in core; the per-tag hash
 * asserts below catch layout changes on existing subtypes.
 */
#define PPC_COUNT_ONE_ARG(t)			+ 1
#define PPC_COUNT_TWO_ARG(t, expected)	+ 1

StaticAssertDecl((0 PATH_TAG_LIST(PPC_COUNT_ONE_ARG)) ==
				 (0 PPC_WALK_PATH_EXPECTED_HASHES(PPC_COUNT_TWO_ARG)),
				 "pg_pathcheck: number of Path subtypes in pathnodes.h "
				 "no longer matches the set handled by walk_path(); add "
				 "or remove entries in PPC_WALK_PATH_EXPECTED_HASHES "
				 "and teach walk_path() about the change.");

#undef PPC_COUNT_ONE_ARG
#undef PPC_COUNT_TWO_ARG

/*
 * Per-subtype hash check: expand each entry into a StaticAssertDecl that
 * compares the blessed hash against the one gen_pathtags.pl just computed.
 * Token-paste PPC_PATH_HASH_ onto the tag to reference the generated
 * constant; stringify the tag so the diagnostic names the exact struct.
 */
#define PPC_HASH_ASSERT(tag, expected)										\
	StaticAssertDecl((expected) == PPC_PATH_HASH_##tag,						\
					 "pg_pathcheck: struct layout for " #tag " changed in "	\
					 "pathnodes.h; audit walk_path() and run "				\
					 "`make bless-path-hashes` to refresh "					\
					 "PPC_WALK_PATH_EXPECTED_HASHES.");

PPC_WALK_PATH_EXPECTED_HASHES(PPC_HASH_ASSERT)

#undef PPC_HASH_ASSERT

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
