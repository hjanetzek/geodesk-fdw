-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION geodesk_fdw" to load this file. \quit

-- Create the foreign data wrapper handler function
CREATE FUNCTION geodesk_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Create the foreign data wrapper validator function
CREATE FUNCTION geodesk_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Create the foreign data wrapper
CREATE FOREIGN DATA WRAPPER geodesk_fdw
  HANDLER geodesk_fdw_handler
  VALIDATOR geodesk_fdw_validator;

-- Grant usage to public by default (can be revoked if needed)
GRANT USAGE ON FOREIGN DATA WRAPPER geodesk_fdw TO public;

-- Utility functions for debugging and statistics
CREATE FUNCTION geodesk_fdw_version()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION geodesk_fdw_drivers()
RETURNS SETOF text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;