/*-------------------------------------------------------------------------
 *
 * geodesk_ring_assembler.cpp
 *      Ring assembly for multipolygon relations
 *      Connects ways at shared endpoints to form complete rings
 *
 *-------------------------------------------------------------------------
 */

#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "postgres.h"
#include "liblwgeom.h"
}

#include <geodesk/feature/WayPtr.h>
#include <geodesk/feature/WayCoordinateIterator.h>
#include <geodesk/feature/types.h>

using namespace geodesk;

// Hash function for Coordinate
struct CoordinateHash {
    std::size_t operator()(const Coordinate& c) const {
        return std::hash<int64_t>()(((int64_t)c.x << 32) | c.y);
    }
};

// Ring structure to hold ways that form a ring
struct Ring {
    std::vector<WayPtr> ways;
    std::vector<Coordinate> coords;
    
    Coordinate firstCoord() const {
        return coords.empty() ? Coordinate(0, 0) : coords.front();
    }
    
    Coordinate lastCoord() const {
        return coords.empty() ? Coordinate(0, 0) : coords.back();
    }
    
    bool isClosed() const {
        return coords.size() >= 4 && coords.front() == coords.back();
    }
    
    void appendWay(WayPtr way, bool reverse) {
        ways.push_back(way);
        
        WayCoordinateIterator iter;
        // If the way already has AREA flag, it's a complete area - keep its closing coordinate
        // Otherwise, get raw coordinates for assembly
        int flags = way.flags();
        bool isCompleteArea = (flags & AREA) != 0;
        iter.start(way, flags);
        int count = isCompleteArea ? iter.coordinatesRemaining() : iter.storedCoordinatesRemaining();
        
        std::vector<Coordinate> wayCoords;
        wayCoords.reserve(count + 1);  // Reserve space for potential closing coordinate
        
        // Extract coordinates - including closing if way is already a complete area
        if (isCompleteArea) {
            // Way is already a complete area - get all coordinates including closing
            while (iter.coordinatesRemaining() > 0) {
                wayCoords.push_back(iter.next());
            }
        } else {
            // Way needs assembly - get stored coordinates only
            while (iter.storedCoordinatesRemaining() > 0) {
                wayCoords.push_back(iter.next());
            }
        }
        
        if (reverse) {
            std::reverse(wayCoords.begin(), wayCoords.end());
        }
        
        // Skip first coordinate if it matches our last coordinate (avoid duplicates)
        size_t startIdx = 0;
        if (!coords.empty() && !wayCoords.empty() && coords.back() == wayCoords.front()) {
            startIdx = 1;
        }
        
        coords.insert(coords.end(), wayCoords.begin() + startIdx, wayCoords.end());
    }
    
    void reverse() {
        std::reverse(coords.begin(), coords.end());
        std::reverse(ways.begin(), ways.end());
    }
    
    void mergeRing(Ring* other, bool reverseOther) {
        if (reverseOther) {
            other->reverse();
        }
        
        // Skip first coordinate if it matches our last coordinate
        size_t startIdx = 0;
        if (!coords.empty() && !other->coords.empty() && coords.back() == other->coords.front()) {
            startIdx = 1;
        }
        
        coords.insert(coords.end(), other->coords.begin() + startIdx, other->coords.end());
        ways.insert(ways.end(), other->ways.begin(), other->ways.end());
    }
};

// Assembly function following imposm3 algorithm
std::vector<POINTARRAY*>
geodesk_assemble_rings(const std::vector<WayPtr>& ways)
{
    std::vector<POINTARRAY*> result;
    if (ways.empty()) return result;
    
    // Create initial rings from ways
    std::vector<Ring*> rings;
    for (const auto& way : ways) {
        Ring* ring = new Ring();
        ring->appendWay(way, false);
        rings.push_back(ring);
    }
    
    // Merge rings iteratively until no more merges are possible
    bool merged = true;
    while (merged) {
        merged = false;
        
        // Build endpoint index for quick lookup
        std::unordered_map<Coordinate, std::vector<size_t>, CoordinateHash> endpoints;
        for (size_t i = 0; i < rings.size(); i++) {
            if (rings[i] && !rings[i]->isClosed()) {
                endpoints[rings[i]->firstCoord()].push_back(i);
                endpoints[rings[i]->lastCoord()].push_back(i);
            }
        }
        
        // Try to merge rings
        for (size_t i = 0; i < rings.size(); i++) {
            if (!rings[i] || rings[i]->isClosed()) continue;
            
            Coordinate first = rings[i]->firstCoord();
            Coordinate last = rings[i]->lastCoord();
            
            // Look for another ring to connect with
            auto it = endpoints.find(last);
            if (it != endpoints.end()) {
                for (size_t j : it->second) {
                    if (j == i || !rings[j]) continue;
                    
                    Ring* other = rings[j];
                    if (last == other->firstCoord()) {
                        // Connect end-to-start
                        rings[i]->mergeRing(other, false);
                        delete rings[j];
                        rings[j] = nullptr;
                        merged = true;
                        break;
                    } else if (last == other->lastCoord()) {
                        // Connect end-to-end (need to reverse other)
                        rings[i]->mergeRing(other, true);
                        delete rings[j];
                        rings[j] = nullptr;
                        merged = true;
                        break;
                    }
                }
            }
            
            if (merged) break;
        }
    }
    
    // Collect completed rings and try to close nearly-closed rings
    const int32_t MAX_GAP = 100; // Small gap tolerance in imp units (about 1cm)
    std::vector<Ring*> completeRings;
    
    for (Ring* ring : rings) {
        if (!ring) continue;
        
        if (ring->isClosed()) {
            completeRings.push_back(ring);
        } else if (ring->coords.size() >= 3) {
            // Try to close if endpoints are very close
            Coordinate first = ring->firstCoord();
            Coordinate last = ring->lastCoord();
            int32_t dx = std::abs(first.x - last.x);
            int32_t dy = std::abs(first.y - last.y);
            
            if (dx < MAX_GAP && dy < MAX_GAP) {
                ring->coords.push_back(first);
                completeRings.push_back(ring);
            } else {
                // Can't close, discard
                if (!ring->ways.empty()) {
                    elog(DEBUG1, "Discarding unclosed ring with %zu coords, gap: dx=%d, dy=%d", 
                         ring->coords.size(), dx, dy);
                }
                delete ring;
            }
        } else {
            // Too few points
            if (!ring->ways.empty()) {
                elog(DEBUG1, "Discarding ring with too few points: %zu", ring->coords.size());
            }
            delete ring;
        }
    }
    
    // Convert complete rings to POINTARRAY
    constexpr double IMP_TO_METERS = 40075016.68558 / 4294967294.9999;
    
    for (Ring* ring : completeRings) {
        if (ring->coords.size() >= 4) { // Minimum for a valid ring
            POINTARRAY* pa = ptarray_construct(0, 0, ring->coords.size());
            if (pa) {
                for (size_t i = 0; i < ring->coords.size(); i++) {
                    POINT4D pt;
                    pt.x = ring->coords[i].x * IMP_TO_METERS;
                    pt.y = ring->coords[i].y * IMP_TO_METERS;
                    ptarray_set_point4d(pa, i, &pt);
                }
                result.push_back(pa);
            }
        }
        delete ring;
    }
    
    return result;
}