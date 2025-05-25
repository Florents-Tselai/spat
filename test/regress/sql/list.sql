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

SELECT DEL('list1');

SELECT DEL('list404');
