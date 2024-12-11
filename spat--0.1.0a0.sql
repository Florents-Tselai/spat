\echo Use "CREATE EXTENSION spat" to load this file. \quit

CREATE FUNCTION spat_db_name()                  RETURNS TEXT      AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_created_at()            RETURNS TIMESTAMP  AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- DB Info -------------------- */

CREATE FUNCTION sp_db_size() RETURNS INTEGER AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- spval -------------------- */

/*
* spval is a shell type returned by GET and similar commands.
* It's not (currently?) used for input, only output.
* To the user it's merely a shell type to facilitate output
* and to be casted to other standard types (int, float, text, jsonb, vector etc.).
* spval_in shouldn't be called in practice (throws error).
* spval_out is called to display the underlying output.
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

CREATE FUNCTION spval_example() RETURNS spvalue AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;

/* -------------------- SSET -------------------- */

CREATE FUNCTION sset_generic(key text, value anyelement, ttl interval default null, nx boolean default null, xx boolean default null) RETURNS spvalue AS 'MODULE_PATHNAME', 'sset_generic' LANGUAGE C;

/* text types */
CREATE FUNCTION sset(text, text,    ttl interval default null, nx boolean default null, xx boolean default null) RETURNS spvalue  AS 'SELECT sset_generic($1, $2::text, $3, $4, $5)' LANGUAGE SQL;

/* jsonb types */
CREATE FUNCTION sset(text, jsonb,    ttl interval default null, nx boolean default null, xx boolean default null) RETURNS spvalue  AS 'SELECT sset_generic($1, $2::jsonb, $3, $4, $5)' LANGUAGE SQL;

/* numeric types */
-- CREATE FUNCTION sset(text, smallint,        ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS smallint AS 'SELECT sset_generic($1, $2::smallint, $3, $4, $5)' LANGUAGE SQL;
CREATE FUNCTION sset(text, integer,         ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS spvalue AS 'SELECT sset_generic($1, $2::integer, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, bigint,          ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS bigint AS 'SELECT sset_generic($1, $2::bigint, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, numeric,         ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS numeric AS 'SELECT sset_generic($1, $2::numeric, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, real,            ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS real AS 'SELECT sset_generic($1, $2::real, $3, $4, $5)' LANGUAGE SQL;

/* binary types */
-- CREATE FUNCTION sset(text, bytea, ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS bytea AS 'SELECT sset_generic($1, $2::bytea, $3, $4, $5)' LANGUAGE SQL;

/* -------------------- GET -------------------- */

CREATE FUNCTION spget(text) RETURNS spvalue AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;

/* -------------------- SPTYPE -------------------- */

CREATE FUNCTION sptype(text) RETURNS text AS 'MODULE_PATHNAME' LANGUAGE SQL;

/* -------------------- DEL -------------------- */

CREATE FUNCTION del(text) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION sp_db_clear() RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- SCAN -------------------- */

-- CREATE FUNCTION spkeys() RETURNS text[] AS 'MODULE_PATHNAME' LANGUAGE C;

-- CREATE FUNCTION spscan() RETURNS setof text AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- Expiration / TTL -------------------- */

CREATE FUNCTION ttl(text) RETURNS timestamptz AS 'MODULE_PATHNAME' LANGUAGE C;


