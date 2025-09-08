-- Integration tests for GeoDesk FDW
-- Requires a GOL file at test/data/test.gol

-- Setup
CREATE EXTENSION IF NOT EXISTS geodesk_fdw;
CREATE EXTENSION IF NOT EXISTS postgis;

-- Create server
CREATE SERVER IF NOT EXISTS geodesk_test_server
FOREIGN DATA WRAPPER geodesk_fdw;

-- Test different table configurations

-- 1. Basic table with minimal columns
CREATE FOREIGN TABLE IF NOT EXISTS test_basic (
    fid bigint,
    type integer,
    tags jsonb
) SERVER geodesk_test_server
OPTIONS (
    datasource 'test/data/test.gol'
);

-- 2. Full table with geometry and is_area
CREATE FOREIGN TABLE IF NOT EXISTS test_full (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    members jsonb,
    parents jsonb
) SERVER geodesk_test_server
OPTIONS (
    datasource 'test/data/test.gol'
);

-- 3. Table with GOQL filter
CREATE FOREIGN TABLE IF NOT EXISTS test_filtered (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean,
    members jsonb,
    parents jsonb
) SERVER geodesk_test_server
OPTIONS (
    datasource 'test/data/test.gol',
    query 'nw[name=*]'  -- Only named nodes and ways
);

-- Test queries (will only work with actual GOL data)

-- Test 1: Basic query
SELECT 'Test 1: Basic query' AS test;
SELECT COUNT(*) AS feature_count FROM test_basic LIMIT 1;

-- Test 2: Type filtering
SELECT 'Test 2: Type filtering' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full WHERE type = 0 LIMIT 5;

-- Test 3: Tag filtering with pushdown
SELECT 'Test 3: Tag pushdown' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full 
WHERE tags->>'highway' = 'primary';

-- Test 4: Spatial filtering
SELECT 'Test 4: Spatial filtering' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full
WHERE geom && ST_MakeEnvelope(0, 0, 100000, 100000, 3857);

-- Test 5: Combined filters
SELECT 'Test 5: Combined filters' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full
WHERE type = 1 
  AND tags->>'building' = 'yes'
  AND geom && ST_MakeEnvelope(0, 0, 100000, 100000, 3857);

-- Test 6: JSONB exists operator
SELECT 'Test 6: JSONB exists operator' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full
WHERE tags ? 'name';

-- Test 7: IS NOT NULL pattern
SELECT 'Test 7: IS NOT NULL pattern' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full
WHERE tags->>'amenity' IS NOT NULL;

-- Test 8: IN clause
SELECT 'Test 8: IN clause' AS test;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_full
WHERE type IN (0, 1);

-- Cleanup
DROP FOREIGN TABLE IF EXISTS test_basic;
DROP FOREIGN TABLE IF EXISTS test_full;
DROP FOREIGN TABLE IF EXISTS test_filtered;
DROP SERVER IF EXISTS geodesk_test_server CASCADE;

SELECT 'All integration tests completed' AS result;