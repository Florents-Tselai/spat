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
 *
 * Supported Types
 * ---------------
 *
 * - text,
 * - smallint, integer, bigint
 * - jsonb (it's in-memory JsonbValue representation is stored)
 * - vector
 *
 *
 * Future Ideas
 * ------------
 * - Use dsa_unpin and a bgw to set a TTL for the whole DB.
 * - Richer dsa statistics
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
    SPVAL_STRING = 2,
    SPVAL_JSON = 3,
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

typedef dsa_pointer SPKey;
typedef struct SpatDBEntry
{
    /* -------------------- Key -------------------- */
    dsa_pointer key; /* pointer to a text* allocated in dsa */

    /* -------------------- Metadata -------------------- */

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
    int val;
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
    case SPVAL_JSON:
        /* Handle JSON type */
        {
            /* Convert the JSONB varlena to a C string */
            text* json_text = DatumGetTextP(DirectFunctionCall1(jsonb_out,
                PointerGetDatum(input->value.varlena_val)));
            appendStringInfoString(&output, text_to_cstring(json_text));
            pfree(json_text); /* Clean up the allocated text object */
        }
        break;
    default:
        elog(ERROR, "Unknown spval type");
    }

    PG_RETURN_CSTRING(output.data);
}

/* ---------------------------------------- Global state ---------------------------------------- */

static SpatDB* g_spat_db; /* Current (working) database */
/* TODO: maybe *g_dsa here too? */

/* ---------------------------------------- GUC Variables ---------------------------------------- */

static char* g_guc_spat_db_name = NULL;

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
}


static char* typ_name(valueType typ)
{
    switch (typ)
    {
    case SPVAL_INVALID:
        return "invalid";
    case SPVAL_NULL:
        return "null";
    case SPVAL_INTEGER:
        return "integer";
    case SPVAL_STRING:
        return "string";
    case SPVAL_JSON:
        return "json";
    default:
        return "unknown";
    }
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

    /* Initialize metadata */
    db->val = -1; /* Default value */

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

    /* Get the DSA handle */
    dsa_handle = g_spat_db->dsa_handle;

    LWLockRelease(&g_spat_db->lck);

    /* Attach to the DSA area */
    dsa = dsa_attach(dsa_handle);

    /* Copy the message from shared memory */
    memcpy(message, dsa_get_address(dsa, g_spat_db->name), NAMEDATALEN);

    /* Ensure the message is null-terminated */
    message[NAMEDATALEN - 1] = '\0';

    dsa_detach(dsa);
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
 */
void makeEntry(dsa_area* dsa, SpatDBEntry* entry, bool found, Oid valueTypeOid, bool valueTypByVal, int16 valueTypLen,
               Datum value)
{
    /* Set the defaults */
    entry->valtypid = InvalidOid;

    entry->value.val = SpInvalidValDatum;

    entry->value.ref.valsz = SpInvalidValSize;
    entry->value.ref.valptr = InvalidDsaPointer;


    /* Copying the value into dsa.
     * How we do that depends on the type of the value, and its size.
     * This should generally follow the logic of
     * Datum datumCopy(Datum value, bool typByVal, int typLen)
     * in src/backend/utils/adt/datum.c
     */

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

            // ExpandedObjectHeader *eoh = DatumGetEOHP(value);
            // Size		resultsize;
            // char	   *resultptr;
            //
            // resultsize = EOH_get_flat_size(eoh);
            // resultptr = (char *) palloc(resultsize);
            // EOH_flatten_into(eoh, resultptr, resultsize);
            // res = PointerGetDatum(resultptr);
        }
        else
        {
            /* Otherwise, just copy the varlena datum verbatim */

            entry->valtypid = valueTypeOid;
            entry->value.ref.valsz = VARSIZE_ANY(value);
            entry->value.ref.valptr = dsa_allocate(dsa, VARSIZE_ANY(value));

            memcpy(dsa_get_address(dsa, entry->value.ref.valptr), DatumGetPointer(value), VARSIZE_ANY(value));

            // Size		realSize;
            // char	   *resultptr;
            //
            // realSize = (Size) VARSIZE_ANY(vl);
            // resultptr = (char *) palloc(realSize);
            // memcpy(resultptr, vl, realSize);
            // res = PointerGetDatum(resultptr);
        }
    }
    else
    {
        /* Pass by reference, but not varlena, so not toasted */

        /* datumCopy */

        // Size		realSize;
        // char	   *resultptr;
        //
        // realSize = datumGetSize(value, typByVal, typLen);
        //
        // resultptr = (char *) palloc(realSize);
        // memcpy(resultptr, DatumGetPointer(value), realSize);
        // res = PointerGetDatum(resultptr);
    }
}

SPValue* makeSpval(dsa_area* dsa, SpatDBEntry* entry)
{
    SPValue* result;

    Assert(entry->valtypid == TEXTOID || entry->valtypid == JSONBOID || entry->valtypid == INT4OID);

    if (entry->valtypid == TEXTOID)
    {
        Assert(entry->valtypid == TEXTOID);

        result = (SPValue*)palloc(sizeof(SPValue));
        text* text_result = (text*)palloc(entry->value.ref.valptr);

        memcpy(VARDATA(text_result), VARDATA(dsa_get_address(dsa, entry->value.ref.valptr)),
               entry->value.ref.valsz - VARHDRSZ);
        SET_VARSIZE(text_result, entry->value.ref.valsz);

        result->type = SPVAL_STRING;
        result->value.varlena_val = text_result;
    }

    if (entry->valtypid == JSONBOID)
    {
        Assert(entry->valtypid == JSONBOID);

        result = (SPValue*)palloc(sizeof(SPValue));
        Jsonb* jsonb_result = (Jsonb*)palloc(entry->value.ref.valsz);

        /* Copy the JSONB data from the shared memory to the local memory */
        memcpy(jsonb_result, dsa_get_address(dsa, entry->value.ref.valptr), entry->value.ref.valsz);

        /* Set up the spval structure */
        result->type = SPVAL_JSON;
        result->value.varlena_val = (struct varlena*)jsonb_result; /* Store as varlena */
    }

    if (entry->valtypid == INT4OID)
    {
        result = (SPValue*)palloc(sizeof(SPValue));
        result->type = SPVAL_INTEGER;
        result->value.integer = DatumGetInt32(entry->value.val);
    }

    return result;
}

PG_FUNCTION_INFO_V1(sset_generic);

Datum
sset_generic(PG_FUNCTION_ARGS)
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
    dsa_handle dsa_handle;
    dsa_pointer dsa_key;
    dsa_pointer dsa_value;
    dsa_area* dsa;
    dshash_table_handle htab_handle;
    dshash_table* htab;
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
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    dsa_handle = g_spat_db->dsa_handle;
    htab_handle = g_spat_db->htab_handle;
    LWLockRelease(&g_spat_db->lck);

    /* in dsa territory now */
    dsa = dsa_attach(dsa_handle);
    htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(dsa, dsa_key), key, VARSIZE_ANY(key));

    Assert(memcmp(dsa_get_address(dsa, dsa_key), key, VARSIZE_ANY(key)) == 0);

    elog(DEBUG1, "DSA allocated for key=%s", text_to_cstring(key));

    elog(DEBUG1, "Searching for key=%s", text_to_cstring(key));
    /* search for the key */
    entry = dshash_find_or_insert(htab, dsa_get_address(dsa, dsa_key), &found);
    if (entry == NULL)
    {
        dsa_free(dsa, dsa_key);
        dsa_detach(dsa); // is this necessary?
        elog(ERROR, "dshash_find_or_insert failed, probably out-of-memory");
    }

    makeEntry(dsa, entry, found, valueTypeOid, valueTypByVal, valueTypLen, value);

    SPValue* result = makeSpval(dsa, entry);

    dshash_release_lock(htab, entry);

    /* leaving dsa territory */
    dsa_detach(dsa);

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
    dsa_handle dsa_handle;
    dsa_pointer dsa_key;
    dsa_pointer dsa_value;
    dsa_area* dsa;
    dshash_table_handle htab_handle;
    dshash_table* htab;
    bool found;
    SpatDBEntry* entry;

    /* Begin processing */
    spat_attach_shmem();
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    dsa_handle = g_spat_db->dsa_handle;
    htab_handle = g_spat_db->htab_handle;
    LWLockRelease(&g_spat_db->lck);

    /* in dsa territory now */
    dsa = dsa_attach(dsa_handle);
    htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(dsa, dsa_key), key, VARSIZE_ANY(key));

    Assert(memcmp(dsa_get_address(dsa, dsa_key), key, VARSIZE_ANY(key)) == 0);

    /* search for the key */
    entry = dshash_find(htab, dsa_get_address(dsa, dsa_key), false);
    if (entry == NULL)
        result = NULL;

    else
    {
        result = makeSpval(dsa, entry);
        dshash_release_lock(htab, entry);
    }

    /* leaving dsa territory */
    dsa_detach(dsa);

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
    dsa_handle dsa_handle;
    dsa_pointer dsa_key;
    dsa_area* dsa;
    dshash_table_handle htab_handle;
    dshash_table* htab;
    bool found;

    /* Begin processing */
    spat_attach_shmem();
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    dsa_handle = g_spat_db->dsa_handle;
    htab_handle = g_spat_db->htab_handle;
    LWLockRelease(&g_spat_db->lck);

    /* in dsa territory now */
    dsa = dsa_attach(dsa_handle);
    htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

    /* Allocate dsa space for key and copy it from local memory to that dsa*/
    dsa_key = dsa_allocate(dsa, VARSIZE_ANY(key));
    if (dsa_key == InvalidDsaPointer)
        elog(ERROR, "Could not allocate DSA memory for key=%s", text_to_cstring(key));
    memcpy(dsa_get_address(dsa, dsa_key), key, VARSIZE_ANY(key));

    found = dshash_delete_key(htab, dsa_get_address(dsa, dsa_key));

    /* leaving dsa territory */
    dsa_detach(dsa);

    PG_RETURN_BOOL(found);
}

PG_FUNCTION_INFO_V1(get);

Datum
get(PG_FUNCTION_ARGS)
{
    text* key = PG_GETARG_TEXT_PP(0);

    PG_RETURN_TEXT_P(cstring_to_text("Hello World!"));
}

PG_FUNCTION_INFO_V1(sp_db_size);

Datum
sp_db_size(PG_FUNCTION_ARGS)
{
    int32 nitems = 0;

    /* Processing */
    dsa_handle dsa_handle;
    dsa_area* dsa;
    dshash_table_handle htab_handle;
    dshash_table* htab;
    SpatDBEntry* entry;

    /* Begin processing */
    spat_attach_shmem();
    LWLockAcquire(&g_spat_db->lck, LW_SHARED);
    dsa_handle = g_spat_db->dsa_handle;
    htab_handle = g_spat_db->htab_handle;
    LWLockRelease(&g_spat_db->lck);

    /* in dsa territory now */
    dsa = dsa_attach(dsa_handle);
    htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

    dshash_seq_status status;

    /* Initialize a sequential scan (non-exclusive lock assumed here). */
    dshash_seq_init(&status, htab, false);

    while ((entry = dshash_seq_next(&status)) != NULL)
        nitems++;

    dshash_seq_term(&status);

    /* leaving dsa territory */
    dsa_detach(dsa);

    PG_RETURN_INT32(nitems);
}


PG_FUNCTION_INFO_V1(ttl);

Datum
ttl(PG_FUNCTION_ARGS)
{
}

PG_FUNCTION_INFO_V1(sp_db_clear);

Datum
sp_db_clear(PG_FUNCTION_ARGS)
{
    elog(ERROR, "sp_db_clear not implemented yet");
}

PG_FUNCTION_INFO_V1(spval_example);

Datum
spval_example(PG_FUNCTION_ARGS)
{
    SPValue* result;
    const char* example_text = "Hello, PostgreSQL!";

    /* Allocate memory for spval */
    result = (SPValue*)palloc(sizeof(SPValue));

    /* Set the type to SPVAL_VARLENA */
    result->type = SPVAL_STRING;

    /* Convert the C string to a PostgreSQL text datum */
    result->value.varlena_val = cstring_to_text(example_text);

    /* Return the spval structure */
    PG_RETURN_POINTER(result);
}
