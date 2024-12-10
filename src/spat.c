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
	SPVAL_INVALID	= -1,
	SPVAL_NULL		= 0,
	SPVAL_INTEGER	= 1,
	SPVAL_FLOAT		= 2,
	SPVAL_STRING	= 3
} valueType;

typedef struct spval
{
	valueType type;

	union
	{
		int32 int32_val;
		float8 float8_val;
		struct varlena *varlena_val;
	} value;
} spval;

#define SpInvalidValSize InvalidAllocSize
#define SpInvalidValDatum NULL

typedef struct SpatDBEntry
{
	/* -------------------- Key -------------------- */
	dsa_pointer	key;		/* pointer to a text* allocated in dsa */

	valueType	typ;		/* TODO: redundant now */

	Datum		intvalue;	/* TODO: Redundant */

	/* -------------------- Metadata -------------------- */

	/* -------------------- Value -------------------- */

	Oid			valtypid;	/* Oid of the Datum stored at valptr (default=InvalidOid).
							 *
							 * We need this to call get_typlenbyval to get typlen and tybval.
							 * Both are needed to know how to copy a Datum into dsa,
							 * and then how to interpret what's in that dsa position.
							 *
							 * In some occasions we may want align info as well.
							 * If so, use get_typlenbyvalalign
							 */


	Size		valsz;		/* VARSIZE_ANY(value)
							 * = SpInvalidValSize if the value is pass-by-value
							 */

	dsa_pointer valptr;		/* pointer to an opaque Datum allocated in dsa.
							 * To get a backend-local pointer to this use dsa_get_address(valptr).
							 * To correctly interpret it though, you probably should take into account
							 * both the valtypid and the valsz
							 */

	Datum		valval;		/* Only set if valsz = InvalidAllocSize
							 * Datum for pass-by-value value
							 * Set to SpInvalidValDatum if the value is pass-by-reference
							*/

} SpatDBEntry;


typedef struct SpatDB
{
	LWLock				lck;

	dsa_handle			dsa_handle;		/* dsa_handle to DSA area associated with this DB */
	dshash_table_handle	htab_handle;	/* htab_handle pointing to the underlying dshash_table */

	dsa_pointer			name;			/* Metadata about the db itself */
	TimestampTz			created_at;
	int					val;
} SpatDB;



PG_FUNCTION_INFO_V1(spval_in);

Datum
spval_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "spval_in shouldn't be called");
	char *input = PG_GETARG_CSTRING(0);
	spval *result;

	/* Allocate memory for the spval struct */
	result = (spval *) palloc(sizeof(spval));

	/* Try parsing as an integer using int4in */
	PG_TRY();
	{
		result->value.int32_val = DatumGetInt32(DirectFunctionCall1(int4in, CStringGetDatum(input)));
		result->type = SPVAL_INTEGER;
		PG_RETURN_POINTER(result);
	}
	PG_CATCH();
	{
		/* Clear error and proceed to try other types */
		FlushErrorState();
	}
	PG_END_TRY();

	/* Try parsing as a float using float8in */
	PG_TRY();
	{
		result->value.float8_val = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(input)));
		result->type = SPVAL_FLOAT;
		PG_RETURN_POINTER(result);
	}
	PG_CATCH();
	{
		/* Clear error and proceed to try as text */
		FlushErrorState();
	}
	PG_END_TRY();

	/* Default to varlena type (e.g., text) */
	result->type = SPVAL_STRING;
	result->value.varlena_val = cstring_to_text(input);

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(spval_out);

Datum
spval_out(PG_FUNCTION_ARGS)
{
	spval *input = (spval *) PG_GETARG_POINTER(0);
	StringInfoData output;

	/* Initialize output string */
	initStringInfo(&output);

	/* Convert spval back to a string based on its type */
	switch (input->type)
	{
	case SPVAL_INTEGER:
		appendStringInfo(&output, "%d", input->value.int32_val);
		break;
	case SPVAL_FLOAT:
		appendStringInfo(&output, "%g", input->value.float8_val);
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

static SpatDB *g_spat_db;		/* Current (working) database */
								/* TODO: maybe *g_dsa here too? */

/* ---------------------------------------- GUC Variables ---------------------------------------- */

static char *g_guc_spat_db_name = NULL;

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
	memcpy(dsa_get_address(dsa, db->name), g_guc_spat_db_name, SPAT_NAME_MAXSIZE - 1);

	db->created_at = GetCurrentTimestamp();

	dshash_table *htab = dshash_create(dsa, &default_hash_params, NULL);
	db->htab_handle = dshash_get_hash_table_handle(htab);

	dsa_detach(dsa);
}

static void
spat_attach_shmem(void)
{
	bool		found;

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
	Datum   value       = PG_GETARG_DATUM(1);

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
		elog(DEBUG1, "existing entry for key=%s", key);  // Log using 'key' instead of raw pointer
	}
	else
	{
		entry->intvalue = value;
		entry->typ = SPVAL_INTEGER;
		elog(DEBUG1, "new entry for key=%s", key);
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
		result = entry->intvalue;
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
	valueType   result = SPVAL_INVALID;

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


PG_FUNCTION_INFO_V1(sset_generic);
Datum
sset_generic(PG_FUNCTION_ARGS)
{
	/* Input */
	text 		*key 	= PG_GETARG_TEXT_PP(0);
	Datum 		value 	= PG_GETARG_DATUM(1);
	Interval	*ex 	= PG_ARGISNULL(2) ? NULL : PG_GETARG_INTERVAL_P(2);
	bool		nx 		= PG_ARGISNULL(3) ? NULL : PG_GETARG_BOOL(3);
	bool		xx 		= PG_ARGISNULL(4) ? NULL : PG_GETARG_BOOL(4);

	/* Info about value */
	bool		valueIsNull 	= PG_ARGISNULL(1);
	Oid valueTypeOid;
	bool valueTypByVal;
	int16 valueTypLen;

	/* Processing */
	dsa_handle              dsa_handle;
	dsa_pointer             dsa_key;
	dsa_pointer				dsa_value;
	dsa_area                *dsa;
	dshash_table_handle     htab_handle;
	dshash_table			*htab;
	bool					exclusive = false; /* TODO: This depends on the xx / nx */
	bool                    found;
	SpatDBEntry				*entry;

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

	if (!(valueTypeOid == TEXTOID))
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

	/* existing entry */
	if (found)
	{
		elog(DEBUG1, "Found existing entry for key=%s", text_to_cstring(key));
	}

	if (found || !found)
	{
		if (!found)
			elog(DEBUG1, "Inserting new entry for key=%s", text_to_cstring(key));

		if (found)
			elog(DEBUG1, "Inserting existing entry for key=%s", text_to_cstring(key));

		if (!found)
		{
			/* Initialize the entry value */
			entry->valtypid = InvalidOid;
			entry->valsz = InvalidAllocSize;
			entry->valptr = InvalidDsaPointer;
			entry->valval = InvalidOid;

		}

		/* Copying the value into dsa.
		 * How we do that depends on the type of the value, and its size.
		 * This should generally follow the logic of
		 * Datum datumCopy(Datum value, bool typByVal, int typLen)
		 * in src/backend/utils/adt/datum.c
		 */

		if (valueTypByVal)
		{
			entry->valtypid = valueTypeOid;
			entry->valsz = SpInvalidValSize;
			entry->valptr = InvalidDsaPointer;
			entry->valval = value;
			entry->valval = value;
		}
		else if (valueTypLen == -1)
		{
			/* It is a varlena datatype */
			struct varlena *vl = (struct varlena *) DatumGetPointer(value);
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
				entry->valsz = VARSIZE_ANY(value);
				entry->valptr = dsa_allocate(dsa, VARSIZE_ANY(value));

				memcpy(dsa_get_address(dsa, entry->valptr), DatumGetPointer(value), VARSIZE_ANY(value));

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


		elog(DEBUG1, "Inserted new entry key=%s", text_to_cstring(key));
	}

	elog(DEBUG1, "Copying entry value back to result");

	spval *result = (spval *) palloc(sizeof(spval));

	if (entry->valtypid == TEXTOID)
	{
		/* This actually works with any varlena type, but better be explicit for now */

		text *text_result = (text *) palloc(entry->valsz);

		memcpy(VARDATA(text_result), VARDATA(dsa_get_address(dsa, entry->valptr)), entry->valsz - VARHDRSZ);
		SET_VARSIZE(text_result, entry->valsz);

		result->type = SPVAL_STRING;
		result->value.varlena_val = text_result;
	}

	else
	{
		elog(ERROR, "Unsupported spval return for type oid=%d", entry->valtypid);
	}

	if (found || !found)
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
	text 		*key 	= PG_GETARG_TEXT_PP(0);

	/* Output */
	spval 		*result;

	/* Processing */
	dsa_handle              dsa_handle;
	dsa_pointer             dsa_key;
	dsa_pointer				dsa_value;
	dsa_area                *dsa;
	dshash_table_handle     htab_handle;
	dshash_table			*htab;
	bool                    found;
	SpatDBEntry				*entry;

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
		/* found */
		result = (spval *) palloc(sizeof(spval));
		Assert(entry->valtypid == TEXTOID);
		text *text_result = (text *) palloc(entry->valsz);

		memcpy(VARDATA(text_result), VARDATA(dsa_get_address(dsa, entry->valptr)), entry->valsz - VARHDRSZ);
		SET_VARSIZE(text_result, entry->valsz);

		result->type = SPVAL_STRING;
		result->value.varlena_val = text_result;
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
	text 		*key 	= PG_GETARG_TEXT_PP(0);

	/* Processing */
	dsa_handle              dsa_handle;
	dsa_pointer             dsa_key;
	dsa_area                *dsa;
	dshash_table_handle     htab_handle;
	dshash_table			*htab;
	bool                    found;

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
	text 		*key = 	PG_GETARG_TEXT_PP(0);

	PG_RETURN_TEXT_P(cstring_to_text("Hello World!"));
}

PG_FUNCTION_INFO_V1(sp_db_size);
Datum
sp_db_size(PG_FUNCTION_ARGS)
{
	int32 nitems = 0;

	/* Processing */
	dsa_handle              dsa_handle;
	dsa_area                *dsa;
	dshash_table_handle     htab_handle;
	dshash_table			*htab;
	SpatDBEntry				*entry;

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

PG_FUNCTION_INFO_V1(spkeys);
Datum
spkeys(PG_FUNCTION_ARGS)
{
	ArrayType *result = NULL;

	/* Processing */
	dsa_handle              dsa_handle;
	dsa_area                *dsa;
	dshash_table_handle     htab_handle;
	dshash_table			*htab;
	SpatDBEntry				*entry;

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
	{
		PG_RETURN_TEXT_P(cstring_to_text(dsa_get_address(dsa, entry->key)));
	}
	dshash_seq_term(&status);

	/* leaving dsa territory */
	dsa_detach(dsa);
}

PG_FUNCTION_INFO_V1(spscan);
Datum
spscan(PG_FUNCTION_ARGS)
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
	spval *result;
	const char *example_text = "Hello, PostgreSQL!";

	/* Allocate memory for spval */
	result = (spval *) palloc(sizeof(spval));

	/* Set the type to SPVAL_VARLENA */
	result->type = SPVAL_STRING;

	/* Convert the C string to a PostgreSQL text datum */
	result->value.varlena_val = cstring_to_text(example_text);

	/* Return the spval structure */
	PG_RETURN_POINTER(result);
}
