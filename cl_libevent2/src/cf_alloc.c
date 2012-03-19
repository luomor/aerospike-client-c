/*
 *  Citrusleaf Foundation
 *  include/alloc.h - memory allocation framework
 *
 *  Copyright 2008 by Citrusleaf.  All rights reserved.
 *  THIS IS UNPUBLISHED PROPRIETARY SOURCE CODE.  THE COPYRIGHT NOTICE
 *  ABOVE DOES NOT EVIDENCE ANY ACTUAL OR INTENDED PUBLICATION.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "citrusleaf_event2/cf_atomic.h"
#include "citrusleaf_event2/cf_alloc.h"


/* cf_rc_count
 * Get the reservation count for a memory region */
cf_atomic_int_t
cf_rc_count(void *addr)
{
	cf_rc_counter *rc;

	rc = (cf_rc_counter *) (((uint8_t *)addr) - sizeof(cf_rc_counter));

	return(*rc);
}


/* cf_rc_reserve
 * Get a reservation on a memory region */
int
cf_rc_reserve(void *addr)
{
	cf_rc_counter *rc;

	/* Extract the address of the reference counter, then increment it */
	rc = (cf_rc_counter *) (((uint8_t *)addr) - sizeof(cf_rc_counter));

	int i = (int) cf_atomic32_add(rc, 1);
	smb_mb();
	return(i);
}


/* _cf_rc_release
 * Release a reservation on a memory region */
cf_atomic_int_t
_cf_rc_release(void *addr, bool autofree)
{
	cf_rc_counter *rc;
	uint64_t c;
	
	/* Release the reservation; if this reduced the reference count to zero,
	 * then free the block if autofree is set, and return 1.  Otherwise,
	 * return 0 */
	rc = (cf_rc_counter *) (((uint8_t *)addr) - sizeof(cf_rc_counter));
	smb_mb();
	if (0 == (c = cf_atomic32_decr(rc)))
		if (autofree)
			free((void *)rc);

	return(c);
}


/* cf_rc_alloc
 * Allocate a reference-counted memory region.  This region will be filled
 * with uint8_ts of value zero */
void *
cf_rc_alloc(size_t sz)
{
	uint8_t *addr;
	size_t asz = sizeof(cf_rc_counter) + sz;

	addr = malloc(asz);
	if (NULL == addr)
		return(NULL);

	cf_atomic_int_set((cf_atomic_int *)addr, 1);
	uint8_t *base = addr + sizeof(cf_rc_counter);

	return(base);
}


/* cf_rc_free
 * Deallocate a reference-counted memory region */
void
cf_rc_free(void *addr)
{
	cf_rc_counter *rc;

	rc = (cf_rc_counter *) (((uint8_t *)addr) - sizeof(cf_rc_counter));

	free((void *)rc);

	return;
}
