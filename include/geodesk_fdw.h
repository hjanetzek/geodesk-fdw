#ifndef GEODESK_FDW_H
#define GEODESK_FDW_H

#include "postgres.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

/* Connection handle type (opaque pointer to C++ object) */
typedef void* GeodeskConnectionHandle;

/* Feature structure for passing data from C++ to C */
typedef struct GeodeskFeature
{
    int64_t id;           /* OSM feature ID */
    int type;             /* 0=node, 1=way, 2=relation */
    bool is_area;         /* True if way is an area (polygon) */
    void* internal_ptr;   /* Opaque pointer to C++ Feature object */
} GeodeskFeature;

/* FDW relation info stored in baserel->fdw_private */
typedef struct GeodeskFdwRelationInfo
{
    /* Connection options */
    char *datasource;     /* Path to GOL file */
    char *layer;          /* Layer specification */
    char *query;          /* GOQL query filter */
    
    /* Pushdown info */
    List *pushdown_clauses;
    bool has_spatial_filter;
    double bbox_min_x;
    double bbox_min_y;
    double bbox_max_x;
    double bbox_max_y;
    char *goql_filter;
    char *type_prefix;        /* GOQL type prefix (n, w, r, nw, etc.) */
    
    /* ID filter - disabled, libgeodesk doesn't support direct ID lookup
     * bool has_id_filter;
     * int64_t filter_id;
     */
    
    /* Cost estimates */
    double rows;
    int width;
    
    /* Column optimization */
    Bitmapset *attrs_used;  /* Bitmap of columns actually referenced in query */
} GeodeskFdwRelationInfo;

/* Execution state stored in node->fdw_state */
typedef struct GeodeskExecState
{
    /* Connection to GOL file */
    GeodeskConnectionHandle connection;
    
    /* Table metadata */
    Oid foreigntableid;
    List *retrieved_attrs;   /* List of target attribute numbers */
    
    /* Current feature */
    GeodeskFeature current_feature;
    bool feature_valid;
    
    /* Lazy loading optimization */
    bool needs_geometry;      /* True if geom column is requested */
    bool needs_bbox;          /* True if bbox column is requested */
    bool needs_members;       /* True if members column is requested */
    
    /* Statistics */
    uint64 rows_fetched;
} GeodeskExecState;

/* Option names */
#define OPTION_DATASOURCE "datasource"
#define OPTION_LAYER "layer"
#define OPTION_QUERY "query"
#define OPTION_UPDATABLE "updatable"
#define OPTION_SCHEMA_MODE "schema"
#define OPTION_GOQL_FILTER "goql_filter"

/* C++ Bridge Functions (implemented in geodesk_connection.cpp) */
extern GeodeskConnectionHandle geodesk_open(const char* path, const char* query);
extern void geodesk_close(GeodeskConnectionHandle handle);
extern void geodesk_reset_iteration(GeodeskConnectionHandle handle);
extern bool geodesk_get_next_feature(GeodeskConnectionHandle handle, GeodeskFeature* out_feature);
extern char* geodesk_get_tags_json(GeodeskConnectionHandle handle, GeodeskFeature* feature);
extern char* geodesk_get_members_json(GeodeskConnectionHandle handle, GeodeskFeature* feature);
extern Datum geodesk_get_tags_jsonb_direct(GeodeskConnectionHandle handle, GeodeskFeature* feature);
extern void* geodesk_build_lwgeom(GeodeskConnectionHandle handle, GeodeskFeature* feature); /* Returns LWGEOM* */
extern void geodesk_feature_cleanup(GeodeskFeature* feature);
extern void geodesk_set_spatial_filter(GeodeskConnectionHandle handle, 
                                       double min_x, double min_y, 
                                       double max_x, double max_y);
extern void geodesk_set_goql_filter(GeodeskConnectionHandle handle, const char* goql);
extern void geodesk_set_goql_filter_with_prefix(GeodeskConnectionHandle handle, const char* goql, const char* type_prefix);
/* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
 * extern void geodesk_set_id_filter(GeodeskConnectionHandle handle, int64_t id);
 */
extern int64_t geodesk_estimate_count(GeodeskConnectionHandle handle);

/* Option handling functions (geodesk_options.c) */
extern void geodesk_get_options(Oid foreigntableid, 
                                GeodeskFdwRelationInfo *fpinfo);
extern bool geodesk_is_valid_option(const char *option, Oid context);

/* Utility functions */
extern void geodesk_fdw_version_internal(char* version_str);

/* GOQL conversion functions (goql_converter.c) */
extern char *extract_goql_from_clauses(List *clauses, List **pushed_clauses);

/* Type filter functions (type_filter.c) */
extern char *extract_type_filter_prefix(List *clauses, List **pushed_clauses);

#endif /* GEODESK_FDW_H */
