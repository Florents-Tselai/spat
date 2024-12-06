/*--------------------------------------------------------------------------
 *
 * spat.c
 *	  Does this and that
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

typedef struct SpatDBStruct
{
	LWLock			lck;

	dsa_handle		dsa_handle; /* dsa_handle to DSA area associated with this registry */

	/* Storage area */
	dshash_table_handle	htab_handle;

	/* Metadata about the db itself */
	dsa_pointer		name;
	TimestampTz		created_at;
	int				val;
} SpatDBStruct;

static SpatDBStruct *g_spat_db;


void _PG_init()
{
	DefineCustomStringVariable("spat.db",
							   "Working DB name",
							   NULL,
							   &g_spat_db_name,
							   SPAT_NAME_DEFAULT,
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("spat");
}

typedef struct HashEntry
{
	dsa_pointer	key;
	int			value; /* this should be a pointer for most values */
} HashEntry;

static const dshash_parameters hash_params = {
	.key_size = sizeof(dsa_pointer),
	.entry_size = sizeof(HashEntry),
	.compare_function = dshash_memcmp,
	.hash_function = dshash_memhash,
	.copy_function = dshash_memcpy
};

static void
spat_init_shmem(void *ptr)
{
	SpatDBStruct *db = (SpatDBStruct *) ptr;

	int tranche_id = LWLockNewTrancheId();
	LWLockInitialize(&db->lck, tranche_id);

	dsa_area *dsa = dsa_create(tranche_id);
	dsa_pin(dsa);

	db->dsa_handle = dsa_get_handle(dsa);

	/* Initialize metadata */
	db->val = -1; /* Default value */

	db->name = dsa_allocate0(dsa, SPAT_NAME_MAXSIZE); /* Allocate zeroed-out memory */
	memcpy(dsa_get_address(dsa, db->name), g_spat_db_name, SPAT_NAME_MAXSIZE - 1);

	db->created_at = GetCurrentTimestamp();

	/* Create an htab and associate it's handle with the DB */
	dshash_table *htab = dshash_create(dsa, &hash_params, NULL);
	db->htab_handle = dshash_get_hash_table_handle(htab);

	dsa_detach(dsa);
}

static void
spat_attach_shmem(void)
{
	bool		found;

	g_spat_db = GetNamedDSMSegment(g_spat_db_name,
								   sizeof(SpatDBStruct),
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

	/* Attach to shared memory and acquire the lock */
	spat_attach_shmem();
	LWLockAcquire(&g_spat_db->lck, LW_SHARED);
	dsa_handle = g_spat_db->dsa_handle;
	htab_handle = g_spat_db->htab_handle;
	LWLockRelease(&g_spat_db->lck);

	dsa = dsa_attach(dsa_handle);
	htab = dshash_attach(dsa, &hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	/* Copy the key into the allocated memory */
	strcpy(dsa_get_address(dsa, key_dsa), key);

	/* Insert or find the entry in the hash table */
	HashEntry *entry = dshash_find_or_insert(htab, dsa_get_address(dsa, key_dsa), &found);
	if (entry == NULL)
		elog(ERROR, "could not find or insert entry");

	if (found)
	{
		elog(INFO, "existing entry for key=%s", key);  // Log using 'key' instead of raw pointer
	}
	else
	{
		entry->value = value;
		elog(INFO, "new entry for key=%s", key);  // Log using 'key' instead of raw pointer
	}

	/* Release the lock on the entry (correct usage of release_lock) */
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
	htab = dshash_attach(dsa, &hash_params, htab_handle, NULL);

	/* Allocate memory for the key on DSA */
	dsa_pointer key_dsa = dsa_allocate(dsa, strlen(key) + 1);  // +1 for the null terminator
	if (key_dsa == InvalidDsaPointer)
		elog(ERROR, "Could not allocate DSA memory for key=%s", key);

	strcpy(dsa_get_address(dsa, key_dsa), key);

	HashEntry *entry = dshash_find(htab, dsa_get_address(dsa, key_dsa), false);
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