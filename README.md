# spat: Redis-like In-Memory DB Embedded in Postgres

[![Github](https://img.shields.io/static/v1?label=GitHub&message=Repo&logo=GitHub&color=green)](https://github.com/Florents-Tselai/spat)
[![Build Status](https://github.com/Florents-Tselai/spat/actions/workflows/build.yml/badge.svg)](https://github.com/Florents-Tselai/spat/actions)
[![Docker Pulls](https://img.shields.io/docker/pulls/florents/spat)](https://hub.docker.com/r/florents/spat)
[![License](https://img.shields.io/github/license/Florents-Tselai/spat?color=blue)](https://github.com/Florents-Tselai/spat?tab=AGPL-3.0-1-ov-file#readme)
[![Github Sponsors](https://img.shields.io/static/v1?label=Sponsor&message=%E2%9D%A4&logo=GitHub&color=green)](https://github.com/sponsors/Florents-Tselai/)

> [!CAUTION]
> Don't use in production (yet)!
> Delete operations especially can leave some clutter behind,
> but a server restart should clean them up. See [Considerations](#Considerations)

**spat** is a Redis-like in-memory data structure server embedded in Postgres.
Data is stored in Postgres shared memory.
The data model is key-value.
Keys are strings, but values can be strings, lists, sets, or hashes.

```sql
SELECT SPSET('key', 'value');
SELECT SPGET('key');

SELECT SADD('set1', 'elem1', 'elem2');
SELECT SISMEMBER('set1', 'elem1'); -- t

SELECT LPUSH('list1', 'elem1');
SELECT LPUSH('list1', 'elem2');
SELECT LPOP('list1')
```

With **spat**:
- You don't need to maintain an external caching server. This greatly reduces complexity.
- You can express powerful logic by embedding data structures like lists and sets
in your SQL queries.
- You can reduce your infrastructure costs by reusing server resources.

## Motivation

The goal is not to completely replace or recreate Redis within Postgres. 
Redis, however, has been proven to be (arguably) a tool that excels in the “20-80” rule:
most use 20% of its available functionality to support the 80% of use cases. 

My aim is to provide Redis-like semantics and data structures within SQL, 
offering good enough functionality to support that critical 20% of use cases. 
This approach simplifies state and data sharing across queries without the need to manage a separate cache service alongside the primary database.

## Getting Started

To quickly get a Spat instance up and running, pull and run the latest Docker image:

```bash
docker run --name spat -e POSTGRES_PASSWORD=password florents/spat:pg17
```

This will start a Spat instance with default user postgres and password password. You can then connect to the database using psql:

```bash
docker exec -it spat psql -U postgres
```

Then install the extension

```tsql
CREATE EXTENSION spat;
```

For other installation optionse see [Installation](#Installation)

## Usage 

> [!TIP]
> Development follows roughly TDD principles,
> thus, the best and most up-to-date documentation are the test cases in [test/sql](test/sql)

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

Compile and install the extension (supports Postgres 17+)

```sh
cd /tmp
git clone https://github.com/Florents-Tselai/spat.git
cd spat
make
make install # may need sudo
```
### MurmurHash3 

To use the MurmurHash3 hashing algorithm instead of Postgres' default (`tag_hash`)

``` shell
make all install PG_CPPFLAGS=-DSPAT_MURMUR3=1
```

You can also install it with [Docker](#docker)

### Docker

```sh
docker pull florents/spat:pg17
# or
docker pull florents/spat:0.1.0a0-pg17
```

## Considerations

Here are some points made by people who've seen an early version of this:

- What are your thoughts on making this subject to transactional boundaries / general transactional behavior.
- Benchmarks comparing to unlogged table+prepared statements? It can be surprisingly tricky to do a lot better than the conventional implementation, especially for reads.
- How to handle
- Exposing data structures to queries is a bad idea. You're reinventing IMS from the 1960s.
- A major benefit of a cache like Redis it's that its a separate server instance
- A weak point may lie in removing resources (locks and DSM segment) if users create/delete databases more frequently than usual. I know at least one ORM that does it in auto mode.

## Background

Spat relies on the two following features of Postgres

- PG10 Introduced dynamic shared memory areas (DSA) in [13df76a](https://github.com/postgres/postgres/commit/13df76a)
- PG17 Introduced the dynamic shared memory registry in [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)

Internally, it stores its data in a `dshash`: 
This is an open hashing hash table with a linked list at each table entry.  
It supports dynamic resizing to prevent the linked lists from growing too long on average.  
Currently, only growing is supported: the hash table never becomes smaller.

[//]: # (<img src="test/bench/plot.png" width="50%"/>)
