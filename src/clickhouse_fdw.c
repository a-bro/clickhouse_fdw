/*-------------------------------------------------------------------------
 *
 * clickhousedb_fdw.c
 *		  Foreign-data wrapper for remote ClickHouse servers
 *
 * Portions Copyright (c) 2012-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/clickhousedb_fdw/clickhousedb_fdw.c
 *
 *-------------------------------------------------------------------------
 */
/* PosrgreSQL main header file */
#include "postgres.h"

#include <sys/time.h>
#include "access/htup_details.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#include "optimizer/optimizer.h"
#endif

#include "clickhousedb_fdw.h"

PG_MODULE_MAGIC;


/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST		0.01

/* If no remote estimates, assume a sort costs 20% extra */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwScanPrivateSelectSql,
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs,
	/* Integer representing the desired fetch_size */
	FdwScanPrivateFetchSize,

	/*
	 * String describing join i.e. names of relations being joined and types
	 * of join, added when the scan is join
	 */
	FdwScanPrivateRelations
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 1) INSERT statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwModifyPrivateInsertSQL,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
	FdwModifyPrivateTargetAttnums
};


/*
 * Execution state of a foreign scan using postgres_fdw.
 */
typedef struct ChFdwScanState
{
	Relation	rel;			/* relcache entry for the foreign table. NULL
						 * for a foreign join scan. */
	TupleDesc	tupdesc;		/* tuple descriptor of scan */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data */
	char	   *query;			/* text of SELECT command */
	List	   *retrieved_attrs;	/* list of retrieved attribute numbers */

	/* for remote query execution */
	ch_connection	conn;			/* connection for the scan */
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */
	ch_cursor  *ch_cursor;		/* result of query from clickhouse */

	/* for storing result tuple */
	HeapTuple  tuple;			/* array of currently-retrieved tuples */

	/* working memory contexts */
	MemoryContext batch_cxt;	/* context holding current batch of tuples */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */

	int			fetch_size;		/* number of tuples per fetch */
} ChFdwScanState;

/*
 * Execution state of a foreign insert.
 */
typedef struct CHFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* for remote query execution */
	ch_connection	conn;		/* connection for the scan */

	/* constructed query */
	char		   *sql_begin;	/* beginning part of constructed sql */
	StringInfoData	sql;

	/* extracted fdw_private data */
	char	   *query;			/* text of INSERT/UPDATE/DELETE command */
	List	   *target_attrs;	/* list of target attribute numbers */

	/* info about parameters for prepared statement */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} CHFdwModifyState;

/* Callback argument for ec_member_matches_foreign */
typedef struct
{
	Expr	   *current;		/* current expr, or NULL if not yet found */
	List	   *already_used;	/* expressions already dealt with */
} ec_member_foreign_arg;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(clickhousedb_fdw_handler);
PG_FUNCTION_INFO_V1(clickhousedb_raw_query);
extern PGDLLEXPORT void _PG_init(void);
static double time_used = 0;

/*
 * FDW callback routines
 */
static void clickhouseGetForeignRelSize(PlannerInfo *root,
                                        RelOptInfo *baserel,
                                        Oid foreigntableid);
static ForeignScan *clickhouseGetForeignPlan(PlannerInfo *root,
        RelOptInfo *foreignrel,
        Oid foreigntableid,
        ForeignPath *best_path,
        List *tlist,
        List *scan_clauses,
        Plan *outer_plan);
static int
clickhouseAcquireSampleRowsFunc(Relation relation, int elevel,
                                HeapTuple *rows, int targrows,
                                double *totalrows,
                                double *totaldeadrows);
static void clickhouseBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *clickhouseIterateForeignScan(ForeignScanState *node);
static void clickhouseEndForeignScan(ForeignScanState *node);
static List *clickhousePlanForeignModify(PlannerInfo *root,
        ModifyTable *plan,
        Index resultRelation,
        int subplan_index);
static void clickhouseBeginForeignModify(ModifyTableState *mtstate,
        ResultRelInfo *resultRelInfo,
        List *fdw_private,
        int subplan_index,
        int eflags);
static TupleTableSlot *clickhouseExecForeignInsert(EState *estate,
        ResultRelInfo *resultRelInfo,
        TupleTableSlot *slot,
        TupleTableSlot *planSlot);
static void clickhouseBeginForeignInsert(ModifyTableState *mtstate,
        ResultRelInfo *resultRelInfo);
static void clickhouseEndForeignInsert(EState *estate,
                                       ResultRelInfo *resultRelInfo);
static void clickhouseExplainForeignScan(ForeignScanState *node,
        ExplainState *es);
static void clickhouseGetForeignUpperPaths(PlannerInfo *root,
        UpperRelationKind stage,
        RelOptInfo *input_rel, RelOptInfo *output_rel,
        void *extra);
static bool clickhouseAnalyzeForeignTable(Relation relation,
        AcquireSampleRowsFunc *func,
        BlockNumber *totalpages);
static bool clickhouseRecheckForeignScan(ForeignScanState *node,
        TupleTableSlot *slot);

/*
 * Helper functions
 */
static void estimate_path_cost_size(double *p_rows, int *p_width,
                                    Cost *p_startup_cost, Cost *p_total_cost,
									double coef);
static CHFdwModifyState *create_foreign_modify(EState *estate,
        RangeTblEntry *rte,
        ResultRelInfo *resultRelInfo,
        CmdType operation,
        Plan *subplan,
        char *query,
        List *target_attrs);
static void prepare_foreign_modify(TupleTableSlot *slot,
                                   CHFdwModifyState *fmstate);
static const char **extend_insert_query(CHFdwModifyState *fmstate, TupleTableSlot *slot);
static void finish_foreign_modify(CHFdwModifyState *fmstate);
static void prepare_query_params(PlanState *node,
                                 List *fdw_exprs,
                                 int numParams,
                                 FmgrInfo **param_flinfo,
                                 List **param_exprs,
                                 const char ***param_values);
static int postgresAcquireSampleRowsFunc(Relation relation, int elevel,
        HeapTuple *rows, int targrows,
        double *totalrows,
        double *totaldeadrows);
static bool foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
                            JoinType jointype, RelOptInfo *outerrel, RelOptInfo *innerrel,
                            JoinPathExtraData *extra);
static bool foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
                                Node *havingQual);
static List *get_useful_pathkeys_for_relation(PlannerInfo *root,
        RelOptInfo *rel);
static List *get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
        Path *epq_path);
static void add_foreign_grouping_paths(PlannerInfo *root,
                                       RelOptInfo *input_rel,
                                       RelOptInfo *grouped_rel,
                                       GroupPathExtraData *extra);
static void apply_server_options(CHFdwRelationInfo *fpinfo);
static void apply_table_options(CHFdwRelationInfo *fpinfo);
static void merge_fdw_options(CHFdwRelationInfo *fpinfo,
                              const CHFdwRelationInfo *fpinfo_o,
                              const CHFdwRelationInfo *fpinfo_i);

/* empty _PG_init_ function */
void _PG_init(void){}


/* Make one query and close the connection */
Datum
clickhousedb_raw_query(PG_FUNCTION_ARGS)
{
	char *connstring = TextDatumGetCString(PG_GETARG_TEXT_P(1)),
		 *query = TextDatumGetCString(PG_GETARG_TEXT_P(0));

	ch_connection	conn = chfdw_http_connect(connstring);
	ch_cursor	   *cursor = conn.methods->simple_query(conn.conn, query);
	text		   *res = chfdw_http_fetch_raw_data(cursor);

	MemoryContextDelete(cursor->memcxt);
	conn.methods->disconnect(conn.conn);

	if (res)
		PG_RETURN_TEXT_P(res);

	PG_RETURN_NULL();
}

//calculate difference
double
time_diff(struct timeval * prior, struct timeval * latter)
{
	double x =
		(double)(latter->tv_usec - prior->tv_usec) / 1000.0L +
		(double)(latter->tv_sec - prior->tv_sec) * 1000.0L;

	return x;
}

/*
 * clickhouseGetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void
clickhouseGetForeignRelSize(PlannerInfo *root,
                            RelOptInfo *baserel,
                            Oid foreigntableid)
{
	CHFdwRelationInfo *fpinfo;
	ListCell   *lc;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	char *relname,
		 *refname;

	/*
	 * We use CHFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (CHFdwRelationInfo *) palloc0(sizeof(CHFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	/* Base foreign tables need to be pushed down always. */
	fpinfo->pushdown_safe = true;

	/* Look up foreign-table catalog info. */
	fpinfo->table = GetForeignTable(foreigntableid);
	fpinfo->server = GetForeignServer(fpinfo->table->serverid);

	/*
	 * Extract user-settable option values.  Note that per-table setting of
	 * use_remote_estimate overrides per-server setting.
	 */
	fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	fpinfo->shippable_extensions = NIL;

	chfdw_apply_custom_table_options(fpinfo, foreigntableid);

	fpinfo->user = NULL;

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	chfdw_classify_conditions(root, baserel, baserel->baserestrictinfo,
	                   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server.  These include all attrs needed for joins or final output, plus
	 * all attrs used in the local_conds.  (Note: if we end up using a
	 * parameterized scan, it's possible that some of the join clauses will be
	 * sent to the remote and thus we wouldn't really need to retrieve the
	 * columns used in them.  Doesn't seem worth detecting that case though.)
	 */
	fpinfo->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
	               &fpinfo->attrs_used);
	foreach (lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
		               &fpinfo->attrs_used);
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
	                          fpinfo->local_conds,
	                          baserel->relid,
	                          JOIN_INNER,
	                          NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * Set cached relation costs to some negative value, so that we can detect
	 * when they are set to some sensible costs during one (usually the first)
	 * of the calls to estimate_path_cost_size().
	 */
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the name of relation in fpinfo, while we are constructing it here.
	 * It will be used to build the string describing the join relation in
	 * EXPLAIN output. We can't know whether VERBOSE option is specified or
	 * not, so always schema-qualify the foreign table name.
	 */
	fpinfo->relation_name = makeStringInfo();
	relname = get_rel_name(foreigntableid);
	refname = rte->eref->aliasname;
	appendStringInfo(fpinfo->relation_name, "%s", quote_identifier(relname));
	if (*refname && strcmp(refname, relname) != 0)
		appendStringInfo(fpinfo->relation_name, " %s",
		                 quote_identifier(rte->eref->aliasname));

	/* No outer and inner relations. */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	fpinfo->lower_subquery_rels = NULL;
	/* Set the relation index. */
	fpinfo->relation_index = baserel->relid;
}

/*
 * get_useful_ecs_for_relation
 *		Determine which EquivalenceClasses might be involved in useful
 *		orderings of this relation.
 *
 * This function is in some respects a mirror image of the core function
 * pathkeys_useful_for_merging: for a regular table, we know what indexes
 * we have and want to test whether any of them are useful.  For a foreign
 * table, we don't know what indexes are present on the remote side but
 * want to speculate about which ones we'd like to use if they existed.
 *
 * This function returns a list of potentially-useful equivalence classes,
 * but it does not guarantee that an EquivalenceMember exists which contains
 * Vars only from the given relation.  For example, given ft1 JOIN t1 ON
 * ft1.x + t1.x = 0, this function will say that the equivalence class
 * containing ft1.x + t1.x is potentially useful.  Supposing ft1 is remote and
 * t1 is local (or on a different server), it will turn out that no useful
 * ORDER BY clause can be generated.  It's not our job to figure that out
 * here; we're only interested in identifying relevant ECs.
 */
static List *
get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_eclass_list = NIL;
	ListCell   *lc;
	Relids		relids;

	/*
	 * First, consider whether any active EC is potentially useful for a merge
	 * join against this relation.
	 */
	if (rel->has_eclass_joins)
	{
		foreach(lc, root->eq_classes)
		{
			EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);

			if (eclass_useful_for_merging(root, cur_ec, rel))
				useful_eclass_list = lappend(useful_eclass_list, cur_ec);
		}
	}

	/*
	 * Next, consider whether there are any non-EC derivable join clauses that
	 * are merge-joinable.  If the joininfo list is empty, we can exit
	 * quickly.
	 */
	if (rel->joininfo == NIL)
		return useful_eclass_list;

	/* If this is a child rel, we must use the topmost parent rel to search. */
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
	else
		relids = rel->relids;

	/* Check each join clause in turn. */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/* Consider only mergejoinable clauses */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;

		/* Make sure we've got canonical ECs. */
		update_mergeclause_eclasses(root, restrictinfo);

		/*
		 * restrictinfo->mergeopfamilies != NIL is sufficient to guarantee
		 * that left_ec and right_ec will be initialized, per comments in
		 * distribute_qual_to_rels.
		 *
		 * We want to identify which side of this merge-joinable clause
		 * contains columns from the relation produced by this RelOptInfo. We
		 * test for overlap, not containment, because there could be extra
		 * relations on either side.  For example, suppose we've got something
		 * like ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON
		 * A.y = D.y.  The input rel might be the joinrel between A and B, and
		 * we'll consider the join clause A.y = D.y. relids contains a
		 * relation not involved in the join class (B) and the equivalence
		 * class for the left-hand side of the clause contains a relation not
		 * involved in the input rel (C).  Despite the fact that we have only
		 * overlap and not containment in either direction, A.y is potentially
		 * useful as a sort column.
		 *
		 * Note that it's even possible that relids overlaps neither side of
		 * the join clause.  For example, consider A LEFT JOIN B ON A.x = B.x
		 * AND A.x = 1.  The clause A.x = 1 will appear in B's joininfo list,
		 * but overlaps neither side of B.  In that case, we just skip this
		 * join clause, since it doesn't suggest a useful sort order for this
		 * relation.
		 */
		if (bms_overlap(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->right_ec);
		else if (bms_overlap(relids, restrictinfo->left_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->left_ec);
	}

	return useful_eclass_list;
}

/*
 * get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join.  Here, we try
 * to figure out which pathkeys to consider.
 */
static List *
get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_eclass_list;
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) rel->fdw_private;
	EquivalenceClass *query_ec = NULL;
	ListCell   *lc;

	/*
	 * Pushing the query_pathkeys to the remote server is always worth
	 * considering, because it might let us avoid a local sort.
	 */
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);
			EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
			Expr	   *em_expr;

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 *
			 * chfdw_is_foreign_expr would detect volatile expressions as well, but
			 * checking ec_has_volatile here saves some cycles.
			 */
			if (pathkey_ec->ec_has_volatile ||
				!(em_expr = chfdw_find_em_expr(pathkey_ec, rel)) ||
				!chfdw_is_foreign_expr(root, rel, em_expr))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
		{
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
		}
	}

	/*
	 * Even if we're not using remote estimates, having the remote side do the
	 * sort generally won't be any worse than doing it locally, and it might
	 * be much better if the remote side can generate data in the right order
	 * without needing a sort at all.  However, what we're going to do next is
	 * try to generate pathkeys that seem promising for possible merge joins,
	 * and that's more speculative.  A wrong choice might hurt quite a bit, so
	 * bail out if we can't use remote estimates.
	 */
	if (!fpinfo->use_remote_estimate)
		return useful_pathkeys_list;

	/* Get the list of interesting EquivalenceClasses. */
	useful_eclass_list = get_useful_ecs_for_relation(root, rel);

	/* Extract unique EC for query, if any, so we don't consider it again. */
	if (list_length(root->query_pathkeys) == 1)
	{
		PathKey    *query_pathkey = linitial(root->query_pathkeys);

		query_ec = query_pathkey->pk_eclass;
	}

	/*
	 * As a heuristic, the only pathkeys we consider here are those of length
	 * one.  It's surely possible to consider more, but since each one we
	 * choose to consider will generate a round-trip to the remote side, we
	 * need to be a bit cautious here.  It would sure be nice to have a local
	 * cache of information about remote index definitions...
	 */
	foreach(lc, useful_eclass_list)
	{
		EquivalenceClass *cur_ec = lfirst(lc);
		Expr	   *em_expr;
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it. */
		if (cur_ec == query_ec)
			continue;

		/* If no pushable expression for this rel, skip it. */
		em_expr = chfdw_find_em_expr(cur_ec, rel);
		if (em_expr == NULL || !chfdw_is_foreign_expr(root, rel, em_expr))
			continue;

		/* Looks like we can generate a pathkey, so let's do it. */
		pathkey = make_canonical_pathkey(root, cur_ec,
										 linitial_oid(cur_ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		useful_pathkeys_list = lappend(useful_pathkeys_list,
									   list_make1(pathkey));
	}

	return useful_pathkeys_list;
}

/*
 * postgresGetForeignPaths
 *		Create possible scan paths for a scan on the foreign table
 */
static void
clickhouseGetForeignPaths(PlannerInfo *root,
                          RelOptInfo *baserel,
                          Oid foreigntableid)
{
	ForeignPath			*path;
	CHFdwRelationInfo	*fpinfo = (CHFdwRelationInfo *) baserel->fdw_private;

	path= create_foreignscan_path(root, baserel, NULL,
		fpinfo->rows, fpinfo->startup_cost, fpinfo->total_cost,
		NULL, NULL, NULL, NIL);

	add_path(baserel, (Path *) path);
	add_paths_with_pathkeys_for_rel(root, baserel, NULL);
}

/*
 * clickhouseGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
clickhouseGetForeignPlan(PlannerInfo *root,
                         RelOptInfo *foreignrel,
                         Oid foreigntableid,
                         ForeignPath *best_path,
                         List *tlist,
                         List *scan_clauses,
                         Plan *outer_plan)
{
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid;
	List	   *fdw_private;
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	List	   *params_list = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *fdw_recheck_quals = NIL;
	List	   *retrieved_attrs;
	StringInfoData sql;
	ListCell   *lc;
	struct timeval time1,time2;

	gettimeofday(&time1, NULL);

	if (IS_SIMPLE_REL(foreignrel))
	{
		/*
		 * For base relations, set scan_relid as the relid of the relation.
		 */
		scan_relid = foreignrel->relid;

		/*
		 * In a base-relation scan, we must apply the given scan_clauses.
		 *
		 * Separate the scan_clauses into those that can be executed remotely
		 * and those that can't.  baserestrictinfo clauses that were
		 * previously determined to be safe or unsafe by chfdw_classify_conditions
		 * are found in fpinfo->remote_conds and fpinfo->local_conds. Anything
		 * else in the scan_clauses list will be a join clause, which we have
		 * to check for remote-safety.
		 *
		 * Note: the join clauses we see here should be the exact same ones
		 * previously examined by postgresGetForeignPaths.  Possibly it'd be
		 * worth passing forward the classification work done then, rather
		 * than repeating it here.
		 *
		 * This code must match "extract_actual_clauses(scan_clauses, false)"
		 * except for the additional decision about remote versus local
		 * execution.
		 */
		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			/* Ignore any pseudoconstants, they're dealt with elsewhere */
			if (rinfo->pseudoconstant)
				continue;

			if (list_member_ptr(fpinfo->remote_conds, rinfo))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else if (list_member_ptr(fpinfo->local_conds, rinfo))
				local_exprs = lappend(local_exprs, rinfo->clause);
			else if (chfdw_is_foreign_expr(root, foreignrel, rinfo->clause))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else
				local_exprs = lappend(local_exprs, rinfo->clause);
		}

		/*
		 * For a base-relation scan, we have to support EPQ recheck, which
		 * should recheck all the remote quals.
		 */
		fdw_recheck_quals = remote_exprs;
	}
	else
	{
		/*
		 * Join relation or upper relation - set scan_relid to 0.
		 */
		scan_relid = 0;

		/*
		 * For a join rel, baserestrictinfo is NIL and we are not considering
		 * parameterization right now, so there should be no scan_clauses for
		 * a joinrel or an upper rel either.
		 */
		Assert(!scan_clauses);

		/*
		 * Instead we get the conditions to apply from the fdw_private
		 * structure.
		 */
		remote_exprs = extract_actual_clauses(fpinfo->remote_conds, false);
		local_exprs = extract_actual_clauses(fpinfo->local_conds, false);

		/*
		 * We leave fdw_recheck_quals empty in this case, since we never need
		 * to apply EPQ recheck clauses.  In the case of a joinrel, EPQ
		 * recheck is handled elsewhere --- see postgresGetForeignJoinPaths().
		 * If we're planning an upperrel (ie, remote grouping or aggregation)
		 * then there's no EPQ to do because SELECT FOR UPDATE wouldn't be
		 * allowed, and indeed we *can't* put the remote clauses into
		 * fdw_recheck_quals because the unaggregated Vars won't be available
		 * locally.
		 */

		/* Build the list of columns to be fetched from the foreign server. */
		fdw_scan_tlist = chfdw_build_tlist_to_deparse(foreignrel);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot. This is safe because all scans and
		 * joins support projection, so we never need to insert a Result node.
		 * Also, remove the local conditions from outer plan's quals, lest
		 * they will be evaluated twice, once by the local plan and once by
		 * the scan.
		 */
		if (outer_plan)
		{
			ListCell   *lc;

			/*
			 * Right now, we only consider grouping and aggregation beyond
			 * joins. Queries involving aggregates or grouping do not require
			 * EPQ mechanism, hence should not have an outer plan here.
			 */
			Assert(!IS_UPPER_REL(foreignrel));

			outer_plan->targetlist = fdw_scan_tlist;

			foreach (lc, local_exprs)
			{
				Join	   *join_plan = (Join *) outer_plan;
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join the local conditions of foreign scan plan
				 * can be part of the joinquals as well.
				 */
				if (join_plan->jointype == JOIN_INNER)
					join_plan->joinqual = list_delete(join_plan->joinqual,
					                                  qual);
			}
		}
	}

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 */
	initStringInfo(&sql);
	chfdw_deparse_select_stmt_for_rel(&sql, root, foreignrel, fdw_scan_tlist,
	                        remote_exprs, best_path->path.pathkeys,
	                        false, &retrieved_attrs, &params_list);

	/* Remember remote_exprs for possible use by postgresPlanDirectModify */
	fpinfo->final_remote_exprs = remote_exprs;

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match order in enum FdwScanPrivateIndex.
	 */
	fdw_private = list_make3(makeString(sql.data),
							 retrieved_attrs,
							 makeInteger(fpinfo->fetch_size));
	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name->data));

	gettimeofday(&time2, NULL);
	time_used += time_diff(&time1, &time2);

	/*
	 * Create the ForeignScan node for the given relation.
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 */
	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							params_list,
							fdw_private,
							fdw_scan_tlist,
							fdw_recheck_quals,
							outer_plan);
}

/*
 * clickhouseBeginForeignScan
 *		Initiate an executor scan of a foreign PostgreSQL table.
 */
static void
clickhouseBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	ChFdwScanState *fsstate;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			rtindex;
	int			numParams;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * We'll save private state in node->fdw_state.
	 */
	fsstate = (ChFdwScanState *) palloc0(sizeof(ChFdwScanState));
	node->fdw_state = (void *) fsstate;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.  In case of a join or aggregate, use the
	 * lowest-numbered member RTE as a representative; we would get the same
	 * result from any.
	 */
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
	rte = rt_fetch(rtindex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(rte->relid);
	user = GetUserMapping(userid, table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fsstate->conn = chfdw_get_connection(user);

	/* Get private info created by planner functions. */
	fsstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwScanPrivateSelectSql));
	fsstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
												 FdwScanPrivateRetrievedAttrs);
	fsstate->fetch_size = intVal(list_nth(fsplan->fdw_private,
										  FdwScanPrivateFetchSize));

	/* Create contexts for batches of tuples and per-tuple temp workspace. */
	fsstate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt,
	                     "clickhousedb_fdw tuple data",
	                     ALLOCSET_DEFAULT_SIZES);
	fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
	                    "clickhousedb_fdw temporary data",
	                    ALLOCSET_SMALL_SIZES);

	/*
	 * Get info we'll need for converting data fetched from the foreign server
	 * into local representation and error reporting during that process.
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		fsstate->rel = node->ss.ss_currentRelation;
		fsstate->tupdesc = RelationGetDescr(fsstate->rel);
	}
	else
	{
		fsstate->rel = NULL;
		fsstate->tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

	fsstate->attinmeta = TupleDescGetAttInMetadata(fsstate->tupdesc);

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 */
	numParams = list_length(fsplan->fdw_exprs);
	fsstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &fsstate->param_flinfo,
							 &fsstate->param_exprs,
							 &fsstate->param_values);
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * temp_context is a working context that can be reset after each tuple.
 */
static HeapTuple
fetch_tuple(ChFdwScanState *fsstate, TupleDesc tupdesc)
{
	AttInMetadata *attinmeta = fsstate->attinmeta;
	Datum	   *values;
	HeapTuple	tuple = NULL;
	ItemPointer ctid = NULL;
	ListCell   *lc;
	MemoryContext oldcontext;
	bool	   *nulls;
	int			j;
	int			r;
	void      **row_values;

	oldcontext = MemoryContextSwitchTo(fsstate->temp_cxt);

	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));

	/* Initialize to nulls for any columns not present in result */
	memset(nulls, true, tupdesc->natts * sizeof(bool));

	row_values = fsstate->conn.methods->fetch_row(fsstate->ch_cursor,
		fsstate->retrieved_attrs, tupdesc, values, nulls);

	/* in both cases (binary and non binary), NULL means end of tuples */
	if (row_values == NULL)
		goto cleanup;

	/* Parse clickhouse result */
	if (!fsstate->conn.is_binary)
	{
		/*
		 * for non binary connections we will get strings which we will try
		 * convert using postgres functions.
		 */
		j = 0;
		foreach(lc, fsstate->retrieved_attrs)
		{
			int		i = lfirst_int(lc);
			char   *valstr = (char *) row_values[j];

			Oid pgtype = TupleDescAttr(tupdesc, i - 1)->atttypid;

			/* that's the easy way to check array, otherwise use get_element_type on pgtype */
			if (valstr && valstr[0] == '[')
			{
				size_t	pos = 0;

				while (valstr[pos] != '\0')
				{
					if (valstr[pos] == '[')
						valstr[pos] = '{';
					if (valstr[pos] == ']')
						valstr[pos] = '}';
					pos++;
				}
			}
            else if (valstr && valstr[0] == '0' && valstr[1] == '0')
            {
                /* clickhouse supports such values which are invalid in postgres,
                 * so we just set set as NULL
                 */
                if (strcmp(valstr, "0000-00-00 00:00:00") == 0)
                    valstr = NULL;
            }
			else if (valstr && pgtype == VARCHAROID
					&& TupleDescAttr(tupdesc, i - 1)->atttypmod != 0)
			{
				char *pos;
				if ((pos = strstr(valstr, "\\0")) != NULL)
					pos[0] = '\0';
			}

			/* Apply the input function even to nulls, to support domains */
			nulls[i - 1] = (valstr == NULL);
			values[i - 1] = InputFunctionCall(&attinmeta->attinfuncs[i - 1],
										  valstr,
										  attinmeta->attioparams[i - 1],
										  attinmeta->atttypmods[i - 1]);
			j++;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * If we have a CTID to return, install it in both t_self and t_ctid.
	 * t_self is the normal place, but if the tuple is converted to a
	 * composite Datum, t_self will be lost; setting t_ctid allows CTID to be
	 * preserved during EvalPlanQual re-evaluations (see ROW_MARK_COPY code).
	 */
	if (ctid)
		tuple->t_self = tuple->t_data->t_ctid = *ctid;

	/*
	 * Stomp on the xmin, xmax, and cmin fields from the tuple created by
	 * heap_form_tuple.  heap_form_tuple actually creates the tuple with
	 * DatumTupleFields, not HeapTupleFields, but the executor expects
	 * HeapTupleFields and will happily extract system columns on that
	 * assumption.  If we don't do this then, for example, the tuple length
	 * ends up in the xmin field, which isn't what we want.
	 */
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetXmin(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetCmin(tuple->t_data, InvalidTransactionId);

cleanup:
	MemoryContextReset(fsstate->temp_cxt);
	return tuple;
}

/*
 * clickhouseIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 */
static TupleTableSlot *
clickhouseIterateForeignScan(ForeignScanState *node)
{
	HeapTuple		tup;
	ChFdwScanState *fsstate = (ChFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	struct timeval time1,time2;
	TupleDesc		tupdesc;

	/* make query if needed */
	if (fsstate->ch_cursor == NULL)
	{
		EState	*estate = node->ss.ps.state;
		MemoryContext	old = MemoryContextSwitchTo(fsstate->batch_cxt);
		fsstate->ch_cursor = fsstate->conn.methods->simple_query(fsstate->conn.conn,
				fsstate->query);

		time_used += fsstate->ch_cursor->request_time;
		MemoryContextSwitchTo(old);
	}

	if (fsstate->rel)
		tupdesc = RelationGetDescr(fsstate->rel);
	else
	{
		Assert(fsstate);
		tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

	gettimeofday(&time1, NULL);
	tup = fetch_tuple(fsstate, tupdesc);
	gettimeofday(&time2, NULL);
	time_used += time_diff(&time1, &time2);

	if (tup == NULL)
		return ExecClearTuple(slot);

	/*
	 * Return the next tuple.
	 */
	ExecStoreHeapTuple(tup, slot, false);
	return slot;
}

/*
 * clickhouseEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
clickhouseEndForeignScan(ForeignScanState *node)
{
	ChFdwScanState *fsstate = (ChFdwScanState *) node->fdw_state;

	time_used = 0;
	if (fsstate && fsstate->ch_cursor)
	{
		MemoryContextDelete(fsstate->ch_cursor->memcxt);
		fsstate->ch_cursor = NULL;
	}
}

/*
 * clickhousePlanForeignModify
 *		Plan an insert operation on a foreign table
 */
static List *
clickhousePlanForeignModify(PlannerInfo *root,
                            ModifyTable *plan,
                            Index resultRelation,
                            int subplan_index)
{
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	List	   *targetAttrs = NIL;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = heap_open(rte->relid, NoLock);
	if (operation == CMD_INSERT)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
	case CMD_INSERT:
		chfdw_deparse_insert_sql(&sql, rte, resultRelation, rel, targetAttrs);
		break;
	case CMD_UPDATE:
		elog(ERROR, "ClickHouse does not support updates");
	case CMD_DELETE:
		elog(ERROR, "ClickHouse does not support deletes");
	default:
		elog(ERROR, "unexpected operation: %d", (int) operation);
		break;
	}

	heap_close(rel, NoLock);


	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
	return list_make2(makeString(sql.data), targetAttrs);
}

/*
 * clickhouseBeginForeignModify
 *		Begin an insertoperation on a foreign table
 */
static void
clickhouseBeginForeignModify(ModifyTableState *mtstate,
                             ResultRelInfo *resultRelInfo,
                             List *fdw_private,
                             int subplan_index,
                             int eflags)
{
	CHFdwModifyState *fmstate;
	char	   *query;
	List	   *target_attrs = NULL;
	RangeTblEntry *rte;


	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Deconstruct fdw_private data. */
	query = strVal(list_nth(fdw_private, FdwModifyPrivateInsertSQL));
	target_attrs = (List *) list_nth(fdw_private, FdwModifyPrivateTargetAttnums);

	/* Find RTE. */
	rte = rt_fetch(resultRelInfo->ri_RangeTableIndex,
	               mtstate->ps.state->es_range_table);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
	                                rte,
	                                resultRelInfo,
	                                mtstate->operation,
	                                mtstate->mt_plans[subplan_index]->plan,
	                                query,
	                                target_attrs);

	resultRelInfo->ri_FdwState = fmstate;
}

ForeignServer *
chfdw_get_foreign_server(Relation rel)
{
	ForeignServer       *server;
	ForeignTable        *table;

	table = GetForeignTable(RelationGetRelid(rel));
	server = GetForeignServer(table->serverid);
	return server;
}

/*
 * clickhouseBeginForeignInsert
 *		Begin an insert operation on a foreign table
 */
static void
clickhouseBeginForeignInsert(ModifyTableState *mtstate,
                             ResultRelInfo *resultRelInfo)
{
	CHFdwModifyState *fmstate;
	ModifyTable *plan = castNode(ModifyTable, mtstate->ps.plan);
	EState	   *estate = mtstate->ps.state;
	Index		resultRelation = resultRelInfo->ri_RangeTableIndex;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	RangeTblEntry *rte;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			attnum;
	List	   *targetAttrs = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;
	StringInfoData	sql;

	/* We transmit all columns that are defined in the foreign table. */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (!attr->attisdropped)
			targetAttrs = lappend_int(targetAttrs, attnum);
	}

	/*
	 * If the foreign table is a partition, we need to create a new RTE
	 * describing the foreign table for use by chfdw_deparse_insert_sql and
	 * create_foreign_modify() below, after first copying the parent's RTE and
	 * modifying some fields to describe the foreign partition to work on.
	 * However, if this is invoked by UPDATE, the existing RTE may already
	 * correspond to this partition if it is one of the UPDATE subplan target
	 * rels; in that case, we can just use the existing RTE as-is.
	 */
	rte = list_nth(estate->es_range_table, resultRelation - 1);
	if (rte->relid != RelationGetRelid(rel))
	{
		rte = (RangeTblEntry *) copyObjectImpl(rte);
		rte->relid = RelationGetRelid(rel);
		rte->relkind = RELKIND_FOREIGN_TABLE;
	}

	initStringInfo(&sql);
	chfdw_deparse_insert_sql(&sql, rte, resultRelation, rel, targetAttrs);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
	                                rte,
	                                resultRelInfo,
	                                CMD_INSERT,
	                                NULL,
	                                sql.data,
	                                targetAttrs);

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * clickhouseExecForeignInsert
 *		Put one row to buffer, if buffer is big enough push it to ClickHouse
 */
static TupleTableSlot *
clickhouseExecForeignInsert(EState *estate,
                            ResultRelInfo *resultRelInfo,
                            TupleTableSlot *slot,
                            TupleTableSlot *planSlot)
{
	CHFdwModifyState *fmstate = (CHFdwModifyState *) resultRelInfo->ri_FdwState;

	extend_insert_query(fmstate, slot);

	if (fmstate->sql.len > (MaxAllocSize / 2 /* 512MB */))
	{
		fmstate->conn.methods->simple_insert(fmstate->conn.conn, fmstate->sql.data);
		resetStringInfo(&fmstate->sql);
	}

	MemoryContextReset(fmstate->temp_cxt);

	return slot;
}

/*
 * clickhouseEndForeignInsert
 *		Finish an insert operation on a foreign table
 */
static void
clickhouseEndForeignInsert(EState *estate,
                           ResultRelInfo *resultRelInfo)
{
	CHFdwModifyState *fmstate = (CHFdwModifyState *) resultRelInfo->ri_FdwState;

	if (fmstate)
	{
		if (fmstate->sql.len > 0)
			fmstate->conn.methods->simple_insert(fmstate->conn.conn, fmstate->sql.data);

		/* Destroy the execution state */
		finish_foreign_modify(fmstate);
	}
}

/*
 * clickhouseRecheckForeignScan
 *		Execute a local join execution plan for a foreign join
 */
static bool
clickhouseRecheckForeignScan(ForeignScanState *node, TupleTableSlot *slot)
{
	Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;
	PlanState  *outerPlan = outerPlanState(node);
	TupleTableSlot *result;

	/* For base foreign relations, it suffices to set fdw_recheck_quals */
	if (scanrelid > 0)
		return true;

	Assert(outerPlan != NULL);

	/* Execute a local join execution plan */
	result = ExecProcNode(outerPlan);
	if (TupIsNull(result))
		return false;

	/* Store result in the given slot */
	ExecCopySlot(slot, result);


	return true;
}

/*
 * clickhouseExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
clickhouseExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	List	   *fdw_private;
	char	   *sql;
	char	   *relations;

	fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;

	/*
	 * Add names of relation handled by the foreign scan when the scan is a
	 * join
	 */
	if (list_length(fdw_private) > FdwScanPrivateRelations)
	{
		relations = strVal(list_nth(fdw_private, FdwScanPrivateRelations));
		ExplainPropertyText("Relations", relations, es);
	}

	/*
	 * Add remote query, when VERBOSE option is specified.
	 */
	if (es->verbose)
	{
		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}

	if (es->timing && time_used > 0)
		ExplainPropertyFloat("FDW Time", "ms", time_used, 3, es);
}

/*
 * estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign relation
 *		either a base relation or a join between foreign relations or an upper
 *		relation containing foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 *
 * The function returns the cost and size estimates in p_row, p_width,
 * p_startup_cost and p_total_cost variables.
 */
static void
estimate_path_cost_size(double *p_rows, int *p_width,
                        Cost *p_startup_cost, Cost *p_total_cost, double coef)
{
	*p_rows = 1;
	*p_width = 1;
	*p_startup_cost = 1.0;
	*p_total_cost = -1 + coef;
}

/*
 * create_foreign_modify
 *		Construct an execution state of a foreign insert
 *		operation
 */
static CHFdwModifyState *
create_foreign_modify(EState *estate,
                      RangeTblEntry *rte,
                      ResultRelInfo *resultRelInfo,
                      CmdType operation,
                      Plan *subplan,
                      char *query,
                      List *target_attrs)
{
	CHFdwModifyState *fmstate;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;

	/* Begin constructing CHFdwModifyState. */
	fmstate = (CHFdwModifyState *) palloc0(sizeof(CHFdwModifyState));
	fmstate->rel = rel;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(rel));
	user = GetUserMapping(userid, table->serverid);

	fmstate->conn = chfdw_get_connection(user);

	/* Set up remote query information. */
	fmstate->query = query;
	fmstate->target_attrs = target_attrs;

	/* Init query prefix */
	initStringInfo(&fmstate->sql);
	fmstate->sql_begin = psprintf("%s FORMAT TSV\n", query);

	/* Create context for per-query temp workspace. */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
	                    "postgres_fdw temporary data",
	                    ALLOCSET_SMALL_SIZES);

	/* Prepare for output conversion of parameters used in prepared stmt. */
	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (operation == CMD_INSERT)
	{
		/* Set up for remaining transmittable parameters */
		foreach (lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			Assert(!attr->attisdropped);

			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);
	return fmstate;
}

/*
 * extend_insert_query
 *		Construct values part of INSERT query
 */
static const char **
extend_insert_query(CHFdwModifyState *fmstate, TupleTableSlot *slot)
{
	int			pindex = 0;
	MemoryContext oldcontext;
	bool first = true;

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	if (fmstate->sql.len == 0)
		appendStringInfoString(&fmstate->sql, fmstate->sql_begin);

	/* get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach (lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Datum		value;
			Oid		    type;
			bool		isnull;

			value = slot_getattr(slot, attnum, &isnull);
			type = TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->atttypid;

			if (!first)
				appendStringInfoChar(&fmstate->sql, '\t');
			first = false;

			if (isnull)
			{
				appendStringInfoString(&fmstate->sql, "\\N");
				pindex++;
				continue;
			}

			switch (type)
			{
			case BOOLOID:
			case INT2OID:
			case INT4OID:
				appendStringInfo(&fmstate->sql, "%d", DatumGetInt32(value));
			break;
			case INT8OID:
				appendStringInfo(&fmstate->sql, INT64_FORMAT, DatumGetInt64(value));
			break;
			case FLOAT4OID:
				appendStringInfo(&fmstate->sql, "%f", DatumGetFloat4(value));
			break;
			case FLOAT8OID:
				appendStringInfo(&fmstate->sql, "%f", DatumGetFloat8(value));
			break;
			case NUMERICOID:
			{
				Datum  valueDatum;
				float8 f;

				valueDatum = DirectFunctionCall1(numeric_float8, value);
				f = DatumGetFloat8(valueDatum);
				appendStringInfo(&fmstate->sql, "%f", f);
			}
			break;
			case BPCHAROID:
			case VARCHAROID:
			case TEXTOID:
			case JSONOID:
			case NAMEOID:
			case BITOID:
			case BYTEAOID:
			{
				char   *str = NULL;
				bool	tl = false;
				Oid		typoutput = InvalidOid;

				getTypeOutputInfo(type, &typoutput, &tl);
				str = OidOutputFunctionCall(typoutput, value);
				appendStringInfo(&fmstate->sql, "%s", str);
			}
			break;
			case DATEOID:
			{
				/* we expect Date on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_date_out, value));
				appendStringInfoString(&fmstate->sql, extval);
				pfree(extval);
				break;
			}
			case TIMEOID:
			{
				/* we expect DateTime on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_time_out, value));
				appendStringInfo(&fmstate->sql, "0001-01-01 %s", extval);
				pfree(extval);
				break;
			}
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
			{
				/* we expect DateTime on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_timestamp_out, value));
				appendStringInfoString(&fmstate->sql, extval);
				pfree(extval);
				break;
			}
			default:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				                errmsg("cannot convert constant value to clickhouse value"),
				                errhint("Constant value data type: %u", type)));
			}
			pindex++;
		}
		appendStringInfoChar(&fmstate->sql, '\n');
	}

	Assert(pindex == fmstate->p_nums);
	MemoryContextSwitchTo(oldcontext);

	return NULL;
}

/*
 * finish_foreign_modify
 *		Release resources for a foreign insert/delete operation
 */
static void
finish_foreign_modify(CHFdwModifyState *fmstate)
{
	Assert(fmstate != NULL);
	memset(&fmstate->conn, 0, sizeof(fmstate->conn));
}

/*
 * Prepare for processing of parameters used in remote query.
 */
static void
prepare_query_params(PlanState *node,
                     List *fdw_exprs,
                     int numParams,
                     FmgrInfo **param_flinfo,
                     List **param_exprs,
                     const char ***param_values)
{
	int			i;
	ListCell   *lc;

	Assert(numParams > 0);

	/* Prepare for output conversion of parameters used in remote query. */
	*param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

	i = 0;
	foreach (lc, fdw_exprs)
	{
		Node	   *param_expr = (Node *) lfirst(lc);
		Oid			typefnoid;
		bool		isvarlena;

		getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &(*param_flinfo)[i]);
		i++;
	}

	/*
	 * Prepare remote-parameter expressions for evaluation.  (Note: in
	 * practice, we expect that all these expressions will be just Params, so
	 * we could possibly do something more efficient than using the full
	 * expression-eval machinery for this.  But probably there would be little
	 * benefit, and it'd require postgres_fdw to know more than is desirable
	 * about Param evaluation.)
	 */
	*param_exprs = ExecInitExprList(fdw_exprs, node);

	/* Allocate buffer for text form of query parameters. */
	*param_values = (const char **) palloc0(numParams * sizeof(char *));
}

/*
 * clickhouseAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
clickhouseAnalyzeForeignTable(Relation relation,
                              AcquireSampleRowsFunc *func,
                              BlockNumber *totalpages)
{
	*func = clickhouseAcquireSampleRowsFunc;
	return true;
}

/*
 * Acquire a random sample of rows from foreign table managed by postgres_fdw.
 *
 * We fetch the whole table from the remote side and pick out some sample rows.
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the table and return it into
 * *totalrows.  Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the table.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int
clickhouseAcquireSampleRowsFunc(Relation relation, int elevel,
                                HeapTuple *rows, int targrows,
                                double *totalrows,
                                double *totaldeadrows)
{
	return 0;
}

static bool
is_simple_join_clause(Expr *expr)
{
	if (IsA(expr, RestrictInfo))
	{
		expr = ((RestrictInfo *) expr)->clause;
	}

	if (IsA(expr, OpExpr))
	{
		OpExpr	*opexpr = (OpExpr *) expr;
		if (chfdw_is_equal_op(opexpr->opno) == 1
				&& list_length(opexpr->args) == 2
				&& IsA(list_nth(opexpr->args, 0), Var)
				&& IsA(list_nth(opexpr->args, 1), Var))
			return true;
	}
	return false;
}

static List *
extract_join_equals(List *conds, List **to)
{
	ListCell *lc;
	List *res = NIL;
	foreach (lc, conds)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		if (is_simple_join_clause(expr))
			*to = lappend(*to, expr);
		else
			res = lappend(res, expr);
	}
	return res;
}

/*
 * Assess whether the join between inner and outer relations can be pushed down
 * to the foreign server. As a side effect, save information we obtain in this
 * function to CHFdwRelationInfo passed in.
 */
static bool
foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel, JoinType jointype,
                RelOptInfo *outerrel, RelOptInfo *innerrel,
                JoinPathExtraData *extra)
{
	CHFdwRelationInfo *fpinfo;
	CHFdwRelationInfo *fpinfo_o;
	CHFdwRelationInfo *fpinfo_i;
	ListCell   *lc;
	List	   *joinclauses;

	/*
	 * We support pushing down INNER, LEFT, RIGHT and FULL OUTER joins.
	 * Constructing queries representing SEMI and ANTI joins is hard, hence
	 * not considered right now.
	 */
	if (jointype != JOIN_INNER && jointype != JOIN_LEFT &&
	        jointype != JOIN_RIGHT && jointype != JOIN_FULL)
	{
		return false;
	}

	/*
	 * If either of the joining relations is marked as unsafe to pushdown, the
	 * join can not be pushed down.
	 */
	fpinfo = (CHFdwRelationInfo *) joinrel->fdw_private;
	fpinfo_o = (CHFdwRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (CHFdwRelationInfo *) innerrel->fdw_private;
	if (!fpinfo_o || !fpinfo_o->pushdown_safe ||
	        !fpinfo_i || !fpinfo_i->pushdown_safe)
	{
		return false;
	}

	/*
	 * If joining relations have local conditions, those conditions are
	 * required to be applied before joining the relations. Hence the join can
	 * not be pushed down.
	 */
	if (fpinfo_o->local_conds || fpinfo_i->local_conds)
	{
		return false;
	}

	/*
	 * Merge FDW options.  We might be tempted to do this after we have deemed
	 * the foreign join to be OK.  But we must do this beforehand so that we
	 * know which quals can be evaluated on the foreign server, which might
	 * depend on shippable_extensions.
	 */
	fpinfo->server = fpinfo_o->server;
	merge_fdw_options(fpinfo, fpinfo_o, fpinfo_i);

	/*
	 * Separate restrict list into join quals and pushed-down (other) quals.
	 *
	 * Join quals belonging to an outer join must all be shippable, else we
	 * cannot execute the join remotely.  Add such quals to 'joinclauses'.
	 *
	 * Add other quals to fpinfo->remote_conds if they are shippable, else to
	 * fpinfo->local_conds.  In an inner join it's okay to execute conditions
	 * either locally or remotely; the same is true for pushed-down conditions
	 * at an outer join.
	 *
	 * Note we might return failure after having already scribbled on
	 * fpinfo->remote_conds and fpinfo->local_conds.  That's okay because we
	 * won't consult those lists again if we deem the join unshippable.
	 */
	joinclauses = NIL;
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		bool		is_remote_clause = chfdw_is_foreign_expr(root, joinrel,
													   rinfo->clause);

		if (IS_OUTER_JOIN(jointype) &&
			!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
		{
			if (!is_remote_clause)
				return false;
			joinclauses = lappend(joinclauses, rinfo);
		}
		else
		{
			if (is_remote_clause)
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * deparseExplicitTargetList() isn't smart enough to handle anything other
	 * than a Var.  In particular, if there's some PlaceHolderVar that would
	 * need to be evaluated within this join tree (because there's an upper
	 * reference to a quantity that may go to NULL as a result of an outer
	 * join), then we can't try to push the join down because we'll fail when
	 * we get to deparseExplicitTargetList().  However, a PlaceHolderVar that
	 * needs to be evaluated *at the top* of this join tree is OK, because we
	 * can do that locally after fetching the results from the remote side.
	 */
	foreach (lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = lfirst(lc);
		Relids		relids;

		/* PlaceHolderInfo refers to parent relids, not child relids. */
		relids = IS_OTHER_REL(joinrel) ?
		         joinrel->top_parent_relids : joinrel->relids;

		if (bms_is_subset(phinfo->ph_eval_at, relids) &&
		        bms_nonempty_difference(relids, phinfo->ph_eval_at))
		{
			return false;
		}
	}

	/* Save the join clauses, for later use. */
	fpinfo->joinclauses = joinclauses;

	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;

	/*
	 * By default, both the input relations are not required to be deparsed as
	 * subqueries, but there might be some relations covered by the input
	 * relations that are required to be deparsed as subqueries, so save the
	 * relids of those relations for later use by the deparser.
	 */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	Assert(bms_is_subset(fpinfo_o->lower_subquery_rels, outerrel->relids));
	Assert(bms_is_subset(fpinfo_i->lower_subquery_rels, innerrel->relids));
	fpinfo->lower_subquery_rels = bms_union(fpinfo_o->lower_subquery_rels,
	                                        fpinfo_i->lower_subquery_rels);

	/*
	 * Pull the other remote conditions from the joining relations into join
	 * clauses or other remote clauses (remote_conds) of this relation
	 * wherever possible. This avoids building subqueries at every join step.
	 *
	 * For an inner join, clauses from both the relations are added to the
	 * other remote clauses. For LEFT and RIGHT OUTER join, the clauses from
	 * the outer side are added to remote_conds since those can be evaluated
	 * after the join is evaluated. The clauses from inner side are added to
	 * the joinclauses, since they need to be evaluated while constructing the
	 * join.
	 *
	 * For a FULL OUTER JOIN, the other clauses from either relation can not
	 * be added to the joinclauses or remote_conds, since each relation acts
	 * as an outer relation for the other.
	 *
	 * The joining sides can not have local conditions, thus no need to test
	 * shippability of the clauses being pulled up.
	 */
	switch (jointype)
	{
	case JOIN_INNER:
		fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
		                                   list_copy(fpinfo_i->remote_conds));
		fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
		                                   list_copy(fpinfo_o->remote_conds));

		/*
		 * For an inner join, some restrictions can be treated alike. Treating the
		 * pushed down conditions as join conditions allows a top level full outer
		 * join to be deparsed without requiring subqueries.
		 */
		Assert(!fpinfo->joinclauses);
		fpinfo->remote_conds = extract_join_equals(fpinfo->remote_conds,
										&fpinfo->joinclauses);
		break;

	case JOIN_LEFT:
		fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
		                                  list_copy(fpinfo_i->remote_conds));
		fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
		                                   list_copy(fpinfo_o->remote_conds));
		break;

	case JOIN_RIGHT:
		fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
		                                  list_copy(fpinfo_o->remote_conds));
		fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
		                                   list_copy(fpinfo_i->remote_conds));
		break;

	case JOIN_FULL:

		/*
		 * In this case, if any of the input relations has conditions, we
		 * need to deparse that relation as a subquery so that the
		 * conditions can be evaluated before the join.  Remember it in
		 * the fpinfo of this relation so that the deparser can take
		 * appropriate action.  Also, save the relids of base relations
		 * covered by that relation for later use by the deparser.
		 */
		if (fpinfo_o->remote_conds)
		{
			fpinfo->make_outerrel_subquery = true;
			fpinfo->lower_subquery_rels =
			    bms_add_members(fpinfo->lower_subquery_rels,
			                    outerrel->relids);
		}
		if (fpinfo_i->remote_conds)
		{
			fpinfo->make_innerrel_subquery = true;
			fpinfo->lower_subquery_rels =
			    bms_add_members(fpinfo->lower_subquery_rels,
			                    innerrel->relids);
		}
		break;

	default:
		/* Should not happen, we have just checked this above */
		elog(ERROR, "unsupported join type %d", jointype);
	}

	/* Mark that this join can be pushed down safely */
	fpinfo->pushdown_safe = true;

	/* Get user mapping */
	if (fpinfo->use_remote_estimate)
	{
		if (fpinfo_o->use_remote_estimate)
			fpinfo->user = fpinfo_o->user;
		else
			fpinfo->user = fpinfo_i->user;
	}
	else
		fpinfo->user = NULL;

	/*
	 * Set cached relation costs to some negative value, so that we can detect
	 * when they are set to some sensible costs, during one (usually the
	 * first) of the calls to estimate_path_cost_size().
	 */
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this join relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.
	 */
	fpinfo->relation_name = makeStringInfo();
	appendStringInfo(fpinfo->relation_name, "(%s) %s JOIN (%s)",
					 fpinfo_o->relation_name->data,
					 chfdw_get_jointype_name(fpinfo->jointype),
					 fpinfo_i->relation_name->data);

	/*
	 * Set the relation index.  This is defined as the position of this
	 * joinrel in the join_rel_list list plus the length of the rtable list.
	 * Note that since this joinrel is at the end of the join_rel_list list
	 * when we are called, we can get the position by list_length.
	 */
	Assert(fpinfo->relation_index == 0);	/* shouldn't be set yet */
	fpinfo->relation_index =
		list_length(root->parse->rtable) + list_length(root->join_rel_list);

	return true;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path)
{
	List	   *useful_pathkeys_list = NIL; /* List of all pathkeys */
	ListCell   *lc;

	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

	/* Create one path for each set of pathkeys we found above. */
	foreach(lc, useful_pathkeys_list)
	{
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);
		Path	   *sorted_epq_path;

		estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.5);

		/*
		 * The EPQ path must be at least as well sorted as the path itself, in
		 * case it gets used as input to a mergejoin.
		 */
		sorted_epq_path = epq_path;
		if (sorted_epq_path != NULL &&
			!pathkeys_contained_in(useful_pathkeys,
								   sorted_epq_path->pathkeys))
			sorted_epq_path = (Path *)
				create_sort_path(root,
								 rel,
								 sorted_epq_path,
								 useful_pathkeys,
								 -1.0);

		if (IS_SIMPLE_REL(rel))
			add_path(rel, (Path *)
					 create_foreignscan_path(root, rel,
											 NULL,
											 rows,
											 startup_cost,
											 total_cost,
											 useful_pathkeys,
											 NULL,
											 sorted_epq_path,
											 NIL));
		else
			add_path(rel, (Path *)
					 create_foreign_join_path(root, rel,
											  NULL,
											  rows,
											  startup_cost,
											  total_cost,
											  useful_pathkeys,
											  NULL,
											  sorted_epq_path,
											  NIL));
	}
}

/*
 * Merge FDW options from input relations into a new set of options for a join
 * or an upper rel.
 *
 * For a join relation, FDW-specific information about the inner and outer
 * relations is provided using fpinfo_i and fpinfo_o.  For an upper relation,
 * fpinfo_o provides the information for the input relation; fpinfo_i is
 * expected to NULL.
 */
static void
merge_fdw_options(CHFdwRelationInfo *fpinfo,
                  const CHFdwRelationInfo *fpinfo_o,
                  const CHFdwRelationInfo *fpinfo_i)
{
	/* We must always have fpinfo_o. */
	Assert(fpinfo_o);

	/* fpinfo_i may be NULL, but if present the servers must both match. */
	Assert(!fpinfo_i ||
		   fpinfo_i->server->serverid == fpinfo_o->server->serverid);

	/*
	 * Copy the server specific FDW options.  (For a join, both relations come
	 * from the same server, so the server options should have the same value
	 * for both relations.)
	 */
	fpinfo->fdw_startup_cost = fpinfo_o->fdw_startup_cost;
	fpinfo->fdw_tuple_cost = fpinfo_o->fdw_tuple_cost;
	fpinfo->shippable_extensions = fpinfo_o->shippable_extensions;
	fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate;
	fpinfo->fetch_size = fpinfo_o->fetch_size;

	/* Merge the table level options from either side of the join. */
	if (fpinfo_i)
	{
		/*
		 * We'll prefer to use remote estimates for this join if any table
		 * from either side of the join is using remote estimates.  This is
		 * most likely going to be preferred since they're already willing to
		 * pay the price of a round trip to get the remote EXPLAIN.  In any
		 * case it's not entirely clear how we might otherwise handle this
		 * best.
		 */
		fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate ||
			fpinfo_i->use_remote_estimate;

		/*
		 * Set fetch size to maximum of the joining sides, since we are
		 * expecting the rows returned by the join to be proportional to the
		 * relation sizes.
		 */
		fpinfo->fetch_size = Max(fpinfo_o->fetch_size, fpinfo_i->fetch_size);
	}
}

/*
 * clickhouseGetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if join is safe to push down.
 */
static void
clickhouseGetForeignJoinPaths(PlannerInfo *root,
                              RelOptInfo *joinrel,
                              RelOptInfo *outerrel,
                              RelOptInfo *innerrel,
                              JoinType jointype,
                              JoinPathExtraData *extra)
{
	CHFdwRelationInfo *fpinfo;
	ForeignPath *joinpath;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	Path	   *epq_path;		/* Path to create plan to be executed when
					 * EvalPlanQual gets triggered. */

	struct timeval time1,time2;
	gettimeofday(&time1, NULL);

	/*
	 * Skip if this join combination has been considered already.
	 */
	if (joinrel->fdw_private)
		return;

	/*
	 * Create unfinished CHFdwRelationInfo entry which is used to indicate
	 * that the join relation is already considered, so that we won't waste
	 * time in judging safety of join pushdown and adding the same paths again
	 * if found safe. Once we know that this join can be pushed down, we fill
	 * the entry.
	 */
	fpinfo = (CHFdwRelationInfo *) palloc0(sizeof(CHFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	joinrel->fdw_private = fpinfo;
	/* attrs_used is only for base relations. */
	fpinfo->attrs_used = NULL;
	epq_path = NULL;

	if (!foreign_join_ok(root, joinrel, jointype, outerrel, innerrel, extra))
	{
		/* Free path required for EPQ if we copied one; we don't need it now */
		if (epq_path)
			pfree(epq_path);

		return;
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path. The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 * The local conditions are applied after the join has been computed on
	 * the remote side like quals in WHERE clause, so pass jointype as
	 * JOIN_INNER.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);
	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * If we are going to estimate costs locally, estimate the join clause
	 * selectivity here while we have special join info.
	 */
	if (!fpinfo->use_remote_estimate)
		fpinfo->joinclause_sel = clauselist_selectivity(root, fpinfo->joinclauses,
														0, fpinfo->jointype,
														extra->sjinfo);

	/* Estimate costs for bare join relation */
	estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0);
	/* Now update this information in the joinrel */
	joinrel->rows = rows;
	joinrel->reltarget->width = width;
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/*
	 * Create a new join path and add it to the joinrel which represents a
	 * join between foreign tables.
	 */
	joinpath = create_foreign_join_path(root,
										joinrel,
										NULL,	/* default pathtarget */
										rows,
										startup_cost,
										total_cost,
										NIL,	/* no pathkeys */
										NULL,
										epq_path,
										NIL);	/* no fdw_private */

	/* Add generated path into joinrel by add_path(). */
	add_path(joinrel, (Path *) joinpath);

	/* Consider pathkeys for the join relation */
	add_paths_with_pathkeys_for_rel(root, joinrel, epq_path);

	gettimeofday(&time2, NULL);
	time_used += time_diff(&time1, &time2);
}

/*
 * Assess whether the aggregation, grouping and having operations can be pushed
 * down to the foreign server.  As a side effect, save information we obtain in
 * this function to CHFdwRelationInfo of the input relation.
 */
static bool
foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
                    Node *havingQual)
{
	Query	   *query = root->parse;
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) grouped_rel->fdw_private;
	PathTarget *grouping_target = grouped_rel->reltarget;
	CHFdwRelationInfo *ofpinfo;
	List	   *aggvars;
	ListCell   *lc;
	int			i;
	List	   *tlist = NIL;

	/* We currently don't support pushing Grouping Sets. */
	if (query->groupingSets)
		return false;

	/* Get the fpinfo of the underlying scan relation. */
	ofpinfo = (CHFdwRelationInfo *) fpinfo->outerrel->fdw_private;

	/*
	 * If underlying scan relation has any local conditions, those conditions
	 * are required to be applied before performing aggregation.  Hence the
	 * aggregate cannot be pushed down.
	 */
	if (ofpinfo->local_conds)
		return false;

	/*
	 * Examine grouping expressions, as well as other expressions we'd need to
	 * compute, and check whether they are safe to push down to the foreign
	 * server.  All GROUP BY expressions will be part of the grouping target
	 * and thus there is no need to search for them separately.  Add grouping
	 * expressions into target list which will be passed to foreign server.
	 */
	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);
		ListCell   *l;

		/* Check whether this expression is part of GROUP BY clause */
		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/*
			 * If any GROUP BY expression is not shippable, then we cannot
			 * push down aggregation to the foreign server.
			 */
			if (!chfdw_is_foreign_expr(root, grouped_rel, expr))
				return false;

			/*
			 * Pushable, so add to tlist.  We need to create a TLE for this
			 * expression and apply the sortgroupref to it.  We cannot use
			 * add_to_flat_tlist() here because that avoids making duplicate
			 * entries in the tlist.  If there are duplicate entries with
			 * distinct sortgrouprefs, we have to duplicate that situation in
			 * the output tlist.
			 */
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else
		{
			/*
			 * Non-grouping expression we need to compute.  Is it shippable?
			 */
			if (chfdw_is_foreign_expr(root, grouped_rel, expr))
			{
				/* Yes, so add to tlist as-is; OK to suppress duplicates */
				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
			else
			{
				/* Not pushable as a whole; extract its Vars and aggregates */
				aggvars = pull_var_clause((Node *) expr,
				                          PVC_INCLUDE_AGGREGATES);

				/*
				 * If any aggregate expression is not shippable, then we
				 * cannot push down aggregation to the foreign server.
				 */
				if (!chfdw_is_foreign_expr(root, grouped_rel, (Expr *) aggvars))
					return false;

				/*
				 * Add aggregates, if any, into the targetlist.  Plain Vars
				 * outside an aggregate can be ignored, because they should be
				 * either same as some GROUP BY column or part of some GROUP
				 * BY expression.  In either case, they are already part of
				 * the targetlist and thus no need to add them again.  In fact
				 * including plain Vars in the tlist when they do not match a
				 * GROUP BY column would cause the foreign server to complain
				 * that the shipped query is invalid.
				 */
				foreach(l, aggvars)
				{
					Expr	   *expr = (Expr *) lfirst(l);

					if (IsA(expr, Aggref))
						tlist = add_to_flat_tlist(tlist, list_make1(expr));
				}
			}
		}

		i++;
	}

	/*
	 * Classify the pushable and non-pushable HAVING clauses and save them in
	 * remote_conds and local_conds of the grouped rel's fpinfo.
	 */
	if (havingQual)
	{
		ListCell   *lc;

		foreach(lc, (List *) havingQual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			RestrictInfo *rinfo;

			/*
			 * Currently, the core code doesn't wrap havingQuals in
			 * RestrictInfos, so we must make our own.
			 */
			Assert(!IsA(expr, RestrictInfo));
			rinfo = make_restrictinfo(expr,
									  true,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
			if (chfdw_is_foreign_expr(root, grouped_rel, expr))
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * If there are any local conditions, pull Vars and aggregates from it and
	 * check whether they are safe to pushdown or not.
	 */
	if (fpinfo->local_conds)
	{
		List	   *aggvars = NIL;
		ListCell   *lc;

		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			aggvars = list_concat(aggvars,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_INCLUDE_AGGREGATES));
		}

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			/*
			 * If aggregates within local conditions are not safe to push
			 * down, then we cannot push down the query.  Vars are already
			 * part of GROUP BY clause which are checked above, so no need to
			 * access them again here.
			 */
			if (IsA(expr, Aggref))
			{
				if (!chfdw_is_foreign_expr(root, grouped_rel, expr))
					return false;

				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
		}
	}

	/* Store generated targetlist */
	fpinfo->grouped_tlist = tlist;

	/* Safe to pushdown */
	fpinfo->pushdown_safe = true;

	/*
	 * Set cached relation costs to some negative value, so that we can detect
	 * when they are set to some sensible costs, during one (usually the
	 * first) of the calls to estimate_path_cost_size().
	 */
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this grouped relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.
	 */
	fpinfo->relation_name = makeStringInfo();
	appendStringInfo(fpinfo->relation_name, "Aggregate on (%s)",
					 ofpinfo->relation_name->data);

	return true;
}

/*
 * clickhouseGetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 *
 * Right now, we only support aggregate, grouping and having clause pushdown.
 */
static void
clickhouseGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
                               RelOptInfo *input_rel, RelOptInfo *output_rel,
                               void *extra)
{
	CHFdwRelationInfo *fpinfo;
	struct timeval time1,time2;

	gettimeofday(&time1, NULL);

	/*
	 * If input rel is not safe to pushdown, then simply return as we cannot
	 * perform any post-join operations on the foreign server.
	 */
	if (!input_rel->fdw_private ||
	        !((CHFdwRelationInfo *) input_rel->fdw_private)->pushdown_safe)
		return;

	/* Ignore stages we don't support; and skip any duplicate calls. */
	if (stage != UPPERREL_GROUP_AGG || output_rel->fdw_private)
		return;

	fpinfo = (CHFdwRelationInfo *) palloc0(sizeof(CHFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	output_rel->fdw_private = fpinfo;

	add_foreign_grouping_paths(root, input_rel, output_rel,
	                           (GroupPathExtraData *) extra);

	gettimeofday(&time2, NULL);
	time_used += time_diff(&time1, &time2);
}

/*
 * add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * Given input_rel represents the underlying scan.  The paths are added to the
 * given grouped_rel.
 */
static void
add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
                           RelOptInfo *grouped_rel,
                           GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	CHFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	CHFdwRelationInfo *fpinfo = grouped_rel->fdw_private;
	ForeignPath *grouppath;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Nothing to be done, if there is no grouping or aggregation required. */
	if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
		!root->hasHavingQual)
		return;

	Assert(extra->patype == PARTITIONWISE_AGGREGATE_NONE ||
		   extra->patype == PARTITIONWISE_AGGREGATE_FULL);

	/* save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * Assess if it is safe to push down aggregation and grouping.
	 *
	 * Use HAVING qual from extra. In case of child partition, it will have
	 * translated Vars.
	 */
	if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual))
		return;

	/* Estimate the cost of push down */
	estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);

	/* Now update this information in the fpinfo */
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/* Create and add foreign path to the grouping relation. */
#if (PG_VERSION_NUM < 120000)
	grouppath = create_foreignscan_path(root,
	                                    grouped_rel,
	                                    grouped_rel->reltarget,
	                                    rows,
	                                    startup_cost,
	                                    total_cost,
	                                    NIL,	/* no pathkeys */
	                                    NULL,	/* no required_outer */
	                                    NULL,
	                                    NIL);	/* no fdw_private */
#else
	grouppath = create_foreign_upper_path(root,
										  grouped_rel,
										  grouped_rel->reltarget,
										  rows,
										  startup_cost,
										  total_cost,
										  NIL,	/* no pathkeys */
										  NULL,
										  NIL); /* no fdw_private */
#endif

	/* Add generated path into grouped_rel by add_path(). */
	add_path(grouped_rel, (Path *) grouppath);
}

/*
 * Find an equivalence class member expression, all of whose Vars, come from
 * the indicated relation.
 */
Expr *
chfdw_find_em_expr(EquivalenceClass *ec, RelOptInfo *rel)
{
	ListCell   *lc_em;

	foreach(lc_em, ec->ec_members)
	{
		EquivalenceMember *em = lfirst(lc_em);

		if (bms_is_subset(em->em_relids, rel->relids) &&
			!bms_is_empty(em->em_relids))
		{
			/*
			 * If there is more than one equivalence member whose Vars are
			 * taken entirely from this relation, we'll be content to choose
			 * any one of those.
			 */
			return em->em_expr;
		}
	}

	/* We didn't find any suitable equivalence class expression */
	return NULL;
}

static List *
clickhouseImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	ForeignServer       *server;

	server = GetForeignServer(serverOid);
	return chfdw_construct_create_tables(stmt, server);
}


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
clickhousedb_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables */
	routine->GetForeignRelSize = clickhouseGetForeignRelSize;
	routine->GetForeignPaths = clickhouseGetForeignPaths;
	routine->GetForeignPlan = clickhouseGetForeignPlan;
	routine->BeginForeignScan = clickhouseBeginForeignScan;
	routine->IterateForeignScan = clickhouseIterateForeignScan;
	routine->ReScanForeignScan = clickhouseEndForeignScan;
	routine->EndForeignScan = clickhouseEndForeignScan;

	/* Functions for updating foreign tables */
	routine->PlanForeignModify = clickhousePlanForeignModify;
	routine->BeginForeignModify = clickhouseBeginForeignModify;
	routine->ExecForeignInsert = clickhouseExecForeignInsert;
	routine->BeginForeignInsert = clickhouseBeginForeignInsert;

	routine->EndForeignInsert = clickhouseEndForeignInsert;
	routine->EndForeignModify = clickhouseEndForeignInsert;

	/* Function for EvalPlanQual rechecks */
	routine->RecheckForeignScan = clickhouseRecheckForeignScan;

	/* Support functions for EXPLAIN */
	routine->ExplainForeignScan = clickhouseExplainForeignScan;

	/* Support functions for ANALYZE */
	routine->AnalyzeForeignTable = clickhouseAnalyzeForeignTable;

	/* Support functions for join push-down */
	routine->GetForeignJoinPaths = clickhouseGetForeignJoinPaths;

	/* Support functions for upper relation push-down */
	routine->GetForeignUpperPaths = clickhouseGetForeignUpperPaths;

	/* IMPORT FOREIGN SCHEMA */
	routine->ImportForeignSchema = clickhouseImportForeignSchema;

	PG_RETURN_POINTER(routine);
}
