/*
 * goql_converter.c
 *
 * Convert PostgreSQL WHERE clauses to GOQL queries for tag filtering
 */

#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/jsonb.h"
#include "lib/stringinfo.h"

#include "geodesk_fdw.h"

/*
 * Check if an expression is a JSONB field access (tags->>'key')
 */
static bool
is_jsonb_field_access(Expr *expr, char **key_out, Var **var_out)
{
    OpExpr *op;
    List *args;
    Expr *arg1, *arg2;
    
    if (!IsA(expr, OpExpr))
        return false;
    
    op = (OpExpr *)expr;
    
    /* Check if this is the ->> operator (jsonb_get_text) */
    if (get_func_name(op->opfuncid) == NULL ||
        strcmp(get_func_name(op->opfuncid), "jsonb_object_field_text") != 0)
        return false;
    
    args = op->args;
    if (list_length(args) != 2)
        return false;
    
    arg1 = (Expr *)linitial(args);
    arg2 = (Expr *)lsecond(args);
    
    /* First argument should be a Var (the tags column) */
    if (!IsA(arg1, Var))
        return false;
    
    /* Second argument should be a Const (the key) */
    if (!IsA(arg2, Const))
        return false;
    
    {
        Const *key_const = (Const *)arg2;
        if (key_const->consttype != TEXTOID || key_const->constisnull)
            return false;
        
        /* Extract the key */
        if (key_out)
            *key_out = TextDatumGetCString(key_const->constvalue);
        
        if (var_out)
            *var_out = (Var *)arg1;
    }
    
    return true;
}

/*
 * Extract tag filter from equality expression
 * Returns allocated GOQL string or NULL if not a tag filter
 */
static char *
extract_tag_equality(Expr *expr)
{
    OpExpr *op;
    List *args;
    Expr *left, *right;
    char *key = NULL;
    Var *var = NULL;
    Const *value_const;
    char *value;
    StringInfoData goql;
    
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
    
    /* Check if left side is tags->>'key' */
    if (!is_jsonb_field_access(left, &key, &var))
    {
        Expr *temp;
        
        /* Maybe it's on the right side */
        if (!is_jsonb_field_access(right, &key, &var))
            return NULL;
        
        /* Swap so value is on the right */
        temp = left;
        left = right;
        right = temp;
    }
    
    /* Right side should be a constant */
    if (!IsA(right, Const))
        return NULL;
    
    value_const = (Const *)right;
    if (value_const->consttype != TEXTOID || value_const->constisnull)
        return NULL;
    
    value = TextDatumGetCString(value_const->constvalue);
    
    /* Build GOQL expression: [key=value] */
    initStringInfo(&goql);
    appendStringInfo(&goql, "[%s=%s]", key, value);
    
    return goql.data;
}

/*
 * Extract tag filter from IN expression
 * Returns allocated GOQL string or NULL if not a tag filter
 */
static char *
extract_tag_in_list(Expr *expr)
{
    ScalarArrayOpExpr *saop;
    Expr *left, *right;
    char *key = NULL;
    Var *var = NULL;
    ArrayExpr *arr;
    ListCell *lc;
    StringInfoData goql;
    bool first = true;
    
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
    
    /* Check if left side is tags->>'key' */
    if (!is_jsonb_field_access(left, &key, &var))
        return NULL;
    
    /* Right side should be an array */
    if (!IsA(right, ArrayExpr))
        return NULL;
    
    arr = (ArrayExpr *)right;
    
    /* Build GOQL expression: [key=value1,value2,...] */
    initStringInfo(&goql);
    appendStringInfo(&goql, "[%s=", key);
    
    foreach(lc, arr->elements)
    {
        Const *elem = (Const *)lfirst(lc);
        
        if (!IsA(elem, Const) || elem->consttype != TEXTOID || elem->constisnull)
            continue;
        
        if (!first)
            appendStringInfoChar(&goql, ',');
        
        appendStringInfoString(&goql, TextDatumGetCString(elem->constvalue));
        first = false;
    }
    
    appendStringInfoChar(&goql, ']');
    
    return goql.data;
}

/*
 * Extract tag existence check: tags ? 'key' (jsonb_exists operator)
 * Returns GOQL filter [key=*] or NULL if not a tag existence check
 */
static char *
extract_tag_exists(Expr *expr)
{
    OpExpr *op;
    List *args;
    Expr *left, *right;
    Const *key_const;
    char *key;
    StringInfoData goql;
    
    if (!IsA(expr, OpExpr))
        return NULL;
    
    op = (OpExpr *)expr;
    
    /* Check if this is the ? operator (jsonb_exists) */
    if (get_func_name(op->opfuncid) == NULL ||
        strcmp(get_func_name(op->opfuncid), "jsonb_exists") != 0)
        return NULL;
    
    args = op->args;
    if (list_length(args) != 2)
        return NULL;
    
    left = (Expr *)linitial(args);
    right = (Expr *)lsecond(args);
    
    /* Check if left side is 'tags' column */
    if (!IsA(left, Var))
        return NULL;
    
    /* Var is verified to be tags column, no need to save it */
    /* TODO: More robust column identification */
    
    /* Right side should be a text constant (the key) */
    if (!IsA(right, Const))
        return NULL;
    
    key_const = (Const *)right;
    if (key_const->consttype != TEXTOID || key_const->constisnull)
        return NULL;
    
    key = TextDatumGetCString(key_const->constvalue);
    
    /* Build GOQL filter for key existence: [key=*] */
    initStringInfo(&goql);
    appendStringInfo(&goql, "[%s=*]", key);
    
    return goql.data;
}

/*
 * Extract tag IS NOT NULL check: tags->>'key' IS NOT NULL
 * Returns GOQL filter [key=*] or NULL if not a tag null check
 */
static char *
extract_tag_is_not_null(Expr *expr)
{
    NullTest *nulltest;
    char *key;
    Var *var;
    StringInfoData goql;
    
    if (!IsA(expr, NullTest))
        return NULL;
    
    nulltest = (NullTest *)expr;
    
    /* We want IS NOT NULL, not IS NULL */
    if (nulltest->nulltesttype != IS_NOT_NULL)
        return NULL;
    
    /* Check if the argument is tags->>'key' */
    if (!is_jsonb_field_access((Expr *)nulltest->arg, &key, &var))
        return NULL;
    
    /* Build GOQL filter for key existence: [key=*] */
    initStringInfo(&goql);
    appendStringInfo(&goql, "[%s=*]", key);
    
    return goql.data;
}

/*
 * Combine multiple GOQL filters with AND logic
 * GOQL uses concatenation for AND: [key1=value1][key2=value2]
 */
static char *
combine_goql_filters(List *filters)
{
    StringInfoData result;
    ListCell *lc;
    
    if (filters == NIL)
        return NULL;
    
    initStringInfo(&result);
    
    foreach(lc, filters)
    {
        char *filter = (char *)lfirst(lc);
        appendStringInfoString(&result, filter);
    }
    
    return result.data;
}

/*
 * Extract tag filters from WHERE clause and convert to GOQL
 * Returns allocated GOQL string or NULL if no tag filters found
 */
char *
extract_goql_from_clauses(List *clauses, List **pushed_clauses)
{
    List *goql_filters = NIL;
    ListCell *lc;
    
    foreach(lc, clauses)
    {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);
        Expr *expr = rinfo->clause;
        char *goql = NULL;
        
        /* Try to extract tag equality filter */
        goql = extract_tag_equality(expr);
        
        /* Try to extract tag IN list filter */
        if (!goql)
            goql = extract_tag_in_list(expr);
        
        /* Try to extract tag existence check (? operator) */
        if (!goql)
            goql = extract_tag_exists(expr);
        
        /* Try to extract tag IS NOT NULL check */
        if (!goql)
            goql = extract_tag_is_not_null(expr);
        
        if (goql)
        {
            goql_filters = lappend(goql_filters, goql);
            if (pushed_clauses)
                *pushed_clauses = lappend(*pushed_clauses, rinfo);
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Extracted GOQL filter: %s", goql)));
        }
    }
    
    if (goql_filters == NIL)
        return NULL;
    
    /* Combine all filters with AND logic */
    return combine_goql_filters(goql_filters);
}