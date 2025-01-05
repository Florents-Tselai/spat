# spat: Redis-like In-Memory DB Embedded in Postgres

[![Build Status](https://github.com/Florents-Tselai/spat/actions/workflows/build.yml/badge.svg)](https://github.com/Florents-Tselai/spat/actions)

**spat** is a Redis-like in-memory data structure server embedded in Postgres.
Data is stored in Postgres shared memory.
The data model is key-value.
Keys are strings, but values can be strings, lists, sets or hashes.

With **spat**:
- You don't need to maintain an external caching server. This greatly reduces complexity.
- You can express powerful logic by embedding data structures like lists and sets
in your SQL queries.
- You can reduce your infrastructure costs by reusing server resources.

```sql
SELECT SPSET('key', 'value');
SELECT SPGET('key');

SELECT SADD('set1', 'elem1', 'elem2');
SELECT SISMEMBER('set1', 'elem1'); -- t

SELECT LPUSH('list1', 'elem1');
SELECT LPUSH('list1', 'elem2');
SELECT LPOP('list1')
```

## Usage 

spat `key`s are always `text`. 

Values can be strings (aka `text` Postgres type) or data structures containing strings (sets, lists etc.).
You can, however, pass `anyelement` to `SPSET,` and the value will be stored as a string according to the type's textual representation.

For example, to cache a `jsonb` object and get it back, you can do something like:

```sql
SELECT SPSET('k', '{"a": {"b": {"c": 1}}}'::jsonb);

SELECT SPGET('k')::text::jsonb;
```

If the value stored for a key is not a string,
it will return a human-friendly representation of the value.
Usually the data structure type and its current size.

### Strings

`SPSET(key, value[, echo bool, ttl interval]) → spval`

Set key to hold the value. 
If key already holds a value, it is overwritten, regardless of its type.
Any previous time to live associated with the key is discarded on successful SET operation.
The optional `ttl` sets the TTL interval.
Returns the value if `echo` is set. If not (the default), the function returns NULL.

`SPGET(key) → text`

Get the value of key. 
If the key does not exist NULL is returned. 
An error is returned if the value stored at key is not a string, because GET only handles string values.

### Sets

`SADD(key, member) → int`

Add the specified members to the set stored at key. 
Specified members that are already a member of this set are ignored. 
If key does not exist, a new set is created before adding the specified members.
Returns the number of elements that were added to the set, not including all the elements already present in the set.
An error is returned when the value stored at key is not a set

`SISMEMBER(key, member) → bool`

Returns if `member` is a member of the set stored at `key`.

`SREM(key, member) → int`

Remove the specified members from the set stored at key. 
Specified members that are not a member of this set are ignored. 
If key does not exist, it is treated as an empty set and this command returns 0.
Returns the number of members that were removed from the set, not including non existing members.
An error is returned when the value stored at key is not a set.

`SCARD(key) → int`

Returns the set cardinality (number of elements) of the set stored at key,
or 0 if the key does not exist.

### Lists

`LPUSH(key, element) → int`

Insert all the specified values at the head of the list stored at key. 
If key does not exist, it is created as empty list before performing the push operations. 
Returns the length of the list after the push operation.
When key holds a value that is not a list, an error is returned.

`LPOP(key, element) → text`

Removes and returns the first elements of the list stored at key.
Returns NULL if the key does not exist.

`LLEN(key) → int`

Returns the length of the list stored at key. If key does not exist, it is interpreted as an empty list and 0 is returned. An error is returned when the value stored at key is not a list.

### Hashes

`HSET(key, field, value)`

Sets the specified fields to their respective values in the hash stored at key.
This command overwrites the values of specified fields that exist in the hash. If key doesn't exist, a new key holding a hash is created.

`HGET(key, field) → text`

Returns the value associated with field in the hash stored at key.

### Generic

`SPTYPE(key) → text`

Returns the string representation of the type of the value stored at key. 
The different types that can be returned are: string, list, set, and hash.
Returns `NULL` when key doesn't exist.

`DEL(key) → boolean`

Remove an entry by key. 
Returns true if the key was found and the corresponding entry was removed.

`TTL(key) → interval`

Returns the TTL interval of a key

`GETEXPIREAT(key) → timestamptz`

Shorthand for `GETEXPIREAT(key) - NOW()`

`SP_DB_NITEMS() → integer`

Returns the current number of entries in the database.

`SP_DB_SIZE_BYTES() → bigint`

`SP_DB_SIZE() → text`

Size of the database in bytes and in human-friendly text.

### Multiple DBs 

A spat database is just a segment of Postgres' memory addressable by a name.
You can switch between different databases (namespaces really),
by setting the `spat.db` GUC during a session.
Subsequent operations will apply to that db only.

```tsql
SET spat.db = 'db1';
```

Once done you can switch back to `spat-default`.

```tsql
SET spat.db = 'spat-default';
```

## Installation

Compile and install the extension (supports Postgres 13+)

```sh
cd /tmp
git clone --branch v0.8.0 https://github.com/Florents-Tselai/spat.git
cd spat
make
make install # may need sudo
```
You can also install it with [Docker](#docker), [Homebrew](#homebrew), [PGXN](#pgxn)

### Docker

```sh
docker pull florents/spat:pg17
# or
docker pull florents/spat:0.1.0-pg17
```

### pgxn

Install from the [PostgreSQL Extension Network](https://pgxn.org/dist/spat) with:

```sh
pgxn install spat
```

### MurmurHash3 

To use the MurmurHash3 hasing algorithm instead of Postgres' default (`tag_hash`)

```shell
make all install PG_CPPFLAGS=-DSPAT_MURMUR3=1
```

### Docker

> [!NOTE]
> Don't use this in production yet.

> [!CAUTION]
> This is far from ready for production.
> There are definitely memory leak bugs in the code,
> which could potentially mess-up your shared memory
> and degrade performance.

> [!WARNING]
> You should assume that the data you cache, are visible
> and accessible even between different users in the same database cluster.

## Configuration

You can't just turn Postgres into an in-memory database.
But maybe, just maybe, you can get close enough by configuring it accordingly.

The first step would be to use `PGDATA=/dev/shm` or other similar memory-backed filesystem.
Below are a few more ideas in that direction. 

<details>
<summary>Sample Configuration</summary>

```sql
-- Disable Logging
ALTER SYSTEM SET logging_collector = 'off';

-- Minimize Temporary Disk Usage
ALTER SYSTEM SET work_mem = '1GB';
ALTER SYSTEM SET maintenance_work_mem = '10GB';

-- Disable WAL Writes
ALTER SYSTEM SET wal_level = 'none';
ALTER SYSTEM SET archive_mode = 'off';
ALTER SYSTEM SET synchronous_commit = 'off';
ALTER SYSTEM SET wal_writer_delay = '10min';
ALTER SYSTEM SET max_wal_size = '1GB'; -- Set a high threshold to minimize flushing
ALTER SYSTEM SET wal_buffers = '16MB'; -- Minimal size

--Disable Checkpoints
ALTER SYSTEM SET checkpoint_timeout = '10min'; -- Or higher
ALTER SYSTEM SET checkpoint_completion_target = '0'; -- Prevent intermediate flushes
ALTER SYSTEM SET max_wal_size = '128GB'; -- Keep WAL size large to delay checkpoints
      
-- Disable Auto-Vacuum
ALTER SYSTEM SET checkpoint_timeout = '10min'; -- Or higher
ALTER SYSTEM SET checkpoint_completion_target = '0'; -- Prevent intermediate flushes
ALTER SYSTEM SET max_wal_size = '128GB'; -- Keep WAL size large to delay checkpoints
      
-- Use Temporary Tablespaces in Memory
ALTER SYSTEM SET temp_tablespaces = '/dev/shm'; -- Or equivalent memory-backed filesystem
      
-- Disable Disk Writes for Statistics
ALTER SYSTEM SET stats_temp_directory = '/dev/shm';
      
-- Disable Background Writer
ALTER SYSTEM SET bgwriter_lru_maxpages = 0;
ALTER SYSTEM SET bgwriter_delay = '10min'; -- Delay any operations
```
</details>

## FAQ 

## Background

Spat relies on the two following features of Postgres

- PG10 Introduced dynamic shared memory areas (DSA) in [13df76a](https://github.com/postgres/postgres/commit/13df76a)
- PG17 Introduced the dynamic shared memory registry in [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)

Internally it stores its data in a `dshash`: 
this is an open hashing hash table, with a linked list at each table entry.  
It supports dynamic resizing, as required to prevent the linked lists from growing too long on average.  
Currently, only growing is supported: the hash table never becomes smaller.

<img src="test/bench/plot.png" width="50%"/>
