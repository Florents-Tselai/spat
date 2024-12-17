/*--------------------------------------------------------------------------
 *
 * spat.c
 *		Redis-like in-memory database embedded in Postgres
 *
 * A SpatDB is just a segment of Postgres' memory addressable by a name.
 * Its data model is key-value.
 *
 * Keys are strings, values can have different types (data structures)
 * Both keys and values, underneath are traditional Datums,
 * stored in a DSA and allocated with dsa_allocate,
 * instead of palloc-ed in the CurrentMemoryContext.
 *
 * It uses a dshash_table to store its data internally.
 * This is an open hashing hash table, with a linked list at each table
 * entry.  It supports dynamic resizing, as required to prevent the linked
 * lists from growing too long on average.  Currently, only growing is
 * supported: the hash table never becomes smaller.
 *
 * Copyright (c) 2024, Florents Tselai
 *
 * IDENTIFICATION
 *		https://github.com/Florents-Tselai/spat
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

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


PG_MODULE_MAGIC;

#define SPAT_NAME_MAXSIZE NAMEDATALEN /* TODO: remove this */
#define SPAT_NAME_DEFAULT "spat-default"


/* ---------------------------------------- Types ---------------------------------------- */

/*
* spval is a shell type returned by GET and similar commands.
* To the user it's merely a shell type to facilitate output
* and to be casted to other types (int, float, text, jsonb, vector etc.)
* Internally it can store either pass-by-value fixed-length Datums
* or varlena datums.
*
* The actually in-memory representation though is SpatDBEntry.
* spval is created by such an entry.
* At some point spval could be used as a value type, but not yet.
*
* We set ALIGNMENT = double, 8-byte,
* as it satisfies both varlena, 8-byte int/float and 4-byte int/float
*/


typedef enum valueType
{
    SPVAL_INVALID,
    SPVAL_NULL, /* not in DB */
    SPVAL_STRING,
    SPVAL_SET
} valueType;

static const char* typename(valueType t) {
    switch (t) {
        case SPVAL_STRING:
            return "string";
        case SPVAL_INVALID:
            return "invalid";
        case SPVAL_NULL:
            return "null";
        case SPVAL_SET:
            return "set";
    }
}

/* In-memory representation of an SPValue */
typedef struct SPValue
{
    valueType valtyp;

    union
    {
        struct varlena* varlena_val;
    } value;
} SPValue;

#define SpInvalidValSize InvalidAllocSize
#define SpInvalidValDatum PointerGetDatum(NULL)
#define SpMaxTTL DT_NOEND

typedef struct SpatDBEntry
{
    /* -------------------- Key -------------------- */
    dsa_pointer key; /* pointer to a text* allocated in dsa */

    /* -------------------- Metadata -------------------- */

    TimestampTz expireat;

    /* -------------------- Value -------------------- */

    valueType valtyp;

    union
    {
        struct
        {
            Size valsz;
            dsa_pointer valptr;
        } ref;

        struct {
            dshash_table_handle  hshsethndl;
            uint32 card;
        } set;
    } value;
} SpatDBEntry;


typedef struct SpatDB
{
    LWLock lck;

    dsa_handle dsa_handle; /* dsa_handle to DSA area associated with this DB */
    dshash_table_handle htab_handle; /* htab_handle pointing to the underlying dshash_table */

    dsa_pointer name; /* Metadata about the db itself */
    TimestampTz created_at;
} SpatDB;

/* ---------------------------------------- GUC Variables ---------------------------------------- */

static char* g_guc_spat_db_name = NULL;

/* ---------------------------------------- Shared Memory Declarations ---------------------------------------- */

void _PG_init();
static void spat_init_shmem(void* ptr);
static void spat_attach_shmem(void);

/* ---------------------------------------- Global state ---------------------------------------- */

static const dshash_parameters default_hash_params = {
    .key_size = sizeof(dsa_pointer),
    .entry_size = sizeof(SpatDBEntry),
    .compare_function = dshash_memcmp,
    .hash_function = dshash_memhash,
    .copy_function = dshash_memcpy
};

static SpatDB* g_spat_db = NULL;
static dsa_area* g_dsa = NULL;
static dshash_table* g_htab = NULL;

/* ---------------------------------------- Shared Memory Implementation ---------------------------------------- */

void _PG_init()
{
    DefineCustomStringVariable("spat.db",
                               "Current DB name",
                               "long desc here",
                               &g_guc_spat_db_name,
                               SPAT_NAME_DEFAULT,
                               PGC_USERSET, 0,
                               NULL, NULL, NULL);

    MarkGUCPrefixReserved("spat");

//    on_proc_exit(spat_clean_on_exit, (Datum) 0);
}


static void
spat_init_shmem(void* ptr)
{
    SpatDB* db = (SpatDB*)ptr;

    int tranche_id = LWLockNewTrancheId();
    LWLockInitialize(&db->lck, tranche_id);

    dsa_area* dsa = dsa_create(tranche_id);
    dsa_pin(dsa);

    db->dsa_handle = dsa_get_handle(dsa);

    db->name = dsa_allocate0(dsa, SPAT_NAME_MAXSIZE); /* Allocate zeroed-out memory */
    memcpy(dsa_get_address(dsa, db->name), g_guc_spat_db_name, SPAT_NAME_MAXSIZE - 1);

    db->created_at = GetCurrentTimestamp();

    dshash_table* htab = dshash_create(dsa, &default_hash_params, NULL);
    db->htab_handle = dshash_get_hash_table_handle(htab);

    dsa_detach(dsa);
}

static void
spat_attach_shmem(void)
{
    bool found;

    g_spat_db = GetNamedDSMSegment(g_guc_spat_db_name,
                                   sizeof(SpatDB),
                                   spat_init_shmem,
                                   &found);
    LWLockRegisterTranche(g_spat_db->lck.tranche, g_guc_spat_db_name);

    if (!g_dsa) {
        Assert(!g_dsa);
        LWLockAcquire(&g_spat_db->lck, LW_SHARED);
        g_dsa = dsa_attach(g_spat_db->dsa_handle);
        LWLockRelease(&g_spat_db->lck);
    }

    if (!g_htab) {
        Assert(!g_htab);
        LWLockAcquire(&g_spat_db->lck, LW_SHARED);
        g_htab = dshash_attach(g_dsa, &default_hash_params, g_spat_db->htab_handle, NULL);
        LWLockRelease(&g_spat_db->lck);
    }

}

static void
spat_detach_shmem(void) {
    if (g_dsa) {
        dsa_detach(g_dsa);
        g_dsa = NULL;
    }

    if (g_htab) {
        dshash_detach(g_htab);
        g_htab = NULL;
    }
}

/* ---------------------------------------- Commands Common ---------------------------------------- */

dsa_pointer dsa_copy_to(dsa_area *dsa, void *val, Size valsz);

dsa_pointer dsa_copy_to(dsa_area *dsa, void *val, Size valsz)
{
    dsa_pointer allocedptr = dsa_allocate0(dsa, valsz);
    if (!DsaPointerIsValid(allocedptr))
        elog(ERROR, "dsa_allocate0 failed");

    memcpy(dsa_get_address(dsa, allocedptr), val, valsz);

    return allocedptr;
}

#define DSA_POINTER_TO_LOCAL(p) dsa_get_address(g_dsa, (p))

/* ---------------------------------------- Commands Implementation ---------------------------------------- */

PG_FUNCTION_INFO_V1(spvalue_in);
Datum
spvalue_in(PG_FUNCTION_ARGS)
{
    elog(ERROR, "spvalue_in shouldn't be called");
}

PG_FUNCTION_INFO_V1(spvalue_out);

Datum
spvalue_out(PG_FUNCTION_ARGS)
{
    SPValue* input = (SPValue*)PG_GETARG_POINTER(0);
    StringInfoData output;

    /* Initialize output string */
    initStringInfo(&output);

    /* Convert spval back to a string based on its type */
    switch (input->valtyp)
    {
        case SPVAL_STRING:
            appendStringInfoString(&output, text_to_cstring(input->value.varlena_val));
            break;
        case SPVAL_INVALID:
            appendStringInfoString(&output, "invalid");
            break;
        case SPVAL_NULL:
            appendStringInfoString(&output, "null");
            break;
        case SPVAL_SET:
            appendStringInfoString(&output, "set {}");
            break;
    }

    PG_RETURN_CSTRING(output.data);
}


PG_FUNCTION_INFO_V1(spat_db_name);

Datum
spat_db_name(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text(g_guc_spat_db_name));
}

PG_FUNCTION_INFO_V1(spat_db_created_at);

Datum
spat_db_created_at(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    TimestampTz result;

    LWLockAcquire(&g_spat_db->lck, LW_SHARED);

    result = g_spat_db->created_at;

    LWLockRelease(&g_spat_db->lck);

    spat_detach_shmem();
    PG_RETURN_TIMESTAMPTZ(result);
}

SPValue* makeSpvalFromEntry(dsa_area* dsa, SpatDBEntry* entry)
{
    SPValue* result;

    Assert(entry->valtyp == SPVAL_STRING);

    result = (SPValue*)palloc(sizeof(SPValue));

    Size realSize = entry->value.ref.valsz;

    text* text_result = (text*)palloc(realSize);
    SET_VARSIZE(text_result, realSize);

    memcpy(text_result, dsa_get_address(dsa, entry->value.ref.valptr), realSize);

    result->valtyp = SPVAL_STRING;
    result->value.varlena_val = text_result;

    return result;
}

PG_FUNCTION_INFO_V1(spset_generic);

Datum
spset_generic(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Input */
    text* key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    Datum value = PG_GETARG_DATUM(1);
    Interval* ex = PG_ARGISNULL(2) ? NULL : PG_GETARG_INTERVAL_P(2);
    bool nx = PG_ARGISNULL(3) ? NULL : PG_GETARG_BOOL(3);
    bool xx = PG_ARGISNULL(4) ? NULL : PG_GETARG_BOOL(4);

    /* Info about value */
    bool valueIsNull = PG_ARGISNULL(1);
    Oid valueTypeOid;
    bool valueTypByVal;
    int16 valueTypLen;

    /* Processing */
    dsa_pointer dsa_key= dsa_copy_to(g_dsa, key, keysz);

    bool exclusive = false; /* TODO: This depends on the xx / nx */
    bool found;
    SpatDBEntry* entry;

    /* Input validation */
    if (nx || xx)
        elog(ERROR, "nx and xx are not implemented yet");

    if (valueIsNull)
        elog(ERROR, "value cannot be NULL");

    /* Get value type info. We need this for the copying part */
    valueTypeOid = get_fn_expr_argtype(fcinfo->flinfo, 1);

    Assert(valueTypeOid == TEXTOID);

    /* Insert the key */
    entry = dshash_find_or_insert(g_htab, dsa_get_address(g_dsa, dsa_key), &found);
    if (entry == NULL)
    {
        dsa_free(g_dsa, dsa_key);
        elog(ERROR, "dshash_find_or_insert failed, probably out-of-memory");
    }

    if (ex)
    {
        /* if ttl interval is given */
        entry->expireat = DatumGetTimestampTz(DirectFunctionCall2(
                timestamptz_pl_interval,
                TimestampTzGetDatum(GetCurrentTimestamp()),
                PointerGetDatum(ex)));
    }

    entry->valtyp = SPVAL_STRING;
    entry->value.ref.valsz = VARSIZE_ANY(value);
    entry->value.ref.valptr = dsa_allocate(g_dsa, VARSIZE_ANY(value));
    memcpy(dsa_get_address(g_dsa, entry->value.ref.valptr), DatumGetPointer(value), VARSIZE_ANY(value));

    /* Prepare the SPvalue to echo / return */
    SPValue* result = makeSpvalFromEntry(g_dsa, entry);

    dshash_release_lock(g_htab, entry);

    spat_detach_shmem();

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(spget);

Datum
spget(PG_FUNCTION_ARGS)
{
    bool found = false;
    SPValue *result;

    /* Begin processing */
    spat_attach_shmem();
    text* key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    dsa_pointer dsa_key= dsa_copy_to(g_dsa, key, keysz);

    SpatDBEntry *entry = dshash_find(g_htab, dsa_get_address(g_dsa, dsa_key), false);
    if (entry) {
        found = true;
        result = makeSpvalFromEntry(g_dsa, entry);
        dshash_release_lock(g_htab, entry);
    }

    spat_detach_shmem();

    if (!found) {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(sptype);

Datum
sptype(PG_FUNCTION_ARGS)
{
    valueType result = SPVAL_NULL;

    /* Begin processing */
    spat_attach_shmem();
    text* key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    dsa_pointer dsa_key= dsa_copy_to(g_dsa, key, keysz);

    SpatDBEntry *entry = dshash_find(g_htab, dsa_get_address(g_dsa, dsa_key), false);
    if (entry) {
        result = entry->valtyp;
        elog(DEBUG1, "valtyp=%d", entry->valtyp);
        dshash_release_lock(g_htab, entry);
    }

    spat_detach_shmem();

    PG_RETURN_TEXT_P(cstring_to_text(typename(result)));

}

PG_FUNCTION_INFO_V1(del);

Datum
del(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    text* key = PG_GETARG_TEXT_PP(0);
    dsa_pointer dsa_key = dsa_copy_to(g_dsa, key, VARSIZE_ANY(key));

    bool found = dshash_delete_key(g_htab, DSA_POINTER_TO_LOCAL(dsa_key));

    spat_detach_shmem();
    PG_RETURN_BOOL(found);
}


PG_FUNCTION_INFO_V1(getexpireat);
Datum
getexpireat(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    text *key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    dsa_pointer dsa_key = dsa_copy_to(g_dsa, key, keysz);

    /* Output */
    TimestampTz result;

    bool found;
    SpatDBEntry *entry = dshash_find(g_htab, DSA_POINTER_TO_LOCAL(dsa_key), false);
    if (!entry)
    {
        found = false;
    }
    else
    {
        found = true;
        result = entry->expireat;
        dshash_release_lock(g_htab, entry);
    }

    spat_detach_shmem();
    if (!found || result == SpMaxTTL)
        PG_RETURN_NULL();
    PG_RETURN_TIMESTAMPTZ(result);
}

PG_FUNCTION_INFO_V1(sp_db_nitems);

Datum
sp_db_nitems(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    int32 nitems = 0;

    SpatDBEntry* entry;

    dshash_seq_status status;

    /* Initialize a sequential scan (non-exclusive lock assumed here). */
    dshash_seq_init(&status, g_htab, false);

    while ((entry = dshash_seq_next(&status)) != NULL)
        nitems++;

    dshash_seq_term(&status);

    spat_detach_shmem();
    PG_RETURN_INT32(nitems);
}

PG_FUNCTION_INFO_V1(sp_db_size_bytes);
Datum sp_db_size_bytes(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();
    Size result = -1;
    result = dsa_get_total_size(g_dsa);
    spat_detach_shmem();
    PG_RETURN_INT64(result);
}


/* ---------------------------------------- SETS ---------------------------------------- */

typedef struct set_element {
    char key[64];
} set_element;

static const dshash_parameters params_hashset = {
        .key_size = 64, /* Fixed size for keys */
        .entry_size = sizeof(set_element),
        .compare_function = dshash_strcmp,
        .hash_function = dshash_strhash,
        .copy_function = dshash_strcpy
};

PG_FUNCTION_INFO_V1(sadd);
Datum
sadd(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();
    text *key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    dsa_pointer dsa_key = dsa_copy_to(g_dsa, key, keysz);
    char *elemstr = text_to_cstring(PG_GETARG_TEXT_PP(1));

    /* Insert/find the set in the global hash table */
    bool dbentryfound;
    dshash_table *htab;
    dshash_table_handle htabhandl;

    SpatDBEntry *dbentry = dshash_find_or_insert(g_htab, DSA_POINTER_TO_LOCAL(dsa_key), &dbentryfound);

    if (!dbentryfound) {
        dbentry->valtyp = SPVAL_SET;
        htab = dshash_create(g_dsa, &params_hashset, NULL);
        htabhandl = dshash_get_hash_table_handle(htab);
        dbentry->value.set.hshsethndl = htabhandl;
        dbentry->value.set.card = 0;
    } else {
        /* Existing entry */
        Assert(dbentry->valtyp == SPVAL_SET);
        htabhandl = dbentry->value.set.hshsethndl;

        htab = dshash_attach(g_dsa, &params_hashset, htabhandl, NULL);
    }

    /* Insert the value into the set */
    bool setelementfound;
    set_element *elem = dshash_find_or_insert(htab, elemstr, &setelementfound);
    if (!setelementfound)
        dbentry->value.set.card++;

    elog(DEBUG1, "key=%s\tdbentryfound=%d\tsetelementfound=%d\tcard=%d",
         text_to_cstring(key), dbentryfound, setelementfound, dbentry->value.set.card);

    dshash_release_lock(htab, elem);
    dshash_detach(htab);

    dshash_release_lock(g_htab, dbentry);

    spat_detach_shmem();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(scard);
Datum
scard(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Input key and value */
    text *key = PG_GETARG_TEXT_PP(0);
    Size keysz = VARSIZE_ANY(key);
    dsa_pointer dsa_key = dsa_copy_to(g_dsa, key, keysz);

    uint32 result;
    bool isaset;
    SpatDBEntry *dbentry =  dshash_find(g_htab, DSA_POINTER_TO_LOCAL(dsa_key), false);

    if (dbentry) {
        if (dbentry->valtyp == SPVAL_SET) {
            isaset = true;
            result = dbentry->value.set.card;
        }
        else {
            isaset = false;
        }
        dshash_release_lock(g_htab, dbentry);
    }
    else {
        isaset = false;
    }
    spat_detach_shmem();

    if (isaset)
         PG_RETURN_INT32(result);
    else
        PG_RETURN_NULL();
}