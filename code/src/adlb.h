/*
 * Copyright 2013 University of Chicago and Argonne National Laboratory
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */


#ifndef ADLB_H
#define ADLB_H

// Need _GNU_SOURCE for asprintf()
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>

#include <mpi.h>

#include "adlb-defs.h"

#include "version.h"

#define XLB
#define XLB_VERSION 0

/*
   These are the functions available to ADLB application code
 */

adlb_code ADLBP_Init(int nservers, int ntypes, int type_vect[],
                     int *am_server, MPI_Comm adlb_comm,
                     MPI_Comm *worker_comm);
adlb_code ADLB_Init(int nservers, int ntypes, int type_vect[],
                    int *am_server, MPI_Comm adlb_comm,
                    MPI_Comm *worker_comm);

adlb_code ADLB_Server(long max_memory);

adlb_code ADLB_Version(version* output);

adlb_code ADLB_Hostmap_stats(uint* count, uint* name_max);

adlb_code ADLB_Hostmap_lookup(const char* name, int count,
                              int* output, int* actual);

/**
   Obtain RS-separated buffer of host names
   @param output: OUT Buffer into which to write result
   @param max: Maximal number of characters to write
   @param offset: Start with this hostname
   @param actual: OUT Number of hostnames written
 */
adlb_code ADLB_Hostmap_list(char* output, uint max, uint offset,
                            int* actual);

adlb_code ADLBP_Put(const void* payload, int length, int target, int answer,
                    int type, int priority, int parallelism);
adlb_code ADLB_Put(const void* payload, int length, int target, int answer,
                   int type, int priority, int parallelism);

adlb_code ADLBP_Get(int type_requested, void* payload, int* length,
                    int* answer, int* type_recvd, MPI_Comm* comm);
adlb_code ADLB_Get(int type_requested, void* payload, int* length,
                   int* answer, int* type_recvd, MPI_Comm* comm);

adlb_code ADLBP_Iget(int type_requested, void* payload, int* length,
                     int* answer, int* type_recvd);
adlb_code ADLB_Iget(int type_requested, void* payload, int* length,
                    int* answer, int* type_recvd);

/**
   Obtain server rank responsible for data id
 */
int ADLB_Locate(adlb_datum_id id);

// Applications should not call these directly but
// should use the typed forms defined below
adlb_code ADLBP_Create(adlb_datum_id id, adlb_data_type type,
                       adlb_type_extra type_extra,
                       adlb_create_props props,
                       adlb_datum_id *new_id);
adlb_code ADLB_Create(adlb_datum_id id, adlb_data_type type,
                      adlb_type_extra type_extra,
                      adlb_create_props props, adlb_datum_id *new_id);

// Create multiple variables.
// Currently we assume that spec[i].id is ADLB_DATA_ID_NULL and
// will be filled in with a new id
adlb_code ADLB_Multicreate(ADLB_create_spec *specs, int count);
adlb_code ADLBP_Multicreate(ADLB_create_spec *specs, int count);


adlb_code ADLB_Create_integer(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_float(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_string(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_blob(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_ref(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_file_ref(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_struct(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id);

adlb_code ADLB_Create_container(adlb_datum_id id,
                                adlb_data_type key_type, 
                                adlb_data_type val_type, 
                                adlb_create_props props,
                                adlb_datum_id *new_id);

adlb_code ADLB_Create_multiset(adlb_datum_id id,
                                adlb_data_type val_type, 
                                adlb_create_props props,
                                adlb_datum_id *new_id);

adlb_code ADLBP_Exists(adlb_datum_id id, adlb_subscript subscript, bool* result,
                       adlb_refcounts decr);
adlb_code ADLB_Exists(adlb_datum_id id, adlb_subscript subscript, bool* result,
                       adlb_refcounts decr);

/*
  Store value into datum
  data: binary representation
  length: length of binary representation
 */
adlb_code ADLBP_Store(adlb_datum_id id, adlb_subscript subscript,
                      adlb_data_type type, const void *data, int length,
                      adlb_refcounts refcount_decr);
adlb_code ADLB_Store(adlb_datum_id id, adlb_subscript subscript,
                      adlb_data_type type, const void *data, int length,
                      adlb_refcounts refcount_decr);

/*
   Retrieve contents of datum.
    
   refcounts: specify how reference counts should be changed
      read_refcount: decrease read refcount of this datum
      incr_read_referand: increase read refcount of referands,
                to ensure they aren't cleaned up prematurely
   type: output arg for the type of the datum
   data: a buffer of at least size ADLB_DATA_MAX
   length: output arg for data size in bytes
 */
adlb_code ADLBP_Retrieve(adlb_datum_id id, adlb_subscript subscript,
      adlb_retrieve_rc refcounts,
      adlb_data_type *type, void *data, int *length);
adlb_code ADLB_Retrieve(adlb_datum_id id, adlb_subscript subscript,
      adlb_retrieve_rc refcounts, adlb_data_type *type, 
      void *data, int *length);

/*
   List contents of container
   
   data: binary encoded keys and values (if requested), with each member
          encoded as follows:
        - key length encoded with vint_encode()
        - key data without null terminator
        - value length encoded with vint_encode()
        - value data encoded with ADLB_Pack
   length: number of bytes of data
   records: number of elements returned
 */
adlb_code ADLBP_Enumerate(adlb_datum_id container_id,
                   int count, int offset, adlb_refcounts decr,
                   bool include_keys, bool include_vals,
                   void** data, int* length, int* records,
                   adlb_type_extra *kv_type);
adlb_code ADLB_Enumerate(adlb_datum_id container_id,
                   int count, int offset, adlb_refcounts decr,
                   bool include_keys, bool include_vals,
                   void** data, int* length, int* records,
                   adlb_type_extra *kv_type);

// Switch on read refcounting and memory management, which is off by default
adlb_code ADLBP_Read_refcount_enable(void);
adlb_code ADLB_Read_refcount_enable(void);

adlb_code ADLBP_Refcount_incr(adlb_datum_id id, adlb_refcounts change);
adlb_code ADLB_Refcount_incr(adlb_datum_id id, adlb_refcounts change);

/*
  Try to reserve an insert position in container
  result: true if could be created, false if already present
  data: optionally, if this is not NULL, return the existing value in
        this buffer of at least size ADLB_DATA_MAX
  length: length of existing value, -1 if value not yet present
  type: type of existing value
 */
adlb_code ADLBP_Insert_atomic(adlb_datum_id id, adlb_subscript subscript,
                        bool* result, void *data, int *length,
                        adlb_data_type *type);
adlb_code ADLB_Insert_atomic(adlb_datum_id id, adlb_subscript subscript,
                       bool* result, void *data, int *length,
                       adlb_data_type *type);

/*
  returns: ADLB_SUCCESS if datum found
       ADLB_DATA_ERROR_NOT_FOUND if datum not found (can indicate it was gced)
 */
adlb_code ADLBP_Subscribe(adlb_datum_id id, adlb_subscript subscript,
                          int* subscribed);
adlb_code ADLB_Subscribe(adlb_datum_id id, adlb_subscript subscript,
                          int* subscribed);

adlb_code ADLBP_Container_reference(adlb_datum_id id, adlb_subscript subscript,
                              adlb_datum_id reference,
                              adlb_data_type ref_type);
adlb_code ADLB_Container_reference(adlb_datum_id id, adlb_subscript subscript,
                             adlb_datum_id reference,
                              adlb_data_type ref_type);

adlb_code ADLBP_Unique(adlb_datum_id *result);
adlb_code ADLB_Unique(adlb_datum_id *result);

adlb_code ADLBP_Typeof(adlb_datum_id id, adlb_data_type* type);
adlb_code ADLB_Typeof(adlb_datum_id id, adlb_data_type* type);

adlb_code ADLBP_Container_typeof(adlb_datum_id id, adlb_data_type* key_type,
                                 adlb_data_type* val_type);
adlb_code ADLB_Container_typeof(adlb_datum_id id, adlb_data_type* key_type,
                                 adlb_data_type* val_type);

adlb_code ADLBP_Container_size(adlb_datum_id container_id, int* size,
                               adlb_refcounts decr);
adlb_code ADLB_Container_size(adlb_datum_id container_id, int* size,
                              adlb_refcounts decr);

adlb_code ADLBP_Lock(adlb_datum_id id, bool* result);
adlb_code ADLB_Lock(adlb_datum_id id, bool* result);

adlb_code ADLBP_Unlock(adlb_datum_id id);
adlb_code ADLB_Unlock(adlb_datum_id id);

adlb_code ADLB_Data_string_totype(const char* type_string,
                                  adlb_data_type* type, bool *has_extra,
                                  adlb_type_extra *extra);

const char *ADLB_Data_type_tostring(adlb_data_type type);

adlb_code ADLB_Server_idle(int rank, bool* result);
adlb_code ADLB_Server_shutdown(int rank);

adlb_code ADLBP_Finalize(void);
adlb_code ADLB_Finalize(void);

adlb_code ADLB_Fail(int code);

void ADLB_Abort(int code);

#endif

