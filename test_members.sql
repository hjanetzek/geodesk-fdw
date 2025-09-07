-- Test script for members column functionality

-- Create test server with test data
DROP SERVER IF EXISTS test_server CASCADE;
CREATE SERVER test_server
FOREIGN DATA WRAPPER geodesk_fdw
OPTIONS (
    datasource '/home/jeff/work/geodesk/geodesk-fdw/test/data/test.gol'
);

-- Create foreign table with members column
DROP FOREIGN TABLE IF EXISTS test_members CASCADE;
CREATE FOREIGN TABLE test_members (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    members jsonb  -- NEW: OSM structure data
) SERVER test_server;

-- Test 1: Check if any relations exist
SELECT COUNT(*) AS relation_count 
FROM test_members 
WHERE type = 2;

-- Test 2: Check if any ways exist and get their node counts
SELECT fid, 
       jsonb_array_length(members->'nodes') as node_count
FROM test_members 
WHERE type = 1
LIMIT 5;

-- Test 3: Get first way with its nodes
SELECT fid, 
       members->'nodes' as nodes
FROM test_members
WHERE type = 1 
  AND members IS NOT NULL
LIMIT 1;

-- Test 4: Check for relations with members
SELECT fid,
       members->'members' as members
FROM test_members
WHERE type = 2
  AND members IS NOT NULL
LIMIT 1;

-- Test 5: Performance - query without members column
EXPLAIN (ANALYZE, BUFFERS)
SELECT fid, type, tags
FROM test_members
LIMIT 10;

-- Test 6: Performance - query with members column
EXPLAIN (ANALYZE, BUFFERS)
SELECT fid, type, tags, members
FROM test_members
LIMIT 10;