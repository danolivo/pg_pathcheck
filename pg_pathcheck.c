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
 *	  allocated in a freed slot after pfree) is reported as a WARNING.
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
#include "optimizer/planner.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_pathcheck",
	.version = "0.9"
);

void		_PG_init(void);

/* Chained upstream hooks. */
static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static planner_shutdown_hook_type prev_planner_shutdown_hook = NULL;

/* Lazily resolved ID for our extension_state slot on PlannerGlobal. */
static int	ppc_ext_id = -1;

/*
 * Visited-pointer hash, rebuilt per top-level walk.  Prevents exponential
 * blow-up when the same sub-path is reachable from multiple parents
 * (AppendPath children, cheapest_*_path aliases, etc.).
 */
static HTAB *visited = NULL;

/* Forward declarations. */
static void ppc_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								   RelOptInfo *input_rel, RelOptInfo *output_rel,
								   void *extra);
static void ppc_planner_shutdown(PlannerGlobal *glob, Query *parse,
								 const char *query_string, PlannedStmt *pstmt);
static void walk_planner_info(PlannerInfo *root);
static void walk_rel(RelOptInfo *rel);
static void walk_pathlist(List *paths);
static void walk_path(Path *path);
static bool mark_visited(void *ptr);
static bool is_path_tag(NodeTag tag);


/*
 * _PG_init
 *		Chain onto the two planner hooks we need.
 */
void
_PG_init(void)
{
	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = ppc_create_upper_paths;

	prev_planner_shutdown_hook = planner_shutdown_hook;
	planner_shutdown_hook = ppc_planner_shutdown;
}


/*
 * ppc_create_upper_paths
 *		Capture the top-level PlannerInfo on its way through UPPERREL_FINAL.
 *		Subqueries (parent_root != NULL) are reached later via recursion.
 */
static void
ppc_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	if (prev_create_upper_paths_hook)
		(*prev_create_upper_paths_hook) (root, stage, input_rel, output_rel,
										 extra);

	if (root->parent_root != NULL || stage != UPPERREL_FINAL)
		return;

	if (ppc_ext_id < 0)
		ppc_ext_id = GetPlannerExtensionId("pg_pathcheck");

	/*
	 * SetPlannerGlobalExtensionState palloc's the slot array in planner_cxt,
	 * so it is freed automatically whether the planner exits cleanly or via
	 * elog(ERROR).  No leak, no dangling pointer across invocations.
	 */
	SetPlannerGlobalExtensionState(root->glob, ppc_ext_id, root);
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
		HASHCTL		ctl;
		ListCell   *lc;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(void *);
		ctl.entrysize = sizeof(void *);
		ctl.hcxt = CurrentMemoryContext;
		visited = hash_create("pg_pathcheck visited", 1024, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		walk_planner_info(top_root);

		/* Subplan PlannerInfos and their backing top-level Paths. */
		foreach(lc, glob->subroots)
			walk_planner_info(lfirst_node(PlannerInfo, lc));

		walk_pathlist(glob->subpaths);

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
	ListCell   *lc;
	int			i;

	if (root == NULL || !mark_visited(root))
		return;

	check_stack_depth();

	/* Upper rels: one List per UpperRelationKind. */
	for (i = 0; i <= UPPERREL_FINAL; i++)
	{
		foreach(lc, root->upper_rels[i])
			walk_rel((RelOptInfo *) lfirst(lc));
	}

	/* Base rels and appendrel children. */
	Assert(root->simple_rel_array_size == 0 ||
		   root->simple_rel_array != NULL);
	for (i = 1; i < root->simple_rel_array_size; i++)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel == NULL)
			continue;
		walk_rel(rel);
		/* Recurse into subquery PlannerInfos. */
		if (rel->subroot != NULL)
			walk_planner_info(rel->subroot);
	}

	/* Join rels collected during dynamic programming. */
	foreach(lc, root->join_rel_list)
		walk_rel((RelOptInfo *) lfirst(lc));

	/* Non-recursive term of a recursive CTE, if any. */
	walk_path(root->non_recursive_path);
}


/*
 * walk_rel
 *		Visit every Path slot on a RelOptInfo.
 */
static void
walk_rel(RelOptInfo *rel)
{
	if (rel == NULL || !mark_visited(rel))
		return;

	walk_pathlist(rel->pathlist);
	walk_pathlist(rel->partial_pathlist);
	walk_pathlist(rel->cheapest_parameterized_paths);
	walk_path(rel->cheapest_startup_path);
	walk_path(rel->cheapest_total_path);
}


/*
 * walk_pathlist
 *		Visit each Path in a List.
 */
static void
walk_pathlist(List *paths)
{
	ListCell   *lc;

	foreach(lc, paths)
		walk_path((Path *) lfirst(lc));
}


/*
 * walk_path
 *		Validate a Path's NodeTag and descend into every embedded sub-Path.
 *
 *		The NodeTag check is the main "is this memory still a Path" probe.
 *		With CLOBBER_FREED_MEMORY the tag becomes 0x7F7F7F7F which fails
 *		is_path_tag; with a real freed-and-reused chunk the odds of landing
 *		on a valid Path tag are vanishingly low.
 */
static void
walk_path(Path *path)
{
	NodeTag		tag;

	if (path == NULL)
		return;

	check_stack_depth();

	tag = nodeTag(path);
	if (!is_path_tag(tag))
	{
		elog(WARNING,
			 "pg_pathcheck: Path %p has invalid NodeTag %d (freed or corrupt?)",
			 path, (int) tag);
		return;
	}

	if (!mark_visited(path))
		return;

	switch (tag)
	{
		case T_Path:
		case T_IndexPath:
		case T_TidPath:
		case T_TidRangePath:
		case T_GroupResultPath:
		case T_MinMaxAggPath:
			/* No sub-Paths. */
			break;

		case T_BitmapHeapPath:
			walk_path(((BitmapHeapPath *) path)->bitmapqual);
			break;
		case T_BitmapAndPath:
			walk_pathlist(((BitmapAndPath *) path)->bitmapquals);
			break;
		case T_BitmapOrPath:
			walk_pathlist(((BitmapOrPath *) path)->bitmapquals);
			break;

		case T_SubqueryScanPath:
			walk_path(((SubqueryScanPath *) path)->subpath);
			break;

		case T_ForeignPath:
			walk_path(((ForeignPath *) path)->fdw_outerpath);
			break;

		case T_CustomPath:
			walk_pathlist(((CustomPath *) path)->custom_paths);
			break;

		case T_AppendPath:
			walk_pathlist(((AppendPath *) path)->subpaths);
			break;
		case T_MergeAppendPath:
			walk_pathlist(((MergeAppendPath *) path)->subpaths);
			break;

		case T_MaterialPath:
			walk_path(((MaterialPath *) path)->subpath);
			break;
		case T_MemoizePath:
			walk_path(((MemoizePath *) path)->subpath);
			break;
		case T_GatherPath:
			walk_path(((GatherPath *) path)->subpath);
			break;
		case T_GatherMergePath:
			walk_path(((GatherMergePath *) path)->subpath);
			break;

		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			walk_path(((JoinPath *) path)->outerjoinpath);
			walk_path(((JoinPath *) path)->innerjoinpath);
			break;

		case T_ProjectionPath:
			walk_path(((ProjectionPath *) path)->subpath);
			break;
		case T_ProjectSetPath:
			walk_path(((ProjectSetPath *) path)->subpath);
			break;
		case T_SortPath:
			walk_path(((SortPath *) path)->subpath);
			break;
		case T_IncrementalSortPath:
			walk_path(((IncrementalSortPath *) path)->spath.subpath);
			break;
		case T_GroupPath:
			walk_path(((GroupPath *) path)->subpath);
			break;
		case T_UniquePath:
			walk_path(((UniquePath *) path)->subpath);
			break;
		case T_AggPath:
			walk_path(((AggPath *) path)->subpath);
			break;
		case T_GroupingSetsPath:
			walk_path(((GroupingSetsPath *) path)->subpath);
			break;
		case T_WindowAggPath:
			walk_path(((WindowAggPath *) path)->subpath);
			break;

		case T_SetOpPath:
			walk_path(((SetOpPath *) path)->leftpath);
			walk_path(((SetOpPath *) path)->rightpath);
			break;
		case T_RecursiveUnionPath:
			walk_path(((RecursiveUnionPath *) path)->leftpath);
			walk_path(((RecursiveUnionPath *) path)->rightpath);
			break;

		case T_LockRowsPath:
			walk_path(((LockRowsPath *) path)->subpath);
			break;
		case T_ModifyTablePath:
			walk_path(((ModifyTablePath *) path)->subpath);
			break;
		case T_LimitPath:
			walk_path(((LimitPath *) path)->subpath);
			break;

		default:
			/* is_path_tag accepted it, so this can't happen. */
			Assert(false);
			break;
	}
}


/*
 * mark_visited
 *		Insert ptr into the visited set; true on first visit, false otherwise.
 */
static bool
mark_visited(void *ptr)
{
	bool		found;

	Assert(visited != NULL);
	Assert(ptr != NULL);

	(void) hash_search(visited, &ptr, HASH_ENTER, &found);
	return !found;
}


/*
 * is_path_tag
 *		True if tag names a Path descendant.  Keeping this as an explicit
 *		whitelist (rather than a range test) makes freed-memory tags fall
 *		through, and forces a conscious update whenever a new Path type is
 *		added upstream.
 */
static bool
is_path_tag(NodeTag tag)
{
	switch (tag)
	{
		case T_Path:
		case T_IndexPath:
		case T_BitmapHeapPath:
		case T_BitmapAndPath:
		case T_BitmapOrPath:
		case T_TidPath:
		case T_TidRangePath:
		case T_SubqueryScanPath:
		case T_ForeignPath:
		case T_CustomPath:
		case T_AppendPath:
		case T_MergeAppendPath:
		case T_GroupResultPath:
		case T_MaterialPath:
		case T_MemoizePath:
		case T_GatherPath:
		case T_GatherMergePath:
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
		case T_ProjectionPath:
		case T_ProjectSetPath:
		case T_SortPath:
		case T_IncrementalSortPath:
		case T_GroupPath:
		case T_UniquePath:
		case T_AggPath:
		case T_GroupingSetsPath:
		case T_MinMaxAggPath:
		case T_WindowAggPath:
		case T_SetOpPath:
		case T_RecursiveUnionPath:
		case T_LockRowsPath:
		case T_ModifyTablePath:
		case T_LimitPath:
			return true;
		default:
			return false;
	}
}
