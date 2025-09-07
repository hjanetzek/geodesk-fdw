# OSM Data Model Access in GeoDesk

## Summary

Both GeoDesk Java and libgeodesk C++ provide access to the full OSM data model:
- **Relations**: Can access member features via `members()` method
- **Ways**: Can access constituent nodes via `nodes()` method  
- **Features**: Can access parent relations/ways via `parents()` method
- **Roles**: Members returned from relations include their role string

## Current FDW Limitations

The GeoDesk FDW currently only exposes:
- Feature ID (fid)
- Feature type (node/way/relation)
- Tags (as JSONB)
- Geometry (as PostGIS geometry)
- is_area flag

It does NOT expose:
- Relation member IDs and roles
- Way node IDs
- Parent relation/way IDs

## API Capabilities

### libgeodesk C++ API

```cpp
// For Relations:
Features members() const;                 // Get all members
Features members(const char* query);      // Filtered members  

// For Ways:
Nodes nodes() const;                     // Get all nodes
Nodes nodes(const char* query);          // Filtered nodes

// For any Feature:
Features parents() const;                 // Get parent ways/relations
StringValue role() const;                 // Role in parent relation
```

### GeoDesk Java API

```java
// Similar API in Java:
Features members();                       // Get relation members
Features members(String query);           // Filtered members
Features nodes();                         // Get way nodes
Features parents();                       // Get parent features
String role();                           // Role in parent
```

## Potential FDW Extensions

### Option 1: Add Array Columns
Add PostgreSQL array columns to expose IDs:
```sql
CREATE FOREIGN TABLE gd_features (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    -- New columns:
    member_ids bigint[],        -- For relations
    member_roles text[],        -- For relations  
    node_ids bigint[]          -- For ways
);
```

### Option 2: Separate Tables with Relationships
Create separate tables for membership relationships:
```sql
-- Relation members table
CREATE FOREIGN TABLE gd_relation_members (
    relation_id bigint,
    member_id bigint,
    member_type integer,
    role text,
    sequence integer
);

-- Way nodes table  
CREATE FOREIGN TABLE gd_way_nodes (
    way_id bigint,
    node_id bigint,
    sequence integer
);
```

### Option 3: JSONB for Structural Data
Encode structural data as JSONB:
```sql
CREATE FOREIGN TABLE gd_features (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    -- New column:
    structure jsonb  -- Contains members/nodes/roles
);

-- Example structure for relation:
-- {"members": [{"id": 123, "type": "way", "role": "outer"}, ...]}
-- Example structure for way:
-- {"nodes": [456, 789, ...]}
```

## Implementation Notes

The libgeodesk API provides iterators over members/nodes which return full Feature objects, not just IDs. To expose just IDs would require:

1. Iterating through members/nodes
2. Extracting IDs into arrays
3. Optionally extracting roles for relation members

Example code pattern:
```cpp
// For a relation
std::vector<int64_t> member_ids;
std::vector<std::string> member_roles;
for (Feature member : relation.members()) {
    member_ids.push_back(member.id());
    member_roles.push_back(member.role().toString());
}

// For a way
std::vector<int64_t> node_ids;
for (Node node : way.nodes()) {
    node_ids.push_back(node.id());
}
```

## Recommendation

Start with **Option 3 (JSONB structure)** as it:
- Is most flexible for varying data
- Doesn't require schema changes for different feature types
- Allows incremental addition of more OSM data model details
- Can be queried efficiently with PostgreSQL's JSONB operators

## Next Steps

1. ✅ Research complete - both APIs support full OSM data model access
2. ⏳ Implement JSONB structure column for member/node data
3. ⏳ Add configuration option to enable/disable structure column (performance)
4. ⏳ Test with complex relations and ways
5. ⏳ Document usage patterns and examples