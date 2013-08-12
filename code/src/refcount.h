
/*
  Helper functions to manipulate refcounts from a server
 */

#ifndef __XLB_REFCOUNT_H
#define __XLB_REFCOUNT_H

#include "adlb-defs.h"
#include "data.h"
#include "data_internal.h"


// Decrement reference count of given id
adlb_data_code incr_rc_svr(adlb_datum_id id, adlb_refcounts change);

/* Modify refcount of referenced items */
adlb_data_code
incr_rc_referand(adlb_datum_storage *d,
        adlb_data_type type, adlb_refcounts change);

/* Modify refcount of referenced items.  If to_scavenge is positive,
   scavenge that number of read references to referands.
 */
adlb_data_code
incr_scav_rc_referand(adlb_datum_storage *d, adlb_data_type type,
        adlb_refcounts change, adlb_refcounts to_scavenge);

/*
  Decrements refcount of datum, while incrementing
  refcount of other datums referred to by this datum
  TODO: in future, ability to offload this work to client
 */
adlb_data_code
update_read_refcount_scav(adlb_datum_id id, const char *subscript,
        const void *ref_data, int ref_data_len, adlb_data_type ref_type,
        adlb_refcounts decr_self, adlb_refcounts incr_referand,
        adlb_ranks *notifications);

#endif // __XLB_REFCOUNT_H