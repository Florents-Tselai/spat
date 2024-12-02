-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION spat" to load this file. \quit

CREATE FUNCTION set_val_in_shmem(val INT) RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION get_val_in_shmem() RETURNS INT
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_set(text, text) RETURNS INT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION store_size() RETURNS INT
AS 'MODULE_PATHNAME' LANGUAGE C;
