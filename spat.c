/*--------------------------------------------------------------------------
 *
 * spat.c
 *	  Redis-like in-memory database embedded in Postgres
 *
 * A SpatDB is just a segment of Postgres' memory addressable by a name.
 * Its data model is key-value.
 * Keys are strings, values can have different types (data structures)
 *
 * It uses a dshash_table to store its internal.
 *
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

PG_MODULE_MAGIC;

#define SPAT_NAME_MAXSIZE NAMEDATALEN
#define SPAT_NAME_DEFAULT "spat-default"

/* GUC variables */
static char *g_spat_db_name = NULL;

typedef struct SpatDB
{
	LWLock				lck;

	dsa_handle			dsa_handle;		/* dsa_handle to DSA area associated with this DB */
	dshash_table_handle	htab_handle;	/* htab_handle pointing to the underlying dshash_table */

	dsa_pointer			name;			/* Metadata about the db itself */
	TimestampTz			created_at;
	int					val;
} SpatDB;

/* -------------------- Global state -------------------- */

static SpatDB *g_spat_db; /* Current (working) database */

void _PG_init()
{
	DefineCustomStringVariable("spat.db",
							   "Current DB name",
							   "long desc here",
							   &g_spat_db_name,
							   SPAT_NAME_DEFAULT,
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("spat");
}

typedef enum valueType
{
	SPAT_INVALID	= -1,
	SPAT_NULL		= 0,
	SPAT_INT		= 1,
	SPAT_STRING		= 2
} valueType;

#define TYPE_IS_SCALAR(t) ((t) == SPAT_INT || (t) == SPAT_NULL)

static char* typ_name(valueType typ)
{
	switch (typ)
	{
		case SPAT_INVALID:
			return "invalid";
		case SPAT_NULL:
			return "null";
		case SPAT_INT:
			return "integer";
		case SPAT_STRING:
			return "string";
		default:
			return "unknown";

	}
}

typedef struct SpatDBEntry
{
	dsa_pointer	key;
	valueType	typ;
	int			value; /* this should be a pointer for most values */
} SpatDBEntry;

static const dshash_parameters default_hash_params = {
	.key_size = sizeof(dsa_pointer),
	.entry_size = sizeof(SpatDBEntry),
	.compare_function = dshash_memcmp,
	.hash_function = dshash_memhash,
	.copy_function = dshash_memcpy
};

static void
spat_init_shmem(void *ptr)
{
	SpatDB *db = (SpatDB *) ptr;

	int tranche_id = LWLockNewTrancheId();
	LWLockInitialize(&db->lck, tranche_id);

	dsa_area *dsa = dsa_create(tranche_id);
	dsa_pin(dsa);
	//dsa_pin_mapping() TOOD: maybe?

	db->dsa_handle = dsa_get_handle(dsa);

	/* Initialize metadata */
	db->val = -1; /* Default value */

	db->name = dsa_allocate0(dsa, SPAT_NAME_MAXSIZE); /* Allocate zeroed-out memory */
	memcpy(dsa_get_address(dsa, db->name), g_spat_db_name, SPAT_NAME_MAXSIZE - 1);

	db->created_at = GetCurrentTimestamp();

	dshash_table *htab = dshash_create(dsa, &default_hash_params, NULL);
	db->htab_handle = dshash_get_hash_table_handle(htab);

	dsa_detach(dsa);
}

static void
spat_attach_shmem(void)
{
	bool		found;

	g_spat_db = GetNamedDSMSegment(g_spat_db_name,
								   sizeof(SpatDB),
								   spat_init_shmem,
								   &found);
	LWLockRegisterTranche(g_spat_db->lck.tranche, g_spat_db_name);
}

PG_FUNCTION_INFO_V1(spat_db_name);
Datum
spat_db_name(PG_FUNCTION_ARGS)
{
	dsa_handle	dsa_handle;
	dsa_area	*dsa;
	char		message[NAMEDATALEN];

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
	TimestampTz	result;

	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);

	result = g_spat_db->created_at;

	LWLockRelease(&g_spat_db->lck);

	PG_RETURN_TIMESTAMPTZ(result);
}

PG_FUNCTION_INFO_V1(spat_db_set_int);
Datum
spat_db_set_int(PG_FUNCTION_ARGS)
{
	char    *key        = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int     value       = PG_GETARG_INT32(1);

	dsa_handle               dsa_handle;
	dsa_area                 *dsa;
	dshash_table_handle      htab_handle;
	dshash_table             *htab;
	bool                     found;

	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);
	dsa_handle = g_spat_db->dsa_handle;
	htab_handle = g_spat_db->htab_handle;
	LWLockRelease(&g_spat_db->lck);

	dsa = dsa_attach(dsa_handle);
	htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	/* Copy the key into the allocated memory */
	strcpy(dsa_get_address(dsa, key_dsa), key);

	/* Insert or find the entry in the hash table */
	SpatDBEntry *entry = dshash_find_or_insert(htab, dsa_get_address(dsa, key_dsa), &found);
	if (entry == NULL)
		elog(ERROR, "could not find or insert entry");

	if (found)
	{
		elog(INFO, "existing entry for key=%s", key);  // Log using 'key' instead of raw pointer
	}
	else
	{
		entry->value = value;
		entry->typ = SPAT_INT;
		elog(INFO, "new entry for key=%s", key);
	}

	/* Release the lock on the entry (correct usage of release_lock) */
	dshash_release_lock(htab, entry);

	dsa_detach(dsa);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spat_db_set_string);
Datum
spat_db_set_string(PG_FUNCTION_ARGS)
{
	char    *key        = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char    *value      = text_to_cstring(PG_GETARG_TEXT_PP(1));

	dsa_handle           dsa_handle;
	dsa_area             *dsa;
	dshash_table_handle  htab_handle;
	dshash_table         *htab;
	bool                 found;

	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);
	dsa_handle = g_spat_db->dsa_handle;
	htab_handle = g_spat_db->htab_handle;
	LWLockRelease(&g_spat_db->lck);

	dsa = dsa_attach(dsa_handle);
	htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	/* Copy the key into the allocated memory */
	strcpy(dsa_get_address(dsa, key_dsa), key);

	/* Insert or find the entry in the hash table */
	SpatDBEntry *entry = dshash_find_or_insert(htab, dsa_get_address(dsa, key_dsa), &found);
	if (entry == NULL)
		elog(ERROR, "could not find or insert entry");

	if (found)
	{
		/* If the type is scalar we need to allocate
		 * If not we need to free the previ
		 */
		if (TYPE_IS_SCALAR(entry->typ))
		{
		}
	}
	else
	{
		entry->value = value;
		entry->typ = SPAT_INT;
		elog(INFO, "new entry for key=%s", key);
	}

	Assert(entry);
	dshash_release_lock(htab, entry);

	dsa_detach(dsa);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spat_db_get_int);
Datum
spat_db_get_int(PG_FUNCTION_ARGS)
{
	char    *key        = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int     result;
	dsa_handle               dsa_handle;
	dsa_area                 *dsa;
	dshash_table_handle      htab_handle;
	dshash_table             *htab;
	bool                     found;

	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);
	dsa_handle = g_spat_db->dsa_handle;
	htab_handle = g_spat_db->htab_handle;
	LWLockRelease(&g_spat_db->lck);

	dsa = dsa_attach(dsa_handle);
	htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	strcpy(dsa_get_address(dsa, key_dsa), key);

	SpatDBEntry *entry = dshash_find(htab, dsa_get_address(dsa, key_dsa), false);
	if (entry)
	{
		found = true;
		result = entry->value;
		dshash_release_lock(htab, entry);
	}

	else{
		found = false;
		dsa_free(dsa, key_dsa);
	}

	dsa_detach(dsa);

	if (found)
		PG_RETURN_INT32(result);
	else
		PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(spat_db_type);
Datum
spat_db_type(PG_FUNCTION_ARGS)
{
	char		*key   = text_to_cstring(PG_GETARG_TEXT_PP(0));
	valueType   result = SPAT_INVALID;

	dsa_handle               dsa_handle;
	dsa_area                 *dsa;
	dshash_table_handle      htab_handle;
	dshash_table             *htab;
	bool                     found;

	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);
	dsa_handle = g_spat_db->dsa_handle;
	htab_handle = g_spat_db->htab_handle;
	LWLockRelease(&g_spat_db->lck);

	dsa = dsa_attach(dsa_handle);
	htab = dshash_attach(dsa, &default_hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	strcpy(dsa_get_address(dsa, key_dsa), key);

	SpatDBEntry *entry = dshash_find(htab, dsa_get_address(dsa, key_dsa), false);

	if (entry) {
		found = true;
		result = entry->typ;
		dshash_release_lock(htab, entry);
	}

	else {
		found = false;
		dsa_free(dsa, key_dsa);
	}

	dsa_detach(dsa);

	PG_RETURN_TEXT_P(cstring_to_text(typ_name(result)));

}