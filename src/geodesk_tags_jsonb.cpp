/*-------------------------------------------------------------------------
 *
 * geodesk_tags_jsonb.cpp
 *      Direct JSONB construction for OSM tags (optimized)
 *
 *-------------------------------------------------------------------------
 */

// Standard library includes first
#include <string>
#include <exception>

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
 * Build JSONB directly from tags without intermediate JSON string
 * This is much more efficient than building a string and parsing it
 * 
 * Returns a JSONB Datum that can be directly stored in a tuple
 */
__attribute__((visibility("default")))
Datum
geodesk_get_tags_jsonb_direct(GeodeskConnectionHandle handle, GeodeskFeature* feature)
{
    if (!handle || !feature) 
    {
        /* Return empty JSONB object {} */
        JsonbParseState *state = NULL;
        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
        JsonbValue *result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        return JsonbPGetDatum(JsonbValueToJsonb(result));
    }
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    /* Return empty object for null feature */
    if (!conn->current_feature) 
    {
        JsonbParseState *state = NULL;
        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
        JsonbValue *result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        return JsonbPGetDatum(JsonbValueToJsonb(result));
    }
    
    try
    {
        geodesk::Feature f = *conn->current_feature;
        geodesk::Tags tags = f.tags();
        
        /* Build JSONB directly */
        JsonbParseState *state = NULL;
        JsonbValue key_val, value_val;
        
        /* Start the JSONB object */
        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
        
        /* Iterate through tags */
        for(geodesk::Tag tag : tags)
        {
            /* Add the key */
            std::string_view key_sv = tag.key();
            key_val.type = jbvString;
            key_val.val.string.val = const_cast<char*>(key_sv.data());
            key_val.val.string.len = key_sv.length();
            pushJsonbValue(&state, WJB_KEY, &key_val);
            
            /* Add the value */
            std::string value_str = tag.value();  /* TagValue converts to std::string */
            value_val.type = jbvString;
            
            /* 
             * We need to ensure the string stays valid during JSONB construction
             * PostgreSQL will copy the data when building the final JSONB
             */
            char* value_copy = (char*)palloc(value_str.length() + 1);
            memcpy(value_copy, value_str.c_str(), value_str.length());
            value_copy[value_str.length()] = '\0';
            
            value_val.val.string.val = value_copy;
            value_val.val.string.len = value_str.length();
            pushJsonbValue(&state, WJB_VALUE, &value_val);
        }
        
        /* End the JSONB object */
        JsonbValue *result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        
        /* Convert to Jsonb datum */
        return JsonbPGetDatum(JsonbValueToJsonb(result));
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to build tags JSONB: %s", e.what())));
        
        /* Return empty object on error */
        JsonbParseState *state = NULL;
        pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
        JsonbValue *result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        return JsonbPGetDatum(JsonbValueToJsonb(result));
    }
}

} // extern "C"