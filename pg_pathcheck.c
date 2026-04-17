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
#include "optimizer/planner.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

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

/* Lazily resolved ID for our extension_state slot on PlannerGlobal. */
static int	ppc_ext_id = -1;

/*
 * GUC: pg_pathcheck.level
 *		Controls the elevel used when a corrupt Path is detected.
 *		WARNING (default) logs and continues; ERROR aborts the statement;
 *		PANIC crashes the backend so you get a core dump for post-mortem.
 */
static int	ppc_level = WARNING;

static const struct config_enum_entry ppc_level_options[] = {
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{"panic", PANIC, false},
	{NULL, 0, false}
};

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
	DefineCustomEnumVariable(PPC_NAME ".level",
							 "Sets the message level on corrupt Path detection.",
							 "WARNING logs and continues, ERROR aborts the "
							 "statement, PANIC crashes for a core dump.",
							 &ppc_level,
							 WARNING,
							 ppc_level_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved(PPC_NAME);

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

	/*
	 * Top root is not presented in the 'glob' structures. So, extension
	 * should save this pointer here for the furhter use.
	 */
	if (ppc_ext_id < 0)
		ppc_ext_id = GetPlannerExtensionId(PPC_NAME);
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

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(void *);
		ctl.entrysize = sizeof(void *);
		ctl.hcxt = CurrentMemoryContext;

		/*
		 * Do not care about previous value of the pointer. It might stay
		 * initialised in case of previous internal error. But memory already
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

	/* Purely rendundant. Just to be paranoid. */
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
	 * ->parent doesn't match.  As the memory reused it might happen we see a
	 * sort of garbage here.
	 */
	if (actual == NULL || !IsA(actual, RelOptInfo))
	{
		ereport(ppc_level,
				errmsg(PPC_NAME ": path has non-RelOptInfo parent in %s, target rel %s",
					   source, format_relnames(expected, root)),
				errhint("query: %s",
						debug_query_string ? debug_query_string : "(null)"));
		return;
	}

	/*
	 * Classic same-size-class alias: the slot was reused by another rel's
	 * path.  Name both rels by their contributing base relations.
	 */
	if (container != NULL)
		ereport(ppc_level,
				errmsg(PPC_NAME ": path parent mismatch in %s, target rel %s",
					   source, format_relnames(expected, root)),
				errdetail("path claims rel %s; %s contents: %s",
						  format_relnames(actual, root),
						  source, format_pathlist(container)),
				errhint("query: %s",
						debug_query_string ? debug_query_string : "(null)"));
	else
		ereport(ppc_level,
				errmsg(PPC_NAME ": path parent mismatch in %s, target rel %s",
					   source, format_relnames(expected, root)),
				errdetail("path claims rel %s",
						  format_relnames(actual, root)),
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
		if (container != NULL)
			ereport(ppc_level,
					errmsg(PPC_NAME ": invalid NodeTag %s in %s, rel %s",
						   tag_name((int) tag), source,
						   format_relnames(rel, root)),
					errdetail("%s contents: %s",
							  source, format_pathlist(container)),
					errhint("query: %s",
							debug_query_string ? debug_query_string : "(null)"));
		else
			ereport(ppc_level,
					errmsg(PPC_NAME ": invalid NodeTag %s in %s, rel %s",
						   tag_name((int) tag), source,
						   format_relnames(rel, root)),
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
		case T_MinMaxAggPath:
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
 * Entries for unused slots are NULL.
 */
#define PPC_MAX_TAG 502
static const char * const nodetag_names[PPC_MAX_TAG + 1] = {
#include "nodetag_names.h"
};

/*
 * tag_name
 *		Return the symbolic name for a NodeTag value, or "UNDEF(nnn)" when
 *		the value falls outside the known range.
 */
static const char *
tag_name(int tag)
{
	static char	buf[32];

	if (tag >= 0 && tag <= PPC_MAX_TAG && nodetag_names[tag] != NULL)
		return nodetag_names[tag];

	snprintf(buf, sizeof(buf), "UNDEF(%d)", tag);
	return buf;
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
