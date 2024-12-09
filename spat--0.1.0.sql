\echo Use "CREATE EXTENSION spat" to load this file. \quit

CREATE FUNCTION spat_db_name()                  RETURNS TEXT      AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_created_at()            RETURNS TIMESTAMP  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_set_int(text, int)      RETURNS VOID       AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_get_int(text)           RETURNS INT         AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_type(text)              RETURNS TEXT        AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- DB Info -------------------- */

CREATE FUNCTION sp_db_size() RETURNS INTEGER AS 'MODULE_PATHNAME' LANGUAGE C;

/* -------------------- SSET -------------------- */

CREATE FUNCTION sset_generic(key text, value anyelement, ex interval default null, nx boolean default null, xx boolean default null) RETURNS anyelement AS 'MODULE_PATHNAME', 'sset_generic' LANGUAGE C;

/* text types */
CREATE FUNCTION sset(text, text,    ex interval default null, nx boolean default null, xx boolean default null) RETURNS text  AS 'SELECT sset_generic($1, $2::text, $3, $4, $5)' LANGUAGE SQL;

/* numeric types */
-- CREATE FUNCTION sset(text, smallint,        ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS smallint AS 'SELECT sset_generic($1, $2::smallint, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, integer,         ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS integer AS 'SELECT sset_generic($1, $2::integer, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, bigint,          ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS bigint AS 'SELECT sset_generic($1, $2::bigint, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, numeric,         ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS numeric AS 'SELECT sset_generic($1, $2::numeric, $3, $4, $5)' LANGUAGE SQL;
-- CREATE FUNCTION sset(text, real,            ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS real AS 'SELECT sset_generic($1, $2::real, $3, $4, $5)' LANGUAGE SQL;

/* binary types */
-- CREATE FUNCTION sset(text, bytea, ex interval DEFAULT null, nx boolean DEFAULT null, xx boolean DEFAULT null) RETURNS bytea AS 'SELECT sset_generic($1, $2::bytea, $3, $4, $5)' LANGUAGE SQL;

/* -------------------- spval -------------------- */

-- Shell type that can be casted to the supported types above
-- That's necessary because we can't just define get(text) returns anyelement.

CREATE TYPE spval;

/* -------------------- GET -------------------- */

-- CREATE FUNCTION get(text)  RETURNS spval AS 'MODULE_PATHNAME' LANGUAGE SQL;

/* -------------------- DEL -------------------- */

CREATE FUNCTION del(text) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C;
