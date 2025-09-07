/*-------------------------------------------------------------------------
 *
 * geodesk_fdw.c
 *      Foreign Data Wrapper for GeoDesk GOL files
 *
 * Copyright (c) 2024, GeoDesk Contributors
 *
 * IDENTIFICATION
 *      src/geodesk_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "geodesk_fdw.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/bitmapset.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/jsonb.h"
#include "lib/stringinfo.h"

/* PostGIS includes for geometry handling - required */
#include "liblwgeom.h"
#include "liblwgeom_internal.h"  /* For gserialized_from_lwgeom */
#include "lwgeom_pg.h"
/* #include "lwgeom_geos.h" -- Not needed, using direct LWGEOM builder */

PG_MODULE_MAGIC;

/* Module initialization */
void _PG_init(void);
void _PG_fini(void);

/*
 * Module load callback
 */
void
_PG_init(void)
{
    /* Install PostGIS handlers - safe because we require PostGIS extension */
    pg_install_lwgeom_handlers();
    
    /* Note: We don't need to initialize GEOS since we're building LWGEOM directly */
    
    elog(DEBUG1, "GeoDesk FDW loaded with PostGIS support");
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    /* Nothing to clean up currently */
}

/* SQL-callable functions */
PG_FUNCTION_INFO_V1(geodesk_fdw_handler);
PG_FUNCTION_INFO_V1(geodesk_fdw_validator);
PG_FUNCTION_INFO_V1(geodesk_fdw_version);
PG_FUNCTION_INFO_V1(geodesk_fdw_drivers);

/* Forward declarations */
static bool extract_bbox_from_expr(Expr *expr, GeodeskFdwRelationInfo *fpinfo);
/* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
 * See WIP_fid_pushdown_limitations.md for details
 * static bool extract_fid_from_expr(Expr *expr, GeodeskFdwRelationInfo *fpinfo);
 */

/* FDW callback functions */
static void geodeskGetForeignRelSize(PlannerInfo *root,
                                     RelOptInfo *baserel,
                                     Oid foreigntableid);
static void geodeskGetForeignPaths(PlannerInfo *root,
                                   RelOptInfo *baserel,
                                   Oid foreigntableid);
static ForeignScan *geodeskGetForeignPlan(PlannerInfo *root,
                                          RelOptInfo *baserel,
                                          Oid foreigntableid,
                                          ForeignPath *best_path,
                                          List *tlist,
                                          List *scan_clauses,
                                          Plan *outer_plan);
static void geodeskBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *geodeskIterateForeignScan(ForeignScanState *node);
static void geodeskReScanForeignScan(ForeignScanState *node);
static void geodeskEndForeignScan(ForeignScanState *node);
static void geodeskExplainForeignScan(ForeignScanState *node,
                                      ExplainState *es);
static bool geodeskAnalyzeForeignTable(Relation relation,
                                       AcquireSampleRowsFunc *func,
                                       BlockNumber *totalpages);

/*
 * FDW handler function
 */
Datum
geodesk_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine *fdwroutine = makeNode(FdwRoutine);

    /* Required functions */
    fdwroutine->GetForeignRelSize = geodeskGetForeignRelSize;
    fdwroutine->GetForeignPaths = geodeskGetForeignPaths;
    fdwroutine->GetForeignPlan = geodeskGetForeignPlan;
    fdwroutine->BeginForeignScan = geodeskBeginForeignScan;
    fdwroutine->IterateForeignScan = geodeskIterateForeignScan;
    fdwroutine->ReScanForeignScan = geodeskReScanForeignScan;
    fdwroutine->EndForeignScan = geodeskEndForeignScan;

    /* Optional functions */
    fdwroutine->ExplainForeignScan = geodeskExplainForeignScan;
    fdwroutine->AnalyzeForeignTable = geodeskAnalyzeForeignTable;

    /* TODO: Add write support in future phases */
    /* fdwroutine->AddForeignUpdateTargets = geodeskAddForeignUpdateTargets; */
    /* fdwroutine->PlanForeignModify = geodeskPlanForeignModify; */
    /* fdwroutine->BeginForeignModify = geodeskBeginForeignModify; */
    /* fdwroutine->ExecForeignInsert = geodeskExecForeignInsert; */
    /* fdwroutine->ExecForeignUpdate = geodeskExecForeignUpdate; */
    /* fdwroutine->ExecForeignDelete = geodeskExecForeignDelete; */
    /* fdwroutine->EndForeignModify = geodeskEndForeignModify; */

    PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validator function
 */
Datum
geodesk_fdw_validator(PG_FUNCTION_ARGS)
{
    List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid catalog = PG_GETARG_OID(1);
    ListCell *cell;

    foreach(cell, options_list)
    {
        DefElem *def = (DefElem *) lfirst(cell);

        if (!geodesk_is_valid_option(def->defname, catalog))
        {
            struct GeodeskFdwOption
            {
                const char *optname;
                Oid optcontext;
            };

            struct GeodeskFdwOption valid_options[] =
            {
                /* Server options */
                {OPTION_DATASOURCE, ForeignServerRelationId},
                {OPTION_UPDATABLE, ForeignServerRelationId},
                
                /* Table options */
                {OPTION_LAYER, ForeignTableRelationId},
                {OPTION_SCHEMA_MODE, ForeignTableRelationId},
                {OPTION_GOQL_FILTER, ForeignTableRelationId},
                
                {NULL, InvalidOid}
            };

            StringInfoData buf;
            initStringInfo(&buf);
            
            appendStringInfo(&buf, "Valid options for %s are: ",
                           catalog == ForeignServerRelationId ? "server" : "foreign table");
            
            for (int i = 0; valid_options[i].optname; i++)
            {
                if (valid_options[i].optcontext == catalog)
                {
                    if (buf.len > 0)
                        appendStringInfo(&buf, ", ");
                    appendStringInfo(&buf, "%s", valid_options[i].optname);
                }
            }

            ereport(ERROR,
                    (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                     errmsg("invalid option \"%s\"", def->defname),
                     errhint("%s", buf.data)));
        }
    }

    PG_RETURN_VOID();
}

/*
 * Get table size estimates
 */
static void
geodeskGetForeignRelSize(PlannerInfo *root,
                         RelOptInfo *baserel,
                         Oid foreigntableid)
{
    GeodeskFdwRelationInfo *fpinfo;
    ListCell *lc;

    /* Allocate and initialize relation info */
    fpinfo = (GeodeskFdwRelationInfo *) palloc0(sizeof(GeodeskFdwRelationInfo));
    baserel->fdw_private = fpinfo;

    /* Get table options */
    geodesk_get_options(foreigntableid, fpinfo);

    /* Analyze WHERE clauses for pushdown possibilities */
    fpinfo->pushdown_clauses = NIL;
    fpinfo->type_prefix = NULL;
    /* FID pushdown disabled - libgeodesk doesn't support direct ID lookup */
    /* fpinfo->has_id_filter = false; */
    
    /* First extract type filter to get GOQL prefix */
    fpinfo->type_prefix = extract_type_filter_prefix(baserel->baserestrictinfo, &fpinfo->pushdown_clauses);
    if (!fpinfo->type_prefix)
        fpinfo->type_prefix = "*";  /* Default to all types */
    
    foreach(lc, baserel->baserestrictinfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
        Expr *expr = rinfo->clause;
        
        /* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
         * See WIP_fid_pushdown_limitations.md for details
         * 
         * if (extract_fid_from_expr(expr, fpinfo))
         * {
         *     fpinfo->pushdown_clauses = lappend(fpinfo->pushdown_clauses, rinfo);
         *     ereport(DEBUG1,
         *             (errcode(ERRCODE_FDW_ERROR),
         *              errmsg("Found pushable FID filter: fid=%lld", (long long)fpinfo->filter_id)));
         * }
         */
        
        /* Check if this is a spatial filter we can push down */
        if (extract_bbox_from_expr(expr, fpinfo))
        {
            /* Mark this clause as pushed down */
            fpinfo->pushdown_clauses = lappend(fpinfo->pushdown_clauses, rinfo);
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Found pushable spatial filter in planning phase")));
        }
        /* Check for tag filters we can convert to GOQL */
        else
        {
            /* Try to extract tag filters for this single clause */
            List *single_clause = list_make1(rinfo);
            char *goql = extract_goql_from_clauses(single_clause, NULL);
            if (goql)
            {
                /* If we already have a GOQL filter, combine them */
                if (fpinfo->goql_filter)
                {
                    StringInfoData combined;
                    initStringInfo(&combined);
                    appendStringInfo(&combined, "%s%s", fpinfo->goql_filter, goql);
                    fpinfo->goql_filter = combined.data;
                }
                else
                {
                    fpinfo->goql_filter = goql;
                }
                
                /* Mark this clause as pushed down */
                fpinfo->pushdown_clauses = lappend(fpinfo->pushdown_clauses, rinfo);
                
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Found pushable tag filter: %s", goql)));
            }
        }
    }

    /* Identify which columns are actually needed in the query */
    fpinfo->attrs_used = NULL;
    
    /* Extract columns from target list */
    pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
                   &fpinfo->attrs_used);
    
    /* Also extract columns used in local conditions (WHERE clauses we can't push down) */
    foreach(lc, baserel->baserestrictinfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
        
        /* Only consider clauses that won't be pushed down */
        if (!list_member(fpinfo->pushdown_clauses, rinfo))
        {
            pull_varattnos((Node *) rinfo->clause, baserel->relid,
                           &fpinfo->attrs_used);
        }
    }
    
    /* Debug output to see which columns are needed */
    if (fpinfo->attrs_used)
    {
        int col = -1;
        while ((col = bms_next_member(fpinfo->attrs_used, col)) >= 0)
        {
            /* Adjust for system columns */
            int attnum = col + FirstLowInvalidHeapAttributeNumber;
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Column %d is referenced in query", attnum)));
        }
    }
    else
    {
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("No specific columns referenced (COUNT(*) case?)")));
    }
    
    /* Estimate rows based on filters
     * 
     * Start with a base estimate and apply selectivity factors for each filter type
     */
    double base_rows = 100000;  /* Default estimate for unfiltered data */
    double selectivity = 1.0;
    
    /* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
     * if (fpinfo->has_id_filter)
     * {
     *     baserel->rows = 1;
     *     baserel->tuples = 1;
     * }
     * else 
     */
    
    /* Apply selectivity for spatial filter */
    if (fpinfo->has_spatial_filter)
    {
        /* Spatial filter typically reduces to 1-10% of data depending on bbox size */
        selectivity *= 0.05;  /* 5% selectivity for spatial filter */
    }
    
    /* Apply selectivity for GOQL tag filter */
    if (fpinfo->goql_filter && strlen(fpinfo->goql_filter) > 0)
    {
        /* Tag filters are highly selective
         * Common tags like building=yes might match 10-20% of features
         * Specific tags like amenity=restaurant might match <1%
         */
        if (strstr(fpinfo->goql_filter, "building"))
            selectivity *= 0.15;  /* Buildings are common */
        else if (strstr(fpinfo->goql_filter, "highway"))
            selectivity *= 0.20;  /* Highways are very common */
        else if (strstr(fpinfo->goql_filter, "amenity"))
            selectivity *= 0.01;  /* Amenities are rare */
        else
            selectivity *= 0.05;  /* Default tag selectivity */
    }
    
    /* Apply selectivity for type filter */
    if (fpinfo->type_prefix && strlen(fpinfo->type_prefix) > 0)
    {
        /* Type distribution approximation:
         * Nodes: ~25%, Ways: ~70%, Relations: ~5%
         */
        if (strcmp(fpinfo->type_prefix, "n") == 0)
            selectivity *= 0.25;
        else if (strcmp(fpinfo->type_prefix, "wa") == 0 || strcmp(fpinfo->type_prefix, "w") == 0)
            selectivity *= 0.70;
        else if (strcmp(fpinfo->type_prefix, "r") == 0)
            selectivity *= 0.05;
        else if (strcmp(fpinfo->type_prefix, "nwa") == 0)
            selectivity *= 0.95;  /* Nodes + ways */
        else if (strcmp(fpinfo->type_prefix, "nr") == 0)
            selectivity *= 0.30;  /* Nodes + relations */
        else if (strcmp(fpinfo->type_prefix, "war") == 0)
            selectivity *= 0.75;  /* Ways + relations */
        /* "*" means all types, no additional selectivity */
    }
    
    /* Calculate final row estimate */
    baserel->rows = base_rows * selectivity;
    
    /* Ensure minimum of 1 row */
    if (baserel->rows < 1)
        baserel->rows = 1;
    
    /* Cap at maximum reasonable value */
    if (baserel->rows > 1000000)
        baserel->rows = 1000000;
    
    baserel->tuples = baserel->rows;
    baserel->pages = baserel->rows / 100;  /* Rough estimate of pages */
    if (baserel->pages < 1)
        baserel->pages = 1;
}

/*
 * Create possible access paths
 */
static void
geodeskGetForeignPaths(PlannerInfo *root,
                      RelOptInfo *baserel,
                      Oid foreigntableid)
{
    Cost startup_cost;
    Cost total_cost;
    GeodeskFdwRelationInfo *fpinfo = (GeodeskFdwRelationInfo *)baserel->fdw_private;
    
    /* Calculate costs based on filters */
    startup_cost = 100;  /* Base cost for opening GOL file */
    
    /* Add cost for applying filters */
    if (fpinfo->has_spatial_filter)
        startup_cost += 10;  /* Spatial index lookup cost */
    
    if (fpinfo->goql_filter && strlen(fpinfo->goql_filter) > 0)
        startup_cost += 20;  /* GOQL filter compilation/setup cost */
    
    if (fpinfo->type_prefix && strlen(fpinfo->type_prefix) > 0)
        startup_cost += 5;   /* Type filter setup cost */
    
    /* Per-row processing cost */
    double cpu_per_tuple = 0.01;  /* Base CPU cost per tuple */
    
    /* Geometry construction is expensive */
    cpu_per_tuple += 0.05;  /* Cost of building LWGEOM */
    
    /* Tag processing cost */
    cpu_per_tuple += 0.02;  /* Cost of JSON tag extraction */
    
    total_cost = startup_cost + baserel->rows * cpu_per_tuple;

    /* Create a ForeignPath */
    add_path(baserel, (Path *)
             create_foreignscan_path(root, baserel,
                                    NULL,    /* default pathtarget */
                                    baserel->rows,
                                    startup_cost,
                                    total_cost,
                                    NIL,     /* no pathkeys */
                                    baserel->lateral_relids,
                                    NULL,    /* no extra plan */
                                    NIL,     /* no private data */
                                    NIL));   /* no fdw_restrictinfo */
}

/*
 * Create a ForeignScan plan
 */
static ForeignScan *
geodeskGetForeignPlan(PlannerInfo *root,
                      RelOptInfo *baserel,
                      Oid foreigntableid,
                      ForeignPath *best_path,
                      List *tlist,
                      List *scan_clauses,
                      Plan *outer_plan)
{
    GeodeskFdwRelationInfo *fpinfo = (GeodeskFdwRelationInfo *) baserel->fdw_private;
    List *fdw_private;
    List *local_exprs = NIL;
    List *remote_exprs = NIL;
    List *params_list = NIL;
    List *retrieved_attrs;
    ListCell *lc;

    /* Separate pushed-down clauses from local evaluation */
    foreach(lc, scan_clauses)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        
        /* Check if this clause was marked for pushdown */
        if (list_member(fpinfo->pushdown_clauses, rinfo))
        {
            /* This clause will be evaluated remotely */
            remote_exprs = lappend(remote_exprs, rinfo->clause);
        }
        else
        {
            /* This clause needs local evaluation */
            local_exprs = lappend(local_exprs, rinfo->clause);
        }
    }

    /* Build the list of attributes to retrieve based on attrs_used */
    retrieved_attrs = NIL;
    
    if (fpinfo->attrs_used)
    {
        /* Convert Bitmapset to list of attribute numbers */
        int col = -1;
        while ((col = bms_next_member(fpinfo->attrs_used, col)) >= 0)
        {
            /* Adjust for system columns and convert to 1-based attribute number */
            int attnum = col + FirstLowInvalidHeapAttributeNumber;
            
            /* Only add user columns (attnum > 0) */
            if (attnum > 0)
            {
                retrieved_attrs = lappend_int(retrieved_attrs, attnum);
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Adding column %d from attrs_used", attnum)));
            }
        }
    }
    
    /* If no columns identified (e.g., COUNT(*)), we still need to return something */
    /* Just return the first column (usually fid) to have valid tuples */
    if (retrieved_attrs == NIL)
    {
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("No columns needed (COUNT(*)?), returning just column 1 for valid tuples")));
        retrieved_attrs = lappend_int(retrieved_attrs, 1);  /* Just fid */
    }

    /* Build FDW private list - include pushdown info */
    fdw_private = list_make3(retrieved_attrs,
                            makeString(fpinfo->datasource ? fpinfo->datasource : ""),
                            fpinfo);  /* Pass the entire fpinfo for use in BeginForeignScan */

    return make_foreignscan(tlist,
                           local_exprs,  /* Only non-pushed clauses */
                           baserel->relid,
                           params_list,
                           fdw_private,
                           NIL,  /* no custom tlist */
                           remote_exprs,  /* Pushed-down clauses for EXPLAIN */
                           outer_plan);
}

/*
 * Helper function to check if an expression contains a bbox operator (&&)
 * and extract the bounding box if possible
 */
static bool
extract_bbox_from_expr(Expr *expr, GeodeskFdwRelationInfo *fpinfo)
{
    if (!expr || !fpinfo)
        return false;
    
    /* Check if this is an OpExpr (operator expression) */
    if (IsA(expr, OpExpr))
    {
        OpExpr *op = (OpExpr *) expr;
        Oid opno = op->opno;
        char *opname = get_opname(opno);
        
        /* Check for && operator (bbox overlap) */
        if (opname && strcmp(opname, "&&") == 0)
        {
            /* Get the arguments */
            List *args = op->args;
            if (list_length(args) == 2)
            {
                /* Node *arg1 = (Node *) linitial(args); -- column reference */
                Node *arg2 = (Node *) lsecond(args);
                
                /* Try to extract bbox from constant geometry */
                /* This is simplified - full implementation would handle more cases */
                if (IsA(arg2, Const))
                {
                    Const *c = (Const *) arg2;
                    if (!c->constisnull)
                    {
                        /* Get geometry and extract bounds */
                        GSERIALIZED *geom = (GSERIALIZED *) DatumGetPointer(c->constvalue);
                        if (geom)
                        {
                            LWGEOM *lwgeom = lwgeom_from_gserialized(geom);
                            if (lwgeom)
                            {
                                GBOX gbox;
                                if (lwgeom_calculate_gbox(lwgeom, &gbox) == LW_SUCCESS)
                                {
                                    fpinfo->has_spatial_filter = true;
                                    fpinfo->bbox_min_x = gbox.xmin;
                                    fpinfo->bbox_min_y = gbox.ymin;
                                    fpinfo->bbox_max_x = gbox.xmax;
                                    fpinfo->bbox_max_y = gbox.ymax;
                                    
                                    ereport(INFO,
                                            (errcode(ERRCODE_FDW_ERROR),
                                             errmsg("Extracted bbox: [%.2f,%.2f,%.2f,%.2f]",
                                                    gbox.xmin, gbox.ymin, gbox.xmax, gbox.ymax)));
                                    
                                    lwgeom_free(lwgeom);
                                    return true;
                                }
                                lwgeom_free(lwgeom);
                            }
                        }
                    }
                }
            }
        }
    }
    
    return false;
}

/* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
 * See WIP_fid_pushdown_limitations.md for details
 *
 * Helper function to check if an expression contains an FID equality condition
 * and extract the ID value if possible
 *
 * static bool
 * extract_fid_from_expr(Expr *expr, GeodeskFdwRelationInfo *fpinfo)
 * {
 *     if (!expr || !fpinfo)
 *         return false;
 *     
 *     ereport(DEBUG1,
 *             (errcode(ERRCODE_FDW_ERROR),
 *              errmsg("extract_fid_from_expr: checking expression type %d", nodeTag(expr))));
 *     
 *     // Check if this is an OpExpr (operator expression)
 *     if (IsA(expr, OpExpr))
 *     {
 *         OpExpr *op = (OpExpr *) expr;
 *         Oid opno = op->opno;
 *         char *opname = get_opname(opno);
 *         
 *         // Check for = operator (equality)
 *         if (opname && strcmp(opname, "=") == 0)
 *         {
 *             // Get the arguments
 *             List *args = op->args;
 *             if (list_length(args) == 2)
 *             {
 *                 Node *arg1 = (Node *) linitial(args);
 *                 Node *arg2 = (Node *) lsecond(args);
 *                 
 *                 // Check if arg1 is a Var referring to the 'fid' column
 *                 if (IsA(arg1, Var))
 *                 {
 *                     Var *var = (Var *) arg1;
 *                     // Get column name - fid is typically the first column (attnum 1)
 *                     // TODO: More robust column name checking
 *                     if (var->varattno == 1)  // Assuming fid is first column
 *                     {
 *                         // Check if arg2 is a constant
 *                         if (IsA(arg2, Const))
 *                         {
 *                             Const *c = (Const *) arg2;
 *                             if (!c->constisnull && c->consttype == INT8OID)
 *                             {
 *                                 fpinfo->has_id_filter = true;
 *                                 fpinfo->filter_id = DatumGetInt64(c->constvalue);
 *                                 
 *                                 ereport(INFO,
 *                                         (errcode(ERRCODE_FDW_ERROR),
 *                                          errmsg("Extracted FID filter: fid=%lld", 
 *                                                 (long long)fpinfo->filter_id)));
 *                                 
 *                                 return true;
 *                             }
 *                         }
 *                     }
 *                 }
 *                 // Also check if args are reversed (constant = column)
 *                 else if (IsA(arg2, Var) && IsA(arg1, Const))
 *                 {
 *                     Var *var = (Var *) arg2;
 *                     if (var->varattno == 1)  // Assuming fid is first column
 *                     {
 *                         Const *c = (Const *) arg1;
 *                         if (!c->constisnull && c->consttype == INT8OID)
 *                         {
 *                             fpinfo->has_id_filter = true;
 *                             fpinfo->filter_id = DatumGetInt64(c->constvalue);
 *                             
 *                             ereport(INFO,
 *                                     (errcode(ERRCODE_FDW_ERROR),
 *                                      errmsg("Extracted FID filter: fid=%lld", 
 *                                             (long long)fpinfo->filter_id)));
 *                             
 *                             return true;
 *                         }
 *                     }
 *                 }
 *             }
 *         }
 *     }
 *     
 *     return false;
 * }
 */

/*
 * Begin foreign scan
 */
static void
geodeskBeginForeignScan(ForeignScanState *node, int eflags)
{
    GeodeskExecState *festate;
    ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
    GeodeskFdwRelationInfo fpinfo;

    /* Do nothing in EXPLAIN (no ANALYZE) case */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    /* Allocate state structure */
    festate = (GeodeskExecState *) palloc0(sizeof(GeodeskExecState));
    node->fdw_state = festate;

    /* Get info from plan */
    festate->retrieved_attrs = (List *) linitial(fsplan->fdw_private);
    
    /* Check if geometry or bbox columns are needed */
    festate->needs_geometry = false;
    festate->needs_bbox = false;
    if (festate->retrieved_attrs)
    {
        ListCell *lc;
        TupleDesc tupdesc = RelationGetDescr(node->ss.ss_currentRelation);
        
        foreach(lc, festate->retrieved_attrs)
        {
            int attnum = lfirst_int(lc);
            Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
            char *attname = NameStr(attr->attname);
            
            if (strcmp(attname, "geom") == 0 || strcmp(attname, "way") == 0)
            {
                festate->needs_geometry = true;
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Geometry column requested - will build geometry")));
            }
            else if (strcmp(attname, "bbox") == 0)
            {
                festate->needs_bbox = true;
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Bbox column requested - will extract bounds")));
            }
        }
    }
    
    if (!festate->needs_geometry)
    {
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Geometry column NOT requested - using lazy geometry optimization")));
    }
    
    /* Get pushdown info from planning phase */
    if (list_length(fsplan->fdw_private) >= 3)
    {
        /* Use the fpinfo passed from planning phase */
        GeodeskFdwRelationInfo *plan_fpinfo = (GeodeskFdwRelationInfo *) lthird(fsplan->fdw_private);
        memcpy(&fpinfo, plan_fpinfo, sizeof(GeodeskFdwRelationInfo));
        
        if (fpinfo.has_spatial_filter)
        {
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Using spatial filter from planning phase: [%.2f,%.2f,%.2f,%.2f]",
                            fpinfo.bbox_min_x, fpinfo.bbox_min_y,
                            fpinfo.bbox_max_x, fpinfo.bbox_max_y)));
        }
    }
    else
    {
        /* Fallback: get table options if fpinfo wasn't passed */
        geodesk_get_options(RelationGetRelid(node->ss.ss_currentRelation), &fpinfo);
    }

    /* Open connection to GOL file */
    if (fpinfo.datasource)
    {
        festate->connection = geodesk_open(fpinfo.datasource, fpinfo.query);
        if (!festate->connection)
            ereport(ERROR,
                    (errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
                     errmsg("failed to open GOL file \"%s\"", fpinfo.datasource)));
        
        /* Apply filters if any */
        if (fpinfo.has_spatial_filter)
        {
            geodesk_set_spatial_filter(festate->connection,
                                      fpinfo.bbox_min_x, fpinfo.bbox_min_y,
                                      fpinfo.bbox_max_x, fpinfo.bbox_max_y);
        }
        
        /* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
         * if (fpinfo.has_id_filter)
         * {
         *     geodesk_set_id_filter(festate->connection, fpinfo.filter_id);
         * }
         */
        
        if (fpinfo.goql_filter || fpinfo.type_prefix)
        {
            geodesk_set_goql_filter_with_prefix(festate->connection, 
                                               fpinfo.goql_filter, 
                                               fpinfo.type_prefix ? fpinfo.type_prefix : "*");
        }
        
        /* Start iteration */
        geodesk_reset_iteration(festate->connection);
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                 errmsg("datasource and layer options are required")));
    }
}

/*
 * Fetch next row
 */
static TupleTableSlot *
geodeskIterateForeignScan(ForeignScanState *node)
{
    GeodeskExecState *festate = (GeodeskExecState *) node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    bool found;

    /* Clear slot */
    ExecClearTuple(slot);

    /* Get next feature */
    found = geodesk_get_next_feature(festate->connection, &festate->current_feature);
    
    if (found)
    {
        /* Build the tuple */
        Datum *values = slot->tts_values;
        bool *nulls = slot->tts_isnull;
        ListCell *lc;

        memset(values, 0, sizeof(Datum) * slot->tts_tupleDescriptor->natts);
        memset(nulls, true, sizeof(bool) * slot->tts_tupleDescriptor->natts);

        /* Fill in the values */
        foreach(lc, festate->retrieved_attrs)
        {
            int attnum = lfirst_int(lc);
            Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1);
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Processing column %d: %s", attnum, NameStr(attr->attname))));
            
            /* Handle different column types */
            if (strcmp(NameStr(attr->attname), "fid") == 0)
            {
                values[attnum - 1] = Int64GetDatum(festate->current_feature.id);
                nulls[attnum - 1] = false;
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Set fid = %lld", (long long)festate->current_feature.id)));
            }
            else if (strcmp(NameStr(attr->attname), "tags") == 0)
            {
                /* Get tags as JSON */
                char* json_str = geodesk_get_tags_json(festate->connection, &festate->current_feature);
                if (json_str)
                {
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Tags JSON: %s", json_str)));
                    
                    /* Convert JSON C string directly to JSONB using jsonb_in */
                    values[attnum - 1] = DirectFunctionCall1(jsonb_in, 
                                                            CStringGetDatum(json_str));
                    nulls[attnum - 1] = false;
                    free(json_str);
                }
                else
                {
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("No tags JSON returned")));
                    nulls[attnum - 1] = true;
                }
            }
            else if (strcmp(NameStr(attr->attname), "type") == 0)
            {
                /* Feature type: 0=node, 1=way, 2=relation */
                values[attnum - 1] = Int32GetDatum(festate->current_feature.type);
                nulls[attnum - 1] = false;
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Set type = %d", festate->current_feature.type)));
            }
            else if (strcmp(NameStr(attr->attname), "is_area") == 0)
            {
                /* Is this way an area (polygon)? */
                values[attnum - 1] = BoolGetDatum(festate->current_feature.is_area);
                nulls[attnum - 1] = false;
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Set is_area = %s", festate->current_feature.is_area ? "true" : "false")));
            }
            else if (strcmp(NameStr(attr->attname), "geom") == 0 ||
                     strcmp(NameStr(attr->attname), "way") == 0)
            {
                /* Check if geometry is actually needed */
                if (festate->needs_geometry)
                {
                    /* Build LWGEOM directly from libgeodesk feature */
                    LWGEOM* lwgeom = geodesk_build_lwgeom(festate->connection, &festate->current_feature);
                    
                    if (lwgeom)
                    {
                        /* Serialize LWGEOM to GSERIALIZED for PostGIS */
                        size_t size;
                        GSERIALIZED* geom_serialized = gserialized_from_lwgeom(lwgeom, &size);
                        
                        if (geom_serialized)
                        {
                            values[attnum - 1] = PointerGetDatum(geom_serialized);
                            nulls[attnum - 1] = false;
                            ereport(DEBUG1,
                                    (errcode(ERRCODE_FDW_ERROR),
                                     errmsg("Geometry set: size = %zu bytes", size)));
                        }
                        else
                        {
                            nulls[attnum - 1] = true;
                            ereport(DEBUG1,
                                    (errcode(ERRCODE_FDW_ERROR),
                                     errmsg("Failed to serialize LWGEOM")));
                        }
                        
                        /* Clean up LWGEOM */
                        lwgeom_free(lwgeom);
                    }
                    else
                    {
                        nulls[attnum - 1] = true;
                        ereport(DEBUG1,
                                (errcode(ERRCODE_FDW_ERROR),
                                 errmsg("Failed to build LWGEOM from feature")));
                    }
                }
                else
                {
                    /* Geometry not needed - set NULL to save processing time */
                    nulls[attnum - 1] = true;
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Skipping geometry construction (lazy optimization)")));
                }
            }
            else
            {
                /* Unknown column - return NULL */
                nulls[attnum - 1] = true;
            }
        }

        ExecStoreVirtualTuple(slot);
        festate->rows_fetched++;

        /* Clean up feature resources */
        geodesk_feature_cleanup(&festate->current_feature);
        
        return slot;
    }
    
    /* No more rows - return NULL to signal end of scan */
    return NULL;
}

/*
 * Rescan foreign table
 */
static void
geodeskReScanForeignScan(ForeignScanState *node)
{
    GeodeskExecState *festate = (GeodeskExecState *) node->fdw_state;

    if (festate->connection)
        geodesk_reset_iteration(festate->connection);
}

/*
 * End foreign scan
 */
static void
geodeskEndForeignScan(ForeignScanState *node)
{
    GeodeskExecState *festate = (GeodeskExecState *) node->fdw_state;

    if (festate && festate->connection)
    {
        geodesk_close(festate->connection);
        festate->connection = NULL;
    }
}

/*
 * Explain foreign scan
 */
static void
geodeskExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
    GeodeskExecState *festate = (GeodeskExecState *) node->fdw_state;
    
    if (es->verbose)
    {
        if (festate)
            ExplainPropertyInteger("Rows Fetched", NULL, festate->rows_fetched, es);
    }
}

/*
 * Analyze foreign table
 */
static bool
geodeskAnalyzeForeignTable(Relation relation,
                           AcquireSampleRowsFunc *func,
                           BlockNumber *totalpages)
{
    /* TODO: Implement sampling */
    *func = NULL;
    *totalpages = 1;
    return false;
}

/*
 * Version function
 */
Datum
geodesk_fdw_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("1.0"));
}

/*
 * List available drivers (for compatibility)
 */
Datum
geodesk_fdw_drivers(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;
    Datum values[1];
    bool nulls[1];

    /* check to see if caller supports us returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));

    /* Build tupdesc for result tuples */
    tupdesc = CreateTemplateTupleDesc(1);
    TupleDescInitEntry(tupdesc, (AttrNumber) 1, "driver",
                      TEXTOID, -1, 0);

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    MemoryContextSwitchTo(oldcontext);

    /* Add single driver entry */
    values[0] = CStringGetTextDatum("GeoDesk");
    nulls[0] = false;
    
    tuplestore_putvalues(tupstore, tupdesc, values, nulls);

    /* Mark end of tuples */
    /* tuplestore_donestoring(tupstore); -- removed, not available in newer PG */

    PG_RETURN_NULL();
}
