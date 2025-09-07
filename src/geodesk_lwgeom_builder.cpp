/*-------------------------------------------------------------------------
 *
 * geodesk_lwgeom_builder.cpp
 *      LWGEOM geometry builder for GeoDesk features
 *      Builds PostGIS LWGEOM structures directly from libgeodesk features
 *
 *-------------------------------------------------------------------------
 */

#include <memory>
#include <vector>
#include <geodesk/geodesk.h>
#include <geodesk/feature/WayCoordinateIterator.h>
#include <geodesk/feature/RelationPtr.h>
#include <geodesk/feature/MemberIterator.h>
#include <geodesk/feature/types.h>  // For FeatureFlags
#include <geodesk/geom/polygon/Polygonizer.h>

extern "C" {
#include "postgres.h"
#include "liblwgeom.h"
#include "geodesk_fdw.h"
}

using namespace geodesk;

// Include shared connection structure
#include "geodesk_connection_internal.h"

// Ring assembly function
std::vector<POINTARRAY*> geodesk_assemble_rings(const std::vector<WayPtr>& ways);

/*
 * Build LWGEOM from a GeoDesk feature
 */
extern "C" void*
geodesk_build_lwgeom(GeodeskConnectionHandle handle, GeodeskFeature* feature)
{
    if (!handle || !feature) return nullptr;
    
    auto conn = reinterpret_cast<GeodeskConnection*>(handle);
    
    try
    {
        // Use the cached feature
        if (!conn->current_feature)
        {
            return nullptr;
        }
        
        Feature f = *conn->current_feature;
        FeatureType ftype = f.type();
        
        if (ftype == FeatureType::NODE)
        {
            // Build LWPOINT
            NodePtr node(f.ptr());
            // Convert GeoDesk coordinates (int32 "imp" units) to Web Mercator meters
            // GeoDesk uses full int32 range, Web Mercator uses meters at equator
            // Conversion factor: EARTH_CIRCUMFERENCE / MAP_WIDTH
            constexpr double IMP_TO_METERS = 40075016.68558 / 4294967294.9999;
            double x = node.x() * IMP_TO_METERS;
            double y = node.y() * IMP_TO_METERS;
            
            // Create POINTARRAY with one point
            POINTARRAY* pa = ptarray_construct(0, 0, 1);  // no Z, no M, 1 point
            if (!pa) return nullptr;
            
            POINT4D pt;
            pt.x = x;
            pt.y = y;
            ptarray_set_point4d(pa, 0, &pt);
            
            // Create LWPOINT
            LWPOINT* point = lwpoint_construct(3857, NULL, pa);
            return lwpoint_as_lwgeom(point);
        }
        else if (ftype == FeatureType::WAY)
        {
            WayPtr way(f.ptr());
            WayCoordinateIterator iter;
            int areaFlag = way.flags() & FeatureFlags::AREA;
            iter.start(way, areaFlag);
            int count = iter.storedCoordinatesRemaining() + (areaFlag ? 1 : 0);
            
            // Create POINTARRAY
            POINTARRAY* pa = ptarray_construct(0, 0, count);  // no Z, no M
            if (!pa) return nullptr;
            
            // Conversion factor from GeoDesk "imp" units to Web Mercator meters
            constexpr double IMP_TO_METERS = 40075016.68558 / 4294967294.9999;
            
            // Fill coordinates
            for (int i = 0; i < count; i++)
            {
                Coordinate c = iter.next();
                POINT4D pt;
                pt.x = c.x * IMP_TO_METERS;
                pt.y = c.y * IMP_TO_METERS;
                ptarray_set_point4d(pa, i, &pt);
            }
            
            if (areaFlag)
            {
                // Build LWPOLY
                POINTARRAY** rings = (POINTARRAY**)lwalloc(sizeof(POINTARRAY*));
                rings[0] = pa;
                LWPOLY* poly = lwpoly_construct(3857, NULL, 1, rings);
                return lwpoly_as_lwgeom(poly);
            }
            else
            {
                // Build LWLINE
                LWLINE* line = lwline_construct(3857, NULL, pa);
                return lwline_as_lwgeom(line);
            }
        }
        else if (ftype == FeatureType::RELATION)
        {
            // Build LWGEOM for relations (multipolygons, routes, etc.)
            geodesk::RelationPtr rel(f.ptr());
            
            // Check if it's an area relation (multipolygon)
            if (rel.isArea())
            {
                // Simple multipolygon implementation
                // Collect all outer and inner ways
                std::vector<LWPOLY*> polygons;
                std::vector<WayPtr> outerWays;
                std::vector<WayPtr> innerWays;
                
                // Get the FeatureStore from connection
                FeatureStore* store = nullptr;
                if (conn->features)
                {
                    store = conn->features->store();
                }
                if (!store)
                {
                    ereport(DEBUG1,
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("No FeatureStore available for relation geometry")));
                    return nullptr;
                }
                
                
                // Iterate through relation members
                // We need to use MemberIterator to get roles
                DataPtr pMembers = rel.bodyptr();
                MemberIterator iter(store, pMembers, FeatureTypes::WAYS, 
                                  store->borrowAllMatcher(), nullptr);
                
                int memberCount = 0;
                for (;;)
                {
                    WayPtr way(iter.next());
                    if (way.isNull()) break;
                    if (way.isPlaceholder()) {
                        continue;
                    }
                    
                    memberCount++;
                    std::string_view role = iter.currentRole();
                    if (role == "outer")
                    {
                        outerWays.push_back(way);
                    }
                    else if (role == "inner")
                    {
                        innerWays.push_back(way);
                    }
                }
                
                // Handle multipolygon assembly with ring assembly
                
                // Helper lambda to check if a point is inside a ring (ray casting algorithm)
                auto isPointInRing = [](double x, double y, POINTARRAY* ring) -> bool {
                    int n = ring->npoints;
                    if (n < 3) return false;
                    
                    int crossings = 0;
                    POINT4D p1, p2;
                    getPoint4d_p(ring, 0, &p1);
                    
                    for (int i = 1; i <= n; i++)
                    {
                        getPoint4d_p(ring, i % n, &p2);
                        
                        // Check if ray from point to +infinity crosses this edge
                        if (((p1.y <= y && y < p2.y) || (p2.y <= y && y < p1.y)) &&
                            x < (p2.x - p1.x) * (y - p1.y) / (p2.y - p1.y) + p1.x)
                        {
                            crossings++;
                        }
                        p1 = p2;
                    }
                    return (crossings % 2) == 1;
                };
                
                // Use ring assembly to connect ways into complete rings
                std::vector<POINTARRAY*> outerRings = geodesk_assemble_rings(outerWays);
                std::vector<POINTARRAY*> innerRings = geodesk_assemble_rings(innerWays);
                
                if (outerRings.empty())
                {
                    // No valid outer rings
                    return nullptr;
                }
                
                // Now assign inner rings to outer rings
                std::vector<std::vector<POINTARRAY*>> polygonRings(outerRings.size());
                for (size_t i = 0; i < outerRings.size(); i++)
                {
                    polygonRings[i].push_back(outerRings[i]);
                }
                
                // For each inner ring, find which outer ring contains it
                for (size_t innerIdx = 0; innerIdx < innerRings.size(); innerIdx++)
                {
                    POINTARRAY* inner = innerRings[innerIdx];
                    
                    // Use first point of inner ring for point-in-polygon test
                    POINT4D testPt;
                    getPoint4d_p(inner, 0, &testPt);
                    
                    // Find containing outer ring
                    bool assigned = false;
                    for (size_t i = 0; i < outerRings.size(); i++)
                    {
                        if (isPointInRing(testPt.x, testPt.y, outerRings[i]))
                        {
                            polygonRings[i].push_back(inner);
                            assigned = true;
                            break; // Inner ring can only belong to one outer
                        }
                    }
                    
                    // Warn if inner ring couldn't be assigned to any outer ring
                    if (!assigned)
                    {
                        ereport(WARNING,
                                (errcode(ERRCODE_FDW_ERROR),
                                 errmsg("Relation %ld: Inner ring %zu was not assigned to any outer ring",
                                        rel.id(), innerIdx)));
                    }
                }
                
                // Create polygons with their holes
                for (const auto& rings : polygonRings)
                {
                    if (rings.empty()) continue;
                    
                    POINTARRAY** ringArray = (POINTARRAY**)lwalloc(sizeof(POINTARRAY*) * rings.size());
                    for (size_t i = 0; i < rings.size(); i++)
                    {
                        ringArray[i] = rings[i];
                    }
                    
                    LWPOLY* poly = lwpoly_construct(3857, NULL, rings.size(), ringArray);
                    if (poly)
                    {
                        polygons.push_back(poly);
                    }
                }
                
                // Create multipolygon if we have any polygons
                if (!polygons.empty())
                {
                    LWGEOM** geoms = (LWGEOM**)lwalloc(sizeof(LWGEOM*) * polygons.size());
                    for (size_t i = 0; i < polygons.size(); i++)
                    {
                        geoms[i] = lwpoly_as_lwgeom(polygons[i]);
                    }
                    LWCOLLECTION* coll = lwcollection_construct(MULTIPOLYGONTYPE, 3857, 
                                                               NULL, polygons.size(), geoms);
                    return (void*)coll;
                }
                
                ereport(DEBUG1,
                        (errcode(ERRCODE_FDW_ERROR),
                         errmsg("Could not build multipolygon from relation %ld", rel.id())));
                return nullptr;
            }
            else
            {
                // Non-area relations (routes, etc.) - return NULL
                return nullptr;
            }
        }
        else
        {
            // Unknown feature type
            return nullptr;
        }
    }
    catch (const std::exception& e)
    {
        ereport(WARNING,
                (errcode(ERRCODE_FDW_ERROR),
                 errmsg("Error building LWGEOM: %s", e.what())));
        return nullptr;
    }
}