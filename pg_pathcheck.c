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
 *	  allocated in a freed slot after pfree) is reported at
 *	  pg_pathcheck.elevel together with the relation names resolved from the
 *	  owning RelOptInfo's relids and the full contents of the containing
 *	  pathlist.
 *
 *	  planner_shutdown_hook is not handed the top PlannerInfo directly, so we
 *	  stash it from create_upper_paths_hook into PlannerGlobal->extension_state.
 *	  That slot lives in the planner's per-query context and is reclaimed on
 *	  both normal exit and elog(ERROR), so nothing to clean up ourselves.
 *
 *	  Everything here runs on memory we already suspect of being corrupt, so
 *	  the house rule is: validate before dereferencing, and never abort.  A
 *	  diagnostic tool that Assert()s its way out of an unexpected state takes
 *	  down the very run it was supposed to explain -- and the cassert build
 *	  this module is meant to be used with is where that hurts most.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "optimizer/extendplan.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

#include "pathtags_generated.h"

#define PPC_NAME	"pg_pathcheck"
#define PPC_VERSION	"0.10.0"

/*
 * Caps on the diagnostic strings we build.  Both exist because a finding
 * fires once per offending list element: an uncapped dump of a wholly
 * clobbered pathlist is quadratic in the list length, and debug_query_string
 * can be megabytes of generated SQL.
 */
#define PPC_MAX_LIST_DUMP	20
#define PPC_MAX_QUERY_LEN	1024

/* Passed as the "idx" argument for a Path held in a scalar slot. */
#define PPC_NO_INDEX		(-1)

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

/* ID of our extension_state slot on PlannerGlobal; assigned in _PG_init. */
static int	ppc_ext_id = -1;

/*
 * GUC: pg_pathcheck.elevel
 *		The elevel passed to ereport() when a corrupt Path is detected.
 *		LOG writes to the server log only; WARNING (default) also notifies
 *		the client and continues; ERROR aborts the statement; PANIC crashes
 *		the backend so you get a core dump for post-mortem.
 *
 *		PGC_SUSET, not PGC_USERSET: PANIC takes down the whole cluster, not
 *		just the session that asked for it, and findings are easy to provoke
 *		from ordinary SQL.  Letting an unprivileged user arm that would hand
 *		them a restart button.
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

/*
 * PpcContext
 *		Which call site asked for this check, formatted only if something is
 *		actually reported.
 *
 *		The stage hooks fire on every base rel, every join pair and every
 *		upper-rel stage of every query.  Building "outer side of join rel
 *		{a, b}" eagerly would psprintf() and initStringInfo() -- a kilobyte
 *		a time, into the planner's per-query context -- on every firing,
 *		whether or not a finding ever materialises.  So keep the pieces and
 *		assemble them in ppc_context_str(), which only the reporting paths
 *		call.
 *
 *		A NULL PpcContext * means the end-of-planning walker.
 */
typedef struct PpcContext
{
	const char *kind;			/* constant description; never NULL */
	const char *detail;			/* constant suffix, or NULL */
	RelOptInfo *rel;			/* rel whose name follows kind, or NULL */
	PlannerInfo *root;			/* root used to resolve rel's relids */
} PpcContext;

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
static NodeTag ppc_check_tag(Path *path, const char *listname, int idx,
							 List *container, RelOptInfo *rel,
							 PlannerInfo *root, const PpcContext *ctx);
static void ppc_check_parent(Path *path, NodeTag tag, RelOptInfo *owner,
							 const char *listname, int idx, List *container,
							 PlannerInfo *root, const PpcContext *ctx);
static void ppc_check_pathlist(List *paths, const char *listname,
							   RelOptInfo *rel, PlannerInfo *root,
							   const PpcContext *ctx);
static void ppc_check_rel_pathlists(RelOptInfo *rel, PlannerInfo *root,
									const PpcContext *ctx);
static const char *ppc_context_str(const PpcContext *ctx);
static const char *ppc_slot_str(const char *listname, int idx);
static const char *ppc_query_text(void);
static const char *upper_stage_name(UpperRelationKind stage);
static void walk_planner_info(PlannerInfo *root);
static void walk_rel(RelOptInfo *rel, PlannerInfo *root);
static void walk_pathlist(List *paths, const char *listname, RelOptInfo *owner,
						  RelOptInfo *rel, PlannerInfo *root);
static void walk_path(Path *path, const char *listname, int idx,
					  List *container, RelOptInfo *owner, RelOptInfo *rel,
					  PlannerInfo *root);
static void walk_subpath(Path *path, const char *source, RelOptInfo *rel,
						 PlannerInfo *root);
static bool mark_visited(void *ptr);
static bool is_path_tag(NodeTag tag);
static const char *tag_name(int tag);
static const char *format_relnames(RelOptInfo *rel, PlannerInfo *root);
static const char *format_pathlist(List *paths);


/*
 * _PG_init
 *		Chain onto the planner hooks we need and claim our extension_state slot.
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
							 PGC_SUSET,
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

	/*
	 * Claim the slot now rather than lazily on first use.  Registering an
	 * extension ID in the middle of planning would mean the current
	 * PlannerGlobal's extension_state array was sized before our ID existed.
	 */
	ppc_ext_id = GetPlannerExtensionId(PPC_NAME);

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

	/*
	 * Contexts are built inside the guard, not at the top of the function:
	 * this hook fires once per candidate join pair, which is the DP loop, and
	 * in the default configuration it must do nothing at all.
	 */
	{
		PpcContext	outer_ctx = {.kind = "outer side of join rel",
			.rel = joinrel,.root = root};
		PpcContext	inner_ctx = {.kind = "inner side of join rel",
			.rel = joinrel,.root = root};
		PpcContext	join_ctx = {.kind = "join rel",
			.rel = joinrel,.root = root};

		ppc_check_rel_pathlists(outerrel, root, &outer_ctx);
		ppc_check_rel_pathlists(innerrel, root, &inner_ctx);

		/*
		 * The join rel's own paths are the freshest thing in sight, so check
		 * them too rather than only the inputs they were built from.
		 */
		ppc_check_rel_pathlists(joinrel, root, &join_ctx);
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
	/* Constant: no per-firing work, and nothing to build past the guard. */
	static const PpcContext ctx = {.kind = "base rel"};

	if (prev_set_rel_pathlist_hook)
		(*prev_set_rel_pathlist_hook) (root, rel, rti, rte);

	if (!ppc_stage_checks)
		return;

	Assert(rel != NULL);

	ppc_check_rel_pathlists(rel, root, &ctx);
}


/*
 * upper_stage_name
 *		Symbolic name for an UpperRelationKind value, for diagnostics.
 *
 *		Deliberately no default arm: -Wswitch then flags a new upstream
 *		UpperRelationKind here, which is also a cue to re-check the
 *		upper_rels[] loop in walk_planner_info().
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
		Assert(ppc_ext_id >= 0);
		SetPlannerGlobalExtensionState(root->glob, ppc_ext_id, root);
	}

	/* (2) per-stage pathlist check on input and output rels (opt-in) */
	if (ppc_stage_checks)
	{
		const char *sname = upper_stage_name(stage);
		PpcContext	in_ctx = {.kind = "create_upper_paths input, stage",
			.detail = sname};
		PpcContext	out_ctx = {.kind = "create_upper_paths output, stage",
			.detail = sname};

		if (input_rel != NULL)
			ppc_check_rel_pathlists(input_rel, root, &in_ctx);
		if (output_rel != NULL)
			ppc_check_rel_pathlists(output_rel, root, &out_ctx);
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
	HASHCTL		ctl = {0};

	/*
	 * Run the chained hook before we do anything.  Our walk can throw at
	 * pg_pathcheck.elevel = 'error', and an extension that loaded before us
	 * should not lose its shutdown callback just because we found a corrupt
	 * Path.
	 */
	if (prev_planner_shutdown_hook)
		(*prev_planner_shutdown_hook) (glob, parse, query_string, pstmt);

	/*
	 * No finished plan means we were not reached along the ordinary success
	 * path.  Walking a half-built tree buys little, and raising an error
	 * from inside error cleanup would turn a diagnostic into a second
	 * failure, so decline.
	 */
	if (pstmt == NULL)
		return;

	Assert(ppc_ext_id >= 0);
	top_root = GetPlannerGlobalExtensionState(glob, ppc_ext_id);
	if (top_root == NULL)
		return;

	ctl.keysize = sizeof(void *);
	ctl.entrysize = sizeof(void *);
	ctl.hcxt = CurrentMemoryContext;

	/*
	 * A single static suffices because this never runs re-entrantly: a nested
	 * planner() call completes its own shutdown hook, walk and all, before the
	 * outer planner reaches its own.  Nothing we call below plans a query.
	 */
	Assert(visited == NULL);

	visited = hash_create(PPC_NAME " visited", 1024, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * From here on the static must be cleared on the way out however we
	 * leave: at elevel 'error' the walk throws, and leaving "visited"
	 * pointing at a destroyed hash would be a loaded gun for the next
	 * planner run.  The sigsetjmp() this costs is the savemask == 0 flavour,
	 * so it is register saves only, no sigprocmask() syscall -- and it is
	 * reached only once per query that produced a plan.
	 */
	PG_TRY();
	{
		walk_planner_info(top_root);

		/* Subplan PlannerInfos and their backing top-level Paths. */
		foreach_node(PlannerInfo, subroot, glob->subroots)
			walk_planner_info(subroot);

		/*
		 * Deliberately duplicated crawler - just to find potential
		 * low-probability discrepancies or dangled pointers in this list
		 * itself.  No owning rel, so no parent-identity claim to make.
		 */
		walk_pathlist(glob->subpaths, "glob->subpaths", NULL, NULL, top_root);
	}
	PG_FINALLY();
	{
		hash_destroy(visited);
		visited = NULL;
	}
	PG_END_TRY();
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
	for (i = 0; i < (int) lengthof(root->upper_rels); i++)
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
	walk_subpath(root->non_recursive_path, "non_recursive_path", NULL, root);
}


/*
 * walk_rel
 *		Visit every Path slot on a RelOptInfo, and recurse into parallel
 *		RelOptInfos that hang off it (unique_rel, grouped_rel, part_rels[]).
 *		Upward links (parent / top_parent) are intentionally not followed:
 *		those rels are reached via simple_rel_array or join_rel_list anyway.
 *
 *		Every list below is passed with rel as its "owner", so walk_path()
 *		additionally asserts the parent-identity invariant on each element.
 *		Sub-path lists reached from inside a compound Path are not owned by
 *		anybody -- see walk_subpath().
 */
static void
walk_rel(RelOptInfo *rel, PlannerInfo *root)
{
	if (rel == NULL || !mark_visited(rel))
		return;

	check_stack_depth();

	walk_pathlist(rel->pathlist, "pathlist", rel, rel, root);
	walk_pathlist(rel->partial_pathlist, "partial_pathlist", rel, rel, root);
	walk_pathlist(rel->cheapest_parameterized_paths,
				  "cheapest_parameterized_paths", rel, rel, root);
	walk_path(rel->cheapest_startup_path, "cheapest_startup_path",
			  PPC_NO_INDEX, NIL, rel, rel, root);
	walk_path(rel->cheapest_total_path, "cheapest_total_path",
			  PPC_NO_INDEX, NIL, rel, rel, root);

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
		int			i;

		for (i = 0; i < rel->nparts; i++)
			if (rel->part_rels[i] != NULL)
				walk_rel(rel->part_rels[i], root);
	}
}


/*
 * ppc_check_tag
 *		Validate a Path pointer's NodeTag, the main "is this memory still a
 *		Path" probe.  Returns true when the tag is a live Path-family tag,
 *		false for NULL or for anything else -- having reported the latter.
 *
 *		With CLOBBER_FREED_MEMORY a freed chunk reads as 0x7F7F7F7F, which
 *		fails is_path_tag(); with a genuinely reused chunk the odds of
 *		landing on a valid Path tag are vanishingly low.
 */
static NodeTag
ppc_check_tag(Path *path, const char *listname, int idx, List *container,
			  RelOptInfo *rel, PlannerInfo *root, const PpcContext *ctx)
{
	NodeTag		tag;

	if (path == NULL)
		return T_Invalid;

	tag = nodeTag(path);
	if (is_path_tag(tag))
		return tag;

	ereport(ppc_elevel,
			errcode(ERRCODE_DATA_CORRUPTED),
			errmsg(PPC_NAME ": invalid NodeTag %s in %s, rel %s",
				   tag_name((int) tag), ppc_slot_str(listname, idx),
				   format_relnames(rel, root)),
			container != NIL
			? errdetail_internal("detected at %s; %s contents: %s",
								 ppc_context_str(ctx), listname,
								 format_pathlist(container))
			: errdetail_internal("detected at %s", ppc_context_str(ctx)),
			errcontext("while planning: %s", ppc_query_text()));

	return T_Invalid;
}


/*
 * ppc_check_parent
 *		Confirm that a Path found in one of owner's own pathlist-family
 *		slots actually claims owner as its parent.  A mismatch catches the
 *		aliasing case that escapes the NodeTag check: the memory chunk was
 *		freed and re-allocated as a different Path (possibly belonging to
 *		another rel) within the same planning session.
 *
 *		owner is NULL for Paths reached from inside a compound Path, where
 *		the parent legitimately varies (JoinPath.outerjoinpath belongs to
 *		the child rel, not the join rel), and for lists that no rel owns.
 *
 *		Note there is no exemption for the case "owner is a base or join
 *		rel but the path claims an upper rel".  That configuration has no
 *		legitimate producer in core, and it is exactly the shape a recycled
 *		upper-rel Path would take, so it must be reported.
 */
static void
ppc_check_parent(Path *path, NodeTag tag, RelOptInfo *owner,
				 const char *listname, int idx, List *container,
				 PlannerInfo *root, const PpcContext *ctx)
{
	RelOptInfo *actual;

	if (path == NULL || owner == NULL)
		return;

	/*
	 * The caller has already validated the tag -- ->parent is not readable
	 * otherwise -- and hands it over so we neither re-read nodeTag() nor walk
	 * is_path_tag()'s switch a second time on the walker's hot path.
	 */
	Assert(is_path_tag(tag));

	/*
	 * Upper rels legitimately hold paths whose ->parent is the input rel:
	 * apply_scanjoin_target_to_paths() and friends install scan/join paths
	 * directly into the upper rel's lists.  The identity invariant only
	 * holds for base and join rels.
	 *
	 * Note rel->grouped_rel and rel->unique_rel are *not* upper rels -- both
	 * build_grouped_rel() and the unique-rel builder memcpy() the RelOptInfo
	 * they derive from, reloptkind included -- so walk_rel() does apply this
	 * check to them.  That is intended: every path they receive is created
	 * with the derived rel as its parent (see create_final_unique_paths()).
	 */
	if (IS_UPPER_REL(owner))
		return;

	actual = path->parent;
	if (actual == owner)
		return;

	/*
	 * ->parent doesn't match.  As the memory is reused it might happen we see
	 * a sort of garbage here, so validate the pointer before dereferencing it
	 * via format_relnames() (which reads ->relids).
	 */
	if (actual == NULL || !IsA(actual, RelOptInfo))
	{
		ereport(ppc_elevel,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg(PPC_NAME ": path has non-RelOptInfo parent in %s, rel %s",
					   ppc_slot_str(listname, idx),
					   format_relnames(owner, root)),
				errdetail_internal("detected at %s; path %s",
								   ppc_context_str(ctx), tag_name((int) tag)),
				errcontext("while planning: %s", ppc_query_text()));
		return;
	}

	/*
	 * Classic same-size-class alias: the slot was reused by another rel's
	 * path.  Name both rels by their contributing base relations.
	 */
	ereport(ppc_elevel,
			errcode(ERRCODE_DATA_CORRUPTED),
			errmsg(PPC_NAME ": path parent mismatch in %s, rel %s",
				   ppc_slot_str(listname, idx), format_relnames(owner, root)),
			container != NIL
			? errdetail_internal("detected at %s; path %s claims rel %s; rows %.0f, startup_cost %.2f, total_cost %.2f; %s contents: %s",
								 ppc_context_str(ctx), tag_name((int) tag),
								 format_relnames(actual, root),
								 path->rows, path->startup_cost,
								 path->total_cost,
								 listname, format_pathlist(container))
			: errdetail_internal("detected at %s; path %s claims rel %s; rows %.0f, startup_cost %.2f, total_cost %.2f",
								 ppc_context_str(ctx), tag_name((int) tag),
								 format_relnames(actual, root),
								 path->rows, path->startup_cost,
								 path->total_cost),
			errcontext("while planning: %s", ppc_query_text()));
}


/*
 * ppc_check_pathlist
 *		Shallow validation of one list owned by rel: NodeTag then parent
 *		identity, for every element.  Used by the per-stage hooks, which
 *		have no visited set and so cannot descend.  The end-of-planning
 *		walker applies the very same two rules from walk_path().
 */
static void
ppc_check_pathlist(List *paths, const char *listname, RelOptInfo *rel,
				   PlannerInfo *root, const PpcContext *ctx)
{
	ListCell   *lc;

	foreach(lc, paths)
	{
		Path	   *path = (Path *) lfirst(lc);
		int			idx = foreach_current_index(lc);
		NodeTag		tag;

		tag = ppc_check_tag(path, listname, idx, paths, rel, root, ctx);
		if (tag != T_Invalid)
			ppc_check_parent(path, tag, rel, listname, idx, paths, root, ctx);
	}
}


/*
 * ppc_check_rel_pathlists
 *		Check both pathlist and partial_pathlist of one rel.
 */
static void
ppc_check_rel_pathlists(RelOptInfo *rel, PlannerInfo *root,
						const PpcContext *ctx)
{
	ppc_check_pathlist(rel->pathlist, "pathlist", rel, root, ctx);
	ppc_check_pathlist(rel->partial_pathlist, "partial_pathlist", rel, root,
					   ctx);
}


/*
 * walk_pathlist
 *		Visit each Path in a List, tagging every element with the list's name.
 *		owner, when non-NULL, is the rel that must own every element.
 */
static void
walk_pathlist(List *paths, const char *listname, RelOptInfo *owner,
			  RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, paths)
		walk_path((Path *) lfirst(lc), listname, foreach_current_index(lc),
				  paths, owner, rel, root);
}


/*
 * walk_subpath
 *		Descend into a Path embedded in another Path.  Such a child has no
 *		index, no containing list to dump, and no ownership claim to check:
 *		inside a compound Path the parent legitimately varies.
 */
static void
walk_subpath(Path *path, const char *source, RelOptInfo *rel,
			 PlannerInfo *root)
{
	walk_path(path, source, PPC_NO_INDEX, NIL, NULL, rel, root);
}


/*
 * walk_path
 *		Validate a Path's NodeTag and parent, then descend into every
 *		embedded sub-Path.
 *
 *		listname names the field or list that contains this Path, and idx
 *		its position when it came from a list.  container, when non-NIL, is
 *		the List whose full contents are dumped in the errdetail.  owner is
 *		the rel required to own this Path, or NULL if none.
 *
 *		Layout safety net: every Path subtype dereferenced below is guarded
 *		by a structural-hash entry in PPC_WALK_PATH_EXPECTED_HASHES further
 *		down in this file, and every *abstract* parent reached through a
 *		cast by one in PPC_ABSTRACT_PATH_EXPECTED_HASHES.  If you are here
 *		because a field name no longer compiles, *do not* paper over it by
 *		renaming the access — read the mirror blocks' header comments
 *		first: the hash-mismatch diagnostic will tell you which struct
 *		moved and why you are being forced to look.
 */
static void
walk_path(Path *path, const char *listname, int idx, List *container,
		  RelOptInfo *owner, RelOptInfo *rel, PlannerInfo *root)
{
	NodeTag		tag;

	if (path == NULL)
		return;

	check_stack_depth();

	tag = ppc_check_tag(path, listname, idx, container, rel, root, NULL);
	if (tag == T_Invalid)
		return;

	ppc_check_parent(path, tag, owner, listname, idx, container, root, NULL);

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
			walk_subpath(((BitmapHeapPath *) path)->bitmapqual,
						 "BitmapHeapPath.bitmapqual", rel, root);
			break;
		case T_BitmapAndPath:
			walk_pathlist(((BitmapAndPath *) path)->bitmapquals,
						  "BitmapAndPath.bitmapquals", NULL, rel, root);
			break;
		case T_BitmapOrPath:
			walk_pathlist(((BitmapOrPath *) path)->bitmapquals,
						  "BitmapOrPath.bitmapquals", NULL, rel, root);
			break;

		case T_SubqueryScanPath:
			walk_subpath(((SubqueryScanPath *) path)->subpath,
						 "SubqueryScanPath.subpath", rel, root);
			break;

		case T_ForeignPath:
			walk_subpath(((ForeignPath *) path)->fdw_outerpath,
						 "ForeignPath.fdw_outerpath", rel, root);
			break;

		case T_CustomPath:
			walk_pathlist(((CustomPath *) path)->custom_paths,
						  "CustomPath.custom_paths", NULL, rel, root);
			break;

		case T_AppendPath:
			walk_pathlist(((AppendPath *) path)->subpaths,
						  "AppendPath.subpaths", NULL, rel, root);
			break;
		case T_MergeAppendPath:
			walk_pathlist(((MergeAppendPath *) path)->subpaths,
						  "MergeAppendPath.subpaths", NULL, rel, root);
			break;

		case T_MaterialPath:
			walk_subpath(((MaterialPath *) path)->subpath,
						 "MaterialPath.subpath", rel, root);
			break;
		case T_MemoizePath:
			walk_subpath(((MemoizePath *) path)->subpath,
						 "MemoizePath.subpath", rel, root);
			break;
		case T_GatherPath:
			walk_subpath(((GatherPath *) path)->subpath,
						 "GatherPath.subpath", rel, root);
			break;
		case T_GatherMergePath:
			walk_subpath(((GatherMergePath *) path)->subpath,
						 "GatherMergePath.subpath", rel, root);
			break;

		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			walk_subpath(((JoinPath *) path)->outerjoinpath,
						 "JoinPath.outerjoinpath", rel, root);
			walk_subpath(((JoinPath *) path)->innerjoinpath,
						 "JoinPath.innerjoinpath", rel, root);
			break;

		case T_ProjectionPath:
			walk_subpath(((ProjectionPath *) path)->subpath,
						 "ProjectionPath.subpath", rel, root);
			break;
		case T_ProjectSetPath:
			walk_subpath(((ProjectSetPath *) path)->subpath,
						 "ProjectSetPath.subpath", rel, root);
			break;
		case T_SortPath:
			walk_subpath(((SortPath *) path)->subpath,
						 "SortPath.subpath", rel, root);
			break;
		case T_IncrementalSortPath:
			walk_subpath(((IncrementalSortPath *) path)->spath.subpath,
						 "IncrementalSortPath.subpath", rel, root);
			break;
		case T_GroupPath:
			walk_subpath(((GroupPath *) path)->subpath,
						 "GroupPath.subpath", rel, root);
			break;
		case T_UniquePath:
			walk_subpath(((UniquePath *) path)->subpath,
						 "UniquePath.subpath", rel, root);
			break;
		case T_AggPath:
			walk_subpath(((AggPath *) path)->subpath,
						 "AggPath.subpath", rel, root);
			break;
		case T_GroupingSetsPath:
			walk_subpath(((GroupingSetsPath *) path)->subpath,
						 "GroupingSetsPath.subpath", rel, root);
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
					MinMaxAggInfo *info = (MinMaxAggInfo *) lfirst(lc);

					if (info == NULL)
						continue;

					/*
					 * Not lfirst_node(): castNode() Asserts on the tag, and
					 * this list is as liable to have been recycled as
					 * anything else we inspect.  Report and carry on.
					 */
					if (!IsA(info, MinMaxAggInfo))
					{
						ereport(ppc_elevel,
								errcode(ERRCODE_DATA_CORRUPTED),
								errmsg(PPC_NAME ": invalid NodeTag %s in MinMaxAggPath.mmaggregates, rel %s",
									   tag_name((int) nodeTag(info)),
									   format_relnames(rel, root)),
								errcontext("while planning: %s",
										   ppc_query_text()));
						continue;
					}

					walk_subpath(info->path, "MinMaxAggInfo.path", rel, root);
					if (info->subroot != NULL)
						walk_planner_info(info->subroot);
				}
			}
			break;
		case T_WindowAggPath:
			walk_subpath(((WindowAggPath *) path)->subpath,
						 "WindowAggPath.subpath", rel, root);
			break;

		case T_SetOpPath:
			walk_subpath(((SetOpPath *) path)->leftpath,
						 "SetOpPath.leftpath", rel, root);
			walk_subpath(((SetOpPath *) path)->rightpath,
						 "SetOpPath.rightpath", rel, root);
			break;
		case T_RecursiveUnionPath:
			walk_subpath(((RecursiveUnionPath *) path)->leftpath,
						 "RecursiveUnionPath.leftpath", rel, root);
			walk_subpath(((RecursiveUnionPath *) path)->rightpath,
						 "RecursiveUnionPath.rightpath", rel, root);
			break;

		case T_LockRowsPath:
			walk_subpath(((LockRowsPath *) path)->subpath,
						 "LockRowsPath.subpath", rel, root);
			break;
		case T_ModifyTablePath:
			walk_subpath(((ModifyTablePath *) path)->subpath,
						 "ModifyTablePath.subpath", rel, root);
			break;
		case T_LimitPath:
			walk_subpath(((LimitPath *) path)->subpath,
						 "LimitPath.subpath", rel, root);
			break;

		default:

			/*
			 * is_path_tag() accepted this tag, so core has grown a Path
			 * subtype that this switch has not been taught about.  The
			 * count-parity StaticAssertDecl below fires on such a change,
			 * but `make bless-path-hashes` can be run without auditing the
			 * switch, so this arm is reachable in practice.
			 *
			 * Report it.  An Assert() here would abort the backend, and the
			 * cassert build that README recommends is precisely where that
			 * would turn a stale walker into a dead check-world run.
			 */
			ereport(ppc_elevel,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg(PPC_NAME ": unhandled Path subtype %s in %s, rel %s",
						   tag_name((int) tag), ppc_slot_str(listname, idx),
						   format_relnames(rel, root)),
					errdetail_internal("Sub-paths of this node were not checked."),
					errhint("Add a case to walk_path() and an entry to PPC_WALK_PATH_EXPECTED_HASHES."),
					errcontext("while planning: %s", ppc_query_text()));
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
static const char *const nodetag_names[] = {
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
 * ppc_slot_str
 *		Name the slot a Path was found in: "pathlist[3]" for a list element,
 *		or just "cheapest_total_path" for a scalar one.
 */
static const char *
ppc_slot_str(const char *listname, int idx)
{
	if (idx < 0)
		return listname;

	return psprintf("%s[%d]", listname, idx);
}


/*
 * ppc_context_str
 *		Describe the call site that requested the check.  NULL means the
 *		end-of-planning walker.  Only reporting paths call this; see the
 *		PpcContext comment for why.
 */
static const char *
ppc_context_str(const PpcContext *ctx)
{
	if (ctx == NULL)
		return "end of planning";

	Assert(ctx->kind != NULL);

	if (ctx->rel != NULL)
		return psprintf("%s %s", ctx->kind,
						format_relnames(ctx->rel, ctx->root));
	if (ctx->detail != NULL)
		return psprintf("%s %s", ctx->kind, ctx->detail);

	return ctx->kind;
}


/*
 * ppc_query_text
 *		The statement being planned, clipped to something a log file can
 *		stand.  Generated SQL runs to megabytes, and a finding fires once
 *		per offending list element.  Clip on a character boundary so the
 *		log stays valid in the server encoding.
 */
static const char *
ppc_query_text(void)
{
	int			len;

	if (debug_query_string == NULL)
		return "(none)";

	/*
	 * Bound the scan at the clip length.  A finding fires once per offending
	 * list element and a check-world run produces tens of thousands of them,
	 * against machine-generated SQL that can run to hundreds of kilobytes --
	 * so an unbounded strlen() here makes reporting cost O(statement length x
	 * findings) for an answer that only depends on the first PPC_MAX_QUERY_LEN
	 * bytes.
	 */
	if (memchr(debug_query_string, '\0', PPC_MAX_QUERY_LEN + 1) != NULL)
		return debug_query_string;

	len = pg_mbcliplen(debug_query_string, PPC_MAX_QUERY_LEN + 1,
					   PPC_MAX_QUERY_LEN);
	return psprintf("%.*s...", len, debug_query_string);
}


/*
 * format_relnames
 *		Build a human-readable string from the owning RelOptInfo's relids,
 *		resolving each member through root->simple_rte_array[i]->eref.
 *		Returns a palloc'd string like "{t1, t2}", or one of the fixed
 *		labels below.
 *
 *		"(upper)" means the rel has no base-relation members at all, which
 *		in core only happens for a query-wide upper rel.  "{}" -- an empty
 *		but present relids -- is a different thing and prints as such.
 */
static const char *
format_relnames(RelOptInfo *rel, PlannerInfo *root)
{
	StringInfoData buf;
	int			x;
	bool		first = true;

	if (rel == NULL || root == NULL)
		return "(unknown)";
	if (rel->relids == NULL)
		return "(upper)";
	if (!IsA(rel->relids, Bitmapset))
		return "(invalid relids)";

	/*
	 * A rel name list is a few dozen bytes; the 1kB StringInfo default would
	 * be almost entirely waste, and this runs up to three times per finding
	 * into the planner's per-query context, which is not reset until planning
	 * ends.
	 */
	initStringInfoExt(&buf, 64);
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
 *		Dump the contents of a List of Path pointers: position and NodeTag
 *		for each element.  Used in errdetail when corruption is detected.
 *
 *		Capped at PPC_MAX_LIST_DUMP entries: the caller reports once per
 *		bad element, so an uncapped dump of a wholly clobbered list is
 *		quadratic in the list length.
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

		if (i >= PPC_MAX_LIST_DUMP)
		{
			appendStringInfo(&buf, "; ... %d more",
							 list_length(paths) - i);
			break;
		}

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
	bool		found;

	Assert(visited != NULL);
	Assert(ptr != NULL);

	(void) hash_search(visited, &ptr, HASH_ENTER, &found);
	return !found;
}


/*
 * PPC_WALK_PATH_EXPECTED_HASHES
 *		Single hand-maintained source of truth for the set of concrete Path
 *		subtypes walk_path() knows how to descend into, together with each
 *		subtype's expected structural hash.  Drives two compile-time checks:
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
 *			 automated and is mandatory.  bless_path_hashes.pl refuses to
 *			 introduce a subtype it has never seen here, so case 1 above
 *			 stays a hand edit.)
 *
 *		All concrete subtypes are listed — even the ones walk_path() treats
 *		as leaves (T_Path, T_IndexPath, T_TidPath, T_TidRangePath,
 *		T_GroupResultPath) — because "no sub-Paths to descend" is itself a
 *		layout claim that can rot if core grows a new Path * field in one
 *		of them.
 *
 *		Each hash covers only its subtype's own body, not the bodies of
 *		embedded parent structs, so a struct that walk_path() reaches
 *		through a cast to its abstract parent needs its own entry in
 *		PPC_ABSTRACT_PATH_EXPECTED_HASHES below.
 *
 *		See README.md section "Bumping PostgreSQL" for the end-to-end
 *		workflow.
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
 * PPC_ABSTRACT_PATH_EXPECTED_HASHES
 *		Same idea, for the abstract Path subtypes.  These carry no NodeTag
 *		of their own, so they never appear in PATH_TAG_LIST — but walk_path()
 *		still reads fields through them, by casting a concrete node to its
 *		abstract parent:
 *
 *			((JoinPath *) path)->outerjoinpath
 *
 *		T_NestPath's hash covers the text "JoinPath jpath;", which does not
 *		change when JoinPath's own body does.  Without an entry here, a
 *		reorder or rename inside JoinPath would sail past every guard and
 *		leave walk_path() reading the wrong offset.
 *
 *		Add an entry whenever walk_path() starts dereferencing a new
 *		abstract parent, and keep the count in step with PATH_ABSTRACT_LIST.
 */
#define PPC_ABSTRACT_PATH_EXPECTED_HASHES(X) \
	X(JoinPath, 0x1198455f4125caa6ULL)

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

StaticAssertDecl((0 PATH_ABSTRACT_LIST(PPC_COUNT_ONE_ARG)) ==
				 (0 PPC_ABSTRACT_PATH_EXPECTED_HASHES(PPC_COUNT_TWO_ARG)),
				 "pg_pathcheck: number of abstract Path subtypes in "
				 "pathnodes.h no longer matches "
				 "PPC_ABSTRACT_PATH_EXPECTED_HASHES; add or remove entries "
				 "and check whether walk_path() dereferences the new one.");

#undef PPC_COUNT_ONE_ARG
#undef PPC_COUNT_TWO_ARG

/*
 * Per-subtype hash check: expand each entry into a StaticAssertDecl that
 * compares the blessed hash against the one gen_pathtags.pl just computed.
 * Token-paste PPC_PATH_HASH_ onto the tag to reference the generated
 * constant; stringify the tag so the diagnostic names the exact struct.
 * The paste works for both lists, since the generator publishes concrete
 * subtypes as PPC_PATH_HASH_T_<name> and abstract ones as
 * PPC_PATH_HASH_<name>.
 */
#define PPC_HASH_ASSERT(tag, expected)										\
	StaticAssertDecl((expected) == PPC_PATH_HASH_##tag,						\
					 "pg_pathcheck: struct layout for " #tag " changed in "	\
					 "pathnodes.h; audit walk_path() and run "				\
					 "`make bless-path-hashes` to refresh "					\
					 "the expected-hash lists.");

PPC_WALK_PATH_EXPECTED_HASHES(PPC_HASH_ASSERT)
PPC_ABSTRACT_PATH_EXPECTED_HASHES(PPC_HASH_ASSERT)

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
