/*--------------------------------------------------------------------------
 *
 * spat.c
 *  spat: Redis-like In-Memory DB Embedded in Postgres
 *
 * A SpatDB is just a segment of Postgres' shared memory addressable by a name.
 * Each SpatDB is backed by a dshash_table stored in a DSA.
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
#include "spat.h"

PG_MODULE_MAGIC;

#define SPAT_NAME_MAXSIZE NAMEDATALEN	/* TODO: remove this */
#define SPAT_NAME_DEFAULT "spat-default"


/* ---------- Types ---------- */

/*
 * spval is a shell type returned by GET and similar commands. To the user
 * it's merely a shell type to facilitate output and to be casted to other
 * types (int, float, text, jsonb, vector etc.) Internally it can store
 * either pass-by-value fixed-length Datums or varlena datums.
 *
 * The actually in-memory representation though is SpatDBEntry. spval is
 * created by such an entry. At some point spval could be used as a value
 * type, but not yet.
 *
 * We set ALIGNMENT = double, 8-byte, as it satisfies both varlena, 8-byte
 * int/float and 4-byte int/float
 */

/* ---------- DSS ---------- */

/*
 * A Dynamicaly-Shared String (DSS) is a null-terminated char* stored in a
 * DSA
 */
struct dss
{
    dsa_pointer str; /* = VARDATA_ANY(txt) */
    Size len; /* = strlen + 1 = VARSIZE_ANY_EXHDR(t) + 1 */
};

int
dss_cmp_arg(const void* a, const void* b, size_t size, void* arg)
{
    dsa_area* dsa = (dsa_area*)arg;
    const dss* dss_a = (const dss*)a;
    const dss* dss_b = (const dss*)b;

    /* Compare lengths first */
    if (dss_a->len != dss_b->len)
        return (dss_a->len < dss_b->len) ? -1 : 1;

    /* Compare data byte-by-byte */
    return memcmp(dsa_get_address(dsa, dss_a->str),
                  dsa_get_address(dsa, dss_b->str),
                  dss_a->len - 1); /* Exclude null terminator */
}

dshash_hash dss_hash_arg(const void* key, size_t size, void* arg)
{
    dsa_area* dsa = (dsa_area*)arg;
    const dss* dss_key = (const dss*)key;

#ifdef SPAT_MURMUR3
    return hash_murmur3(dsa_get_address(dsa, dss_key->str), dss_key->len - 1, NULL);
#else
	return tag_hash(dsa_get_address(dsa, dss_key->str), dss_key->len - 1);
#endif
}

void
dss_cpy_arg(void* dest, const void* src, size_t size, void* arg)
{
    dsa_area* dsa = (dsa_area*)arg;
    dss* dss_dest = (dss*)dest;
    const dss* dss_src = (const dss*)src;

    /* Allocate memory in the destination DSS */
    dss_dest->len = dss_src->len;
    dss_dest->str = dsa_allocate(dsa, dss_src->len);

    /* Copy the key data */
    memcpy(dsa_get_address(dsa, dss_dest->str),
           dsa_get_address(dsa, dss_src->str),
           dss_src->len);
}

dss dss_new_extended(dsa_area* dsa, const char* str, Size len)
{
    dss result;
    result.str = dsa_allocate(dsa, len);
    result.len = len;
    memcpy(dsa_get_address(dsa, result.str), str, len);
    return result;
}

dss dss_new(dsa_area* dsa, const text* txt)
{
    dss result;

    Size len = VARSIZE_ANY_EXHDR(txt) + 1;

    result.str = dsa_allocate(dsa, len);
    result.len = len;

    /* Copy the text payload into the allocated memory */
    char* dest = dsa_get_address(dsa, result.str);
    memcpy(dest, VARDATA_ANY(txt), len - 1);

    /* Null-terminate the string */
    dest[len - 1] = '\0';

    return result;
}

text* dss_to_text(dsa_area* dsa, dss dss)
{
    Size data_len = dss.len - 1; /* Exclude null terminator */
    text* result = (text*)palloc(data_len + VARHDRSZ);

    /* Set the total size of the text object */
    SET_VARSIZE(result, data_len + VARHDRSZ);

    /* Copy the string data into the text structure */
    memcpy(VARDATA(result), dsa_get_address(dsa, dss.str), data_len);

    return result;
}

void
dss_free(dsa_area* dsa, dss* dss)
{
    dsa_free(dsa, dss->str);
}


const char* spTypeName(spValueType t)
{
    switch (t)
    {
    case SPVAL_STRING:
        return "string";
    case SPVAL_SET:
        return "set";
    case SPVAL_LIST:
        return "list";
    case SPVAL_NULL:
        return "null";
    case SPVAL_INVALID:
    default:
        return "invalid";
    }
}

/* In-memory representation of an SPValue */
typedef struct SPValue
{
    spValueType typ;

    union
    {
        struct varlena* varlena_val;

        struct
        {
            uint32 size;
        } set;

        struct
        {
            uint32 size;
        } list;
    } value;
} SPValue;

#define SpInvalidValSize InvalidAllocSize
#define SpInvalidValDatum PointerGetDatum(NULL)
#define SpMaxTTL DT_NOEND

typedef dshash_table_handle dshash_set_handle;

struct list_element;
typedef struct list_element list_element;

struct SpatDBEntry
{
    /* -------------------- Key -------------------- */
    dss key; /* pointer to a text* allocated in dsa */

    /* -------------------- Metadata -------------------- */

    TimestampTz expireat;

    /* -------------------- Value -------------------- */

    spValueType valtyp;

    union
    {
        dss string;

        struct
        {
            dshash_set_handle hndl;
            uint32 size;
        } set;

        struct
        {
            uint32 size;
            dsa_pointer head;
            dsa_pointer tail;
        } list;

        struct
        {
            dshash_table_handle hndl;
            uint32 size;
        } hash;
    } value;
};

struct SpatDB
{
    LWLock lck;

    dsa_handle dsa_handle;              /* dsa_handle to DSA area associated  with this DB */
    dshash_table_handle htab_handle;    /* htab_handle pointing to the underlying dshash_table */

    dsa_pointer name;                   /* Metadata about the db itself */
    TimestampTz created_at;

    /* The following are set when the db is attached */
    dsa_area* g_dsa;
    dshash_table* g_htab;
};

/*
 * ---------------------------------------- SpatDB API
 * ----------------------------------------
 *
 * This is generally a wrapper dshash_* API functions. They just use dss as a
 * key instead of void* and return SpatDBEntry* by casting before return.
 */


bool
spdb_is_attached(SpatDB* db)
{
    return db && db->g_dsa != NULL && db->g_htab != NULL;
}

#define SPDB_LOCK_SHARED(db)    LWLockAcquire(&(db)->lck, LW_SHARED)
#define SPDB_LOCK_EXCLUSIVE(db) LWLockAcquire(&(db)->lck, LW_EXCLUSIVE)
#define SPDB_LOCK_RELEASE(db)   LWLockRelease(&(db)->lck);


SpatDBEntry*
spdb_find(SpatDB* db, dss key, bool exclusive)
{
    SpatDBEntry* entry = dshash_find(db->g_htab, &key, exclusive);
    return entry;
}


SpatDBEntry*
spdb_find_or_insert(SpatDB* db, dss key, bool* found)
{
    SpatDBEntry* entry = dshash_find_or_insert(db->g_htab, &key, found);
    if (entry == NULL)
    {
        elog(ERROR, "dshash_find_or_insert failed, probably out-of-memory");
    }

    return entry;
}


void
spdb_release_lock(SpatDB* db, SpatDBEntry* entry)
{
    dshash_release_lock(db->g_htab, entry);
}

/*
 * ---------------------------------------- GUC Variables
 * ----------------------------------------
 */

static char* g_guc_spat_db_name = NULL; /* With this we identify a
						 * shared SpatDB to
						 * attach/detach */

/*
 * ---------------------------------------- Shared Memory Declarations
 * ----------------------------------------
 */

void _PG_init();
static void spat_init_shmem(void* ptr);
static void spat_attach_shmem(void);

/*
 * ---------------------------------------- Global state
 * ----------------------------------------
 */

static SpatDB* g_spat_db = NULL;

int dss_cmp(const void* a, const void* b, size_t size, void* arg)
{
    return dss_cmp_arg(a, b, size, g_spat_db->g_dsa);
}

dshash_hash dss_hash(const void* key, size_t size, void* arg)
{
    return dss_hash_arg(key, size, g_spat_db->g_dsa);
}

void dss_copy(void* dest, const void* src, size_t size, void* arg)
{
    dss_cpy_arg(dest, src, size, g_spat_db->g_dsa);
}

static const dshash_parameters default_hash_params = {
    .key_size = sizeof(dss),
    .entry_size = sizeof(SpatDBEntry),
    .compare_function = dss_cmp,
    .hash_function = dss_hash,
    .copy_function = dss_copy
};

/*
 * ---------------------------------------- Shared Memory Implementation
 * ----------------------------------------
 */

void
_PG_init()
{
    DefineCustomStringVariable("spat.db",
                               "Current DB name",
                               "",
                               &g_guc_spat_db_name,
                               SPAT_NAME_DEFAULT,
                               PGC_USERSET, 0,
                               NULL, NULL, NULL);

    MarkGUCPrefixReserved("spat");

    //on_proc_exit(spat_clean_on_exit, (Datum) 0);
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

    db->name = dsa_allocate0(dsa, SPAT_NAME_MAXSIZE); /* Allocate zeroed-out
								 * memory */
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

    if (!g_spat_db->g_dsa)
    {
        Assert(!g_spat_db->g_dsa);
        SPDB_LOCK_SHARED(g_spat_db);
        g_spat_db->g_dsa = dsa_attach(g_spat_db->dsa_handle);
        SPDB_LOCK_RELEASE(g_spat_db);
    }

    if (!g_spat_db->g_htab)
    {
        Assert(!g_spat_db->g_htab);
        SPDB_LOCK_SHARED(g_spat_db);
        g_spat_db->g_htab = dshash_attach(g_spat_db->g_dsa, &default_hash_params, g_spat_db->htab_handle, NULL);
        SPDB_LOCK_RELEASE(g_spat_db);
    }

    Assert(spdb_is_attached(g_spat_db));
}

static void
spat_detach_shmem(void)
{
    if (g_spat_db->g_dsa)
    {
        dsa_detach(g_spat_db->g_dsa);
        g_spat_db->g_dsa = NULL;
    }

    if (g_spat_db->g_htab)
    {
        dshash_detach(g_spat_db->g_htab);
        g_spat_db->g_htab = NULL;
    }
}

/*
 * ---------------------------------------- Commands Common
 * ----------------------------------------
 */

#define PG_GETARG_DSS(n)    dss_new(g_spat_db->g_dsa, PG_GETARG_TEXT_PP((n)))
#define DSS_LEN(s)          dsa_get_address(g_dsa, (s))->len
#define DSS_TO_TEXT(s)      dss_to_text(g_spat_db->g_dsa, (s))
#define PG_RETURN_DSS(s)    PG_RETURN_POINTER(DSS_TO_TEXT(s))

/*
 * ---------------------------------------- Commands Implementation
 * ----------------------------------------
 */

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
    switch (input->typ)
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
        appendStringInfo(&output, "set (%d)", input->value.set.size);
        break;
    case SPVAL_LIST:
        appendStringInfo(&output, "list (%d)", input->value.list.size);
        break;
    default:
        appendStringInfoString(&output, "invalid");
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

    SPDB_LOCK_SHARED(g_spat_db);

    result = g_spat_db->created_at;

    SPDB_LOCK_RELEASE(g_spat_db);

    spat_detach_shmem();
    PG_RETURN_TIMESTAMPTZ(result);
}

static SPValue* makeSpvalFromEntry(dsa_area* dsa, SpatDBEntry* entry)
{
    SPValue* result = (SPValue*)palloc(sizeof(SPValue));

    switch (entry->valtyp)
    {
    case SPVAL_INVALID:
    case SPVAL_NULL:
        break;
    case SPVAL_STRING:
        {
            Size realSize = entry->value.string.len;

            text* text_result = (text*)palloc(realSize);
            SET_VARSIZE(text_result, realSize);

            memcpy(text_result, dsa_get_address(dsa, entry->value.string.str), realSize);

            result->typ = SPVAL_STRING;
            result->value.varlena_val = text_result;
            break;
        }
    case SPVAL_SET:
        result->typ = SPVAL_SET;
        result->value.set.size = entry->value.set.size;
        break;
    case SPVAL_LIST:
        result->typ = SPVAL_LIST;
        result->value.list.size = entry->value.list.size;
        break;
    }

    return result;
}

PG_FUNCTION_INFO_V1(spset_generic);

Datum
spset_generic(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Input */
    dss key = PG_GETARG_DSS(0);
    Datum value = PG_GETARG_DATUM(1);
    Interval* ex = PG_ARGISNULL(2) ? NULL : PG_GETARG_INTERVAL_P(2);
    bool nx = PG_ARGISNULL(3) ? NULL : PG_GETARG_BOOL(3);
    bool xx = PG_ARGISNULL(4) ? NULL : PG_GETARG_BOOL(4);

    /* Info about value */
    bool valueIsNull = PG_ARGISNULL(1);
    Oid valueTypeOid;
    bool valueTypByVal;
    int16 valueTypLen;

    bool exclusive = false; /* TODO: This depends on the
						 * xx / nx */
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
    entry = spdb_find_or_insert(g_spat_db, key, &found);

    if (ex)
    {
        /* if ttl interval is given */
        entry->expireat = DatumGetTimestampTz(DirectFunctionCall2(
            timestamptz_pl_interval,
            TimestampTzGetDatum(GetCurrentTimestamp()),
            PointerGetDatum(ex)));
    }

    entry->valtyp = SPVAL_STRING;
    entry->value.string.len = VARSIZE_ANY(value);
    entry->value.string.str = dsa_allocate(g_spat_db->g_dsa, VARSIZE_ANY(value));
    memcpy(dsa_get_address(g_spat_db->g_dsa, entry->value.string.str), DatumGetPointer(value), VARSIZE_ANY(value));

    /* Prepare the SPvalue to echo / return */
    SPValue* result = makeSpvalFromEntry(g_spat_db->g_dsa, entry);

    spdb_release_lock(g_spat_db, entry);

    spat_detach_shmem();

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(spget);

Datum
spget(PG_FUNCTION_ARGS)
{
    bool found = false;
    SPValue* result;

    /* Begin processing */
    spat_attach_shmem();
    dss key = PG_GETARG_DSS(0);

    SpatDBEntry* entry = spdb_find(g_spat_db, key, false);
    if (entry)
    {
        found = true;
        result = makeSpvalFromEntry(g_spat_db->g_dsa, entry);
        spdb_release_lock(g_spat_db, entry);
    }

    spat_detach_shmem();

    if (!found)
    {
        PG_RETURN_NULL();
    }

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(sptype);

Datum
sptype(PG_FUNCTION_ARGS)
{
    spValueType result = SPVAL_NULL;

    /* Begin processing */
    spat_attach_shmem();
    dss key = PG_GETARG_DSS(0);

    SpatDBEntry* entry = spdb_find(g_spat_db, key, false);
    if (entry)
    {
        result = entry->valtyp;
        elog(DEBUG1, "valtyp=%d", entry->valtyp);
        spdb_release_lock(g_spat_db, entry);
    }

    spat_detach_shmem();

    PG_RETURN_TEXT_P(cstring_to_text(spTypeName(result)));
}


PG_FUNCTION_INFO_V1(getexpireat);

Datum
getexpireat(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);


    /* Output */
    TimestampTz result;

    bool found;
    SpatDBEntry* entry = spdb_find(g_spat_db, key, false);
    if (!entry)
    {
        found = false;
    }
    else
    {
        found = true;
        result = entry->expireat;
        spdb_release_lock(g_spat_db, entry);
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
    dshash_seq_init(&status, g_spat_db->g_htab, false);

    while ((entry = dshash_seq_next(&status)) != NULL)
        nitems++;

    dshash_seq_term(&status);

    spat_detach_shmem();
    PG_RETURN_INT32(nitems);
}

PG_FUNCTION_INFO_V1(sp_db_size_bytes);

Datum
sp_db_size_bytes(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();
    Size result = -1;
    result = dsa_get_total_size(g_spat_db->g_dsa);
    spat_detach_shmem();
    PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(dss_echo);

Datum
dss_echo(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();
    dss arg0 = PG_GETARG_DSS(0);
    text* result = DSS_TO_TEXT(arg0);

    spat_detach_shmem();

    PG_RETURN_TEXT_P(result);
}


/* ---------------------------------------- SETS  ---------------------------------------- */

static const dshash_parameters params_hashset = {
    .key_size = sizeof(dss),
    .entry_size = sizeof(dss),
    .compare_function = dss_cmp,
    .hash_function = dss_hash,
    .copy_function = dss_copy
};

PG_FUNCTION_INFO_V1(sadd);

Datum
sadd(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);
    dss elem_dss = PG_GETARG_DSS(1);

    /* Insert/find the set in the global hash table */
    bool dbentryfound;
    dshash_table* htab;
    dshash_table_handle htabhandl;

    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);

    if (!dbentryfound)
    {
        dbentry->valtyp = SPVAL_SET;
        htab = dshash_create(g_spat_db->g_dsa, &params_hashset, NULL);
        htabhandl = dshash_get_hash_table_handle(htab);
        dbentry->value.set.hndl = htabhandl;
        dbentry->value.set.size = 0;
    }
    else
    {
        /* Existing entry */
        Assert(dbentry->valtyp == SPVAL_SET);
        htabhandl = dbentry->value.set.hndl;

        htab = dshash_attach(g_spat_db->g_dsa, &params_hashset, htabhandl, NULL);
    }

    /* Insert the dss element into the set */
    bool setelementfound;
    dss* elem = dshash_find_or_insert(htab, &elem_dss, &setelementfound);
    //Using dss as element
    if (!setelementfound)
        dbentry->value.set.size++;

    elog(DEBUG1, "sadd: key=%s\tdbentryfound=%d\tsetelementfound=%d\tcard=%d", VARDATA_ANY(DSS_TO_TEXT(key)),
         dbentryfound, setelementfound, dbentry->value.set.size);

    /* Release locks */
    dshash_release_lock(htab, elem);
    dshash_detach(htab);

    spdb_release_lock(g_spat_db, dbentry);


    spat_detach_shmem();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(sismember);

Datum
sismember(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();
    dss key = PG_GETARG_DSS(0);

    dss elem_dss = PG_GETARG_DSS(1);

    bool result;

    SpatDBEntry* dbentry = spdb_find(g_spat_db, key, false);
    if (dbentry)
    {
        /* Existing entry */
        Assert(dbentry->valtyp == SPVAL_SET);
        dshash_table* htab = dshash_attach(g_spat_db->g_dsa, &params_hashset, dbentry->value.set.hndl, NULL);
        spdb_release_lock(g_spat_db, dbentry);

        dss* elem = dshash_find(htab, &elem_dss, false);
        if (elem)
        {
            dshash_release_lock(htab, elem);
            result = true;
        }
        else
        {
            result = false;
        }
        dshash_detach(htab);
    }
    else
    {
        result = false;
    }

    spat_detach_shmem();
    PG_RETURN_BOOL(result);
}

/*
 * Remove the specified members from the set stored at key. Specified members
 * that are not a member of this set are ignored. If key does not exist, it
 * is treated as an empty set and this command returns 0. An error is
 * returned when the value stored at key is not a set.
 */
PG_FUNCTION_INFO_V1(srem);

Datum
srem(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);
    dss elem_dss = PG_GETARG_DSS(1);

    int32 result;

    SpatDBEntry* dbentry = spdb_find(g_spat_db, key, true);
    if (dbentry)
    {
        /* Existing entry */
        Assert(dbentry->valtyp == SPVAL_SET);

        dshash_table* htab = dshash_attach(g_spat_db->g_dsa, &params_hashset,
                                           dbentry->value.set.hndl, NULL);

        bool deleted = dshash_delete_key(htab, &elem_dss);

        if (deleted)
            dbentry->value.set.size--;

        result = deleted ? 1 : 0;

        dshash_detach(htab);
        spdb_release_lock(g_spat_db, dbentry);
    }
    else
    {
        result = 0;
    }

    spat_detach_shmem();
    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(scard);

Datum
scard(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Input key and value */
    dss key = PG_GETARG_DSS(0);

    uint32 result;
    bool isaset;
    SpatDBEntry* dbentry = spdb_find(g_spat_db, key, false);

    if (dbentry)
    {
        if (dbentry->valtyp == SPVAL_SET)
        {
            isaset = true;
            result = dbentry->value.set.size;
        }
        else
        {
            isaset = false;
        }
        spdb_release_lock(g_spat_db, dbentry);;
    }
    else
    {
        isaset = false;
    }
    spat_detach_shmem();

    if (isaset)
        PG_RETURN_INT32(result);
    else
        PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(sinter);

Datum
sinter(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key1 = PG_GETARG_DSS(0);
    dss key2 = PG_GETARG_DSS(1);

    spat_detach_shmem();
}

/*
 * ---------------------------------------- LISTS ----------------------------------------
 */

struct list_element
{
    dss data;

    dsa_pointer prev;
    dsa_pointer next;
};

#define LIST_NIL InvalidDsaPointer

PG_FUNCTION_INFO_V1(lpush);

Datum
lpush(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Retrieve key and element arguments */
    dss key = PG_GETARG_DSS(0);
    dss elem = PG_GETARG_DSS(1);

    /* Insert/find the list in the global hash table */
    bool dbentryfound;
    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);

    if (!dbentryfound)
    {
        /* New list initialization */
        dbentry->valtyp = SPVAL_LIST;
        dbentry->value.list.size = 1;

        /* Allocate the first element */
        dsa_pointer new_elem_ptr = dsa_allocate(g_spat_db->g_dsa, sizeof(list_element));
        list_element* new_elem = dsa_get_address(g_spat_db->g_dsa, new_elem_ptr);

        /* Set the element data */
        new_elem->data = elem;
        new_elem->prev = InvalidDsaPointer;
        new_elem->next = InvalidDsaPointer;

        /* Initialize head and tail */
        dbentry->value.list.head = new_elem_ptr;
        dbentry->value.list.tail = new_elem_ptr;
    }
    else
    {
        /* Existing list handling */
        if (dbentry->value.list.size == 0)
        {
            /* Reinitializing an emptied list */
            dsa_pointer new_elem_ptr = dsa_allocate(g_spat_db->g_dsa, sizeof(list_element));
            list_element* new_elem = dsa_get_address(g_spat_db->g_dsa, new_elem_ptr);

            /* Set the element data */
            new_elem->data = elem;
            new_elem->prev = InvalidDsaPointer;
            new_elem->next = InvalidDsaPointer;

            /* Reinitialize head and tail */
            dbentry->value.list.head = new_elem_ptr;
            dbentry->value.list.tail = new_elem_ptr;

            /* Reset the size */
            dbentry->value.list.size = 1;
        }
        else
        {
            /* Normal LPUSH to the head of the list */
            dsa_pointer head_ptr = dbentry->value.list.head;
            dsa_pointer new_elem_ptr = dsa_allocate(g_spat_db->g_dsa, sizeof(list_element));
            list_element* new_elem = dsa_get_address(g_spat_db->g_dsa, new_elem_ptr);

            /* Set the new element's data */
            new_elem->data = elem;

            /* Link the new element to the existing head */
            new_elem->next = head_ptr;
            new_elem->prev = InvalidDsaPointer;

            /* Update the existing head's prev pointer */
            list_element* head_elem = dsa_get_address(g_spat_db->g_dsa, head_ptr);
            head_elem->prev = new_elem_ptr;

            /* Update the list's head pointer */
            dbentry->value.list.head = new_elem_ptr;

            /* Increment the size */
            dbentry->value.list.size++;
        }
    }

    spdb_release_lock(g_spat_db, dbentry);
    spat_detach_shmem();

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(llen);

Datum
llen(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Retrieve the key and the element arguments */
    dss key = PG_GETARG_DSS(0);

    /* Insert/find the list in the global hash table */
    bool dbentryfound;
    uint32 result;

    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);

    if (dbentryfound)
    {
        result = dbentry->value.list.size;
    }

    spdb_release_lock(g_spat_db, dbentry);
    spat_detach_shmem();

    if (dbentryfound)
        PG_RETURN_INT32(result);
    else
        PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(lpop);

Datum
lpop(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Retrieve the key argument */
    dss key = PG_GETARG_DSS(0);

    /* Find the list in the global hash table */
    bool dbentryfound;
    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);

    /*
     * If the list does not exist or is empty, release the lock and
     * return NULL
     */
    if (!dbentryfound || dbentry->valtyp != SPVAL_LIST || dbentry->value.list.size == 0)
    {
        spdb_release_lock(g_spat_db, dbentry);
        spat_detach_shmem();
        PG_RETURN_NULL();
    }

    /* Retrieve the head of the list */
    dsa_pointer head_ptr = dbentry->value.list.head;
    list_element* head_elem = dsa_get_address(g_spat_db->g_dsa, head_ptr);

    /* Prepare the return value (head element data) */
    text* result = DSS_TO_TEXT(head_elem->data);

    /* Update the list to remove the head */
    dbentry->value.list.head = head_elem->next;

    if (dbentry->value.list.head != InvalidDsaPointer)
    {
        /*
         * Update the new head's `prev` pointer to
         * `InvalidDsaPointer`
         */
        list_element* new_head = dsa_get_address(g_spat_db->g_dsa, dbentry->value.list.head);
        new_head->prev = InvalidDsaPointer;
    }
    else
    {
        /* The list is now empty, update the tail pointer */
        dbentry->value.list.tail = InvalidDsaPointer;
    }

    /* Decrement the size of the list */
    dbentry->value.list.size--;

    /* Free the old head element from the DSA */
    dsa_free(g_spat_db->g_dsa, head_ptr);

    /* Release the lock on the hash table entry */
    spdb_release_lock(g_spat_db, dbentry);

    spat_detach_shmem();

    /* Return the result */
    PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(rpush);

Datum
rpush(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    /* Retrieve the key and the element arguments */
    dss key = PG_GETARG_DSS(0);
    dss elem = PG_GETARG_DSS(1);

    /* Insert/find the list in the global hash table */
    bool dbentryfound;
    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);

    if (!dbentryfound || dbentry->value.list.size == 0)
    {
        /* If the list does not exist or is empty, initialize it */
        dbentry->valtyp = SPVAL_LIST;


        /* Allocate and initialize the new element */
        dsa_pointer new_elem_ptr = dsa_allocate(g_spat_db->g_dsa, sizeof(list_element));
        list_element* new_elem = dsa_get_address(g_spat_db->g_dsa, new_elem_ptr);

        new_elem->data = elem;
        new_elem->prev = InvalidDsaPointer;
        new_elem->next = InvalidDsaPointer;

        /* Set the new element as both the head and tail of the list */
        dbentry->value.list.head = new_elem_ptr;
        dbentry->value.list.tail = new_elem_ptr;

        dbentry->value.list.size = 1;
    }
    else
    {
        /* Normal RPUSH to the tail of the list */
        dsa_pointer tail_ptr = dbentry->value.list.tail;
        dsa_pointer new_elem_ptr = dsa_allocate(g_spat_db->g_dsa, sizeof(list_element));
        list_element* new_elem = dsa_get_address(g_spat_db->g_dsa, new_elem_ptr);

        /* Set the new element's data */
        new_elem->data = elem;

        /* Link the new element to the existing tail */
        new_elem->prev = tail_ptr;
        new_elem->next = InvalidDsaPointer;

        /* Update the existing tail's next pointer */
        list_element* tail_elem = dsa_get_address(g_spat_db->g_dsa, tail_ptr);
        tail_elem->next = new_elem_ptr;

        /* Update the list's tail pointer */
        dbentry->value.list.tail = new_elem_ptr;

        /* Increment the size */
        dbentry->value.list.size++;
    }

    spdb_release_lock(g_spat_db, dbentry);
    spat_detach_shmem();

    /* Return success */
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(rpop);

Datum
rpop(PG_FUNCTION_ARGS)
{
}

/*
 * ---------------------------------------- HASHES
 * ----------------------------------------
 */

typedef struct sphash_entry
{
    dss field;
    dss value;
} sphash_entry;

static const dshash_parameters sphash_params = {
    .key_size = sizeof(dss),
    .entry_size = sizeof(sphash_entry),
    .compare_function = dss_cmp,
    .hash_function = dss_hash,
    .copy_function = dss_copy
};

PG_FUNCTION_INFO_V1(hset);

Datum
hset(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);
    dss field = PG_GETARG_DSS(1);
    dss value = PG_GETARG_DSS(2);

    bool dbentryfound;

    SpatDBEntry* dbentry = spdb_find_or_insert(g_spat_db, key, &dbentryfound);
    dshash_table* htab;

    if (!dbentryfound)
    {
        htab = dshash_create(g_spat_db->g_dsa, &sphash_params, NULL);
        dbentry->valtyp = SPVAL_HASH;
        dbentry->value.hash.hndl = dshash_get_hash_table_handle(htab);
        dbentry->value.hash.size = 0;
    }
    else
    {
        /* dbentryfound=true */
        Assert(dbentry->valtyp == SPVAL_HASH);
        htab = dshash_attach(g_spat_db->g_dsa, &sphash_params, dbentry->value.hash.hndl, NULL);
    }

    bool sphsntry_found;
    sphash_entry* sphsntry = dshash_find_or_insert(htab, &field, &sphsntry_found);

    if (!sphsntry_found)
    {
        sphsntry->field = field;
        sphsntry->value = value;
        dbentry->value.hash.size++;
    }
    else
    {
        sphsntry->value = value;
    }

    dshash_release_lock(htab, sphsntry);
    dshash_detach(htab);

    spdb_release_lock(g_spat_db, dbentry);
    spat_detach_shmem();

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(hget);

Datum
hget(PG_FUNCTION_ARGS)
{
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);
    dss field = PG_GETARG_DSS(1);

    bool sphashentryfound = false;
    text* result;

    SpatDBEntry* dbentry = spdb_find(g_spat_db, key, false);

    if (dbentry && dbentry->valtyp == SPVAL_HASH)
    {
        dshash_table* htab = dshash_attach(g_spat_db->g_dsa, &sphash_params,
                                           dbentry->value.hash.hndl, NULL);

        sphash_entry* sphsntry = dshash_find(htab, &field, false);
        if (sphsntry != NULL)
        {
            result = DSS_TO_TEXT(sphsntry->value);
            sphashentryfound = true;
            dshash_release_lock(htab, sphsntry);
        }

        dshash_detach(htab);
    }

    if (dbentry)
        spdb_release_lock(g_spat_db, dbentry);

    spat_detach_shmem();
    if (sphashentryfound)
        PG_RETURN_TEXT_P(result);

    PG_RETURN_NULL();
}

/*
 * To delete an entry we attempt to find it first If its not found do
 * nothing. If it's found though, it's not enought DEL the key, we also have
 * to cleanup the value itself based on the value typee.
 */
PG_FUNCTION_INFO_V1(del);

Datum
del(PG_FUNCTION_ARGS)
{
    bool found = false;
    spat_attach_shmem();

    dss key = PG_GETARG_DSS(0);

    SPDB_LOCK_EXCLUSIVE(g_spat_db); /* sure about this ? */

    /* Look up for the entry and if it's found lock it exclusively */
    SpatDBEntry* entry = spdb_find(g_spat_db, key, SPDB_ENTRY_LOCK_EXCLUSIVE);
    if (entry)
    {
        found = true;
        /* we begin by cleaning up the value */

        /* toggle on the valtype to clean up properly */
        switch (entry->valtyp)
        {
        case SPVAL_STRING:
            {
                dsa_free(g_spat_db->g_dsa, entry->value.string.str);
                break;
            }

        case SPVAL_LIST:
            {
                dsa_pointer current_ptr = entry->value.list.head;

                while (current_ptr != InvalidDsaPointer)
                {
                    list_element* current_elem = dsa_get_address(g_spat_db->g_dsa, current_ptr);
                    dsa_pointer next_ptr = current_elem->next;

                    /* Free the current element */
                    dsa_free(g_spat_db->g_dsa, current_ptr);

                    /* Move to the next element */
                    current_ptr = next_ptr;
                }

                /* Reset the list metadata */
                entry->value.list.head = InvalidDsaPointer;
                entry->value.list.tail = InvalidDsaPointer;
                entry->value.list.size = 0;

                break;
            }

        case SPVAL_HASH:
            {
                /* Attach to the hash table */
                dshash_table_handle htabhandl = entry->value.hash.hndl;
                dshash_table* htab = dshash_attach(g_spat_db->g_dsa, &sphash_params, htabhandl, NULL);

                /* Iterate over all elements and delete them */
                dshash_seq_status status;
                dshash_seq_init(&status, htab, true);

                while (dshash_seq_next(&status) != NULL)
                {
                    dshash_delete_current(&status);
                }

                dshash_seq_term(&status);

                /* Destroy the hash table */
                dshash_destroy(htab);

                /* Reset hash metadata */
                entry->value.hash.hndl = InvalidDsaPointer;
                entry->value.hash.size = 0;

                break;
            }

        case SPVAL_SET:
            {
                /* Attach to the hash table */
                dshash_table_handle htabhandl = entry->value.set.hndl;
                dshash_table* htab = dshash_attach(g_spat_db->g_dsa, &params_hashset, htabhandl, NULL);

                /* Iterate over all elements and delete them */
                dshash_seq_status status;
                dshash_seq_init(&status, htab, true);

                while (dshash_seq_next(&status) != NULL)
                {
                    dshash_delete_current(&status);
                }

                /* Terminate sequence scan */
                dshash_seq_term(&status);

                /* Destroy the hash table */
                dshash_destroy(htab);

                /* Reset set metadata */
                entry->value.set.hndl = InvalidDsaPointer;
                entry->value.set.size = 0;

                break;
            }

        case SPVAL_INVALID:
        case SPVAL_NULL:
        default:
            break;
        }
        spdb_release_lock(g_spat_db, entry);
        dshash_delete_key(g_spat_db->g_htab, &key);
    }
    else
    {
        /* key not found - do nothing */
        found = false;
    }
    SPDB_LOCK_RELEASE(g_spat_db)

    spat_detach_shmem();
    PG_RETURN_BOOL(found);
}
