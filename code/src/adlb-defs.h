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


/**
 * Basic definitions used by the ADLB Data module
 * */

#ifndef ADLB_DEFS_H
#define ADLB_DEFS_H

#include <list_i.h>
#include <list.h>
#include <list_l.h>

/**
   ADLB common return codes
   The only real error condition is ADLB_ERROR
   Cf. ADLB_IS_ERROR()
 */
typedef enum
{
 ADLB_SUCCESS  =  1,
 ADLB_ERROR    = -1,
 /** Rejected: e.g., out of memory, or double-assignment */
 ADLB_REJECTED = -2,
 /** Normal shutdown */
 ADLB_SHUTDOWN = -3,
 /** No error but indicate nothing happened */
 ADLB_NOTHING = -4,
 /** Indicate that caller should retry */
 ADLB_RETRY = -5,
 /** Indicate something is finished and shouldn't call again */
 ADLB_DONE = -6,
} adlb_code;

/**
   Identifier for all ADLB data module user data.
   Negative values are reserved for system functions
 */
typedef int64_t adlb_datum_id;

/**
   User data types
 */
typedef enum
{
  ADLB_DATA_TYPE_NULL = 0,
  ADLB_DATA_TYPE_INTEGER,
  ADLB_DATA_TYPE_FLOAT,
  ADLB_DATA_TYPE_STRING,
  ADLB_DATA_TYPE_BLOB,
  ADLB_DATA_TYPE_CONTAINER,
  ADLB_DATA_TYPE_MULTISET,
  ADLB_DATA_TYPE_STRUCT,
  ADLB_DATA_TYPE_REF,
} adlb_data_type;

// More compact representation for data type
typedef short adlb_data_type_short;

// Identifier for sub-types of ADLB struct
typedef int adlb_struct_type;

#define ADLB_STRUCT_TYPE_NULL (-1)

// Additional type info for particular types
typedef struct {
  union {
    struct {
      adlb_data_type_short key_type;
      adlb_data_type_short val_type;
    } CONTAINER;
    struct {
      adlb_data_type_short val_type;
    } MULTISET;
    struct {
      adlb_struct_type struct_type; 
    } STRUCT;
  };
  bool valid; // If true, data is actually valid
} adlb_type_extra;


/* C++ doesn't support named initializers, no nice way to
 * initialize the above struct in C++ */
#ifndef __cplusplus
static const adlb_type_extra ADLB_TYPE_EXTRA_NULL = {
  .valid = false
};
#endif

// Struct to specify a subscript into e.g. an ADLB data container
typedef struct {
  const void *key; // Set key to NULL to indicate no subscript
  size_t length;
} adlb_subscript;

static const adlb_subscript ADLB_NO_SUB = { 
    NULL, /* key */
    0u, /* length */};

// Check if subscript present
static inline bool adlb_has_sub(adlb_subscript sub)
{
  return sub.key != NULL;
}

typedef struct {
  adlb_datum_id id;
  adlb_subscript subscript;
} adlb_datum_id_sub;

typedef enum
{
  ADLB_READ_REFCOUNT,
  ADLB_WRITE_REFCOUNT,
  ADLB_READWRITE_REFCOUNT, // Used to specify that op should affect both
} adlb_refcount_type;

// Struct used to hold refcount info
typedef struct {
  int read_refcount;
  int write_refcount;
} adlb_refc;

static const adlb_refc ADLB_NO_REFC =
  { 0 /* read_refcount */, 0 /* write_refcount */ };

static const adlb_refc ADLB_READ_REFC =
  { 1 /*read_refcount */, 0 /* write_refcount */ };

static const adlb_refc ADLB_WRITE_REFC =
  { 0 /* read_refcount */, 1 /* write_refcount */ };

static const adlb_refc ADLB_READWRITE_REFC =
  { 1 /* read_refcount */, 1 /* write_refcount */ };

#define ADLB_REFC_IS_NULL(refc) \
    ((refc).read_refcount == 0 && (refc).write_refcount == 0)

#define ADLB_REFC_NOT_NULL(refc) \
    ((refc).read_refcount != 0 || (refc).write_refcount != 0)

#define ADLB_REFC_POSITIVE(refc) \
    ((refc).read_refcount > 0 && (refc).write_refcount > 0)

#define ADLB_REFC_NONNEGATIVE(refc) \
    ((refc).read_refcount >= 0 && (refc).write_refcount >= 0)

#define ADLB_REFC_NEGATIVE(refc) \
    ((refc).read_refcount < 0 && (refc).write_refcount < 0)

#define ADLB_REFC_NONPOSITIVE(refc) \
    ((refc).read_refcount <= 0 && (refc).write_refcount <= 0)

static inline adlb_refc adlb_refc_negate(adlb_refc refc)
{
  adlb_refc result = { -refc.read_refcount, /* read_refcount */
                            -refc.write_refcount /* write_refcount */ };
  return result;
}

/** Identifier for adlb data debug symbol */
typedef uint32_t adlb_dsym;
#define ADLB_DEBUG_SYMBOL_NULL 0u

/** Data associated with adlb data debug symbol */
typedef struct {
  /** Short name identifying data */
  const char *name;

  /** Additional contextual information */
  const char *context;
} adlb_dsym_data;

// Prefer to tightly pack these structs
#pragma pack(push, 1)
typedef struct
{
  int read_refcount;
  int write_refcount;
  bool permanent : 1;
  bool release_write_refs : 1;
  adlb_dsym symbol;
} adlb_create_props;

// Default settings for new variables
static const adlb_create_props DEFAULT_CREATE_PROPS = {
  1, /* read_refcount */
  1, /* write_refcount */
  false, /* permanent */
  false, /* release_write_refs */
  ADLB_DEBUG_SYMBOL_NULL, /* symbol */
};

// Information for new variable creation
typedef struct {
  adlb_datum_id id;
  adlb_data_type type;
  adlb_type_extra type_extra;
  adlb_create_props props;
} ADLB_create_spec;

#pragma pack(pop) // Undo pragma change

/* 
   Describe how refcounts should be changed
 */
typedef struct
{
  // decrease reference count of this datum
  adlb_refc decr_self;
  // increase reference count of anything referenced by this datum
  adlb_refc incr_referand;
} adlb_retrieve_refc;

static const adlb_retrieve_refc ADLB_RETRIEVE_NO_REFC = { 
      { 0, 0 }, /* decr_self read and write refcounts */
      { 0, 0 }, /* incr_reference read and write refcounts */
};

// Read a value variable
static const adlb_retrieve_refc ADLB_RETRIEVE_READ_REFC = { 
      { 1, 0 }, /* decr_self read and write refcounts */
      { 0, 0 }, /* incr_reference read and write refcounts */
};

// Read a reference variable and acquire reference to referand
static const adlb_retrieve_refc ADLB_RETRIEVE_ACQUIRE_REFC = { 
      { 1, 0 }, /* decr_self read and write refcounts */
      { 1, 0 }, /* incr_reference read and write refcounts */
};


/**
   Common return codes
 */
typedef enum
{
  ADLB_DATA_SUCCESS,
  /** Out of memory */
  ADLB_DATA_ERROR_OOM,
  /** Attempt to declare the same thing twice */
  ADLB_DATA_ERROR_DOUBLE_DECLARE,
  /** Attempt to set the same datum twice */
  ADLB_DATA_ERROR_DOUBLE_WRITE,
  /** Attempt to read an unset value */
  ADLB_DATA_ERROR_UNSET,
  /** Data set not found */
  ADLB_DATA_ERROR_NOT_FOUND,
  /** Subscript not present */
  ADLB_DATA_ERROR_SUBSCRIPT_NOT_FOUND,
  /** Parse error in number scanning */
  ADLB_DATA_ERROR_NUMBER_FORMAT,
  /** Invalid input */
  ADLB_DATA_ERROR_INVALID,
  /** Attempt to read/write ADLB_DATA_ID_NULL */
  ADLB_DATA_ERROR_NULL,
  /** Attempt to operate on wrong data type */
  ADLB_DATA_ERROR_TYPE,
  /** Refcount fell below 0 */
  ADLB_DATA_ERROR_REFCOUNT_NEGATIVE,
  /** Exceeded some implementation-defined limit */
  ADLB_DATA_ERROR_LIMIT,
  /** Unresolved future */
  ADLB_DATA_ERROR_UNRESOLVED,
  /** Caller-provided buffer too small */
  ADLB_DATA_BUFFER_TOO_SMALL,
  /** Finished */
  ADLB_DATA_DONE,
  /** Unknown error */
  ADLB_DATA_ERROR_UNKNOWN,
} adlb_data_code;

//// Miscellaneous symbols:
#define ADLB_RANK_ANY  -100
#define ADLB_RANK_NULL -200
#define ADLB_TYPE_ANY  -300
#define ADLB_TYPE_NULL -400

/** The adlb_datum_id of nothing */
#define ADLB_DATA_ID_NULL 0

/** The maximal string length of a container subscript */
#define ADLB_DATA_SUBSCRIPT_MAX 1024

/** The maximal length of an ADLB datum (string, blob, etc.) */
#define ADLB_DATA_MAX (20*1024*1024)

/** Maximum size for a given ADLB transaction */
#define ADLB_PAYLOAD_MAX ADLB_DATA_MAX

/** Maximum size for ADLB checkpoint value */
#define ADLB_XPT_MAX (ADLB_DATA_MAX - 1)

/**
  printf specifiers for printing data identifier with debug symbol.
  Matching arg types: adlb_datum_id (id), char* (debug symbol name),
    char* (debug symbol context)
 */
#define ADLB_PRI_DATUM "<%"PRId64">:%s (%s)"

/** 
  printf specifiers for printing data identifier with subscript and
  debug symbol.
  Matching arg types: adlb_datum_id (id), char* (debug symbol name),
          int (subscript len), char* (subscript),
          char* (debug symbol context)
*/
#define ADLB_PRI_DATUM_SUB "<%"PRId64">:%s[%.*s] (%s)"

#endif
