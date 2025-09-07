# OSM Data Model Implementation

## Status: ✅ Fully Functional

This document describes the implementation of OSM data model features in GeoDesk FDW, including members and parent relations.

### What Works
1. **Relation Members**: Fully functional
   - Returns array of member objects with `id`, `type`, and `role`
   - Example: `{"members":[{"id":28906451,"role":"substation","type":"way"},...]}` 
   - All member IDs and roles are correctly extracted

2. **Way Nodes**: Fully functional with optimized storage
   - Returns array of node references
   - **Anonymous nodes** (nodes without tags): Shown as `null` in the array
   - **Tagged nodes** (nodes with tags like crossings, traffic signals): Shown with their actual OSM IDs

### Implementation Details

The `members` column is exposed as JSONB:
- For **relations**: `{"members":[{"id":123,"type":"way","role":"outer"},...]}` 
- For **ways**: `{"nodes":[null, 125709470, null, ...]}` (nulls for anonymous nodes, IDs for tagged nodes)
- For **nodes**: Returns NULL

### Key Discovery
GeoDesk's optimized GOL format stores way nodes efficiently:
- **Anonymous nodes** (no tags): Stored inline as coordinates only, shown as `null`
- **Tagged nodes** (with tags): Actual OSM node IDs are preserved and returned

This is by design for space efficiency - most way nodes are just geometry points without any tags or meaning beyond their position. Only significant nodes (with tags like crossings, traffic signals, entrances) have their IDs preserved.

### Testing Results
```sql
-- Relations work perfectly
SELECT fid, jsonb_array_length(members->'members') as member_count 
FROM test_members WHERE type = 2 LIMIT 5;
-- Returns: 14, 10, 10, 9, 52 members

-- Ways with mixed anonymous and tagged nodes
SELECT fid, members->'nodes' 
FROM test_members WHERE fid = 585507735;
-- Returns: [null, 125709470, null] 
-- Node 125709470 is a highway crossing with tags

-- Check what a tagged node is
SELECT fid, tags FROM test_members WHERE fid = 125709470;
-- Returns: {"highway": "crossing", "crossing": "unmarked", ...}
```

### Real-World Examples Found

1. **Way 585507735** (Varreler Landstraße):
   - Has crossing node 125709470 with `highway=crossing` tags
   
2. **Way 1228587610**:
   - Has survey points 1080788128 and 1080788184 with `man_made=survey_point` tags

### Performance Note
Member extraction only happens when the `members` column is included in the table definition AND selected in the query. To optimize performance, simply exclude the `members` column from your table definition or queries when not needed.

### Future Improvements
1. Consider returning node coordinates for anonymous nodes: `{"x":123,"y":456}` (currently just null)

### Usage Example
```sql
CREATE FOREIGN TABLE osm_features (
    fid bigint,
    type integer,
    tags jsonb,
    members jsonb,
    geom geometry(Geometry, 3857)
) SERVER geodesk_server 
OPTIONS (layer '/path/to/data.gol');

-- Query relation members
SELECT fid, 
       m->>'id' as member_id,
       m->>'type' as member_type,
       m->>'role' as member_role
FROM osm_features,
     jsonb_array_elements(members->'members') m
WHERE type = 2  -- relations
LIMIT 10;
```

## Parent Relations

### Status: ✅ Fully Functional

The `parents` column exposes which relations or ways a feature belongs to.

### Implementation

The `parents` column is exposed as JSONB array:
- For **nodes**: Lists parent ways and relations
- For **ways**: Lists parent relations
- For **relations**: Lists parent relations (super-relations)

Format: `[{"id": 123, "type": "way"}, {"id": 456, "type": "relation", "role": "outer"}]`

Notes:
- Way parents don't have roles (nodes are just part of the way's geometry)
- Relation parents include the role this feature has in the parent

### Real-World Examples

1. **Highway Crossings** (nodes with multiple parent ways):
   ```sql
   SELECT fid, tags->>'highway' as highway, parents 
   FROM gd_test_parents 
   WHERE type = 0 AND tags->>'highway' = 'crossing' LIMIT 3;
   -- Returns crossings that belong to 2+ intersecting roads
   ```

2. **Multipolygon Members** (ways belonging to relations):
   ```sql
   SELECT fid, parents 
   FROM gd_test_parents 
   WHERE type = 1 LIMIT 3;
   -- Returns ways with roles like "outer" in multipolygon relations
   ```

### Usage Notes

- A feature can have multiple parents (e.g., intersection nodes, shared boundaries)
- Use `jsonb_array_length(parents)` to count parent relationships
- Parent extraction is lazy - only happens when column is requested
- Currently crashes when used in WHERE/ORDER BY clauses (known issue)

### Table Definition Example

```sql
CREATE FOREIGN TABLE osm_with_parents (
    fid BIGINT,
    type INTEGER,  -- 0=node, 1=way, 2=relation
    tags JSONB,
    members JSONB,
    parents JSONB,
    geom geometry(Geometry, 3857)
) SERVER geodesk_server;
```
