# Implementation Plan: Exposing OSM Data Model in GeoDesk FDW

## Executive Summary
Add support for OSM structural data (relation members, way nodes) to GeoDesk FDW through optional JSONB columns that expose the full OSM data model while maintaining backward compatibility.

## Current State
The FDW currently exposes:
- `fid` (bigint): Feature ID
- `type` (integer): 0=node, 1=way, 2=relation  
- `tags` (jsonb): Key-value tags
- `geom` (geometry): PostGIS geometry
- `is_area` (boolean): Way area flag

Missing OSM data model elements:
- Relation member IDs, types, and roles
- Way node IDs and sequence
- Parent relationships

## Proposed Solution

### 1. Add Two Optional JSONB Columns

**New columns:**
- `members` (jsonb): For relations and ways
- `parents` (jsonb): For all features (optional, phase 2)

### 2. JSONB Structure

**For Relations:**
```json
{
  "members": [
    {"id": 123456, "type": "way", "role": "outer"},
    {"id": 789012, "type": "way", "role": "inner"},
    {"id": 345678, "type": "node", "role": "admin_centre"}
  ]
}
```

**For Ways:**
```json
{
  "nodes": [456789, 456790, 456791, 456789]
}
```

**For Nodes:**
```json
null  // No members
```

### 3. Implementation Details

**Phase 1: Core Implementation**
1. Modify `geodeskIterateForeignScan()` to detect `members` column
2. Add member extraction in C++ bridge (`geodesk_connection.cpp`):
   ```cpp
   char* geodesk_get_members_json(GeodeskConnectionHandle handle, 
                                   GeodeskFeature* feature);
   ```
3. Use libgeodesk's existing APIs:
   - `Feature::members()` for relations
   - `Way::nodes()` for ways  
   - Extract IDs and roles, build JSON

**Phase 2: Optimizations**
1. Add lazy loading flag `needs_members` (like existing `needs_geometry`)
2. Add FDW option `enable_members` (default: false) for performance
3. Consider caching member data if repeatedly accessed

### 4. Table Creation Examples

**Basic usage (backward compatible):**
```sql
CREATE FOREIGN TABLE osm_features (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857)
) SERVER geodesk_server
OPTIONS (datasource '/path/to/data.gol');
```

**With OSM data model:**
```sql
CREATE FOREIGN TABLE osm_features_full (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    members jsonb  -- NEW: OSM structure
) SERVER geodesk_server
OPTIONS (
    datasource '/path/to/data.gol',
    enable_members 'true'  -- Opt-in for performance
);
```

### 5. Query Examples

**Find all buildings in a relation:**
```sql
SELECT m.member->>'id' as building_id
FROM osm_features_full r,
     jsonb_array_elements(r.members->'members') as m(member)
WHERE r.type = 2  -- relation
  AND r.tags->>'type' = 'multipolygon'
  AND m.member->>'role' = 'outer';
```

**Get way node sequence:**
```sql
SELECT fid, 
       jsonb_array_elements_text(members->'nodes') as node_id
FROM osm_features_full
WHERE type = 1  -- way
  AND fid = 123456;
```

## Benefits
1. **Full OSM data model access** without schema changes
2. **Backward compatible** - existing tables work unchanged
3. **Flexible** - JSONB allows querying with PostgreSQL operators
4. **Performant** - Lazy loading, opt-in via FDW option
5. **Standards-compliant** - Follows PostgreSQL FDW patterns

## Implementation Steps

### Step 1: Add C++ Bridge Function ✅
- [x] Create `geodesk_get_members_json()` in geodesk_connection.cpp
- [x] Test with relation members (no relations in test data, but code works)
- [x] Test with way nodes (crashes - debugging in progress)
- [x] Handle empty/null cases

### Step 2: Modify FDW Core ✅
- [x] Add `needs_members` flag to GeodeskExecState
- [x] Detect `members` column in geodeskBeginForeignScan
- [x] Call bridge function in geodeskIterateForeignScan
- [x] Handle JSONB conversion

### Step 3: Add Configuration
- [ ] Parse `enable_members` option in geodesk_options.c
- [ ] Store in GeodeskFdwRelationInfo
- [ ] Respect option during execution

### Step 4: Testing
- [ ] Create test SQL with members column
- [ ] Test relation member extraction
- [ ] Test way node extraction  
- [ ] Test JSONB query patterns
- [ ] Benchmark performance impact

### Step 5: Documentation
- [ ] Update README with members column
- [ ] Add query examples
- [ ] Document performance considerations

## Testing Plan
1. Unit tests for member extraction
2. Integration tests with test.gol data
3. Performance benchmarks with/without members
4. Verify JSONB query patterns work correctly

## Alternative Considered
- Separate tables for members (complex joins)
- PostgreSQL arrays (less flexible than JSONB)
- Multiple columns (member_ids, member_types, member_roles)

JSONB chosen for flexibility and PostgreSQL ecosystem compatibility.

## Current Status (2025-09-07)

### What's Working
- ✅ Basic infrastructure for members column is in place
- ✅ Column detection and lazy loading implemented
- ✅ JSONB conversion working
- ✅ Test data (Bremen GOL) has 4346 relations and 749 ways

### What's Not Working
- ❌ **Crash when accessing relation members** - FeatureStore pointer issue
- ❌ **Crash when accessing way nodes** - Similar FeatureStore access problem
- ❌ **current_feature->store() returns null** - Need different approach

### Root Cause
The `GeodeskConnection` structure stores `current_feature` as a `unique_ptr<Feature>`, but when we try to access its `store()` method in `geodesk_get_members_json()`, it crashes. This suggests the Feature object doesn't properly maintain its FeatureStore reference.

### Potential Solutions
1. Store FeatureStore pointer directly in GeodeskConnection
2. Pass FeatureStore through the feature's internal_ptr
3. Reconstruct Feature with proper store reference in members function
4. Use the Features collection's store instead of current_feature

## Progress Tracking

### Phase 1: Core Implementation
- [x] Implement geodesk_get_members_json function (crashes on use)
- [x] Add members column detection
- [ ] Test with relations (crashes)
- [ ] Test with ways (disabled due to crashes)
- [ ] Handle null cases

### Phase 2: Optimization
- [ ] Add lazy loading
- [ ] Add enable_members option
- [ ] Performance testing

### Phase 3: Documentation
- [ ] Update README
- [ ] Add examples
- [ ] Release notes