MODULE_big = geodesk_fdw
OBJS = src/geodesk_fdw.o src/geodesk_connection.o src/geodesk_lwgeom_builder.o src/geodesk_ring_assembler.o src/geodesk_options.o src/goql_converter.o src/type_filter.o src/geodesk_tags_jsonb.o src/geodesk_parents_jsonb.o

EXTENSION = geodesk_fdw
DATA = sql/geodesk_fdw--1.0.sql

REGRESS = basic

# C++ configuration for C++20
CXX = g++
CXXFLAGS = -std=c++20 -fPIC
CXXFLAGS += -w  # Suppress all warnings for now
CXXFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden

# Include paths
PG_CPPFLAGS = -I$(srcdir)/include
PG_CPPFLAGS += -I/home/jeff/work/geodesk/libgeodesk/include
PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/liblwgeom
PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/libpgcommon
PG_CPPFLAGS += -I/home/jeff/work/geodesk/postgis/postgis

# Libraries to link - PostGIS and GEOS are required
SHLIB_LINK = /home/jeff/work/geodesk/libgeodesk/build/libgeodesk.a
SHLIB_LINK += /home/jeff/work/geodesk/postgis/liblwgeom/.libs/liblwgeom.a
SHLIB_LINK += /home/jeff/work/geodesk/postgis/libpgcommon/lwgeom_pg.o
SHLIB_LINK += -lgeos_c -lproj -ljson-c -lSFCGAL -lstdc++

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Special rule for C++ source files with C++20
src/%.o: src/%.cpp
	$(CXX) -std=c++20 $(CXXFLAGS) $(PG_CPPFLAGS) -Wno-sign-compare -Wno-ignored-attributes -Wno-unknown-pragmas -Wno-reorder -Wno-tautological -I$(shell $(PG_CONFIG) --includedir-server) -c -o $@ $<

# Special rule for LLVM bitcode files from C++ sources
src/%.bc: src/%.cpp
	$(CLANG) -xc++ -std=c++20 $(BITCODE_CXXFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -Wno-sign-compare -Wno-ignored-attributes -Wno-unknown-pragmas -Wno-reorder -Wno-tautological -emit-llvm -c -o $@ $<

# Development targets
.PHONY: clean-all install-dev dev-install dev-uninstall test-basic test-compile

clean-all: clean
	rm -f src/*.o

install-dev: install
	@echo "GeoDesk FDW installed for development"

# Development install using symlinks
dev-install: all
	@./dev-install.sh

# Remove development symlinks
dev-uninstall:
	@./dev-uninstall.sh

test-basic: install
	$(MAKE) installcheck REGRESS=basic

# Test compilation with libgeodesk headers - show errors
test-compile:
	@echo "Testing C++ compilation with libgeodesk headers..."
	$(CXX) -std=c++20 $(CXXFLAGS) $(PG_CPPFLAGS) \
		-I$(shell $(PG_CONFIG) --includedir-server) \
		-c src/geodesk_connection_minimal.cpp -o src/geodesk_connection_minimal.o 2>&1 | head -100
