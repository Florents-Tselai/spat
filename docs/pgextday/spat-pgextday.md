## spat

**Hijacking Shared Memory for a Redis-Like Experience in PostgreSQL**

[github.com/Florents-Tselai/spat](https://github.com/Florents-Tselai/spat)

[spat.tsel.ai](https://spat.tsel.ai)

Florents Tselai

[tselai.com](https://tselai.com)

## TL;DR

``` sql
CREATE EXTENSION spat;

SET spat.db = 'spat-default';
SELECT SPSET('key', 'value');
SELECT SPGET('key'); -- value

SELECT LPUSH('list1', 'elem1');
SELECT LPOP('list1'); -- elem2

SELECT SADD('set1', 'elem1', 'elem2');
SELECT SISMEMBER('set1', 'elem1'); -- t

SELECT HSET('h1', 'f1', 'Hello');
SELECT HGET('h1', 'f1'); -- Hello
```

## About Me

Florents (Flo) Tselai | [tselai.com](https://tselai.com)

* Analytics & Data Engineering Background
* Domain-specific algos as extensions
    * e.g. correlation statistics [pgxicor](https://github.com/Florents-Tselai/pgxicor), [vasco](https://github.com/Florents-Tselai/vasco)
* Postgres in ETL pipelines
  * [pgJQ](https://github.com/Florents-Tselai/pgJQ)
  * [pgPDF](https://github.com/Florents-Tselai/pgPDF) PDF type for Postgres
* Doing more with `jsonpath` (reach out)

## Caching

* `SET <key> <value>`   `GET <key>`
* Memcached emerged as one of the earliest and most popular systems for this purpose
* Redis: data structure server.
  * Richer semantics on the value
  * Offered richer data types: lists, sets, hashes, sorted sets
  * Enabled more advanced caching patterns, rate-limiting queues, and pub/sub

## Shared Memory in Postgres

- Traditionally used internally for query execution, caching, and transaction management.
- Pre-allocated at startup and managed internally, not directly accessible to user code.
- PG10 Introduced dynamic shared memory areas (DSA): T. Munro & R. Haas [13df76a](https://github.com/postgres/postgres/commit/13df76a)

## DSM (Dynamic Shared Memory) Registry

- PG17 Introduced the **DSM Registry**: N. Bossart [8b2bcf3](https://github.com/postgres/postgres/commit/8b2bcf3)
- No more `shared_preload_libraries` & `shmem_request_hook`
- Assign a `string` name to each dynamic shared memory (DSM) segment. `dshash_table`
- Different backends can attach the segment using that `string`

## Spat

_Namespaces are one honking great idea – let’s do more of those!_

```sql
SET spat.db = 'db0';

SELECT SPSET('k1', 'v1');
SELECT SPGET('k1'); --v1
SELECT SP_DB_NITEMS(); --1

SET spat.db = 'another';
SELECT SP_DB_NITEMS(); --0

SET spat.db = 'db0';
SELECT DEL('key1');
SELECT SPGET('key1');
```

## `SpatDB`

```cpp
struct SpatDB
{
    LWLock lck;

    dsa_handle dsa_handle;
    dshash_table_handle htab_handle;

    dsa_pointer name;
    TimestampTz created_at;

    dsa_area* g_dsa;
    dshash_table* g_htab;
};
```

## `SpatDBEntry`

```cpp
struct SpatDBEntry
{
    dss key; /* pointer to a text* allocated in dsa */
    TimestampTz expireat;
    spValueType valtyp;

    union {
        dss string;
        struct {
            dshash_set_handle hndl;
            uint32 size;
        } set;
        struct{
            uint32 size;
            dsa_pointer head;
            dsa_pointer tail;
        } list;
        struct {
            dshash_table_handle hndl;
            uint32 size;
        } hash;
    } value;
}
```


## Lists

Cache last N items, pending jobs etc.

```sql
-- Task Queues (Background Jobs, ETL Pipelines)
SELECT LPUSH('email_queue', 'email_1');
SELECT LPUSH('email_queue', 'email_2');
SELECT LPOP('email_queue'); -- email_2 (or use RPUSH/LPOP for FIFO)

-- User Activity Feeds (Last N actions)
SELECT LPUSH('user:123:feed', 'login');
SELECT LPUSH('user:123:feed', 'purchase:item42');
SELECT LRANGE('user:123:feed', 0, 9); -- last 10 actions (WIP)
```

## Hashes

Store basic objects, groupings of counters

```sql
-- User Profiles (Session Info, Preferences)
SELECT HSET('user:123', 'name', 'Alice');
SELECT HSET('user:123', 'last_login', '2025-05-12');
SELECT HGET('user:123', 'name');

-- Counters Grouped per Dimension (Multi-field Metrics)
SELECT HSET('pageviews:product42', 'desktop', '100');
SELECT HSET('pageviews:product42', 'mobile', '200');

-- Configuration Maps (Feature Flags, AB Tests)
SELECT HSET('feature_flags', 'new_checkout', 'enabled');
SELECT HGET('feature_flags', 'new_checkout');
```

## Sets

* Track unique items (e.g., track all unique IP addresses accessing a given blog post).
* Perform common set operations such as intersection, unions, and differences).

```sql
-- Unique Visitors per Post (Approximate with Sets)
SELECT SADD('post:42:visitors', 'ip_1');
SELECT SADD('post:42:visitors', 'ip_2');
SELECT SCARD('post:42:visitors'); -- 2

-- Tagging System (Posts tagged with 'keto')
SELECT SADD('tag:keto', 'post_42');
SELECT SADD('tag:keto', 'post_99');
SELECT SISMEMBER('tag:keto', 'post_99'); -- t
```

## TTL

```sql
SELECT SPSET('expkey1', 'expvalue1', ttl=> '1 second');
SELECT TTL('expkey1') < '1 second' ;

SELECT pg_sleep(1);
SELECT SPGET('expkey1'); -- NULL
```

## Spat & ACID

* Spat operates in **shared memory**, outside PostgreSQL transactional control.
* Behaves like **Redis** – no MVCC, no WAL, no rollbacks.
* **Atomicity**: Immediate, non-rollbackable changes.
* **Consistency**: Per-key locks ensure in-memory consistency.
* **Isolation**: No transaction isolation – changes are visible across sessions instantly.
* **Durability**: In-memory only – data lost on crash or restart.
* **Concurrency**: Writers lock per key; multiple readers allowed.

## Considerations

* Still alpha: **don't use in production!**
* Faster than Redis/Valkey/*: Probably never.
* For SQL queries, unlogged tables could probably be
* _"Exposing data structures to queries is a bad idea. You're reinventing IMS from the 1960s."_ Famous, unnamed DB academic.

## Related & Future Work

- Speaking the Redis/Valkey wire-protocol (probably not)
- C-API for other extensions.
- Richer data structures and operations
- Improve monitoring of shared memory allocations [WIP by Rahila Syed](https://www.postgresql.org/message-id/flat/CAH2L28uGLhkXBKDWFKm5XZtp_0nNqpYQ3Hc35vG%2B%2BmM7wuOhgg%40mail.gmail.com#67c6e056cc682b36449ef242e28274e5)
- System view `pg_dsm_registry` [WIP by Florents Tselai](https://commitfest.postgresql.org/patch/5652/) . Like `pg_shmem_allocations`

## Thank you!

[florents@tselai.com](florents@tselai.com)
