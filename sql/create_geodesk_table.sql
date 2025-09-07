-- Template script for creating GeoDesk FDW tables
-- Usage: Modify the table name, server name, and GOL file path as needed

-- Ensure the extension exists
CREATE EXTENSION IF NOT EXISTS geodesk_fdw;

-- Function to create a GeoDesk foreign table
-- Example usage:
-- SELECT create_geodesk_table('paris_buildings', '/data/paris_buildings.gol');
-- SELECT create_geodesk_table('berlin', '/data/berlin.gol', 'berlin_server');

CREATE OR REPLACE FUNCTION create_geodesk_table(
    table_name text,
    gol_path text,
    server_name text DEFAULT NULL
) RETURNS void AS $$
DECLARE
    srv_name text;
BEGIN
    -- Use provided server name or generate one based on table name
    srv_name := COALESCE(server_name, table_name || '_server');
    
    -- Create the server if it doesn't exist
    IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = srv_name) THEN
        EXECUTE format('CREATE SERVER %I FOREIGN DATA WRAPPER geodesk_fdw OPTIONS (datasource %L)',
                      srv_name, gol_path);
    ELSE
        -- Update the datasource if server already exists
        EXECUTE format('ALTER SERVER %I OPTIONS (SET datasource %L)',
                      srv_name, gol_path);
    END IF;
    
    -- Drop the table if it exists
    EXECUTE format('DROP FOREIGN TABLE IF EXISTS %I CASCADE', table_name);
    
    -- Create the foreign table
    EXECUTE format('
        CREATE FOREIGN TABLE %I (
            fid bigint,
            type integer,
            tags jsonb,
            geom geometry(Geometry, 3857),
            is_area boolean
        )
        SERVER %I',
        table_name, srv_name);
    
    RAISE NOTICE 'Created foreign table % using server % for GOL file %', 
                 table_name, srv_name, gol_path;
END;
$$ LANGUAGE plpgsql;

-- Examples for common GOL files:
-- Note: Adjust paths according to your actual GOL file locations

-- Bremen
-- SELECT create_geodesk_table('bremen', '/home/jeff/work/geodesk/data/bremen.gol');

-- Paris (full)
-- SELECT create_geodesk_table('paris', '/home/jeff/work/geodesk/data/paris.gol');

-- Paris buildings only
-- SELECT create_geodesk_table('paris_buildings', '/home/jeff/work/geodesk/data/paris_buildings.gol');

-- Create table with GOQL filter
-- You can add a goql_filter option to the table to pre-filter features:
/*
CREATE FOREIGN TABLE filtered_buildings (
    fid bigint,
    type integer,
    tags jsonb,
    geom geometry(Geometry, 3857),
    is_area boolean
)
SERVER geodesk_server
OPTIONS (
    goql_filter 'wa[building=*]'  -- Only building areas
);
*/

-- Useful queries:
/*
-- Count features by type
SELECT 
    CASE type 
        WHEN 0 THEN 'Node'
        WHEN 1 THEN 'Way'
        WHEN 2 THEN 'Relation'
    END as feature_type,
    CASE 
        WHEN is_area THEN 'Area'
        ELSE 'Linear'
    END as geometry_type,
    COUNT(*)
FROM your_table_name
GROUP BY type, is_area
ORDER BY type, is_area;

-- Find all unique keys in tags
SELECT DISTINCT jsonb_object_keys(tags) as key
FROM your_table_name
ORDER BY key;

-- Get bounding box of dataset
SELECT ST_Extent(geom) FROM your_table_name;
*/