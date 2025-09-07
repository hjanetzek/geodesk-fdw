/*-------------------------------------------------------------------------
 *
 * geodesk_members_jsonb.cpp
 *      Direct JSONB construction for OSM relation members (optimized)
 *
 *-------------------------------------------------------------------------
 */

// Standard library includes first
#include <string>
#include <exception>
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
 * Build JSONB directly from relation members without intermediate JSON string
 * This is much more efficient than building a string and parsing it
 * 
 * Returns a JSONB Datum that can be directly stored in a tuple
 */
__attribute__((visibility("default")))
Datum
geodesk_get_members_jsonb_direct(GeodeskConnectionHandle handle, GeodeskFeature* feature)
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
        
        /* Only relations have members */
        if (!f.isRelation())
        {
            /* Return NULL for non-relations */
            return (Datum) 0;
        }
        
        /* Check if there are any members first */
        try 
        {
            geodesk::Features members = f.members();
            
            /* Check if there are any members */
            bool has_members = false;
            for (geodesk::Feature member : members)
            {
                has_members = true;
                break;
            }
            
            /* If no members, return NULL */
            if (!has_members)
            {
                return (Datum) 0;
            }
            
            /* Build JSONB directly */
            JsonbParseState *state = NULL;
            JsonbValue jb_val;
            
            /* Start the JSONB array */
            pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
            
            /* Reset iterator and process members */
            members = f.members();
            
            int member_count = 0;
            const int MAX_MEMBERS = 1000;  /* Safety limit */
            
            for (geodesk::Feature member : members)
            {
                /* Safety check to prevent infinite loops */
                if (++member_count > MAX_MEMBERS)
                {
                    ereport(WARNING,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Relation %ld has more than %d members, truncating", 
                                    feature->id, MAX_MEMBERS)));
                    break;
                }
                
                /* Start member object */
                pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
                
                /* Add type field */
                jb_val.type = jbvString;
                jb_val.val.string.val = (char*)"type";
                jb_val.val.string.len = 4;
                pushJsonbValue(&state, WJB_KEY, &jb_val);
                
                const char* type_str;
                if (member.isNode()) type_str = "node";
                else if (member.isWay()) type_str = "way";
                else if (member.isRelation()) type_str = "relation";
                else type_str = "unknown";
                
                jb_val.val.string.val = (char*)type_str;
                jb_val.val.string.len = strlen(type_str);
                pushJsonbValue(&state, WJB_VALUE, &jb_val);
                
                /* Add id field */
                jb_val.type = jbvString;
                jb_val.val.string.val = (char*)"id";
                jb_val.val.string.len = 2;
                pushJsonbValue(&state, WJB_KEY, &jb_val);
                
                /* Convert ID to string */
                char id_str[32];
                snprintf(id_str, sizeof(id_str), "%ld", member.id());
                
                /* Allocate string in current memory context */
                char* id_copy = (char*)palloc(strlen(id_str) + 1);
                strcpy(id_copy, id_str);
                
                jb_val.type = jbvString;
                jb_val.val.string.val = id_copy;
                jb_val.val.string.len = strlen(id_str);
                pushJsonbValue(&state, WJB_VALUE, &jb_val);
                
                /* Add role field */
                jb_val.type = jbvString;
                jb_val.val.string.val = (char*)"role";
                jb_val.val.string.len = 4;
                pushJsonbValue(&state, WJB_KEY, &jb_val);
                
                /* Get the role */
                std::string role_str;
                try 
                {
                    role_str = member.role();
                }
                catch (const std::exception& e)
                {
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Failed to get role for member in relation %ld: %s", 
                                    feature->id, e.what())));
                    role_str = "";
                }
                
                /* Allocate role string in current memory context */
                char* role_copy = (char*)palloc(role_str.length() + 1);
                memcpy(role_copy, role_str.c_str(), role_str.length());
                role_copy[role_str.length()] = '\0';
                
                jb_val.val.string.val = role_copy;
                jb_val.val.string.len = role_str.length();
                pushJsonbValue(&state, WJB_VALUE, &jb_val);
                
                /* End member object */
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
                     errmsg("Failed to get members for relation %ld: %s", 
                            feature->id, e.what())));
            
            /* Return NULL on inner exception */
            return (Datum) 0;
        }
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to build members JSONB: %s", e.what())));
        
        /* Return NULL on error */
        return (Datum) 0;
    }
}

} // extern "C"