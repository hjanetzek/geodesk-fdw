# GeoDesk FDW

A PostgreSQL Foreign Data Wrapper for [GeoDesk](https://github.com/clarisma/libgeodesk) GOL files, enabling SQL queries on OpenStreetMap data with PostGIS geometry support.

## Features

- Direct SQL access to GOL (Geographic Object Library) files
- Full PostGIS geometry support with SRID 3857 (Web Mercator)
- GOQL filter pushdown for optimal performance
- JSONB tags for flexible OSM tag queries
- Support for all OSM types: nodes, ways, and relations
- Zero-copy geometry transfer using LWGEOM

## Prerequisites

- PostgreSQL 12 or later
- PostGIS 3.0 or later (for geometry support)
- C++ compiler with C++20 support (GCC 11+ or Clang 13+)
- Standard development tools (make, git)

## Building from Source

### Quick Build (Recommended)

```bash
# Clone this repository
git clone https://github.com/yourusername/geodesk-fdw.git
cd geodesk-fdw

# Fetch and build dependencies automatically
./fetch-dependencies.sh

# Build the extension
make clean && make

# Install
sudo make install
```

### Docker Build

```bash
# Build and run with Docker Compose
docker-compose up -d

# Connect to the database
psql -h localhost -p 5433 -U postgres -d osm

# Or use pgAdmin at http://localhost:8080
```

### Manual Build

If you prefer to build dependencies manually:

1. **Build libgeodesk**:
```bash
git clone https://github.com/clarisma/libgeodesk.git
cd libgeodesk
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
make -j$(nproc)
```

2. **Build PostGIS** (for liblwgeom):
```bash
wget https://download.osgeo.org/postgis/source/postgis-3.4.1.tar.gz
tar -xzf postgis-3.4.1.tar.gz
cd postgis-3.4.1
./configure --without-raster --without-topology
make -C liblwgeom
make -C libpgcommon
```

3. **Update Makefile** paths and build the extension

## Usage

### Create the Extension

```sql
CREATE EXTENSION geodesk_fdw;
```

### Create a Foreign Server

```sql
CREATE SERVER geodesk_server
    FOREIGN DATA WRAPPER geodesk_fdw
    OPTIONS (datasource '/path/to/your/data.gol');
```

### Create a Foreign Table

```sql
-- Note: Column order matters for type filter optimization
-- The 'type' column must be second (after 'fid') for pushdown to work
CREATE FOREIGN TABLE osm_data (
    fid bigint,                      -- OSM feature ID
    type integer,                    -- 0=node, 1=way, 2=relation
    tags jsonb,                      -- OSM tags as JSONB
    geom geometry(Geometry, 3857),  -- PostGIS geometry (Web Mercator)
    is_area boolean                  -- true for polygonal ways
) SERVER geodesk_server;
```

### Query Examples

```sql
-- Count all buildings
SELECT COUNT(*) FROM osm_data WHERE tags ? 'building';

-- Find restaurants in a bounding box
SELECT fid, tags->>'name' as name, tags->>'cuisine' as cuisine
FROM osm_data
WHERE tags->>'amenity' = 'restaurant'
  AND geom && ST_Transform(ST_MakeEnvelope(2.3, 48.85, 2.35, 48.87, 4326), 3857);

-- Get areas of parks
SELECT tags->>'name' as name, ST_Area(geom) as area_m2
FROM osm_data
WHERE tags->>'leisure' = 'park' AND is_area = true
ORDER BY ST_Area(geom) DESC
LIMIT 10;

-- Complex tag queries with JSONB operators
SELECT COUNT(*) FROM osm_data 
WHERE tags @> '{"highway": "residential"}'::jsonb
  AND tags ? 'name';
```

### Table Options

You can pre-filter features using GOQL (GeoDesk Query Language):

```sql
-- Create a table with only buildings
CREATE FOREIGN TABLE buildings (
    fid bigint,
    tags jsonb,
    geom geometry(Geometry, 3857)
) SERVER geodesk_server
OPTIONS (goql_filter 'wa[building=*]');  -- wa = ways and areas
```

## Filter Pushdown

The FDW automatically pushes down filters to libgeodesk for optimal performance:

- **Spatial filters**: `geom && bbox` uses libgeodesk's spatial index
- **Tag filters**: `tags->>'key' = 'value'` converts to GOQL `[key=value]`
- **Tag existence**: `tags ? 'key'` converts to GOQL `[key=*]`
- **Type filters**: `type = 1` uses GOQL type prefixes

## Performance

Typical query performance on a city-sized extract:

- Full scan: ~500ms for 7.5M features
- Tag filter: 10-50x faster than PostGIS
- Spatial filter: Uses built-in R-tree index
- Combined filters: Multiplicative speedup

## Known Limitations

1. **Type column position**: The `type` column must be the second column (after `fid`) for type filter pushdown to work. This is a known issue that will be fixed in a future version.

2. **Read-only**: This FDW is read-only. You cannot INSERT, UPDATE, or DELETE.

3. **No ANALYZE support**: Statistics gathering is not yet implemented.

## Troubleshooting

### Building Issues

If you encounter build errors:

1. Ensure all paths in the Makefile are correct
2. Check that libgeodesk was built with `-fPIC` flag
3. Verify PostGIS development files are installed
4. Make sure your compiler supports C++20

### Runtime Issues

Enable debug logging to troubleshoot queries:

```sql
SET client_min_messages TO DEBUG1;
```

### Common Errors

- **"could not open GOL file"**: Check file path and permissions
- **"invalid GOQL syntax"**: Check your goql_filter table option
- **Missing geometry**: Ensure PostGIS is installed and enabled

## Development

For development, use symlinks instead of copying files:

```bash
./dev-install.sh    # Install with symlinks
make                # Rebuild after changes
./dev-uninstall.sh  # Remove symlinks
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Note: While this extension is MIT licensed, it depends on:
- libgeodesk (LGPL v3) - linked statically
- PostGIS liblwgeom (GPL v2) - linked statically
- PostgreSQL (PostgreSQL License) - linked dynamically

When distributing binaries, ensure compliance with the respective licenses of these dependencies.

## Credits

- [libgeodesk](https://github.com/clarisma/libgeodesk) by Clarisma
- [PostGIS](https://postgis.net/) for geometry support
- [pgsql-ogr-fdw](https://github.com/pramsey/pgsql-ogr-fdw) - Inspiration for FDW structure and PostGIS integration
- [postgis-sfcgal](https://gitlab.com/Oslandia/SFCGAL) - Reference for direct LWGEOM construction approach
- PostgreSQL FDW API documentation

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## TODO

- [ ] Fix type column position requirement
- [ ] Add ANALYZE support for statistics
- [ ] Implement parallel scan support
- [ ] Add comprehensive test suite
- [ ] Support for multiple GOL files per server
