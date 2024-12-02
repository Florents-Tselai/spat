# spat: Redis-like Data Structure Server in Postgres

**spat** is a data structure server embedded in Postgres.
It offers a Redis-like interface, backed by Postgres Dynamic Shared Memory (DSM).


## Multiple Databases 

You can toggle between different namespaces (= separate in-memory databases)

```tsql
SET spat.name = 'mydb1';
```

**spat** relies Postgres Dynamic Shared Memory Registry 
introduced in Postgres 17 
(see [8b2bcf3f](https://github.com/postgres/postgres/commit/8b2bcf3f) - [discussion](https://www.postgresql.org/message-id/flat/20231205034647.GA2705267%40nathanxps13))