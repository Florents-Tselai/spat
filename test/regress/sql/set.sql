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

-- DEL set
SELECT DEL('set1');
SELECT DEL('set2');
SELECT DEL('set404');