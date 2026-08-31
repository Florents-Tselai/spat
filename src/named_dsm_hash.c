/*
 * named_dsm_hash.c
 *
 * Fallback implementation of GetNamedDSMHash() for PostgreSQL 17/18.
 *
 * Starting with PostgreSQL 19 (commitfest patch 5799), the core provides
 * GetNamedDSMHash() which creates or attaches to a named dshash table
 * backed by a DSA segment in the DSM registry.  This file provides an
 * equivalent local implementation so that spat can use a single code path
 * via the SPAT_GET_NAMED_DSM_HASH macro on all supported versions.
 *
 * This file is only compiled when PG_VERSION_NUM < 190000.
 */

#include "spat.h"

#if PG_VERSION_NUM < 190000

#define DSMR_DSA_TRANCHE_NAME_LEN  128
#define DSMR_DSH_TRANCHE_NAME_LEN  128
#define DSMR_DSA_TRANCHE_SUFFIX    " DSA"
#define DSMR_DSH_TRANCHE_SUFFIX    " Hash"

typedef struct NamedDSMHashState
{
	dsa_handle		dsah;
	dshash_table_handle dshh;
	int				dsa_tranche;
	char			dsa_tranche_name[DSMR_DSA_TRANCHE_NAME_LEN];
	int				dsh_tranche;
	char			dsh_tranche_name[DSMR_DSH_TRANCHE_NAME_LEN];
} NamedDSMHashState;

/*
 * Initialize or attach a named dshash table.
 *
 * This routine returns the address of the table.  The tranche_id member of
 * params is ignored; new tranche IDs will be generated if needed.  Note that
 * the lock tranches will be registered with the provided name with " DSA" or
 * " Hash" appended accordingly.
 */
dshash_table *
spat_get_named_dsm_hash(const char *name,
                        const dshash_parameters *params,
                        bool *found)
{
	NamedDSMHashState *state;
	MemoryContext oldcontext;
	dsa_area   *dsa;
	dshash_table *dsh;
	bool		unused;

	state = GetNamedDSMSegment(name, sizeof(NamedDSMHashState), NULL, &unused);

	/* Use a lock to ensure only one process creates the table. */
	LWLockAcquire(DSMRegistryLock, LW_EXCLUSIVE);

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (state->dshh == DSHASH_HANDLE_INVALID)
	{
		dshash_parameters params_copy;

		/* Initialize LWLock tranche for the DSA. */
		state->dsa_tranche = LWLockNewTrancheId();
		snprintf(state->dsa_tranche_name,
				 DSMR_DSA_TRANCHE_NAME_LEN,
				 "%s%s", name, DSMR_DSA_TRANCHE_SUFFIX);
		LWLockRegisterTranche(state->dsa_tranche, state->dsa_tranche_name);

		/* Initialize LWLock tranche for the dshash table. */
		state->dsh_tranche = LWLockNewTrancheId();
		snprintf(state->dsh_tranche_name,
				 DSMR_DSH_TRANCHE_NAME_LEN,
				 "%s%s", name, DSMR_DSH_TRANCHE_SUFFIX);
		LWLockRegisterTranche(state->dsh_tranche, state->dsh_tranche_name);

		/* Initialize DSA for the hash table. */
		dsa = dsa_create(state->dsa_tranche);
		dsa_pin(dsa);
		dsa_pin_mapping(dsa);

		/* Initialize the dshash table. */
		memcpy(&params_copy, params, sizeof(dshash_parameters));
		params_copy.tranche_id = state->dsh_tranche;
		dsh = dshash_create(dsa, &params_copy, NULL);

		/* Store handles in the DSM segment for other backends to use. */
		state->dsah = dsa_get_handle(dsa);
		state->dshh = dshash_get_hash_table_handle(dsh);

		*found = false;
	}
	else
	{
		/* Initialize existing LWLock tranches for the DSA and dshash table. */
		LWLockRegisterTranche(state->dsa_tranche, state->dsa_tranche_name);
		LWLockRegisterTranche(state->dsh_tranche, state->dsh_tranche_name);

		/* Attach to existing DSA for the hash table. */
		dsa = dsa_attach(state->dsah);
		dsa_pin_mapping(dsa);

		/* Attach to existing dshash table. */
		dsh = dshash_attach(dsa, params, state->dshh, NULL);

		*found = true;
	}

	MemoryContextSwitchTo(oldcontext);
	LWLockRelease(DSMRegistryLock);

	return dsh;
}

#endif /* PG_VERSION_NUM < 190000 */
