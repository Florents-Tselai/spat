# spat: Redis-like In-Memory DB Embedded in Postgres

![GitHub Repo stars](https://img.shields.io/github/stars/Florents-Tselai/spat)
[![Github](https://img.shields.io/static/v1?label=GitHub&message=Repo&logo=GitHub&color=green)](https://github.com/Florents-Tselai/spat)
[![Build Status](https://github.com/Florents-Tselai/spat/actions/workflows/build.yml/badge.svg)](https://github.com/Florents-Tselai/spat/actions)
[![Docker Pulls](https://img.shields.io/docker/pulls/florents/spat)](https://hub.docker.com/r/florents/spat)
[![License](https://img.shields.io/github/license/Florents-Tselai/spat?color=blue)](https://github.com/Florents-Tselai/spat?tab=AGPL-3.0-1-ov-file#readme)

> [!CAUTION]
> This is stil in **alpha** and not production ready!
> Read notes on [ACID](#ACID) below.

**spat** is a Redis-like in-memory data structure server embedded in Postgres.
Data is stored in Postgres shared memory.
The data model is key-value.
Keys are strings, but values can be strings, lists, sets, or hashes.

```sql
SELECT SPSET('key', 'value');
SELECT SPGET('key'); -- value

SELECT LPUSH('list1', 'elem1');
SELECT LPUSH('list1', 'elem2');
SELECT LPOP('list1'); -- elem2

SELECT SADD('set1', 'elem1', 'elem2');
SELECT SISMEMBER('set1', 'elem1'); -- t

SELECT HSET('h1', 'f1', 'Hello');
SELECT HGET('h1', 'f1'); -- Hello
```

With **spat**:
- You don't need to maintain an external caching server. This greatly reduces complexity.
- You can cache and share expensive or static data across SQL queries without having to model them relationally.
- You can express powerful logic by embedding data structures like lists and sets
  in your SQL queries.
- You can reduce your infrastructure costs by reusing server resources.

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
docker pull florents/spat
# or with explicit version
docker pull florents/spat:0.1.0a4-pg17
```

## ACID
Since spat operates in PostgreSQL **shared memory**,
it does not follow standard PostgreSQL **transactional semantics** (e.g., **MVCC, WAL, rollbacks**).
Instead, it behaves similarly to an **in-memory key-value store** like **Redis**.

### Atomicity

Spat operations take effect **immediately** and cannot be rolled back.

```sql
BEGIN;
SELECT SPSET('foo', 'bar');
ROLLBACK;
SELECT SPGET('foo'); -- Will still return 'bar'
```
* Unlike regular PostgreSQL transactions, a ROLLBACK does not undo changes in spat.
* Once a key is set, it remains in memory until explicitly deleted.

### Consistency

* spat ensures **internal consistency** by using **per-entry locks** to prevent data corruption during concurrent updates.
* However, since it **bypasses WAL logging and MVCC**,
its updates do not integrate with **PostgreSQL’s consistency guarantees**.

### Isolation

PostgreSQL **transactions do not isolate shared memory changes** like regular tables

**Session 1**
```sql
BEGIN;
SELECT SPSET('key', 'A');
-- Transaction remains open...
```

**Session 2**
```sql
BEGIN;
SELECT SPGET('key'); -- Returns 'A', even though Session 1 hasn't committed
```

* In a standard database, **Session 2** would not see uncommitted changes from **Session 1**.
* With spat, changes are immediately **visible across all sessions**.

### Durability

* Since spat is **entirely in shared memory, data is lost on restart**.
* There is no disk persistence (yet), meaning:
  * PostgreSQL crash or restart wipes all spat data.
  * Unlike standard tables, spat **does not survive beyond the current instance.**

### Concurrency

* **Per-key locks** ensure that **only one session modifies a given key at a time**.
* Multiple readers are allowed, but **a writer will block other writes**.

## Motivation

The goal is not to completely replace or recreate Redis within Postgres.
Redis, however, has been proven to be (arguably) a tool that excels in the “20-80” rule:
Most use 20% of its available functionality to support the 80% of use cases.

I aim to provide Redis-like semantics and data structures within SQL,
offering good enough functionality to support that critical 20% of use cases.
This approach simplifies state and data sharing across queries without the need to manage a separate cache service alongside the primary database.

## Background

Spat relies on the two following features of Postgres

- PG10 Introduced dynamic shared memory areas (DSA) in [13df76a](https://github.com/postgres/postgres/commit/13df76a)
- PG17 Introduced the dynamic shared memory registry in [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)

Internally, it stores its data in a `dshash`:
This is an open hashing hash table with a linked list at each table entry.
It supports dynamic resizing to prevent the linked lists from growing too long on average.
Currently, only growing is supported: the hash table never becomes smaller.

## FAQ

**What is Spat?**

Spat is a Redis-like in-memory data structure server embedded in PostgreSQL, utilizing PostgreSQL's dynamic shared memory (DSA) to store and manage key-value pairs.

**How does Spat differ from Redis?**

Unlike Redis, which is a standalone in-memory data store, Spat operates entirely within PostgreSQL.

**Can I use Spat for caching?**

Yes. Since Spat stores data in memory, it is well-suited for caching scenarios where frequent access to key-value data is required.

**Does Spat persist data?**

No. Currently, Spat does not provide built-in persistence, meaning data is lost when PostgreSQL is restarted.

**Can I use Spat with multiple PostgreSQL sessions?**

Yes. Since Spat operates in shared memory, multiple PostgreSQL sessions can read and write to Spat concurrently.

**Is there a limit to how much data Spat can store?**

Spat’s storage capacity depends on PostgreSQL’s shared memory configuration (shared_buffers, work_mem, etc.).
You can adjust these settings based on your workload.

**How ACID-compliant is this ?**

Enough. But maybe not enough for you. See [ACID](#ACID)

[//]: # (<img src="test/bench/plot.png" width="50%"/>)
