# spat: Redis-like in-memory database embedded in Postgres

**spat** is a Redis-like in-memory data structure server embedded in Postgres. 
The data model is key-value, but many different kinds of values are supported.

Data is stored in Postgres shared memory,
thus, you don't need an external caching service,
while at the same time, you can easily manage 
your caching layer within your SQL queries.

## Usage 

`SSET(key, value, ttl interval, nx bool, xx bool) → spval`

Set key to hold the string value. If key already holds a value, it is overwritten, regardless of its type.
Any previous time to live associated with the key is discarded on successful SET operation.
`ttl` sets the TTL interval. 
`nx` only set the key if it does not already exist.
`xx` only set the key if already exists.
Returns the value.

`SP_DB_SIZE() → integer`

Returns the current number of entries in the database

`DEL(key) → boolean`

Remove an entry by key.  Returns true if the key was found and the corresponding entry was removed.

`SCAN() → setof text`

Iterates over the key names in the database

`PEXPIRETIME(key) → timestamptz`

Returns the expiration timestamp of a key 

`TTL(key) → interval`

Returns the TTL interval of a key

`PERSIST(key)`

Removes the expiration time of a key

`EXISTS(key) → bool`

Determines whether a key exists

`SCOPY(key, key)`

Copies the value of a key to a new key

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
