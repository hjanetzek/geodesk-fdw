#!/bin/bash
# Simple test script for GeoDesk FDW

set -e

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Database connection parameters
DB_HOST=${DB_HOST:-localhost}
DB_PORT=${DB_PORT:-5432}
DB_NAME=${DB_NAME:-test_geodesk}
DB_USER=${DB_USER:-postgres}

echo -e "${GREEN}=== Testing GeoDesk FDW Extension ===${NC}"

# Check if psql is available
if ! command -v psql &> /dev/null; then
    echo -e "${RED}Error: psql command not found${NC}"
    exit 1
fi

# Create test database
echo -e "${YELLOW}Creating test database...${NC}"
createdb -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" "$DB_NAME" 2>/dev/null || true

# Run tests
echo -e "${YELLOW}Running extension tests...${NC}"
psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" "$DB_NAME" << EOF
-- Enable PostGIS
CREATE EXTENSION IF NOT EXISTS postgis;

-- Enable GeoDesk FDW
CREATE EXTENSION IF NOT EXISTS geodesk_fdw;

-- Check extension is loaded
SELECT extname, extversion 
FROM pg_extension 
WHERE extname = 'geodesk_fdw';

-- Check FDW is available
SELECT fdwname, fdwowner::regrole 
FROM pg_foreign_data_wrapper 
WHERE fdwname = 'geodesk_fdw';

-- Test creating a server (without actual GOL file)
DROP SERVER IF EXISTS test_server CASCADE;
CREATE SERVER test_server
    FOREIGN DATA WRAPPER geodesk_fdw
    OPTIONS (datasource '/tmp/test.gol');

-- Test creating a foreign table
DROP FOREIGN TABLE IF EXISTS test_table;
CREATE FOREIGN TABLE test_table (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean
) SERVER test_server;

-- Check table structure
\d test_table

-- Clean up
DROP FOREIGN TABLE test_table;
DROP SERVER test_server;

SELECT 'All tests passed!' as result;
EOF

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed successfully!${NC}"
else
    echo -e "${RED}✗ Tests failed${NC}"
    exit 1
fi

# Optional: Clean up test database
echo -e "${YELLOW}Cleaning up...${NC}"
dropdb -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" "$DB_NAME" 2>/dev/null || true

echo -e "${GREEN}=== Test complete ===${NC}"