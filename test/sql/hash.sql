/* -------------------- HASHES -------------------- */

SELECT HSET('h1', 'f1', 'Hello');
SELECT HSET('h1', 'f2', 'World');

SELECT HGET('h1', 'f1'); -- Hello
SELECT HGET('h1', 'f2'); -- World
SELECT HGET('h1', 'f3'); -- NULL


SELECT HGET('h3', 'f1'); -- NULL
