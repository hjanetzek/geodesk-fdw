/*-------------------------------------------------------------------------
 *
 * geodesk_options.c
 *      Option handling for GeoDesk FDW
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "geodesk_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

/*
 * Valid options for geodesk_fdw
 */
typedef struct GeodeskFdwOption
{
    const char *keyword;
    Oid context;      /* Oid of catalog in which option may appear */
} GeodeskFdwOption;

static GeodeskFdwOption valid_options[] =
{
    /* Connection options */
    {OPTION_DATASOURCE, ForeignServerRelationId},
    {OPTION_UPDATABLE, ForeignServerRelationId},
    
    /* Table options */
    {OPTION_LAYER, ForeignTableRelationId},
    {OPTION_QUERY, ForeignTableRelationId},
    {OPTION_SCHEMA_MODE, ForeignTableRelationId},
    {OPTION_GOQL_FILTER, ForeignTableRelationId},
    
    /* Sentinel */
    {NULL, InvalidOid}
};

/*
 * Check if an option is valid
 */
bool
geodesk_is_valid_option(const char *option, Oid context)
{
    GeodeskFdwOption *opt;
    
    for (opt = valid_options; opt->keyword; opt++)
    {
        if (context == opt->context && strcmp(opt->keyword, option) == 0)
            return true;
    }
    
    return false;
}

/*
 * Fetch options for a foreign table or server
 */
static void
geodesk_get_options_impl(Oid foreignoid, GeodeskFdwRelationInfo *fpinfo, bool is_server)
{
    ForeignTable *table = NULL;
    ForeignServer *server = NULL;
    List *options = NIL;
    ListCell *lc;
    
    if (is_server)
    {
        server = GetForeignServer(foreignoid);
        options = server->options;
    }
    else
    {
        table = GetForeignTable(foreignoid);
        server = GetForeignServer(table->serverid);
        
        /* Combine server and table options */
        options = list_concat(list_copy(server->options), table->options);
    }
    
    /* Initialize defaults */
    fpinfo->datasource = NULL;
    fpinfo->layer = "all";
    fpinfo->query = NULL;
    fpinfo->goql_filter = NULL;
    fpinfo->has_spatial_filter = false;
    
    /* Process options */
    foreach(lc, options)
    {
        DefElem *def = (DefElem *) lfirst(lc);
        
        if (strcmp(def->defname, OPTION_DATASOURCE) == 0)
        {
            fpinfo->datasource = defGetString(def);
        }
        else if (strcmp(def->defname, OPTION_LAYER) == 0)
        {
            fpinfo->layer = defGetString(def);
        }
        else if (strcmp(def->defname, OPTION_QUERY) == 0)
        {
            fpinfo->query = defGetString(def);
        }
        else if (strcmp(def->defname, OPTION_GOQL_FILTER) == 0)
        {
            fpinfo->goql_filter = defGetString(def);
        }
        else if (strcmp(def->defname, OPTION_SCHEMA_MODE) == 0)
        {
            /* Handle schema mode in future */
        }
        else if (strcmp(def->defname, OPTION_UPDATABLE) == 0)
        {
            /* Handle updatable flag in future */
        }
    }
}

/*
 * Fetch options for a foreign table
 */
void
geodesk_get_options(Oid foreigntableid, GeodeskFdwRelationInfo *fpinfo)
{
    geodesk_get_options_impl(foreigntableid, fpinfo, false);
}