#ifndef SPAT_H
#define SPAT_H

#include "fmgr.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "lib/dshash.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
#include "utils/timestamp.h"
#include "utils/datum.h"
#include "utils/jsonb.h"
#include "common/hashfn.h"

/* -------------------- DSS --------------------
 * A Dynamicaly-Shared String (DSS) is a null-terminated char* stored in a DSA
 */

struct dss;
typedef struct dss dss;

extern dss dss_new(dsa_area* dsa, const text* txt);
extern dss dss_new_extended(dsa_area* dsa, const char* str, Size len);
extern void dss_free(dsa_area* dsa, dss* dss);

extern text* dss_to_text(dsa_area* dsa, dss dss);

extern uint32_t hash_murmur3(const void* key, size_t len, void* arg);

/* necessary for dshash_parameters where dshash keys are dss */
extern int dss_cmp_arg(const void* a, const void* b, size_t size, void* arg);
extern int dss_cmp(const void* a, const void* b, size_t size, void* arg);

extern dshash_hash dss_hash_arg(const void* key, size_t size, void* arg);
extern dshash_hash dss_hash(const void* key, size_t size, void* arg);

extern void dss_cpy_arg(void* dest, const void* src, size_t size, void* arg);
extern void dss_copy(void* dest, const void* src, size_t size, void* arg);

/* -------------------- SpatDB -------------------- */

struct SpatDB;
typedef struct SpatDB SpatDB;

typedef enum spValueType
{
    SPVAL_INVALID,
    SPVAL_NULL, /* not in DB */
    SPVAL_STRING,
    SPVAL_SET,
    SPVAL_LIST,
    SPVAL_HASH
} spValueType;

extern const char* spTypeName(spValueType t);

struct SpatDBEntry;
typedef struct SpatDBEntry SpatDBEntry;

/* syntactic sugar for the fact that dshash functions use bool exclusive as argument */
typedef bool SPDB_LOCK_TYPE ;
#define SPDB_ENTRY_LOCK_EXCLUSIVE true
#define SPDB_ENTRY_LOCK_SHARED false

/* Finding, creating, deleting entries. Like dshash.h*/
extern bool spdb_is_attached(SpatDB* db);
extern SpatDBEntry* spdb_find(SpatDB* db, dss key, SPDB_LOCK_TYPE exclusive);
extern SpatDBEntry* spdb_find_or_insert(SpatDB* db, dss key, bool* found);

extern void spdb_release_lock(SpatDB* db, SpatDBEntry* entry);

#endif
