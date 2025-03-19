-- TTL

SELECT SPSET('expkey1', 'expvalue1', ttl=> '1 second');
SELECT TTL('expkey1') < '1 second' ;
