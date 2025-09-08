-- Setup script for GeoDesk FDW
-- Creates the FDW server and foreign table with proper SRID configuration

-- Create the FDW extension if not exists
CREATE EXTENSION IF NOT EXISTS geodesk_fdw;

-- Drop existing objects if they exist (optional - comment out if you want to preserve)
-- DROP FOREIGN TABLE IF EXISTS gd_features CASCADE;
-- DROP SERVER IF EXISTS geodesk_server CASCADE;

-- Create the FDW server with the GOL file path
CREATE SERVER IF NOT EXISTS geodesk_server
    FOREIGN DATA WRAPPER geodesk_fdw
    OPTIONS (
        datasource '/home/jeff/work/geodesk/data/bremen.gol'
    );

-- Create the foreign table with SRID 3857 (Web Mercator)
-- Note: GeoDesk stores coordinates in integer format (millionths of a degree * 100)
-- The GeoDesk FDW automatically sets SRID 3857 for all geometries returned
--
-- IMPORTANT: For type filter pushdown to work, the 'type' column must be 
-- the second column in the table definition (after 'fid'). This is a known
-- limitation that will be fixed in a future version.
CREATE FOREIGN TABLE IF NOT EXISTS gd_features (
    fid bigint,          -- Column 1: Feature ID
    type integer,        -- Column 2: Feature type (0=node, 1=way, 2=relation)
    tags jsonb,          -- Column 3: Tags as JSONB
    geom geometry(Geometry, 3857),  -- Column 4: Geometry
    is_area boolean,     -- Column 5: True if way is an area
    members jsonb,       -- Column 6: Relation members / way nodes
    parents jsonb        -- Column 7: Parent relations/ways
)
SERVER geodesk_server;

-- Create indexes for better query performance (optional)
-- Note: These are PostgreSQL-side indexes, not pushed to GeoDesk
-- CREATE INDEX IF NOT EXISTS idx_gd_features_tags ON gd_features USING gin(tags);
-- CREATE INDEX IF NOT EXISTS idx_gd_features_geom ON gd_features USING gist(geom);

-- Example queries:
-- 
-- Count all buildings:
-- SELECT COUNT(*) FROM gd_features WHERE tags ? 'building';
--
-- Find buildings in a bounding box:
-- SELECT fid, tags->>'name', ST_Area(geom) as area
-- FROM gd_features 
-- WHERE tags ? 'building'
--   AND geom && ST_MakeEnvelope(8.7, 53.0, 8.9, 53.1, 3857);
--
-- Get all features of a specific type:
-- SELECT * FROM gd_features WHERE type = 1 AND is_area = true LIMIT 10;
--
-- Type values:
-- 0 = Node
-- 1 = Way  
-- 2 = Relation
--
-- is_area distinguishes between linear ways (false) and area ways/polygons (true)