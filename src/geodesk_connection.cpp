/*-------------------------------------------------------------------------
 *
 * geodesk_connection.cpp
 *      C++ bridge to libgeodesk for GeoDesk FDW
 *
 *-------------------------------------------------------------------------
 */

// Standard library includes first
#include <memory>
#include <string>
#include <cstring>      // For strlen, strcpy
#include <cstdio>       // For fopen
#include <cerrno>       // For errno
#include <optional>
#include <exception>
#include <concepts>     // For std::integral
#include <string_view>  // For string_view methods

// Include the full geodesk API with implementations
// Note: We don't use GEOS - we build LWGEOM directly instead
#include <geodesk/geodesk.h>

// We don't need GeometryBuilder since we build LWGEOM directly

extern "C" {
#include "postgres.h"
#include "geodesk_fdw.h"
}

using namespace geodesk;

// Include shared connection structure
#include "geodesk_connection_internal.h"

/*
 * C interface functions
 */
extern "C" {

/*
 * Open a connection to a GOL file
 */
GeodeskConnectionHandle
geodesk_open(const char* path, const char* query)
{
    try
    {
        ereport(INFO,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("geodesk_open: Attempting to open '%s'", path)));
        
        // Check if file exists and is readable
        FILE* fp = fopen(path, "rb");
        if (fp) {
            fclose(fp);
            ereport(INFO,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("geodesk_open: File is readable via fopen")));
        } else {
            int saved_errno = errno;  // Save errno before PostgreSQL macros
            ereport(WARNING,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("geodesk_open: Cannot open file via fopen: %s", strerror(saved_errno))));
        }
        
        auto conn = new GeodeskConnection();
        conn->filename = path;
        
        // Create a Features object that opens the GOL file
        conn->features = new Features(path);
        
        // Apply GOQL query if provided
        if (query && strlen(query) > 0)
        {
            conn->query = query;
            try
            {
                // Create a filtered view using the GOQL query
                // Use operator() to create a filtered view
                conn->filtered_features = new Features(conn->features->operator()(query));
                ereport(INFO,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("geodesk_open: Applied GOQL query: '%s'", query)));
            }
            catch (const std::exception& e)
            {
                ereport(WARNING,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Failed to apply GOQL query '%s': %s", query, e.what())));
                // Continue without filter on error
            }
        }
        
        ereport(INFO,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("geodesk_open: Successfully opened '%s'", path)));
        
        return reinterpret_cast<GeodeskConnectionHandle>(conn);
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to open GOL file '%s': %s", path, e.what())));
        return nullptr;
    }
    catch (...)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to open GOL file '%s': unknown error", path)));
        return nullptr;
    }
}

/*
 * Close a connection
 */
void
geodesk_close(GeodeskConnectionHandle handle)
{
    if (handle)
    {
        delete reinterpret_cast<GeodeskConnection*>(handle);
    }
}

/*
 * Reset iteration to beginning
 */
void
geodesk_reset_iteration(GeodeskConnectionHandle handle)
{
    if (!handle) return;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    // Priority: bbox filter > GOQL filter > all features
    // FID filtering disabled - libgeodesk doesn't support direct ID lookup
    Features* features_to_iterate = conn->bbox_filtered_features ?
                                     conn->bbox_filtered_features :
                                     (conn->filtered_features ? 
                                      conn->filtered_features : conn->features);
    
    if (features_to_iterate)
    {
        // Delete old iterator if exists
        if (conn->current_iter) 
        {
            delete conn->current_iter;
            conn->current_iter = nullptr;
        }
        
        // Create new iterator
        conn->current_iter = new FeatureIterator<Feature>(features_to_iterate->begin());
        conn->iteration_started = true;
    }
}

/*
 * Get next feature - returns true if a feature was found
 */
bool
geodesk_get_next_feature(GeodeskConnectionHandle handle, GeodeskFeature* out_feature)
{
    if (!handle || !out_feature) return false;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    // Initialize iteration if not started
    if (!conn->iteration_started)
    {
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Starting iteration")));
        geodesk_reset_iteration(handle);
    }
    
    // Check if iterator is valid
    if (!conn->current_iter || *conn->current_iter == nullptr)
    {
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Iterator at end or invalid")));
        return false; // End of iteration
    }
    
    try
    {
        // Get the current feature
        Feature f = **conn->current_iter;
        
        // Cache a copy of the feature for tag/geometry access
        conn->current_feature = std::make_unique<Feature>(f);
        
        // Extract basic properties
        out_feature->id = f.id();
        out_feature->type = static_cast<int>(f.type());
        out_feature->is_area = f.isArea();
        
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Got feature: id=%lld, type=%d, is_area=%d", 
                        (long long)out_feature->id, out_feature->type, 
                        out_feature->is_area ? 1 : 0)));
        
        // Move to next feature
        ++(*conn->current_iter);
        
        return true;
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Error iterating features: %s", e.what())));
        return false;
    }
}

/*
 * Get all tags as JSON string for the cached current feature
 */
char*
geodesk_get_tags_json(GeodeskConnectionHandle handle, GeodeskFeature* feature)
{
    if (!handle || !feature) return nullptr;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    try
    {
        // Use the cached feature
        if (!conn->current_feature)
        {
            // Return empty JSON object if no cached feature
            char* result = (char*)malloc(3);
            if (result) strcpy(result, "{}");
            return result;
        }
        
        Feature f = *conn->current_feature;
        
        // Build JSON string manually (avoid external JSON library dependency)
        std::string json = "{";
        bool first = true;
        
        Tags tags = f.tags();
        for(Tag tag : tags)
        {
            if (!first) json += ",";
            first = false;
            
            // Escape and quote the key
            json += "\"";
            std::string_view key = tag.key();
            for (char c : key)
            {
                if (c == '"') json += "\\\"";
                else if (c == '\\') json += "\\\\";
                else if (c == '\n') json += "\\n";
                else if (c == '\r') json += "\\r";
                else if (c == '\t') json += "\\t";
                else if (c == '\b') json += "\\b";
                else if (c == '\f') json += "\\f";
                else if (c < 0x20) continue; // Skip other control characters
                else json += c;
            }
            json += "\":";
            
            // Escape and quote the value
            json += "\"";
            std::string value_str = tag.value();  // TagValue converts to std::string
            for (char c : value_str)
            {
                if (c == '"') json += "\\\"";
                else if (c == '\\') json += "\\\\";
                else if (c == '\n') json += "\\n";
                else if (c == '\r') json += "\\r";
                else if (c == '\t') json += "\\t";
                else if (c == '\b') json += "\\b";
                else if (c == '\f') json += "\\f";
                else if (c < 0x20) continue; // Skip other control characters
                else json += c;
            }
            json += "\"";
        }
        json += "}";
        
        // Allocate and return C string (caller must free)
        char* result = (char*)malloc(json.length() + 1);
        if (result)
        {
            strcpy(result, json.c_str());
        }
        return result;
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Error getting tags: %s", e.what())));
        // Return empty JSON object on error
        char* result = (char*)malloc(3);
        if (result) strcpy(result, "{}");
        return result;
    }
}

/*
 * Clean up feature resources
 */
void
geodesk_feature_cleanup(GeodeskFeature* feature)
{
    // Nothing to do yet
}

/*
 * Set spatial filter (bounding box)
 * Coordinates should be in Web Mercator (EPSG:3857) meters
 */
void
geodesk_set_spatial_filter(GeodeskConnectionHandle handle, 
                           double min_x, double min_y,
                           double max_x, double max_y)
{
    if (!handle) return;
    
    GeodeskConnection* conn = static_cast<GeodeskConnection*>(handle);
    
    try
    {
        // Convert Web Mercator meters to GeoDesk imp units
        // GeoDesk uses full int32 range, Web Mercator uses meters at equator
        // Conversion factor: MAP_WIDTH / EARTH_CIRCUMFERENCE
        constexpr double METERS_TO_IMP = 4294967294.9999 / 40075016.68558;
        
        int32_t imp_min_x = static_cast<int32_t>(min_x * METERS_TO_IMP);
        int32_t imp_min_y = static_cast<int32_t>(min_y * METERS_TO_IMP);
        int32_t imp_max_x = static_cast<int32_t>(max_x * METERS_TO_IMP);
        int32_t imp_max_y = static_cast<int32_t>(max_y * METERS_TO_IMP);
        
        // Create Box for spatial filtering
        Box bbox(imp_min_x, imp_min_y, imp_max_x, imp_max_y);
        
        // Apply bbox filter to features
        Features* base_features = conn->filtered_features ? conn->filtered_features : conn->features;
        
        // Clean up any existing bbox filter
        if (conn->bbox_filtered_features)
        {
            delete conn->bbox_filtered_features;
            conn->bbox_filtered_features = nullptr;
        }
        
        // Apply the bbox filter using operator()
        conn->bbox_filtered_features = new Features((*base_features)(bbox));
        conn->has_bbox_filter = true;
        
        ereport(INFO,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Applied bbox filter: meters[%.2f,%.2f,%.2f,%.2f] -> imp[%d,%d,%d,%d]", 
                        min_x, min_y, max_x, max_y,
                        imp_min_x, imp_min_y, imp_max_x, imp_max_y)));
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to set spatial filter: %s", e.what())));
    }
}

/*
 * Set GOQL filter with type prefix
 */
void
geodesk_set_goql_filter_with_prefix(GeodeskConnectionHandle handle, const char* goql, const char* type_prefix)
{
    if (!handle) return;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    try
    {
        // Apply GOQL filter to the most specific features available
        Features* base_features = conn->bbox_filtered_features ?
                                 conn->bbox_filtered_features :
                                 conn->features;
        
        if (!base_features)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("No base features to apply GOQL filter to")));
            return;
        }
        
        // Clean up any existing filtered features
        if (conn->filtered_features)
        {
            delete conn->filtered_features;
            conn->filtered_features = nullptr;
        }
        
        // Apply the GOQL query with type prefix
        std::string full_query;
        if (type_prefix && strlen(type_prefix) > 0)
        {
            full_query = std::string(type_prefix);
        }
        else
        {
            full_query = "*";  // Default to all types
        }
        
        if (goql && strlen(goql) > 0)
        {
            full_query += goql;
        }
        
        conn->filtered_features = new Features((*base_features)(full_query.c_str()));
        
        ereport(INFO,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Applied GOQL filter: %s", full_query.c_str())));
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Failed to apply GOQL filter '%s': %s", goql ? goql : "null", e.what())));
    }
}

/*
 * Set GOQL filter (legacy, uses default prefix)
 */
void
geodesk_set_goql_filter(GeodeskConnectionHandle handle, const char* goql)
{
    // Just call the new function with default prefix
    geodesk_set_goql_filter_with_prefix(handle, goql, "*");
}

/* FID pushdown disabled - libgeodesk doesn't support direct ID lookup
 * See WIP_fid_pushdown_limitations.md for details
 * 
 * Set ID filter using lambda predicate
 *
 * void
 * geodesk_set_id_filter(GeodeskConnectionHandle handle, int64_t id)
 * {
 *     if (!handle) return;
 *     
 *     auto conn = reinterpret_cast<GeodeskConnection*>(handle);
 *     
 *     try
 *     {
 *         // Create lambda filter for ID matching
 *         auto id_filter = [id](Feature f) { return f.id() == id; };
 *         
 *         // Apply filter to the most specific features available
 *         Features* base_features = conn->bbox_filtered_features ?
 *                                  conn->bbox_filtered_features :
 *                                  (conn->filtered_features ? 
 *                                   conn->filtered_features : conn->features);
 *         
 *         // Clean up any existing ID filter
 *         if (conn->id_filtered_features)
 *         {
 *             delete conn->id_filtered_features;
 *             conn->id_filtered_features = nullptr;
 *         }
 *         
 *         // Apply the ID filter
 *         conn->id_filtered_features = new Features(base_features->filter(id_filter));
 *         conn->has_id_filter = true;
 *         conn->filter_id = id;
 *         
 *         ereport(INFO,
 *                 (errcode(ERRCODE_FDW_ERROR),
 *                  errmsg("Applied ID filter: id=%lld", (long long)id)));
 *     }
 *     catch (const std::exception& e)
 *     {
 *         ereport(WARNING,
 *                 (errcode(ERRCODE_FDW_ERROR),
 *                  errmsg("Failed to set ID filter: %s", e.what())));
 *     }
 * }
 */

/*
 * Get GEOS geometry for the cached current feature
 */
void*
geodesk_get_geometry(GeodeskConnectionHandle handle)
{
    if (!handle) return nullptr;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    try
    {
        // Use the cached feature
        if (!conn->current_feature)
        {
            return nullptr;
        }
        
        Feature f = *conn->current_feature;
        
        // PostGIS uses the global GEOS context (via initGEOS).
        // We need to use nullptr to indicate we want the global context.
        // However, libgeodesk's GeometryBuilder requires a non-null context.
        // So we'll use our wrapper that uses global GEOS functions.
        
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("geodesk_get_geometry: Using global GEOS context for feature type %d", 
                        static_cast<int>(f.type()))));
        
        GEOSGeometry* geom = nullptr;
        try 
        {
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("geodesk_get_geometry: Calling buildFeatureGeometryGlobal")));
            
            geom = buildFeatureGeometryGlobal(conn->features->store(), f.ptr());
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("geodesk_get_geometry: Geometry built successfully: %p", geom)));
        }
        catch (const std::exception& e)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("buildFeatureGeometryGlobal threw exception: %s", e.what())));
            return nullptr;
        }
        catch (...)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("buildFeatureGeometryGlobal threw unknown exception")));
            return nullptr;
        }
        
        return reinterpret_cast<void*>(geom);
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Error getting geometry: %s", e.what())));
        return nullptr;
    }
}

/*
 * Estimate feature count
 */
int64_t
geodesk_estimate_count(GeodeskConnectionHandle handle)
{
    return 1000; // Dummy estimate
}

} // extern "C"

// Implementation of global GEOS geometry builder
// This uses global GEOS functions instead of thread-local context functions
GEOSGeometry* buildFeatureGeometryGlobal(geodesk::FeatureStore* store, geodesk::FeaturePtr feature)
{
    using namespace geodesk;
    
    try
    {
        FeatureType ftype = feature.type();
        
        ereport(DEBUG1,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("buildFeatureGeometryGlobal: feature type = %d", static_cast<int>(ftype))));
        
        if (ftype == FeatureType::NODE)
        {
            NodePtr node(feature);
            double x = node.x();
            double y = node.y();
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("buildFeatureGeometryGlobal: Creating point at (%f, %f)", x, y)));
            
            GEOSCoordSequence* coordSeq = GEOSCoordSeq_create(1, 2);
            if (!coordSeq)
            {
                ereport(WARNING,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Failed to create coordinate sequence")));
                return nullptr;
            }
            
            GEOSCoordSeq_setXY(coordSeq, 0, x, y);
            GEOSGeometry* geom = GEOSGeom_createPoint(coordSeq);
            
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("buildFeatureGeometryGlobal: Point created: %p", geom)));
            
            return geom;
        }
        else
        {
            // TODO: Implement way and relation geometry
            ereport(DEBUG1,
                    (errcode(ERRCODE_FDW_ERROR),
                     errmsg("buildFeatureGeometryGlobal: Unsupported type %d, returning null", static_cast<int>(ftype))));
            return nullptr;
        }
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("buildFeatureGeometryGlobal exception: %s", e.what())));
        return nullptr;
    }
}
