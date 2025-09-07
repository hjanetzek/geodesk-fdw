/*
 * type_filter.c
 *
 * Extract type filters from WHERE clauses and convert to GOQL prefixes
 */

#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "geodesk_fdw.h"

/*
 * Check if an expression is a type column comparison
 */
static bool
is_type_column(Expr *expr, int *type_value)
{
    Var *var;
    Const *c;
    
    if (!IsA(expr, Var))
        return false;
    
    var = (Var *)expr;
    
    /* Check if this is the 'type' column (assuming it follows fid and tags) */
    /* TODO: More robust column name checking */
    if (var->varattno == 2)  /* Assuming type is second column after fid */
    {
        return true;
    }
    
    return false;
}

/*
 * Extract type filter from equality expression
 * Returns GOQL prefix or NULL if not a type filter
 */
static char *
extract_type_equality(Expr *expr)
{
    OpExpr *op;
    List *args;
    Expr *left, *right;
    Var *var = NULL;
    Const *value_const;
    int type_value;
    
    if (!IsA(expr, OpExpr))
        return NULL;
    
    op = (OpExpr *)expr;
    
    /* Check if this is an equality operator */
    {
        char *opname = get_opname(op->opno);
        if (opname == NULL || strcmp(opname, "=") != 0)
            return NULL;
    }
    
    args = op->args;
    if (list_length(args) != 2)
        return NULL;
    
    left = (Expr *)linitial(args);
    right = (Expr *)lsecond(args);
    
    /* Check if left side is type column */
    if (IsA(left, Var))
    {
        var = (Var *)left;
        /* Check if this is column 2 (type column) */
        if (var->varattno != 2)
        {
            /* Maybe it's on the right side */
            if (IsA(right, Var))
            {
                var = (Var *)right;
                if (var->varattno != 2)
                    return NULL;
                /* Swap so value is on the right */
                Expr *temp = left;
                left = right;
                right = temp;
            }
            else
            {
                return NULL;
            }
        }
    }
    else
    {
        return NULL;
    }
    
    /* Right side should be a constant */
    if (!IsA(right, Const))
        return NULL;
    
    value_const = (Const *)right;
    if (value_const->consttype != INT4OID || value_const->constisnull)
        return NULL;
    
    type_value = DatumGetInt32(value_const->constvalue);
    
    /* Convert type value to GOQL prefix
     * Note: For ways (type=1), we use "wa" to include both linear ways
     * and area ways (closed ways with area semantics like buildings).
     * This ensures we don't miss features like buildings which are areas.
     */
    switch (type_value)
    {
        case 0:  /* Node */
            return pstrdup("n");
        case 1:  /* Way - includes both linear ways and areas */
            return pstrdup("wa");
        case 2:  /* Relation */
            return pstrdup("r");
        default:
            return NULL;
    }
}

/*
 * Extract type filter from IN expression
 * Returns GOQL prefix or NULL if not a type filter
 */
static char *
extract_type_in_list(Expr *expr)
{
    ScalarArrayOpExpr *saop;
    Expr *left, *right;
    Var *var = NULL;
    ArrayExpr *arr;
    ListCell *lc;
    bool has_nodes = false;
    bool has_ways = false;
    bool has_relations = false;
    
    if (!IsA(expr, ScalarArrayOpExpr))
        return NULL;
    
    saop = (ScalarArrayOpExpr *)expr;
    
    /* IN uses the = operator with useOr=true */
    if (!saop->useOr)
        return NULL;
    
    if (list_length(saop->args) != 2)
        return NULL;
    
    left = (Expr *)linitial(saop->args);
    right = (Expr *)lsecond(saop->args);
    
    /* Check if left side is type column */
    if (!IsA(left, Var))
        return NULL;
    
    var = (Var *)left;
    if (var->varattno != 2)  /* type column */
        return NULL;
    
    /* Right side should be an array */
    if (!IsA(right, ArrayExpr))
        return NULL;
    
    arr = (ArrayExpr *)right;
    
    /* Check which types are in the list */
    foreach(lc, arr->elements)
    {
        Const *elem = (Const *)lfirst(lc);
        
        if (!IsA(elem, Const) || elem->consttype != INT4OID || elem->constisnull)
            continue;
        
        int type_value = DatumGetInt32(elem->constvalue);
        switch (type_value)
        {
            case 0:
                has_nodes = true;
                break;
            case 1:
                has_ways = true;
                break;
            case 2:
                has_relations = true;
                break;
        }
    }
    
    /* Build appropriate GOQL prefix
     * Note: For ways, we use "wa" to include both linear ways and areas
     */
    if (has_nodes && has_ways && has_relations)
        return pstrdup("*");  /* All types */
    else if (has_nodes && has_ways)
        return pstrdup("nwa");  /* Nodes + ways (including areas) */
    else if (has_nodes && has_relations)
        return pstrdup("nr");
    else if (has_ways && has_relations)
        return pstrdup("war");  /* Ways (including areas) + relations */
    else if (has_nodes)
        return pstrdup("n");
    else if (has_ways)
        return pstrdup("wa");  /* Include both linear ways and areas */
    else if (has_relations)
        return pstrdup("r");
    else
        return NULL;
}

/*
 * Extract type filter from WHERE clauses and return GOQL prefix
 * Returns allocated string or NULL if no type filter found
 */
char *
extract_type_filter_prefix(List *clauses, List **pushed_clauses)
{
    ListCell *lc;
    char *prefix = NULL;
    
    foreach(lc, clauses)
    {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);
        Expr *expr = rinfo->clause;
        
        /* Try to extract type equality filter */
        prefix = extract_type_equality(expr);
        
        /* Try to extract type IN list filter */
        if (!prefix)
            prefix = extract_type_in_list(expr);
        
        if (prefix)
        {
            if (pushed_clauses)
                *pushed_clauses = lappend(*pushed_clauses, rinfo);
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Extracted type filter prefix: %s", prefix)));
            
            return prefix;  /* Return first type filter found */
        }
    }
    
    return NULL;  /* No type filter found, will use "*" default */
}