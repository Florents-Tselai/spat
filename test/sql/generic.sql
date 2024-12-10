SET client_min_messages = ERROR;

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

-- GET
SELECT sset('key1', 'value1');
SELECT spget('key1');

SELECT del('key1');
SELECT spget('key1');
