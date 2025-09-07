#!/bin/bash
# Script to fetch and build dependencies for GeoDesk FDW

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="${SCRIPT_DIR}/deps"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Setting up dependencies for GeoDesk FDW ===${NC}"

# Create deps directory
mkdir -p "${DEPS_DIR}"
cd "${DEPS_DIR}"

# Fetch libgeodesk
echo -e "${YELLOW}Fetching libgeodesk...${NC}"
if [ ! -d "libgeodesk" ]; then
    git clone --depth 1 https://github.com/clarisma/libgeodesk.git
    cd libgeodesk
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    make -j$(nproc)
    cd ../..
    echo -e "${GREEN}✓ libgeodesk built successfully${NC}"
else
    echo -e "${YELLOW}libgeodesk already exists, skipping...${NC}"
fi

# Fetch PostGIS (we only need liblwgeom)
echo -e "${YELLOW}Fetching PostGIS for liblwgeom...${NC}"
if [ ! -d "postgis" ]; then
    # Use a specific version for stability
    POSTGIS_VERSION="3.4.1"
    wget -q "https://download.osgeo.org/postgis/source/postgis-${POSTGIS_VERSION}.tar.gz"
    tar -xzf "postgis-${POSTGIS_VERSION}.tar.gz"
    mv "postgis-${POSTGIS_VERSION}" postgis
    rm "postgis-${POSTGIS_VERSION}.tar.gz"
    
    cd postgis
    ./configure --without-raster --without-topology --without-address-standardizer --without-protobuf
    make -C liblwgeom
    make -C libpgcommon
    cd ..
    echo -e "${GREEN}✓ PostGIS liblwgeom built successfully${NC}"
else
    echo -e "${YELLOW}PostGIS already exists, skipping...${NC}"
fi

# Update Makefile paths
echo -e "${YELLOW}Updating Makefile with dependency paths...${NC}"
cd "${SCRIPT_DIR}"

# Create a backup of the original Makefile
cp Makefile Makefile.bak

# Update paths in Makefile
sed -i "s|PG_CPPFLAGS += -I/home/jeff/work/geodesk/libgeodesk/include|PG_CPPFLAGS += -I\$(srcdir)/deps/libgeodesk/include|" Makefile
sed -i "s|PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/liblwgeom|PG_CPPFLAGS += -I\$(srcdir)/deps/postgis/liblwgeom|" Makefile
sed -i "s|PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/libpgcommon|PG_CPPFLAGS += -I\$(srcdir)/deps/postgis/libpgcommon|" Makefile
sed -i "s|PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/postgis|PG_CPPFLAGS += -I\$(srcdir)/deps/postgis/postgis|" Makefile
sed -i "s|SHLIB_LINK = /home/jeff/work/geodesk/libgeodesk/build/libgeodesk.a|SHLIB_LINK = \$(srcdir)/deps/libgeodesk/build/libgeodesk.a|" Makefile
sed -i "s|SHLIB_LINK += /home/jeff/work/geodesk/postgis/liblwgeom/.libs/liblwgeom.a|SHLIB_LINK += \$(srcdir)/deps/postgis/liblwgeom/.libs/liblwgeom.a|" Makefile
sed -i "s|SHLIB_LINK += /home/jeff/work/geodesk/postgis/libpgcommon/lwgeom_pg.o|SHLIB_LINK += \$(srcdir)/deps/postgis/libpgcommon/lwgeom_pg.o|" Makefile

echo -e "${GREEN}✓ Makefile paths updated${NC}"
echo -e "${GREEN}=== Dependencies ready! ===${NC}"
echo -e "${YELLOW}You can now build the extension with: make${NC}"