/*-------------------------------------------------------------------------
 *
 * geodesk_parents_jsonb.cpp
 *      Direct JSONB construction for OSM parent relations (optimized)
 *
 *-------------------------------------------------------------------------
 */

// Standard library includes first
#include <string>
#include <exception>
#include <sstream>
#include <cstring>

// Include libgeodesk BEFORE PostgreSQL headers to avoid macro conflicts
// Temporarily rename conflicting types
#define Node GEODESK_NODE_AVOID_CONFLICT
#define Relation GEODESK_RELATION_AVOID_CONFLICT
#define RelationPtr GEODESK_RELATIONPTR_AVOID_CONFLICT
#define Query GEODESK_QUERY_AVOID_CONFLICT
#include <geodesk/geodesk.h>
#undef Node
#undef Relation
#undef RelationPtr
#undef Query
#include "geodesk_connection_internal.h"

// PostgreSQL headers must come after C++ headers
extern "C" {
#include "postgres.h"
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "geodesk_fdw.h"
}

extern "C" {

/*
 * Build JSONB directly from parent relations without intermediate JSON string
 * This is much more efficient than building a string and parsing it
 * 
 * Returns a JSONB Datum that can be directly stored in a tuple
 */
Datum
geodesk_get_parents_jsonb_direct(GeodeskConnectionHandle handle, GeodeskFeature* feature)
{
    if (!handle || !feature) 
    {
        /* Return NULL for invalid input */
        return (Datum) 0;
    }
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    /* Return NULL for null feature */
    if (!conn->current_feature) 
    {
        return (Datum) 0;
    }
    
    try
    {
        geodesk::Feature f = *conn->current_feature;
        
        /* Get parent relations first to check if empty */
        try 
        {
            geodesk::Features parents = f.parents();
            
            /* Check if there are any parents */
            bool has_parents = false;
            for (geodesk::Feature parent : parents)
            {
                has_parents = true;
                break;
            }
            
            /* If no parents, return NULL */
            if (!has_parents)
            {
                return (Datum) 0;
            }
            
            /* Build JSONB directly */
            JsonbParseState *state = NULL;
            JsonbValue jb_val;
            
            /* Start the JSONB array */
            pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
            
            /* Reset iterator and process parents */
            parents = f.parents();
            
            int parent_count = 0;
            const int MAX_PARENTS = 100;  /* Safety limit */
            
            for (geodesk::Feature parent : parents)
            {
                /* Safety check to prevent infinite loops */
                if (++parent_count > MAX_PARENTS)
                {
                    ereport(WARNING,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Feature %ld has more than %d parents, truncating", 
                                    feature->id, MAX_PARENTS)));
                    break;
                }
                
                /* Debug logging for problematic nodes */
                if (feature->id == 259654373 || feature->id == 259654332 || 
                    feature->id == 259654265 || feature->id == 259654266)
                {
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Processing parent %d for problematic node %ld",
                                    parent_count, feature->id)));
                }
                
                /* Start parent object */
                pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
                
                /* Add type field */
                jb_val.type = jbvString;
                jb_val.val.string.val = (char*)"type";
                jb_val.val.string.len = 4;
                pushJsonbValue(&state, WJB_KEY, &jb_val);
                
                const char* type_str;
                if (parent.isNode()) type_str = "node";
                else if (parent.isWay()) type_str = "way";
                else if (parent.isRelation()) type_str = "relation";
                else type_str = "unknown";
                
                jb_val.val.string.val = (char*)type_str;
                jb_val.val.string.len = strlen(type_str);
                pushJsonbValue(&state, WJB_VALUE, &jb_val);
                
                /* Add id field */
                jb_val.type = jbvString;
                jb_val.val.string.val = (char*)"id";
                jb_val.val.string.len = 2;
                pushJsonbValue(&state, WJB_KEY, &jb_val);
                
                /* Convert ID to string instead of numeric to avoid memory issues */
                char id_str[32];
                snprintf(id_str, sizeof(id_str), "%ld", parent.id());
                
                /* Allocate string in current memory context */
                char* id_copy = (char*)palloc(strlen(id_str) + 1);
                strcpy(id_copy, id_str);
                
                jb_val.type = jbvString;
                jb_val.val.string.val = id_copy;
                jb_val.val.string.len = strlen(id_str);
                pushJsonbValue(&state, WJB_VALUE, &jb_val);
                
                /* Skip role extraction for now to avoid crashes */
                /* TODO: Re-enable role extraction after fixing the crash issue */
                
                /* End parent object */
                pushJsonbValue(&state, WJB_END_OBJECT, NULL);
            }
            
            /* End the JSONB array */
            JsonbValue *result = pushJsonbValue(&state, WJB_END_ARRAY, NULL);
            
            /* Convert to Jsonb datum */
            return JsonbPGetDatum(JsonbValueToJsonb(result));
        }
        catch (const std::exception& e)
        {
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Failed to get parents for feature %ld: %s", 
                            feature->id, e.what())));
            
            /* Return NULL on inner exception */
            return (Datum) 0;
        }
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to build parents JSONB: %s", e.what())));
        
        /* Return NULL on error */
        return (Datum) 0;
    }
}

} // extern "C"