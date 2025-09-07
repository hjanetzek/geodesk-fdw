#!/bin/bash
# Download a small GOL file for testing

set -e

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DATA_DIR="${TEST_DIR}/data"

echo "Setting up test data..."

# Create test data directory
mkdir -p "${TEST_DATA_DIR}"

# Download a small GOL file (e.g., Malta - one of the smallest OSM extracts)
# Note: You may need to build this from a small PBF file using gol-tool
if [ ! -f "${TEST_DATA_DIR}/malta.gol" ]; then
    echo "Downloading Malta PBF (small test dataset)..."
    wget -q -O "${TEST_DATA_DIR}/malta.pbf" \
        "https://download.geofabrik.de/europe/malta-latest.osm.pbf"
    
    echo "Note: malta.pbf downloaded. You need to convert it to GOL format using gol-tool"
    echo "Example: gol build ${TEST_DATA_DIR}/malta.pbf ${TEST_DATA_DIR}/malta.gol"
else
    echo "Test GOL file already exists: ${TEST_DATA_DIR}/malta.gol"
fi

# Create a minimal test GOL file for CI (empty but valid structure)
# This is a placeholder - in real CI, you'd want a proper small GOL file
touch "${TEST_DATA_DIR}/test.gol"

echo "Test data setup complete"