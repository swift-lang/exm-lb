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


/*
 * messaging.h
 *
 *  Created on: Jun 7, 2012
 *      Author: wozniak
 *
 *  XLB messaging conventions
 */

#ifndef MESSAGING_H
#define MESSAGING_H

#include "adlb.h"
#include "checks.h"
#include "debug.h"
#include "workqueue.h"

/**
   Initialize the messaging functionality
 */
void xlb_msg_init(void);

/**
   Finalize the messaging functionality
 */
void xlb_msg_finalize(void);

#ifndef NDEBUG

/**
   Set a string name for debugging MPI message tags
 */
void xlb_add_tag_name(int tag, char* name);

/**
   Lookup string name for debugging MPI message tags
 */
char* xlb_get_tag_name(int tag);

#else
#define xlb_add_tag_name(tag,name) // noop
#define xlb_get_tag_name(tag)      // noop
#endif

/*
   All of these client/handler functions (adlb.c,handlers.c,etc.)
   use messaging the same way:
   - They use communicator adlb_comm
   - They use the stack-allocated status or request object
   - They check the return code rc with MPI_CHECK() to return errors.
   Thus, we use these macros to ease reading the message protocols
   This also allows us to wrap each call with TRACE_MPI()
        for message debugging
   Note that the rc (return code) used here is in a nested scope
 */

#define SEND(data,length,type,rank,tag) { \
  TRACE_MPI("SEND(to=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Send(data,length,type,rank,tag,adlb_comm); \
  MPI_CHECK(rc); }

#define RSEND(data,length,type,rank,tag) { \
  TRACE_MPI("RSEND(to=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Rsend(data,length,type,rank,tag,adlb_comm); \
  MPI_CHECK(rc); }

#define SSEND(data,length,type,rank,tag) { \
  TRACE_MPI("SSEND(to=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Ssend(data,length,type,rank,tag,adlb_comm); \
  TRACE_MPI("SSENT"); \
  MPI_CHECK(rc); }

#define ISEND(data,length,type,rank,tag,req) { \
  TRACE_MPI("ISEND(to=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Isend(data,length,type,rank,tag,adlb_comm,req); \
  MPI_CHECK(rc); }


#define RECV(data,length,type,rank,tag) \
        RECV_STATUS(data,length,type,rank,tag,&status)

#define RECV_STATUS(data,length,type,rank,tag,status_ptr) { \
  TRACE_MPI("RECV(from=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Recv(data,length,type,rank,tag, \
                    adlb_comm,status_ptr); \
  TRACE_MPI("RECVD"); \
  MPI_CHECK(rc); }

#define IRECV(data,length,type,rank,tag) { \
  TRACE_MPI("IRECV(from=%i,tag=%s)", rank, xlb_get_tag_name(tag)); \
  int rc = MPI_Irecv(data,length,type,rank,tag, \
                     adlb_comm,&request); \
  MPI_CHECK(rc); }

// We don't TRACE this
#define IPROBE(target,tag,flag,status) { \
    int rc = MPI_Iprobe(target,tag,adlb_comm,flag,status); \
    MPI_CHECK(rc); }

#define WAIT(r,s) { \
  TRACE_MPI("WAIT"); \
  int rc = MPI_Wait(r,s); \
  MPI_CHECK(rc); \
  TRACE_MPI("WAITED"); }

/** Simplify cases when only a tag is sent */
#define SEND_TAG(rank,tag) SEND(NULL,0,MPI_BYTE,rank,tag)

/** Simplify cases when only a tag is recvd */
#define RECV_TAG(rank,tag) RECV(NULL,0,MPI_BYTE,rank,tag)

/** MPI data type tags */
// 64-bit int
#define MPI_ADLB_ID MPI_LONG_LONG

// Macros to help with packing and unpacking variables
// TODO: error handling in case buffer too small

#define MSG_PACK_BIN(buf_pos, var)        \
  {                                       \
    memcpy(buf_pos, &var, sizeof(var));   \
    buf_pos += sizeof(var);               \
  }

#define MSG_UNPACK_BIN(buf_pos, var)      \
  {                                       \
    memcpy(var, buf_pos, sizeof(*var));   \
    buf_pos += sizeof(*var);               \
  }

/**
   Simple struct for message packing
 */
struct packed_put
{
  int type;
  int priority;
  int putter;
  int answer;
  int target;
  int length;
  int parallelism;
};

/**
   Simple struct for message packing
 */
struct packed_get_response
{
  adlb_code code;
  int length;
  int answer_rank;
  int type;
  /** From whom the payload will come (may be a redirect) */
  int payload_source;
  /** Parallelism: 1=normal single process task, >1=parallel task */
  int parallelism;
};

struct packed_create_response
{
  adlb_data_code dc;
  /** id of created item */
  adlb_datum_id id;
};

/**
   Simple struct for message packing
 */
struct packed_code_id
{
  adlb_data_code code;
  adlb_datum_id id;
};

/**
   Simple struct for message packing
 */
struct retrieve_response_hdr
{
  adlb_data_code code;
  adlb_data_type type;
  int length;
};

/**
   Simple struct for message packing
 */
struct packed_enumerate
{
  adlb_datum_id id;
  char request_subscripts;
  char request_members;
  int count;
  int offset;
  adlb_refcounts decr;
};

struct packed_enumerate_result
{
  adlb_data_code dc;
  int records; // Count of elements returned
  int length; // length of data in bytes
  adlb_data_type key_type;
  adlb_data_type val_type;
};

struct packed_notif_counts
{
  int notify_closed_count;
  int notify_insert_count;
  int reference_count;
};

/**
   Count increment or decrement
 */
struct packed_incr
{
  adlb_datum_id id;
  adlb_refcounts change;
};

/**
   Response to reference count operation
 */
struct packed_refcount_resp
{
  bool success;
  struct packed_notif_counts notifs;
};

/**
 * Header for store message
 */
struct packed_store_hdr
{
  adlb_datum_id id;
  adlb_data_type type; // Type of data
  adlb_refcounts refcount_decr;
  int subscript_len; // including null byte, 0 if no subscript
};


/**
 * Response for store 
 */
struct packed_store_resp
{
  bool success;
  struct packed_notif_counts notifs;
};

/**
 * Header for retrieve message
 */
struct packed_retrieve_hdr
{
  adlb_datum_id id;
  adlb_retrieve_rc refcounts;
  int subscript_len; // including null byte, 0 if no subscript
  char subscript[];
};

#define PACKED_SUBSCRIPT_MAX (ADLB_DATA_SUBSCRIPT_MAX + \
          sizeof(adlb_datum_id) + sizeof(int))
struct packed_insert_atomic_resp
{
  adlb_data_code dc;
  bool created;
  int value_len; // Value length, negative if not present
  adlb_data_type value_type;
};

struct packed_size_req
{
  adlb_datum_id id;
  adlb_refcounts decr;
};

/*
  Generic boolean response for data op
 */
struct packed_bool_resp
{
  adlb_data_code dc;
  bool result;
};

int
pack_id_subscript(void *buffer, adlb_datum_id id, const char *subscript);

int
unpack_id_subscript(const void *buffer, adlb_datum_id *id,
                    const char **subscript, int *sub_strlen);

/**
 * Request for steal
 */
struct packed_steal
{
  int max_memory;
  int work_type_counts[]; // Sender's count of each work type
};

#define WORK_TYPES_SIZE (sizeof(int) * (size_t)xlb_types_size) 

struct packed_steal_resp
{
  int count; // number of work units
  bool last; // whether last set of stolen work
};

typedef enum
{
  ADLB_SYNC_REQUEST, // Sync for a regular request
  ADLB_SYNC_STEAL, // Trying to steal work
} adlb_sync_mode;

struct packed_sync
{
  adlb_sync_mode mode;
  union
  {
    struct packed_steal steal; // if steal
  };
};

#define PACKED_SYNC_SIZE (sizeof(struct packed_sync) + WORK_TYPES_SIZE)

/**
   Simple data type transfer
 */
static inline void
xlb_pack_work_unit(struct packed_put* p, xlb_work_unit* wu)
{
  p->answer = wu->answer;
  p->length = wu->length;
  p->priority = wu->priority;
  p->putter = wu->putter;
  p->target = wu->target;
  p->type = wu->type;
  p->parallelism = wu->parallelism;
}

/** Member count of enum adlb_tag */
#define XLB_MAX_TAGS 128

/**
   ADLB message tags
   Some RPCs require two incoming messages: a header and a payload
*/
typedef enum
{
  ADLB_TAG_NULL = 0,

  /// tags incoming to server

  // task operations
  ADLB_TAG_PUT = 1,
  ADLB_TAG_GET,
  ADLB_TAG_IGET,

  // data operations
  ADLB_TAG_CREATE_HEADER,
  ADLB_TAG_MULTICREATE,
  ADLB_TAG_CREATE_PAYLOAD,
  ADLB_TAG_EXISTS,
  ADLB_TAG_STORE_HEADER,
  ADLB_TAG_STORE_SUBSCRIPT,
  ADLB_TAG_STORE_PAYLOAD,
  ADLB_TAG_RETRIEVE,
  ADLB_TAG_ENUMERATE,
  ADLB_TAG_SUBSCRIBE,
  ADLB_TAG_PERMANENT,
  ADLB_TAG_REFCOUNT_INCR,
  ADLB_TAG_INSERT_ATOMIC,
  ADLB_TAG_UNIQUE,
  ADLB_TAG_TYPEOF,
  ADLB_TAG_CONTAINER_TYPEOF,
  ADLB_TAG_CONTAINER_REFERENCE,
  ADLB_TAG_CONTAINER_SIZE,
  ADLB_TAG_LOCK,
  ADLB_TAG_UNLOCK,
  ADLB_TAG_SYNC_REQUEST,
  ADLB_TAG_CHECK_IDLE,
  ADLB_TAG_SHUTDOWN_WORKER,
  ADLB_TAG_SHUTDOWN_SERVER,

  /// tags outgoing from server
  ADLB_TAG_RESPONSE,
  ADLB_TAG_RESPONSE_PUT,
  ADLB_TAG_RESPONSE_GET,
  ADLB_TAG_RESPONSE_STEAL_COUNT,
  ADLB_TAG_RESPONSE_STEAL,
  ADLB_TAG_SYNC_RESPONSE,
  ADLB_TAG_WORKUNIT,
  ADLB_TAG_FAIL,

  /// tags that may be to/from server/worker
  /** Work unit payload */
  ADLB_TAG_WORK

} adlb_tag;

#endif
