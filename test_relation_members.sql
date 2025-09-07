-- Test relation members functionality
-- Using Bremen data which has relations

DROP SERVER IF EXISTS bremen_test CASCADE;
CREATE SERVER bremen_test
FOREIGN DATA WRAPPER geodesk_fdw
OPTIONS (
    datasource '/home/jeff/work/geodesk/data/bremen.gol'
);

DROP FOREIGN TABLE IF EXISTS bremen_members CASCADE;
CREATE FOREIGN TABLE bremen_members (
    fid bigint,
    type integer,
    tags jsonb,
    members jsonb
) SERVER bremen_test;

-- Find relations
SELECT COUNT(*) as relation_count 
FROM bremen_members 
WHERE type = 2;

-- Get first 5 relations with their member data
SELECT 
    fid,
    tags->>'name' as name,
    tags->>'type' as rel_type,
    jsonb_array_length(members->'members') as member_count,
    members
FROM bremen_members
WHERE type = 2
  AND members IS NOT NULL
LIMIT 5;

-- Find multipolygon relations
SELECT 
    fid,
    tags->>'name' as name,
    jsonb_array_length(members->'members') as member_count
FROM bremen_members
WHERE type = 2
  AND tags->>'type' = 'multipolygon'
LIMIT 10;

-- Extract member details from a relation
SELECT 
    fid,
    tags->>'name' as name,
    m.member->>'id' as member_id,
    m.member->>'type' as member_type,
    m.member->>'role' as member_role
FROM bremen_members,
     jsonb_array_elements(members->'members') as m(member)
WHERE type = 2
  AND tags->>'type' = 'multipolygon'
LIMIT 20;