SET client_min_messages = ERROR;

SELECT spat_db_name(); -- spat-default
-- switch to a new db
SET spat.db = 'test-spat';
SELECT spat_db_name(); -- test-spat
SELECT spat_db_created_at() NOTNULL;

-- switch back to the default to preserve my sanity when copy-pasting

SET spat.db = 'spat-default';

-- SET - DELETE - SIZE
SELECT spset('tkey1', 'dfgf');
SELECT spset('tkeygdfg1', 'dfgf');
SELECT spset('gdf', 'dfgf');
SELECT spset('gfd', 'dfgf');

SELECT sp_db_nitems(); --4

-- DELETE and inspect size
SELECT del('tkey1'); --existing
SELECT del('aaaa'); --not existing
SELECT sp_db_nitems(); --3
SELECT del('tkeygdfg1');
SELECT del('gdf');
SELECT del('gfd');
SELECT sp_db_nitems(); --0

-- SET / GET / DEL text
SELECT spset('key1', 'value1');
SELECT spget('key1');

SELECT del('key1');
SELECT spget('key1');

-- TTL

SELECT SPSET('expkey1', 'expvalue1', ttl=> '1 second');
SELECT ttl('expkey1') < '1 second' ;

-- DB SIZE BYTES
SELECT sp_db_size_bytes();
SELECT sp_db_size();

-- sptype
SELECT spset('key1', 'value1');
SELECT sptype('key1');

SELECT sptype('gsdgdf');