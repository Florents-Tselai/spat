# spat: Redis-like in-memory database embedded in Postgres

Redis is an in-memory database that persists on disk. 
The data model is key-value, 
but many different kind of values are supported: 
Strings, Lists, Sets, Sorted Sets, Hashes, Streams, HyperLogLogs, Bitmaps

**spat** is a data structure server embedded in Postgres.
It offers a Redis-like interface, backed by Postgres Dynamic Shared Memory (DSM).

## Usage 

### Multiple Databases 

You can switch between different databases (namespaces really),
by setting the `spat.name` GUC in a session.

```tsql
SET spat.name = 'db1';
```

> [!WARNING]
> Don't use this in production yet.

> [!CAUTION]
> You should assume that the data you cache, are visible
> and accessible even between different users in the same database cluster.

## Background

Spat relies on the two following features of Postgres

- PG10 Introduced dynamic shared memory areas (DSA) in [13df76a](https://github.com/postgres/postgres/commit/13df76a)
- PG17 Introduced the dynamic shared memory registry in [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)
