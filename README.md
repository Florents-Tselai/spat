# spat: Redis-like in-memory database embedded in Postgres

Redis is an in-memory database that persists on disk. 
The data model is key-value, 
but many different kind of values are supported: 
Strings, Lists, Sets, Sorted Sets, Hashes, Streams, HyperLogLogs, Bitmaps

**spat** is a data structure server embedded in Postgres.
It offers a Redis-like interface, backed by Postgres Dynamic Shared Memory (DSM).


## Multiple Databases 

You can toggle between different in-memory databases (namespaces really)

```tsql
SET spat.name = 'db1';
```


**spat** relies Postgres Dynamic Shared Memory Registry 
introduced in Postgres 17 
(see [8b2bcf3f](https://github.com/postgres/postgres/commit/8b2bcf3f) - [discussion](https://www.postgresql.org/message-id/flat/20231205034647.GA2705267%40nathanxps13))