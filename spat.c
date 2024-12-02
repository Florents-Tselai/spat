/*--------------------------------------------------------------------------
 *
 * spat.c
 *	  Does this that
 *
 * https://github.com/Florents-Tselai/spat
 * Copyright (c) 2024, Florents Tselai
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/scanner.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "storage/dsm_registry.h"

PG_MODULE_MAGIC;

#define SPAT_TRANCH_NAME "spat"

static char *spat_name		= SPAT_TRANCH_NAME;
static int	spat_init_size	= 100;
static int	spat_max_size	= 1000;

typedef enum spatType
{
	SPAT_STRING
} spatType;

typedef char* spatKey;
typedef Datum spatValue;

typedef struct spatEntry
{
	char*		key;		/* must be first */
	spatType	type;		/* the type of the value e.g. string, list, set etc.*/
	Datum		value;
} spatEntry;

typedef struct SpatState
{
	int			val;
	HTAB		*spat_hash;
	LWLock		lck;
} spatSharedState;

static spatSharedState *spat = NULL;

static void
spat_init_shmem(void *ptr)
{

	HASHCTL hashctl;
	spatSharedState *state = (spatSharedState *) ptr;

	LWLockInitialize(&state->lck, LWLockNewTrancheId());
	state->val = 0;

	hashctl.keysize = sizeof(spatKey);
	hashctl.entrysize = sizeof(spatEntry);
	state->spat_hash = ShmemInitHash(spat_name, spat_init_size, spat_max_size, &hashctl, HASH_ELEM);
}

static void
spat_atach_shmem(void)
{
	bool		found;

	spat = GetNamedDSMSegment(spat_name,
								   sizeof(spatSharedState),
								   spat_init_shmem,
								   &found);
	LWLockRegisterTranche(spat->lck.tranche, spat_name);
}


PG_FUNCTION_INFO_V1(set_val_in_shmem);
Datum
set_val_in_shmem(PG_FUNCTION_ARGS)
{
	spat_atach_shmem();
	LWLockAcquire(&spat->lck, LW_EXCLUSIVE);
	spat->val = PG_GETARG_UINT32(0);
	LWLockRelease(&spat->lck);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_val_in_shmem);
Datum
get_val_in_shmem(PG_FUNCTION_ARGS)
{
	spat_atach_shmem();

	int			ret;

	LWLockAcquire(&spat->lck, LW_SHARED);
	ret = spat->val;
	LWLockRelease(&spat->lck);

	PG_RETURN_UINT32(ret);
}

PG_FUNCTION_INFO_V1(store_size);
Datum
store_size(PG_FUNCTION_ARGS)
{
	spat_atach_shmem();

	int			ret;
	HASHCTL hashctl;

	LWLockAcquire(&spat->lck, LW_SHARED);

	ret = hash_get_num_entries(spat->spat_hash);

	LWLockRelease(&spat->lck);

	PG_RETURN_UINT32(ret);
}

PG_FUNCTION_INFO_V1(spat_set);
Datum
spat_set(PG_FUNCTION_ARGS)
{
	spat_atach_shmem();

	int			ret;
	bool found;
	spatEntry *entry = palloc(sizeof(spatEntry));
	entry->key = text_to_cstring(PG_GETARG_TEXT_P_COPY(0));
	entry->type = SPAT_STRING;
	entry->value = PG_GETARG_DATUM(1);

	LWLockAcquire(&spat->lck, LW_EXCLUSIVE);

	entry = hash_search(spat->spat_hash, entry->key, HASH_ENTER, &found);

	ret = hash_get_num_entries(spat->spat_hash);

	LWLockRelease(&spat->lck);

	PG_RETURN_UINT32(ret);
}