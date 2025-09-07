# Parents Column ORDER BY Crash Investigation

## Problem
When using `ORDER BY parents` on the GeoDesk FDW table, PostgreSQL server crashed with specific nodes (e.g., 259654373, 259654332, 259654265, 259654266).

## Root Cause Analysis

### Initial Symptoms
- Simple queries with `parents` column worked fine
- Adding `ORDER BY parents` caused server crash
- Crash only occurred after ~200 rows were fetched

### Investigation Steps
1. **PostgreSQL Executor Analysis**: Found that `nodeSort.c` fetches ALL tuples before sorting (lines 147-154)
2. **Memory Context Analysis**: `tuplesort_puttupleslot` copies tuples into sort's memory context
3. **Direct JSONB vs String Parsing**: Tags uses direct JSONB construction, original parents used `jsonb_in` parsing
4. **Specific Node Testing**: Direct query on node 259654373 also crashed - not ORDER BY specific

### Root Cause
The crash occurred in the role extraction code when iterating through `parent.members()` to find what role the current feature has in its parent relation. This operation appears to cause memory corruption or access violations with certain parent-child relationships.

## Solution

### Temporary Fix (Implemented)
- Created `geodesk_parents_jsonb.cpp` for direct JSONB construction (avoiding string parsing)
- Removed role extraction code that iterates through parent.members()
- Parents column now returns simplified structure without roles

### Changes Made
1. **New file**: `src/geodesk_parents_jsonb.cpp` - Direct JSONB construction
2. **Modified**: `geodesk_fdw.c` - Use `geodesk_get_parents_jsonb_direct()` instead of JSON string parsing
3. **Updated**: Parent IDs stored as strings instead of numeric to avoid memory issues

## Current Status
- ✅ Parents column works without crashes
- ✅ ORDER BY parents works correctly
- ❌ Role information not included (temporarily disabled)

## TODO
- Investigate why `parent.members()` iteration causes crashes with specific nodes
- Re-implement role extraction with proper error handling
- Consider caching parent-child role relationships to avoid repeated iteration

## Test Cases
```sql
-- These queries now work without crashing:
SELECT fid, parents FROM gd_bremen WHERE fid = 259654373;
SELECT fid, parents FROM gd_bremen WHERE parents IS NOT NULL ORDER BY parents LIMIT 10;
```

## Output Format
Current format (without roles):
```json
[{"id": "24495994", "type": "way"}]
```

Future format (with roles):
```json
[{"id": "24495994", "type": "way", "role": "outer"}]
```