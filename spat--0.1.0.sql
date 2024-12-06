\echo Use "CREATE EXTENSION spat" to load this file. \quit

CREATE FUNCTION spat_db_name()                  RETURNS TEXT      AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_created_at()            RETURNS TIMESTAMP  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_set_int(text, int)      RETURNS VOID       AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_get_int(text)           RETURNS INT         AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_set_string(text, text)   RETURNS VOID       AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_get_string(text)        RETURNS TEXT         AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION spat_db_type(text)              RETURNS TEXT        AS 'MODULE_PATHNAME' LANGUAGE C;


