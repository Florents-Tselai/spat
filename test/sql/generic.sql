SET client_min_messages = ERROR;

SELECT spat_db_name(); -- spat-default
-- switch to a new db
SET spat.db = 'test-spat';
SELECT spat_db_name(); -- test-spat
SELECT spat_db_created_at() NOTNULL;

-- switch back to the default to preserve my sanity when copy-pasting

SET spat.db = 'spat-default';

-- SET - DELETE - SIZE
SELECT sset('tkey1', 'dfgf');
SELECT sset('tkeygdfg1', 'dfgf');
SELECT sset('gdf', 'dfgf');
SELECT sset('gfd', 'dfgf');

SELECT sp_db_size(); --4

-- DELETE and inspect size
SELECT del('tkey1'); --existing
SELECT del('aaaa'); --not existing
SELECT sp_db_size(); --3
SELECT del('tkeygdfg1');
SELECT del('gdf');
SELECT del('gfd');
SELECT sp_db_size(); --0

-- SET / GET / DEL text
SELECT sset('key1', 'value1');
SELECT spget('key1');

SELECT del('key1');
SELECT spget('key1');

-- SET / GET / DEL text
SELECT sset('intkey1', 10);
SELECT spget('intkey1');

SELECT del('intkey1');
SELECT spget('intkey1');

-- SET / GET / DEL jsonb
SELECT sset('js', '{"product": "PostgreSQL", "version": 9.4, "jsonb": true}'::jsonb);
SELECT spget('js');

SELECT del('js');
SELECT spget('js');
