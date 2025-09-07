/*-------------------------------------------------------------------------
 *
 * geodesk_connection_internal.h
 *      Internal connection structure shared between C++ files
 *
 *-------------------------------------------------------------------------
 */

#ifndef GEODESK_CONNECTION_INTERNAL_H
#define GEODESK_CONNECTION_INTERNAL_H

#include <memory>
#include <string>
#include <geodesk/geodesk.h>

using namespace geodesk;

/*
 * Internal connection structure
 */
struct GeodeskConnection
{
    Features* features;
    Features* filtered_features;  // Filtered view if query is provided
    Features* bbox_filtered_features; // Filtered view for bbox queries
    /* FID filtering disabled - libgeodesk doesn't support direct ID lookup
     * Features* id_filtered_features;   // Filtered view for ID queries
     */
    std::string filename;
    std::string query;            // GOQL query string
    bool has_bbox_filter;         // Whether bbox filter is applied
    /* FID filtering disabled - libgeodesk doesn't support direct ID lookup
     * bool has_id_filter;           // Whether ID filter is applied
     * int64_t filter_id;            // The ID to filter for
     */
    
    // Iterator state - heap allocated to avoid issues with move/copy
    FeatureIterator<Feature>* current_iter;
    bool iteration_started;
    
    // Cache the current feature for tag/geometry access
    std::unique_ptr<Feature> current_feature;
    
    GeodeskConnection() : features(nullptr), filtered_features(nullptr), 
                         bbox_filtered_features(nullptr),
                         /* id_filtered_features(nullptr), */
                         has_bbox_filter(false),
                         /* has_id_filter(false), filter_id(0), */
                         current_iter(nullptr), iteration_started(false) {}
    ~GeodeskConnection() 
    {
        if (features) delete features;
        if (filtered_features) delete filtered_features;
        if (bbox_filtered_features) delete bbox_filtered_features;
        /* if (id_filtered_features) delete id_filtered_features; */
        if (current_iter) delete current_iter;
        // current_feature is automatically cleaned up by unique_ptr
    }
};

#endif /* GEODESK_CONNECTION_INTERNAL_H */