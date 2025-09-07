#!/bin/bash
# Development uninstall script for geodesk_fdw
# Removes symlinks created by dev-install.sh

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get PostgreSQL installation directories
PG_CONFIG=${PG_CONFIG:-pg_config}
PKGLIBDIR=$($PG_CONFIG --pkglibdir)
SHAREDIR=$($PG_CONFIG --sharedir)

echo -e "${YELLOW}GeoDesk FDW Development Uninstall${NC}"
echo "PostgreSQL lib directory: $PKGLIBDIR"
echo "PostgreSQL share directory: $SHAREDIR"
echo ""

# Function to remove symlink/file
remove_file() {
    local file=$1
    if [ -L "$file" ] || [ -e "$file" ]; then
        echo -e "Removing: $file"
        sudo rm -f "$file"
    fi
}

# Remove shared library
echo -e "${GREEN}Removing shared library...${NC}"
remove_file "$PKGLIBDIR/geodesk_fdw.so"

# Remove SQL and control files
echo -e "\n${GREEN}Removing extension files...${NC}"
remove_file "$SHAREDIR/extension/geodesk_fdw.control"
remove_file "$SHAREDIR/extension/geodesk_fdw--1.0.sql"

# Remove bitcode files
if [ -d "$PKGLIBDIR/bitcode/geodesk_fdw" ]; then
    echo -e "\n${GREEN}Removing bitcode files...${NC}"
    sudo rm -rf "$PKGLIBDIR/bitcode/geodesk_fdw"
    remove_file "$PKGLIBDIR/bitcode/geodesk_fdw.index.bc"
fi

echo -e "\n${GREEN}Development uninstall complete!${NC}"