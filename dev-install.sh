#!/bin/bash
# Development install script for geodesk_fdw
# Creates symlinks instead of copying files, allowing immediate testing of changes

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
PGXS=$($PG_CONFIG --pgxs)

# Source directory (where this script is located)
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo -e "${GREEN}GeoDesk FDW Development Install${NC}"
echo "Source directory: $SRCDIR"
echo "PostgreSQL lib directory: $PKGLIBDIR"
echo "PostgreSQL share directory: $SHAREDIR"
echo ""

# Function to create symlink with sudo if needed
create_symlink() {
    local source=$1
    local target=$2
    
    # Remove existing file/link if present
    if [ -e "$target" ] || [ -L "$target" ]; then
        echo -e "${YELLOW}Removing existing: $target${NC}"
        sudo rm -f "$target"
    fi
    
    # Create symlink
    echo -e "Linking: ${source##*/} -> $target"
    sudo ln -s "$source" "$target"
}

# Check if we need to build first
if [ ! -f "$SRCDIR/geodesk_fdw.so" ]; then
    echo -e "${YELLOW}Shared library not found. Building first...${NC}"
    make -C "$SRCDIR" clean
    make -C "$SRCDIR"
fi

# Create symlinks for the shared library
echo -e "\n${GREEN}Installing shared library...${NC}"
create_symlink "$SRCDIR/geodesk_fdw.so" "$PKGLIBDIR/geodesk_fdw.so"

# Create symlinks for SQL and control files
echo -e "\n${GREEN}Installing extension files...${NC}"
create_symlink "$SRCDIR/geodesk_fdw.control" "$SHAREDIR/extension/geodesk_fdw.control"
create_symlink "$SRCDIR/sql/geodesk_fdw--1.0.sql" "$SHAREDIR/extension/geodesk_fdw--1.0.sql"

# Create symlinks for bitcode files if they exist
if [ -d "$PKGLIBDIR/bitcode" ]; then
    echo -e "\n${GREEN}Installing bitcode files...${NC}"
    
    # Create bitcode directory structure
    sudo mkdir -p "$PKGLIBDIR/bitcode/geodesk_fdw/src"
    
    # Link bitcode files
    for bc_file in "$SRCDIR"/src/*.bc; do
        if [ -f "$bc_file" ]; then
            bc_name=$(basename "$bc_file")
            create_symlink "$bc_file" "$PKGLIBDIR/bitcode/geodesk_fdw/src/$bc_name"
        fi
    done
    
    # Generate index if llvm-lto is available
    if command -v llvm-lto &> /dev/null; then
        echo -e "${GREEN}Generating bitcode index...${NC}"
        cd "$PKGLIBDIR/bitcode"
        sudo llvm-lto -thinlto -thinlto-action=thinlink -o geodesk_fdw.index.bc \
            geodesk_fdw/src/*.bc 2>/dev/null || true
    fi
fi

echo -e "\n${GREEN}Development installation complete!${NC}"
echo ""
echo "You can now test the extension without reinstalling after each change."
echo "Just rebuild with 'make' and the symlinks will use the updated files."
echo ""
echo "To test in PostgreSQL:"
echo "  psql -d your_database -c 'CREATE EXTENSION geodesk_fdw;'"
echo ""
echo "To use with PGLite:"
echo "  psql -h localhost -p 55432 -d your_database"