/* -------------------- HASHES -------------------- */

SELECT HSET('h1', 'f1', 'Hello');
SELECT HSET('h1', 'f2', 'World');

SELECT HGET('h1', 'f1'); -- Hello
SELECT HGET('h1', 'f2'); -- World
SELECT HGET('h1', 'f3'); -- NULL

SELECT HGET('h2', 'f1'); -- NULL - no previous HSET('h2')

SELECT DEL('h1'); -- t
SELECT DEL('h2'); -- f not exists
