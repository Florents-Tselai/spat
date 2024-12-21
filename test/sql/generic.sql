SET client_min_messages = ERROR;

SELECT spat_db_name(); -- spat-default
-- switch to a new db
SET spat.db = 'test-spat';
SELECT spat_db_name(); -- test-spat
SELECT spat_db_created_at() NOTNULL;

-- switch back to the default to preserve my sanity when copy-pasting

SET spat.db = 'spat-default';

/* -------------------- STRINGS / GENERIC -------------------- */

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

/* -------------------- SETS -------------------- */

SELECT SADD('set1', 'elem1');
SELECT SCARD('set1'); -- 1
SELECT SPTYPE('set1');
SELECT SADD('set1', 'elem2');
SELECT SADD('set1', 'elem2'); --duplicate
SELECT SADD('set1', 'elem3');
SELECT SCARD('set1'); -- 3

SELECT DEL('set1');
SELECT SCARD('set1');

SELECT SADD('set2', 'elem1');
SELECT SCARD('set2'); -- 1
SELECT SPTYPE('set2');
SELECT SADD('set2', 'elem2');
SELECT SADD('set2', 'elem2'); --duplicate
SELECT SADD('set2', 'elem3');
SELECT SCARD('set2'); -- 3
SELECT SPTYPE('set2');

SELECT SCARD('setxxxx');

SELECT SADD('set1', 'elem1'); -- remember we DEL set1 above
SELECT SISMEMBER('set1', 'elem1'); -- t
SELECT SISMEMBER('set1', 'elem4'); -- f
SELECT SISMEMBER('setxxxx', 'elem4'); -- f

-- SREM
SELECT SADD('remset1', 'elem1');
SELECT SCARD('remset1'); -- 1
SELECT SREM('remset1', 'nothere'); -- f
SELECT SISMEMBER('remset1', 'elem1'); -- t
SELECT SREM('remset1', 'elem1'); -- t
SELECT SISMEMBER('remset1', 'elem1'); -- f
SELECT SCARD('remset1'); -- 0

-- SPGET for set
SELECT SADD('set1', 'elem1');
SELECT SADD('set1', 'elem2');

SELECT SPGET('set1');

/* -------------------- LISTS -------------------- */

/* LPUSH -> LEN -> LPOP */
SELECT LPUSH('list1', 'elem1');
SELECT LLEN('list1'); -- 1
SELECT LPUSH('list1', 'elem2');
SELECT LLEN('list1'); -- 2

SELECT SPGET('list1');

SELECT LPOP('list1'); -- elem2
SELECT LLEN('list1'); -- 1

SELECT LPOP('list1'); -- elem1
SELECT LLEN('list1'); -- 0

SELECT LPOP('list1'); -- null
SELECT LLEN('list1'); -- 0

SELECT LPOP('list1'); -- null
SELECT LLEN('list1'); -- 0

SELECT SPGET('list1');

/* -------------------- HASHES -------------------- */

SELECT HSET('h1', 'f1', 'Hello');
SELECT HSET('h1', 'f2', 'World');

SELECT HGET('h1', 'f1'); -- Hello
SELECT HGET('h1', 'f2'); -- World
SELECT HGET('h1', 'f3'); -- NULL


SELECT HGET('h3', 'f1'); -- NULL

