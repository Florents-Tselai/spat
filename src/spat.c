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
    SPVAL_INVALID = -1,
    SPVAL_NULL = 0,
    SPVAL_INTEGER = 1,
    SPVAL_STRING = 2
} valueType;

/* In-memory representation of an SPValue */
typedef struct SPValue
{
    valueType type;

    union
    {
        int32 integer;
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

    Oid valtypid;

    union
    {
        Datum val;
        struct
        {
            Size valsz;
            dsa_pointer valptr;
        } ref;
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


PG_FUNCTION_INFO_V1(spvalue_in);

Datum
spvalue_in(PG_FUNCTION_ARGS)
{
    elog(ERROR, "spval_in shouldn't be called");
    char* input = PG_GETARG_CSTRING(0);
    SPValue* result;

    /* Allocate memory for the spval struct */
    result = (SPValue*)palloc(sizeof(SPValue));

    /* Try parsing as an integer using int4in */
    PG_TRY();
        {
            result->value.integer = DatumGetInt32(DirectFunctionCall1(int4in, CStringGetDatum(input)));
            result->type = SPVAL_INTEGER;
            PG_RETURN_POINTER(result);
        }
    PG_CATCH();
        {
            /* Clear error and proceed to try other types */
            FlushErrorState();
        }
    PG_END_TRY();

    /* Default to varlena type (e.g., text) */
    result->type = SPVAL_STRING;
    result->value.varlena_val = cstring_to_text(input);

    PG_RETURN_POINTER(result);
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
    switch (input->type)
    {
    case SPVAL_INTEGER:
        appendStringInfo(&output, "%d", input->value.integer);
        break;
    case SPVAL_STRING:
        appendStringInfoString(&output, text_to_cstring(input->value.varlena_val));
        break;
    default:
        elog(ERROR, "Unknown spval type");
    }

    PG_RETURN_CSTRING(output.data);
}

/* ---------------------------------------- Global state ---------------------------------------- */

static SpatDB* g_spat_db = NULL; /* Current (working) database */
static dsa_area* g_dsa = NULL;
static dshash_table* g_htab = NULL;

/* TODO: maybe *g_dsa here too? */

/* ---------------------------------------- GUC Variables ---------------------------------------- */

static char* g_guc_spat_db_name = NULL;

void cleanup_dsa_on_exit(int code, Datum arg) {
    if (g_dsa != NULL) {
        dsa_detach(g_dsa);
        g_dsa = NULL;
    }
}



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

    on_proc_exit(cleanup_dsa_on_exit, (Datum) 0);
}


static const dshash_parameters default_hash_params = {
    .key_size = sizeof(dsa_pointer),
    .entry_size = sizeof(SpatDBEntry),
    .compare_function = dshash_memcmp,
    .hash_function = dshash_memhash,
    .copy_function = dshash_memcpy
};

static void
spat_init_shmem(void* ptr)
{
    SpatDB* db = (SpatDB*)ptr;

    int tranche_id = LWLockNewTrancheId();
    LWLockInitialize(&db->lck, tranche_id);

    dsa_area* dsa = dsa_create(tranche_id);
    dsa_pin(dsa);
    //dsa_pin_mapping() TOOD: maybe?

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

    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    g_dsa = dsa_attach(g_spat_db->dsa_handle);
    LWLockRelease(&g_spat_db->lck);

    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    g_htab = dshash_attach(g_dsa, &default_hash_params, g_spat_db->htab_handle, NULL);
    LWLockRelease(&g_spat_db->lck);
}

PG_FUNCTION_INFO_V1(spat_db_name);

Datum
spat_db_name(PG_FUNCTION_ARGS)
{
    dsa_handle dsa_handle;
    dsa_area* dsa;
    char message[NAMEDATALEN];

    /* Attach to shared memory and acquire the lock */
    spat_attach_shmem();
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);


    LWLockRelease(&g_spat_db->lck);

    /* Attach to the DSA area */
    // dsa = dsa_attach(dsa_handle);

    /* Copy the message from shared memory */
    memcpy(message, dsa_get_address(g_dsa, g_spat_db->name), NAMEDATALEN);

    /* Ensure the message is null-terminated */
    message[NAMEDATALEN - 1] = '\0';

    // dsa_detach(dsa);
    PG_RETURN_TEXT_P(cstring_to_text(message));
}

PG_FUNCTION_INFO_V1(spat_db_created_at);

Datum
spat_db_created_at(PG_FUNCTION_ARGS)
{
    TimestampTz result;

    spat_attach_shmem();
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);

    result = g_spat_db->created_at;

    LWLockRelease(&g_spat_db->lck);

    PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Builds an appropriate in the dsa, given a value Datum.
 * The key should have alerady been allocated and copied (e.g. by hash_find_or_insert).
 * Here we basically copy the value like datumCopy does (see src/backend/utils/adt/datum.c)
 * How we do that depends on the type of the value, and its size.
 */
void makeEntry(dsa_area* dsa, SpatDBEntry* entry, Interval* ex, bool found, Oid valueTypeOid, bool valueTypByVal, int16 valueTypLen,
               Datum value)
{
    /* Set the defaults */
    entry->expireat = SpMaxTTL; /* by-default should live forever */
    entry->valtypid = InvalidOid;

    entry->value.val = SpInvalidValDatum;

    entry->value.ref.valsz = SpInvalidValSize;
    entry->value.ref.valptr = InvalidDsaPointer;

    /* Begin by setting the metadata */
    if (ex)
    {
        /* if ttl interval is given */
        entry->expireat = DatumGetTimestampTz(DirectFunctionCall2(
            timestamptz_pl_interval,
            TimestampTzGetDatum(GetCurrentTimestamp()),
            PointerGetDatum(ex)));
    }

    if (valueTypByVal)
    {
        entry->valtypid = valueTypeOid;
        entry->value.val = value;
    }
    else if (valueTypLen == -1)
    {
        /* It is a varlena datatype */
        struct varlena* vl = (struct varlena*)DatumGetPointer(value);
        if (VARATT_IS_EXTERNAL_EXPANDED(vl))
        {
            /* Flatten into the caller's memory context */
            elog(ERROR, "expanded value types are not currently supported");

        }
        else
        {
            /* Otherwise, just copy the varlena datum verbatim */
            Size realSize = (Size) VARSIZE_ANY(value);
            entry->valtypid = valueTypeOid;
            entry->value.ref.valsz = realSize;
            entry->value.ref.valptr = dsa_allocate(dsa, realSize);

            memcpy(dsa_get_address(dsa, entry->value.ref.valptr), DatumGetPointer(value), realSize);

        }
    }
    else
    {
        /* Pass by reference, but not varlena, so not toasted */
        elog(ERROR, "Pass by reference, but not varlena value types are not currently supported");

    }
}

SPValue* makeSpvalFromEntry(dsa_area* dsa, SpatDBEntry* entry)
{
    SPValue* result;

    Assert(entry->valtypid == TEXTOID || entry->valtypid == JSONBOID || entry->valtypid == INT4OID);

    if (entry->valtypid == TEXTOID)
    {
        Assert(entry->valtypid == TEXTOID);
        result = (SPValue*)palloc(sizeof(SPValue));

        Size realSize = entry->value.ref.valsz;

        text* text_result = (text*)palloc(realSize);
        SET_VARSIZE(text_result, realSize);

        memcpy(text_result, dsa_get_address(dsa, entry->value.ref.valptr), realSize);

        result->type = SPVAL_STRING;
        result->value.varlena_val = text_result;
    }


    if (entry->valtypid == INT4OID)
    {
        result = (SPValue*)palloc(sizeof(SPValue));
        result->type = SPVAL_INTEGER;
        result->value.integer = DatumGetInt32(entry->value.val);
    }

    return result;
}

PG_FUNCTION_INFO_V1(spset_generic);

Datum
spset_generic(PG_FUNCTION_ARGS)
{
    /* Input */
    text* key = PG_GETARG_TEXT_PP(0);
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
    dsa_pointer dsa_key;
    dsa_pointer dsa_value;
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

    if (!OidIsValid(valueTypeOid))
        elog(ERROR, "Cannot determine type of value");

    get_typlenbyval(valueTypeOid, &valueTypLen, &valueTypByVal);
    elog(DEBUG1, "Value Type OID: %u, typByVal: %s, typLen: %d", valueTypeOid, valueTypByVal, valueTypLen);

    if (!(valueTypeOid == TEXTOID || valueTypeOid == INT4OID || valueTypeOid == JSONBOID))
        elog(ERROR, "Unsupported value type oid=%d", valueTypeOid);

    /* Begin processing */
    spat_attach_shmem();


    /* in dsa territory now */

    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(g_dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key));

    /* Debug */
    Assert(memcmp(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key)) == 0);
    elog(DEBUG1, "DSA allocated for key=%s", text_to_cstring(key));
    elog(DEBUG1, "Searching for key=%s", text_to_cstring(key));

    /* Insert the key */
    entry = dshash_find_or_insert(g_htab, dsa_get_address(g_dsa, dsa_key), &found);
    if (entry == NULL)
    {
        dsa_free(g_dsa, dsa_key);
        elog(ERROR, "dshash_find_or_insert failed, probably out-of-memory");
    }

    /* The entry should be there, time to populate its value accordingly */
    makeEntry(g_dsa, entry, ex, found, valueTypeOid, valueTypByVal, valueTypLen, value);

    /* Prepare the SPvalue to echo / return */
    SPValue* result = makeSpvalFromEntry(g_dsa, entry);

    dshash_release_lock(g_htab, entry);

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(spget);

Datum
spget(PG_FUNCTION_ARGS)
{
    /* Input */
    text* key = PG_GETARG_TEXT_PP(0);

    /* Output */
    SPValue* result;

    /* Processing */
    dsa_pointer dsa_key;
    bool found;
    SpatDBEntry* entry;

    /* Begin processing */
    spat_attach_shmem();

    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(g_dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key));

    Assert(memcmp(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key)) == 0);

    /* search for the key */
    entry = dshash_find(g_htab, dsa_get_address(g_dsa, dsa_key), false);
    if (entry == NULL)
        result = NULL;

    else
    {
        result = makeSpvalFromEntry(g_dsa, entry);
        dshash_release_lock(g_htab, entry);
    }


    if (!result)
        PG_RETURN_NULL();
    else
        PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(del);

Datum
del(PG_FUNCTION_ARGS)
{
    /* Input */
    text* key = PG_GETARG_TEXT_PP(0);

    /* Processing */
    dsa_pointer dsa_key;
    bool found;

    /* Begin processing */
    spat_attach_shmem();


    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(g_dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key));

    found = dshash_delete_key(g_htab, dsa_get_address(g_dsa, dsa_key));


    PG_RETURN_BOOL(found);
}


PG_FUNCTION_INFO_V1(getexpireat);
Datum
getexpireat(PG_FUNCTION_ARGS)
{
    /* Input */
    text *key = PG_GETARG_TEXT_PP(0);

    /* Output */
    TimestampTz result;

    /* Processing */
    dsa_pointer dsa_key;

    bool found;
    SpatDBEntry *entry;

    /* Attach shared memory and get handles */
    spat_attach_shmem();

    /* Allocate DSA memory for the key */
    dsa_key = dsa_allocate(g_dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(g_dsa, dsa_key), key, VARSIZE_ANY(key));

    /* Find the entry in the hash table */
    entry = dshash_find(g_htab, dsa_get_address(g_dsa, dsa_key), false);
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


    /* Return the result */
    if (!found || result == SpMaxTTL)
        PG_RETURN_NULL();
    PG_RETURN_TIMESTAMPTZ(result);

}

PG_FUNCTION_INFO_V1(sp_db_nitems);

Datum
sp_db_nitems(PG_FUNCTION_ARGS)
{
    int32 nitems = 0;

    /* Processing */
    SpatDBEntry* entry;

    /* Begin processing */
    spat_attach_shmem();

    dshash_seq_status status;

    /* Initialize a sequential scan (non-exclusive lock assumed here). */
    dshash_seq_init(&status, g_htab, false);

    while ((entry = dshash_seq_next(&status)) != NULL)
        nitems++;

    dshash_seq_term(&status);

    PG_RETURN_INT32(nitems);
}

PG_FUNCTION_INFO_V1(sp_db_size_bytes);
Datum sp_db_size_bytes(PG_FUNCTION_ARGS)
{
    Size result;

    SpatDBEntry* entry;

    /* Begin processing */
    spat_attach_shmem();

    result = dsa_get_total_size(g_dsa);


    PG_RETURN_INT64(result);
}