-- Basic tests for GeoDesk FDW
-- This test suite verifies core functionality

-- Test extension creation
CREATE EXTENSION IF NOT EXISTS geodesk_fdw;

-- Test server creation
CREATE SERVER IF NOT EXISTS test_geodesk_server
FOREIGN DATA WRAPPER geodesk_fdw;

-- Test foreign table creation with a test GOL file
-- Note: :test_data_path will be replaced by the test runner
CREATE FOREIGN TABLE IF NOT EXISTS test_features (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean
) SERVER test_geodesk_server
OPTIONS (
    datasource :'test_data_path'
);

-- Test 1: Check extension is loaded
SELECT 1 AS test_1_extension_loaded
WHERE EXISTS (
    SELECT 1 FROM pg_extension WHERE extname = 'geodesk_fdw'
);

-- Test 2: Check foreign server exists
SELECT 1 AS test_2_server_exists
WHERE EXISTS (
    SELECT 1 FROM pg_foreign_server WHERE srvname = 'test_geodesk_server'
);

-- Test 3: Check foreign table exists
SELECT 1 AS test_3_table_exists
WHERE EXISTS (
    SELECT 1 FROM pg_foreign_table ft
    JOIN pg_class c ON ft.ftrelid = c.oid
    WHERE c.relname = 'test_features'
);

-- Test 4: Query the table 
SELECT COUNT(*) AS feature_count FROM test_features LIMIT 10;

-- Test 5: Test GOQL pushdown detection
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM test_features
WHERE tags->>'building' = 'yes';

-- Clean up
DROP FOREIGN TABLE IF EXISTS test_features;
DROP SERVER IF EXISTS test_geodesk_server CASCADE;

-- Report success
SELECT 'All basic tests completed' AS result;