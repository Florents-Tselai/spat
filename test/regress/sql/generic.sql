SET client_min_messages = ERROR;

SELECT SPAT_DB_NAME(); -- spat-default
-- switch to a new db
SET spat.db = 'test-spat';
SELECT SPAT_DB_NAME(); -- test-spat
SELECT SPAT_DB_CREATED_AT() NOTNULL;

-- switch back to the default to preserve my sanity when copy-pasting

SET spat.db = 'spat-default';

/* -------------------- STRINGS / GENERIC -------------------- */

-- SET - DELETE - SIZE
SELECT SPSET('tkey1', 'dfgf');
SELECT SPSET('tkeygdfg1', 'dfgf');
SELECT SPSET('gdf', 'dfgf');
SELECT SPSET('gfd', 'dfgf');

SELECT SP_DB_NITEMS(); --4

-- DELETE and inspect size
SELECT DEL('tkey1'); --existing
SELECT DEL('aaaa'); --not existing
SELECT SP_DB_NITEMS(); --3
SELECT DEL('tkeygdfg1');
SELECT DEL('gdf');
SELECT DEL('gfd');
SELECT SP_DB_NITEMS(); --0

-- SET / GET / DEL text
SELECT SPSET('key1', 'value1');
SELECT SPGET('key1');

SELECT DEL('key1');
SELECT SPGET('key1');

-- DB SIZE BYTES
SELECT SP_DB_SIZE_BYTES();
SELECT SP_DB_SIZE();

-- sptype
SELECT SPSET('key1', 'value1');
SELECT SPTYPE('key1');

SELECT SPTYPE('gsdgdf');
