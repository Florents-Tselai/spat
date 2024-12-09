# spat: Redis-like in-memory database embedded in Postgres

Redis is an in-memory database that persists on disk. 
The data model is key-value, 
but many different kind of values are supported: 
Strings, Lists, Sets, Sorted Sets, Hashes, Streams, HyperLogLogs, Bitmaps

**spat** is a data structure server embedded in Postgres.
It offers a Redis-like interface, backed by Postgres Dynamic Shared Memory (DSM).

## Usage 

- `sset(key, value, ttl interval, nx bool, xx bool) → spval`

  Set key to hold the string value. If key already holds a value, it is overwritten, regardless of its type.
  Any previous time to live associated with the key is discarded on successful SET operation.
  `ttl` sets the TTL interval. 
  `nx` only set the key if it does not already exist.
  `xx` only set the key if already exists.
  Returns the value.

- `sp_db_size() → integer`

  Returns the current number of entries in the database

- `del(key) → boolean`

  Remove an entry by key.  Returns true if the key was found and the corresponding entry was removed.

- `scan() → setof text`

  Iterates over the the key names in the database

- `pexpiretime(key) -> timestamptz`

  Returns the expiration timestamp of a key 

- `ttl(key) -> interval`

  Returns the TTL interval of a key

- `persist(key)`

  Removes the expiration time of a key

- `exists(key) -> bool`

  Determines whether a key exists

- `scopy(key, key)`

  Copies the value of a key to a new key

### Expiration (TTL)

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
