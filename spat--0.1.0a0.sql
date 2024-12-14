\echo Use "CREATE EXTENSION spat" to load this file. \quit

/* -------------------- DB Info -------------------- */

CREATE FUNCTION sp_db_size_bytes() RETURNS BIGINT AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION sp_db_size() RETURNS TEXT AS 'SELECT pg_size_pretty(sp_db_size_bytes())' LANGUAGE SQL;

CREATE FUNCTION sp_db_nitems() RETURNS INTEGER AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_name()                  RETURNS TEXT      AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_created_at()            RETURNS TIMESTAMP  AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- spvalue -------------------- */

/*
* spvalue is a shell type returned by GET and similar commands.
* It's not (currently?) used for input, only output.
* To the user it's merely a shell type to facilitate output
* and to be casted to other standard types (int, float, text, jsonb, vector etc.).
* spvalue_in shouldn't be called in practice (throws error).
* spvalue_out is called to display the underlying output.
* We use 8-byte alignment that works for all types:
* both fixed and varlena
*/

CREATE TYPE spvalue;

CREATE FUNCTION spvalue_in(cstring, oid, integer) RETURNS spvalue   AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION spvalue_out(spvalue)                RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE spvalue (
    INPUT = spvalue_in,
    OUTPUT = spvalue_out,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT = double -- 8-byte alignment
);

/* -------------------- SSET -------------------- */

CREATE FUNCTION spset_generic(key text, value anyelement, ttl interval default null, nx boolean default null, xx boolean default null) RETURNS spvalue AS 'MODULE_PATHNAME' LANGUAGE C;

/* text types */
CREATE FUNCTION spset(text, text,    ttl interval default null, nx boolean default null, xx boolean default null) RETURNS spvalue  AS 'SELECT spset_generic($1, $2::text, $3, $4, $5)' LANGUAGE SQL;

/* numeric types */
CREATE FUNCTION spset(text, integer,         ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS spvalue AS 'SELECT spset_generic($1, $2::integer, $3, $4, $5)' LANGUAGE SQL;

/* -------------------- GET -------------------- */

CREATE FUNCTION spget(text) RETURNS spvalue AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;

/* -------------------- SPTYPE -------------------- */

CREATE FUNCTION sptype(text) RETURNS text AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;

/* -------------------- DEL -------------------- */

CREATE FUNCTION del(text) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- TTL -------------------- */

CREATE FUNCTION getexpireat(text) RETURNS timestamptz AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION ttl(text) RETURNS INTERVAL AS 'select getexpireat($1) - now()' LANGUAGE SQL;
