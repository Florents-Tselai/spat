# spat: Redis-like in-memory database embedded in Postgres

[![Build Status](https://github.com/Florents-Tselai/spat/actions/workflows/build.yml/badge.svg)](https://github.com/Florents-Tselai/spat/actions)

**spat** is a Redis-like in-memory data structure server embedded in Postgres. 
The data model is key-value, but many different kinds of values are supported.

Data is stored in Postgres shared memory.
thus, you don't need an external caching service,
while at the same time, you can easily manage 
your caching layer within your SQL queries.

## Usage 

### Strings

`SPSET(key, value, ttl interval) → spval`

Set key to hold the value. If key already holds a value, it is overwritten, regardless of its type.
Any previous time to live associated with the key is discarded on successful SET operation.
`ttl` sets the TTL interval.
`nx` only set the key if it does not already exist.
`xx` only set the key if already exists.
Returns the value.

### Sets

### Generic

`DEL(key) → boolean`

Remove an entry by key. 
Returns true if the key was found and the corresponding entry was removed.

`TTL(key) → interval`

Returns the TTL interval of a key

`SP_DB_NITEMS() → integer`

Returns the current number of entries in the database.

`SP_DB_SIZE_BYTES() → bigint`

`SP_DB_SIZE() → text`

Size of the database in bytes and in human-friendly text.

### Multiple Databases 

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

> [!NOTE]
> Don't use this in production yet.

> [!WARNING]
> This is far from ready for production.
> There are definitely memory leak bugs in the code,
> which could potentially mess-up your shared memory
> and degrade performance.

> [!WARNING]
> You should assume that the data you cache, are visible
> and accessible even between different users in the same database cluster.

## Configuration

To completely disable disk access 
in Postgres (including writes for checkpointing, WAL flushing, etc.), 
you would need to configure it in a way that ensures all operations are kept in memory.

You can get close to that if you tweak its configuration

Use `PGDATA=/dev/shm` or other similar memory-backed filesystem.

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
## FAQ 

#### What's the performance of this ?

I have no idea.

#### What about persistence ?

In databases persistence is the "D"urability in ACID.
Postgres occasionally flushes its WAL on disk,
but you can disable this with something like 

## Background

Spat relies on the two following features of Postgres

- PG10 Introduced dynamic shared memory areas (DSA) in [13df76a](https://github.com/postgres/postgres/commit/13df76a)
- PG17 Introduced the dynamic shared memory registry in [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)

Internally it stores its data in a `dshash`: 
this is an open hashing hash table, with a linked list at each table entry.  
It supports dynamic resizing, as required to prevent the linked lists from growing too long on average.  
Currently, only growing is supported: the hash table never becomes smaller.
