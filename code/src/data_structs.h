/*
  Support for struct data type in ADLB
 */

#ifndef __XLB_DATA_STRUCTS_H
#define __XLB_DATA_STRUCTS_H

#include "adlb-defs.h"
#include "adlb_types.h"

// Free all memory allocated within this module
adlb_data_code xlb_struct_finalize(void);

// Return the name of a declared struct type
const char *xlb_struct_type_name(adlb_struct_type type);

// Free memory associated with struct, including the
// provided pointer if specified
adlb_data_code xlb_free_struct(adlb_struct *s, bool free_root_ptr);

// Get data for struct field
adlb_data_code xlb_struct_get_field(adlb_struct *s, int field_ix,
                        const adlb_datum_storage **val, adlb_data_type *type);

adlb_data_code
xlb_struct_cleanup(adlb_struct *s, bool free_mem,
        adlb_refcounts rc_change, adlb_refcounts scav_refcounts, int scav_ix);

adlb_data_code
xlb_struct_incr_referand(adlb_struct *s, adlb_refcounts rc_change);

char *xlb_struct_repr(adlb_struct *s);

// Convert subscript to struct field ID
adlb_data_code xlb_struct_str_to_ix(adlb_subscript subscript, int *field_ix);

#endif // __XLB_DATA_STRUCTS_H
