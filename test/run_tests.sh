#!/bin/bash
# Test runner for GeoDesk FDW

set -e

# Get the absolute path to the test data
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DATA_PATH="${TEST_DIR}/data/test.gol"

# For GitHub Actions, the path would be different
if [ -n "$GITHUB_WORKSPACE" ]; then
    TEST_DATA_PATH="$GITHUB_WORKSPACE/test/data/test.gol"
fi

echo "Using test data at: $TEST_DATA_PATH"

# Check if test data exists
if [ ! -f "$TEST_DATA_PATH" ]; then
    echo "Test data not found at $TEST_DATA_PATH"
    echo "Creating test data..."
    
    # Create test data directory
    mkdir -p "$(dirname "$TEST_DATA_PATH")"
    
    # Download small test data or create empty GOL for basic tests
    # For now, we'll just touch the file as a placeholder
    touch "$TEST_DATA_PATH"
fi

# Run tests with psql, setting the test_data_path variable
# Use the appropriate psql command based on environment
if command -v pglite &> /dev/null; then
    # Use pglite if available
    PSQL_CMD="pglite psql --pgdata ${PGDATA:-/tmp/pgdata}"
else
    # Fall back to regular psql
    PSQL_CMD="psql"
fi

echo "Running basic tests..."
$PSQL_CMD -d test_db -v test_data_path="'$TEST_DATA_PATH'" -f "${TEST_DIR}/basic_test.sql"

echo "Tests completed!"