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


#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include <mpi.h>

#include <c-utils.h>
#include <list_i.h>
#include <table.h>
#include <tools.h>

#include "adlb.h"
#include "adlb_types.h"
#include "adlb-version.h"
#include "adlb-xpt.h"
#include "checks.h"
#include "client_internal.h"
#include "common.h"
#include "data.h"
#include "debug.h"
#include "debug_symbols.h"
#include "messaging.h"
#include "mpe-tools.h"
#include "mpi-tools.h"
#include "notifications.h"
#include "server.h"
#include "sync.h"

static adlb_code next_server;

static void print_proc_self_status(void);

void adlb_exit_handler(void);

/** True after a Get() receives a shutdown code */
static bool got_shutdown = false;

/** Cached copy of MPI world group */
static MPI_Group adlb_group;

static int mpi_version;

/**
   Maps string hostname to list of int ranks which are running on
   that host
 */
struct table hostmap;

static inline int choose_data_server();

static int disable_hostmap;

static void
check_versions()
{
  mpi_version = ADLB_MPI_VERSION;

  version av, cuv, rcuv;
  // required c-utils version (rcuv):
  ADLB_Version(&av);
  version_parse(&rcuv, C_UTILS_REQUIRED_VERSION);
  c_utils_version(&cuv);
  version_require("ADLB", &av, "c-utils", &cuv, &rcuv);
}

#define HOSTNAME_MAX 128
static void report_debug_ranks(void);
static bool setup_hostmap(void);

static inline int get_next_server();

adlb_code
ADLBP_Init(int nservers, int ntypes, int type_vect[],
           int *am_server, MPI_Comm comm, MPI_Comm *worker_comm)
{
  adlb_code code = debug_check_environment();
  ADLB_CHECK(code);

  TRACE_START;

  int initialized;
  check_versions();
  int rc;
  rc = MPI_Initialized(&initialized);
  MPI_CHECK(rc);
  CHECK_MSG(initialized, "ADLB: MPI is not initialized!\n");

  xlb_start_time = MPI_Wtime();

  adlb_comm = comm;

  rc = MPI_Comm_size(comm, &xlb_comm_size);
  MPI_CHECK(rc);
  rc = MPI_Comm_rank(comm, &xlb_comm_rank);
  MPI_CHECK(rc);

  xlb_msg_init();

  gdb_spin(xlb_comm_rank);

  xlb_types_size = ntypes;
  xlb_types = malloc((size_t)xlb_types_size * sizeof(int));
  for (int i = 0; i < xlb_types_size; i++)
    xlb_types[i] = type_vect[i];
  xlb_servers = nservers;
  xlb_workers = xlb_comm_size - xlb_servers;
  xlb_master_server_rank = xlb_comm_size - xlb_servers;

  rc = MPI_Comm_group(adlb_comm, &adlb_group);
  assert(rc == MPI_SUCCESS);

  // Set this correctly before initializing other modules
  xlb_perf_counters_enabled = false;
  getenv_boolean("ADLB_PERF_COUNTERS", xlb_perf_counters_enabled, &xlb_perf_counters_enabled);

  xlb_am_server = (xlb_comm_rank >= xlb_workers);
  if (!xlb_am_server)
  {
    *am_server = 0;
    MPI_Comm_split(adlb_comm, 0, xlb_comm_rank, &adlb_worker_comm);
    *worker_comm = adlb_worker_comm;
    xlb_my_server = xlb_map_to_server(xlb_comm_rank);
    DEBUG("my_server_rank: %i", xlb_my_server);
    next_server = xlb_my_server;
  }
  else
  {
    *am_server = 1;
    // Don't have a server: I am one
    xlb_my_server = ADLB_RANK_NULL;
    MPI_Comm_split(adlb_comm, 1, xlb_comm_rank-xlb_workers,
                   &adlb_server_comm);
    code = xlb_server_init();
    ADLB_CHECK(code);
  }

  report_debug_ranks();

  setup_hostmap();

  code = xlb_debug_symbols_init();
  ADLB_CHECK(code);

  srandom((unsigned int)xlb_comm_rank+1);

  xlb_read_refcount_enabled = false;

  adlb_data_code dc = xlb_data_types_init();
  ADLB_DATA_CHECK(dc);
  
  TRACE_END;
  return ADLB_SUCCESS;
}

static void
report_debug_ranks()
{
  int debug_ranks;
  getenv_integer("ADLB_DEBUG_RANKS", 0, &debug_ranks);
  if (!debug_ranks) return;

  struct utsname u;
  uname(&u);

  printf("ADLB_DEBUG_RANKS: rank: %i nodename: %s\n",
         xlb_comm_rank, u.nodename);
}

static bool
setup_hostmap()
{
  getenv_integer("ADLB_DISABLE_HOSTMAP", 0, &disable_hostmap);
  if (disable_hostmap) return true;

  struct utsname u;
  uname(&u);

  // Length of nodenames
  int length = (int) sizeof(u.nodename);

  // This may be too big for the stack
  char* allnames = malloc((size_t)(xlb_comm_size*length) * sizeof(char));

  char myname[length];
  // This prevents valgrind errors:
  memset(myname, 0, (size_t)length);
  strcpy(myname, u.nodename);

  int rc = MPI_Allgather(myname,   length, MPI_CHAR,
                         allnames, length, MPI_CHAR, adlb_comm);
  MPI_CHECK(rc);

  table_init(&hostmap, 1024);

  bool debug_hostmap = false;
  char* t = getenv("ADLB_DEBUG_HOSTMAP");
  if (t != NULL && strcmp(t, "1") == 0)
    debug_hostmap = true;

  char* p = allnames;
  for (int rank = 0; rank < xlb_comm_size; rank++)
  {
    char* name = p;

    if (xlb_comm_rank == 0 && debug_hostmap)
      printf("HOSTMAP: %s -> %i\n", name, rank);

    if (!table_contains(&hostmap, name))
    {
      struct list_i* L = list_i_create();
      table_add(&hostmap, name, L);
    }

    struct list_i* L;
    table_search(&hostmap, name, (void*) &L);
    list_i_add(L, rank);

    p += length;
  }

  free(allnames);
  return true;
}

adlb_code
ADLB_Version(version* output)
{
  version_parse(output, ADLB_VERSION);
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_stats(unsigned int* count, unsigned int* name_max)
{
  CHECK_MSG(!disable_hostmap,
            "ADLB_Hostmap_stats: hostmap is disabled!");
  struct utsname u;
  *count = (uint)hostmap.size;
  *name_max = sizeof(u.nodename);
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_lookup(const char* name, int count,
                    int* output, int* actual)
{
  CHECK_MSG(!disable_hostmap,
             "ADLB_Hostmap_lookup: hostmap is disabled!");
  struct list_i* L;
  bool b = table_search(&hostmap, name, (void*) &L);
  if (!b)
    return ADLB_NOTHING;
  int i = 0;
  for (struct list_i_item* item = L->head; item; item = item->next)
  {
    output[i++] = item->data;
    if (i == count)
      break;
  }
  *actual = i;
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_list(char* output, unsigned int max,
                  unsigned int offset, int* actual)
{
  CHECK_MSG(!disable_hostmap,
               "ADLB_Hostmap_list: hostmap is disabled!");
  // Number of chars written
  int count = 0;
  // Number of hostnames written
  int h = 0;
  // Moving pointer into output
  char* p = output;
  // Counter for offset
  int j = 0;

  TABLE_FOREACH(&hostmap, item)
  {
    if (j++ >= offset)
    {
      int t = (int)strlen(item->key);
      if (count+t >= max)
        goto done;
      append(p, "%s", item->key);
      *p = '\r';
      p++;
      count += t;
      h++;
    }
  }

  done:
  *actual = h;
  return ADLB_SUCCESS;
}

// Server to target with work
__attribute__((always_inline))
static inline adlb_code
adlb_put_target_server(int target, int *to_server)
{
  if (target == ADLB_RANK_ANY)
    *to_server = xlb_my_server;
  else if (target < xlb_comm_size)
    *to_server = xlb_map_to_server(target);
  else
    CHECK_MSG(target >= 0 && target < xlb_comm_size,
        "ADLB_Put(): invalid target rank: %i", target);
  return ADLB_SUCCESS;;
}

static inline adlb_code
adlb_put_check_params(int target, int type, int parallelism)
{
  CHECK_MSG(target == ADLB_RANK_ANY ||
            (target >= 0 && target < xlb_workers),
            "ADLB_Put(): invalid target: %i", target);

  CHECK_MSG(type >= 0 && xlb_type_index(type) >= 0,
            "ADLB_Put(): invalid work type: %d\n", type);

  CHECK_MSG(mpi_version >= 3 || parallelism == 1,
            "ADLB_Put(): "
            "parallel tasks not supported for MPI version %i",
            mpi_version);
  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Put(const void* payload, int length, int target, int answer,
          int type, int priority, int parallelism)
{
  MPI_Status status;
  MPI_Request request;
  int rc, response;

  DEBUG("ADLB_Put: target=%i x%i %.*s",
        target, parallelism, length, (char*) payload);

  rc = adlb_put_check_params(target, type, parallelism);
  ADLB_CHECK(rc);

  /** Server to contact */
  int to_server;
  rc = adlb_put_target_server(target, &to_server);
  ADLB_CHECK(rc);

  int inline_data_len;
  if (length <= PUT_INLINE_DATA_MAX)
  {
    inline_data_len = length;
  }
  else
  {
    inline_data_len = 0;
  }

  size_t p_size = PACKED_PUT_SIZE((size_t)inline_data_len);
  assert(p_size <= XFER_SIZE);
  struct packed_put *p = (struct packed_put*)xfer;
  p->type = type;
  p->priority = priority;
  p->putter = xlb_comm_rank;
  p->answer = answer;
  p->target = target;
  p->length = length;
  p->parallelism = parallelism;
  p->has_inline_data = inline_data_len > 0;
  if (p->has_inline_data)
  {
    memcpy(p->inline_data, payload, (size_t)inline_data_len);
  }

  IRECV(&response, 1, MPI_INT, to_server, ADLB_TAG_RESPONSE_PUT);
  SEND(p, (int)p_size, MPI_BYTE, to_server, ADLB_TAG_PUT);

  WAIT(&request, &status);
  if (response == ADLB_REJECTED)
  {
 //    to_server_rank = next_server++;
//    if (next_server >= (master_server_rank+num_servers))
//      next_server = master_server_rank;
    return response;
  }

  if (p->has_inline_data)
  {
    // Successfully sent: just check response
    ADLB_CHECK(response);
  }
  else
  {
    int payload_dest = response;
    // Still need to send payload
    // In a redirect, we send the payload to a worker
    DEBUG("ADLB_Put: payload to: %i", payload_dest);
    if (payload_dest == ADLB_RANK_NULL)
      return ADLB_ERROR;
    SSEND(payload, length, MPI_BYTE, payload_dest, ADLB_TAG_WORK);
  }
  TRACE("ADLB_Put: DONE");

  return ADLB_SUCCESS;
}

adlb_code ADLBP_Dput(const void* payload, int length, int target,
        int answer, int type, int priority, int parallelism,
        const char *name,
        const adlb_datum_id *wait_ids, int wait_id_count, 
        const adlb_datum_id_sub *wait_id_subs, int wait_id_sub_count)
{
  MPI_Status status;
  MPI_Request request;
  int response, rc;

  DEBUG("ADLB_Dput: target=%i x%i %.*s",
        target, parallelism, length, (char*) payload);
  
  rc = adlb_put_check_params(target, type, parallelism);
  ADLB_CHECK(rc);

  /** Server to contact */
  int to_server;
  rc = adlb_put_target_server(target, &to_server);
  ADLB_CHECK(rc);

  int inline_data_len;
  if (length <= PUT_INLINE_DATA_MAX)
  {
    inline_data_len = length;
  }
  else
  {
    inline_data_len = 0;
  }

  struct packed_put_rule *p = (struct packed_put_rule*)xfer;
  p->type = type;
  p->priority = priority;
  p->putter = xlb_comm_rank;
  p->answer = answer;
  p->target = target;
  p->length = length;
  p->parallelism = parallelism;
  p->has_inline_data = inline_data_len > 0;
  p->id_count = wait_id_count;
  p->id_sub_count = wait_id_sub_count;
  
  int p_len = (int)sizeof(struct packed_put_rule);

  // pack in all needed data at end
  void *p_data = p->inline_data;

  size_t wait_id_len = sizeof(wait_ids[0]) * (size_t)wait_id_count;
  memcpy(p_data, wait_ids, wait_id_len);
  p_data += wait_id_len;
  p_len += (int)wait_id_len;

  for (int i = 0; i < wait_id_sub_count; i++)
  {
    int packed_len = xlb_pack_id_sub(p_data, wait_id_subs[i].id,
                                      wait_id_subs[i].subscript);
    p_data += packed_len;
    p_len += packed_len;
  }

  #ifndef NDEBUG
  // Don't pack name if NDEBUG on
  p->name_strlen = (int)strlen(name);
  memcpy(p_data, name, (size_t)p->name_strlen);
  p_data += p->name_strlen;
  p_len += p->name_strlen;
  #endif
  
  if (p->has_inline_data)
  {
    memcpy(p_data, payload, (size_t)inline_data_len);
    p_data += inline_data_len;
    p_len += inline_data_len;
  }

  // xfer is much larger than we need for ids/subs plus inline data
  assert(p_len < XFER_SIZE); 

  IRECV(&response, 1, MPI_INT, to_server, ADLB_TAG_RESPONSE_PUT);
  SEND(p, (int)p_len, MPI_BYTE, to_server, ADLB_TAG_PUT_RULE);

  WAIT(&request, &status);
  if (response == ADLB_REJECTED)
  {
 //    to_server_rank = next_server++;
//    if (next_server >= (master_server_rank+num_servers))
//      next_server = master_server_rank;
    return response;
  }

  // Check response before sending any payload data
  ADLB_CHECK(response);
  if (!p->has_inline_data)
  {
    // Second response to confirm entered ok
    IRECV(&response, 1, MPI_INT, to_server, ADLB_TAG_RESPONSE_PUT);
    // Still need to send payload
    // Note: don't try to redirect work for rule
    // Use RSEND so that server can pre-allocate a buffer
    RSEND(payload, length, MPI_BYTE, to_server, ADLB_TAG_WORK);
    WAIT(&request, &status);
    ADLB_CHECK(response);
  }
  TRACE("ADLB_Dput: DONE");

  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Get(int type_requested, void* payload, int* length,
          int* answer, int* type_recvd, MPI_Comm* comm)
{
  MPI_Status status;
  MPI_Request request;

  TRACE_START;

  CHECK_MSG(xlb_type_index(type_requested) != -1,
                "ADLB_Get(): Bad work type: %i\n", type_requested);

  struct packed_get_response g;
  IRECV(&g, sizeof(g), MPI_BYTE, xlb_my_server, ADLB_TAG_RESPONSE_GET);
  SEND(&type_requested, 1, MPI_INT, xlb_my_server, ADLB_TAG_GET);
  WAIT(&request, &status);

  mpi_recv_sanity(&status, MPI_BYTE, sizeof(g));

  if (g.code == ADLB_SHUTDOWN)
  {
    DEBUG("ADLB_Get(): SHUTDOWN");
    got_shutdown = true;
    return ADLB_SHUTDOWN;
  }

  DEBUG("ADLB_Get(): payload source: %i", g.payload_source);
  RECV(payload, g.length, MPI_BYTE, g.payload_source, ADLB_TAG_WORK);
  mpi_recv_sanity(&status, MPI_BYTE, g.length);
  // TRACE("ADLB_Get(): got: %s", (char*) payload);

  if (g.parallelism > 1)
  {
    DEBUG("ADLB_Get(): parallelism=%i", g.parallelism);
    // Parallel tasks require MPI 3.  Cf. configure.ac
    #if ADLB_MPI_VERSION >= 3
    // Recv ranks for output comm
    int ranks[g.parallelism];
    RECV(ranks, g.parallelism, MPI_INT, xlb_my_server,
         ADLB_TAG_RESPONSE_GET);
    MPI_Group group;
    int rc = MPI_Group_incl(adlb_group, g.parallelism, ranks, &group);
    assert(rc == MPI_SUCCESS);
    // This is an MPI 3 function:
    rc = MPI_Comm_create_group(adlb_comm, group, 0, comm);
    assert(rc == MPI_SUCCESS);
    #endif
  }
  else
    *comm = MPI_COMM_SELF;

  *length = g.length;
  *answer = g.answer_rank;
  *type_recvd = g.type;

  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Iget(int type_requested, void* payload, int* length,
           int* answer, int* type_recvd)
{
  MPI_Status status;
  MPI_Request request;

  CHECK_MSG(xlb_type_index(type_requested) != -1,
            "ADLB_Iget(): Bad work type: %i\n", type_requested);

  struct packed_get_response g;
  IRECV(&g, sizeof(g), MPI_BYTE, xlb_my_server, ADLB_TAG_RESPONSE_GET);
  SEND(&type_requested, 1, MPI_INT, xlb_my_server, ADLB_TAG_IGET);
  WAIT(&request, &status);

  mpi_recv_sanity(&status, MPI_BYTE, sizeof(g));

  if (g.code == ADLB_SHUTDOWN)
  {
    DEBUG("ADLB_Iget(): SHUTDOWN");
    got_shutdown = true;
    return ADLB_SHUTDOWN;
  }
  if (g.code == ADLB_NOTHING)
  {
    DEBUG("ADLB_Iget(): NOTHING");
    return ADLB_NOTHING;
  }

  DEBUG("ADLB_Iget: payload source: %i", g.payload_source);
  RECV(payload, g.length, MPI_BYTE, g.payload_source, ADLB_TAG_WORK);

  mpi_recv_sanity(&status, MPI_BYTE, g.length);
  TRACE("ADLB_Iget: got: %s", (char*) payload);

  *length = g.length;
  *answer = g.answer_rank;
  *type_recvd = g.type;

  return ADLB_SUCCESS;
}

int
ADLB_Locate(adlb_datum_id id)
{
  int offset = (int) (id % xlb_servers);
  if (offset < 0)
  {
    // Negative numbers continue pattern, e.g. -1 maps to last server
    // and -xlb_servers maps to first server.
    offset += xlb_servers;
  }
  int rank = xlb_comm_size - xlb_servers + offset;
  // DEBUG("ADLB_Locate(%li) => %i\n", id, rank);
  return rank;
}

/**
   Reusable internal data creation function
   Applications should use the ADLB_Create_type functions in adlb.h
   @param filename Only used for file-type data
   @param type_extra Additional type info
 */
static adlb_code
ADLBP_Create_impl(adlb_datum_id id, adlb_data_type type,
                  adlb_type_extra type_extra,
                  adlb_create_props props,
                  adlb_datum_id *new_id)
{
  int to_server_rank;
  MPI_Status status;
  MPI_Request request;

  if (id != ADLB_DATA_ID_NULL) {
    to_server_rank = ADLB_Locate(id);
  } else {
    to_server_rank = xlb_my_server;
  }
  ADLB_create_spec data = { id, type, type_extra, props };

  struct packed_create_response resp;
  IRECV(&resp, sizeof(resp), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&data, sizeof(data), MPI_BYTE,
       to_server_rank, ADLB_TAG_CREATE_HEADER);
  WAIT(&request, &status);

  ADLB_DATA_CHECK(resp.dc);

  // Check id makes sense
  assert(id == ADLB_DATA_ID_NULL || id == resp.id);
  if (id == ADLB_DATA_ID_NULL && new_id != NULL) {
    // Tell caller about new id
    *new_id = resp.id;
  }

  if (resp.dc != ADLB_DATA_SUCCESS) {
    return ADLB_ERROR;
  }

  return ADLB_SUCCESS;
}

/**
   Extern version of this (unused)
 */
adlb_code
ADLBP_Create(adlb_datum_id id, adlb_data_type type,
             adlb_type_extra type_extra,
             adlb_create_props props,
             adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, type, type_extra, props, new_id);
}

adlb_code ADLBP_Multicreate(ADLB_create_spec *specs, int count)
{
  MPI_Request request;
  MPI_Status status;
  int server = choose_data_server();

  // Allocated ids (ADLB_DATA_ID_NULL if failed)
  adlb_datum_id ids[count];
  IRECV(ids, (int)sizeof(ids), MPI_BYTE, server, ADLB_TAG_RESPONSE);

  SEND(specs, (int)sizeof(ADLB_create_spec) * count, MPI_BYTE,
       server, ADLB_TAG_MULTICREATE);
  WAIT(&request, &status);

  // Check success by inspecting ids
  for (int i = 0; i < count; i++) {
    if (ids[i] == ADLB_DATA_ID_NULL) {
      return ADLB_ERROR;
    }
    specs[i].id = ids[i];
  }
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Create_integer(adlb_datum_id id, adlb_create_props props,
                  adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_INTEGER,
                   ADLB_TYPE_EXTRA_NULL, props, new_id);
}

adlb_code
ADLB_Create_float(adlb_datum_id id, adlb_create_props props,
                  adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_FLOAT, ADLB_TYPE_EXTRA_NULL,
                   props, new_id);
}

adlb_code
ADLB_Create_string(adlb_datum_id id, adlb_create_props props,
                  adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_STRING, ADLB_TYPE_EXTRA_NULL,
                   props, new_id);
}

adlb_code
ADLB_Create_blob(adlb_datum_id id, adlb_create_props props,
                  adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_BLOB, ADLB_TYPE_EXTRA_NULL,
                   props, new_id);
}

adlb_code ADLB_Create_ref(adlb_datum_id id, adlb_create_props props,
                              adlb_datum_id *new_id)
{
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_REF, ADLB_TYPE_EXTRA_NULL,
                   props, new_id);
}

adlb_code ADLB_Create_struct(adlb_datum_id id, adlb_create_props props,
                             adlb_struct_type struct_type, adlb_datum_id *new_id)
{
  adlb_type_extra extra;
  extra.STRUCT.struct_type = struct_type;
  extra.valid = (struct_type != ADLB_STRUCT_TYPE_NULL);
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_STRUCT, extra, props,
                           new_id);
}

adlb_code
ADLB_Create_container(adlb_datum_id id, adlb_data_type key_type,
                      adlb_data_type val_type,
                      adlb_create_props props, adlb_datum_id *new_id)
{
  adlb_type_extra extra_type;
  extra_type.valid = true;
  extra_type.CONTAINER.key_type = key_type;
  extra_type.CONTAINER.val_type = val_type;
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_CONTAINER, extra_type,
                           props, new_id);
}

adlb_code ADLB_Create_multiset(adlb_datum_id id,
                                adlb_data_type val_type, 
                                adlb_create_props props,
                                adlb_datum_id *new_id)
{
  adlb_type_extra extra_type;
  extra_type.valid = true;
  extra_type.MULTISET.val_type = val_type;
  return ADLBP_Create_impl(id, ADLB_DATA_TYPE_MULTISET, extra_type,
                           props, new_id);
}

adlb_code
ADLBP_Exists(adlb_datum_id id, adlb_subscript subscript, bool* result,
             adlb_refc decr)
{
  int to_server_rank = ADLB_Locate(id);

  MPI_Status status;
  MPI_Request request;

  TRACE("ADLB_Exists: <%"PRId64">\n", id);

  char req[PACKED_SUBSCRIPT_MAX + sizeof(decr)];
  char *req_ptr = req;
  req_ptr += xlb_pack_id_sub(req_ptr, id, subscript);
  assert(req_ptr > req);

  MSG_PACK_BIN(req_ptr, decr);

  struct packed_bool_resp resp;
  IRECV(&resp, sizeof(resp), MPI_BYTE, to_server_rank,
        ADLB_TAG_RESPONSE);
  SEND(req, (int)(req_ptr - req), MPI_BYTE, to_server_rank, ADLB_TAG_EXISTS);
  WAIT(&request, &status);

  ADLB_DATA_CHECK(resp.dc);
  *result = resp.result;
  return ADLB_SUCCESS;
}
adlb_code
ADLBP_Get_refcounts(adlb_datum_id id, adlb_refc *result,
                              adlb_refc decr)
{
  int to_server_rank = ADLB_Locate(id);

  MPI_Status status;
  MPI_Request request;

  TRACE("ADLB_Get_refcounts: <%"PRId64">\n", id);

  struct packed_refcounts_req req = { .id = id, .decr = decr };

  struct packed_refcounts_resp resp;
  IRECV(&resp, sizeof(resp), MPI_BYTE, to_server_rank,
        ADLB_TAG_RESPONSE);
  SEND(&req, sizeof(req), MPI_BYTE, to_server_rank,
       ADLB_TAG_GET_REFCOUNTS);
  WAIT(&request, &status);

  ADLB_DATA_CHECK(resp.dc);
  *result = resp.refcounts;
  return ADLB_SUCCESS;

}

adlb_code
ADLBP_Store(adlb_datum_id id, adlb_subscript subscript,
          adlb_data_type type, const void *data, int length,
          adlb_refc refcount_decr, adlb_refc store_refcounts)
{
  adlb_notif_t notifs = ADLB_NO_NOTIFS;
  adlb_code rc, final_rc;
  
  final_rc = xlb_store(id, subscript, type, data, length, refcount_decr,
                 store_refcounts, &notifs);
  ADLB_CHECK(final_rc); // Check for ADLB_ERROR, not other codes
  
  rc = xlb_notify_all(&notifs);
  ADLB_CHECK(rc);

  xlb_free_notif(&notifs);

  return final_rc;
}


adlb_code
xlb_store(adlb_datum_id id, adlb_subscript subscript, adlb_data_type type,
            const void *data, int length, adlb_refc refcount_decr,
            adlb_refc store_refcounts, adlb_notif_t *notifs)
{
  adlb_code code;
  adlb_data_code dc;
  MPI_Status status;
  MPI_Request request;

  CHECK_MSG(length < ADLB_DATA_MAX,
            "ADLB_Store(): value too long: %llu\n",
            (long long unsigned) length);

  if (adlb_has_sub(subscript))
  {
    // TODO: support binary subscript
    DEBUG("ADLB_Store: <%"PRId64">[%.*s]=%p[%i]", id,
        (int)subscript.length, (const char*)subscript.key, data, length);
  }
  else
  {
    DEBUG("ADLB_Store: <%"PRId64">=%p[%i]", id, data, length);
  }

  int to_server_rank = ADLB_Locate(id);
  if (to_server_rank == xlb_comm_rank)
  {
    // This is a server-to-server operation on myself
    TRACE("Store SELF");
    dc = xlb_data_store(id, subscript, data, length,
                    type, refcount_decr, store_refcounts, notifs);
    if (dc == ADLB_DATA_ERROR_DOUBLE_WRITE)
      return ADLB_REJECTED;
    ADLB_DATA_CHECK(dc);

    return ADLB_SUCCESS;
  }
  TRACE("Store to server %i", to_server_rank);

  if (xlb_am_server)
  {
    code = xlb_sync(to_server_rank);
    ADLB_CHECK(code);
  }

  struct packed_store_hdr hdr = { .id = id,
      .type = type,
      .subscript_len = adlb_has_sub(subscript) ? (int)subscript.length : 0,
      .refcount_decr = refcount_decr, .store_refcounts = store_refcounts};
  struct packed_store_resp resp;

  IRECV(&resp, sizeof(resp), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&hdr, sizeof(struct packed_store_hdr), MPI_BYTE,
       to_server_rank, ADLB_TAG_STORE_HEADER);
  if (adlb_has_sub(subscript))
  {
    SEND(subscript.key, (int)subscript.length, MPI_BYTE, to_server_rank,
         ADLB_TAG_STORE_SUBSCRIPT);
  }
  SEND(data, length, MPI_BYTE, to_server_rank,
       ADLB_TAG_STORE_PAYLOAD);
  WAIT(&request, &status);

  if (resp.dc == ADLB_DATA_ERROR_DOUBLE_WRITE)
    return ADLB_REJECTED;
  ADLB_DATA_CHECK(resp.dc);

  code = xlb_recv_notif_work(&resp.notifs, to_server_rank, notifs);
  ADLB_CHECK(code);

  return ADLB_SUCCESS;
}

/**
   Obtain the next server index
   Currently implemented as a round-robin loop through the ranks
 */
static inline int
get_next_server()
{
  static int next_server_index = 0;
  int offset = next_server_index % xlb_servers;
  int rank = xlb_comm_size - xlb_servers + offset;
  // DEBUG("xlb_random_server => %i\n", rank);
  next_server_index = (next_server_index + 1) % xlb_servers;
  return rank;
}

/**
  Choose server to create data on
 */
static inline int
choose_data_server()
{
  // For now, create on own server
  return xlb_my_server;
}

adlb_code
ADLBP_Read_refcount_enable(void)
{
  xlb_read_refcount_enabled = true;
  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Refcount_incr(adlb_datum_id id, adlb_refc change)
{
  adlb_code rc;
  
  adlb_notif_t notifs = ADLB_NO_NOTIFS;
  rc = xlb_refcount_incr(id, change, &notifs);
  ADLB_CHECK(rc);

  rc = xlb_notify_all(&notifs);
  ADLB_CHECK(rc);

  xlb_free_notif(&notifs);

  return ADLB_SUCCESS;
}

adlb_code
xlb_refcount_incr(adlb_datum_id id, adlb_refc change,
                    adlb_notif_t *notifs)
{
  int rc;
  MPI_Status status;
  MPI_Request request;
  
  DEBUG("ADLB_Refcount_incr: <%"PRId64"> READ %i WRITE %i", id,
            change.read_refcount, change.write_refcount);

  if (!xlb_read_refcount_enabled)
    change.read_refcount = 0;

  if (ADLB_REFC_IS_NULL(change))
    return ADLB_SUCCESS;

  int to_server_rank = ADLB_Locate(id);

  struct packed_incr_resp resp;
  rc = MPI_Irecv(&resp, sizeof(resp), MPI_BYTE, to_server_rank,
                 ADLB_TAG_RESPONSE, adlb_comm, &request);
  MPI_CHECK(rc);
  struct packed_incr msg = { .id = id, .change = change };
  rc = MPI_Send(&msg, sizeof(msg), MPI_BYTE, to_server_rank,
                ADLB_TAG_REFCOUNT_INCR, adlb_comm);
  MPI_CHECK(rc);
  rc = MPI_Wait(&request, &status);
  MPI_CHECK(rc);

  if (!resp.success)
    return ADLB_ERROR;

  rc = xlb_recv_notif_work(&resp.notifs, to_server_rank, notifs);
  ADLB_CHECK(rc);

  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Insert_atomic(adlb_datum_id id, adlb_subscript subscript,
                        adlb_retrieve_refc refcounts,
                        bool* result, void *data, int *length,
                        adlb_data_type *type)
{
  int rc;
  MPI_Status status;
  MPI_Request request;
  struct packed_insert_atomic_resp resp;

  // TODO: support binary subscript
  DEBUG("ADLB_Insert_atomic: <%"PRId64">[\"%.*s\"]", id,
        (int)subscript.length, (const char*)subscript.key);
  char *xfer_pos = xfer;
  xfer_pos += xlb_pack_id_sub(xfer_pos, id, subscript);

  bool return_value = data != NULL;
  MSG_PACK_BIN(xfer_pos, return_value);

  MSG_PACK_BIN(xfer_pos, refcounts);

  int to_server_rank = ADLB_Locate(id);

  rc = MPI_Irecv(&resp, sizeof(resp), MPI_BYTE, to_server_rank,
                ADLB_TAG_RESPONSE, adlb_comm, &request);
  MPI_CHECK(rc);
  rc = MPI_Send(xfer, (int)(xfer_pos - xfer), MPI_BYTE, to_server_rank,
                ADLB_TAG_INSERT_ATOMIC, adlb_comm);
  MPI_CHECK(rc);
  rc = MPI_Wait(&request, &status);
  MPI_CHECK(rc);

  if (resp.dc != ADLB_DATA_SUCCESS)
    return ADLB_ERROR;
  
  // Receive data before handling notifications
  if (return_value)
  {
    *length = resp.value_len;
    if (resp.value_len >= 0)
    {
      RECV(data, resp.value_len, MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
      *type = resp.value_type;
    }
  }

  adlb_code ac = xlb_handle_client_notif_work(&resp.notifs,
                                              to_server_rank);
  ADLB_CHECK(ac);

  *result = resp.created;
  return ADLB_SUCCESS;
}

/*
   Setting a negative length indicates data not present
 */
adlb_code
ADLBP_Retrieve(adlb_datum_id id, adlb_subscript subscript,
               adlb_retrieve_refc refcounts, adlb_data_type* type,
               void *data, int *length)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(id);

  int subscript_len = adlb_has_sub(subscript) ? (int)subscript.length : 0;

  // Stack allocate small buffer
  int hdr_len = (int)sizeof(struct packed_retrieve_hdr) + subscript_len;
  char hdr_buffer[hdr_len];
  struct packed_retrieve_hdr *hdr = (struct packed_retrieve_hdr*)hdr_buffer;

  // Fill in header
  hdr->id = id;
  hdr->refcounts = refcounts;
  hdr->subscript_len = subscript_len;
  if (subscript_len > 0)
  {
    memcpy(hdr->subscript, subscript.key, (size_t)subscript_len);
  }

  struct retrieve_response_hdr resp_hdr;
  IRECV(&resp_hdr, sizeof(resp_hdr), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(hdr, hdr_len, MPI_BYTE, to_server_rank, ADLB_TAG_RETRIEVE);
  WAIT(&request,&status);


  if (resp_hdr.code == ADLB_DATA_ERROR_NOT_FOUND ||
           resp_hdr.code == ADLB_DATA_ERROR_SUBSCRIPT_NOT_FOUND)
  {
    *length = -1;
    return ADLB_SUCCESS;
  }
  else if (resp_hdr.code != ADLB_DATA_SUCCESS)
  {
    return ADLB_ERROR;
  }
  
  assert(resp_hdr.length <= ADLB_PAYLOAD_MAX);
  RECV(data, resp_hdr.length, MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  // Set length and type output parameters
  *length = resp_hdr.length;
  *type = resp_hdr.type;
  DEBUG("RETRIEVE: <%"PRId64"> (%i bytes)\n", id, *length);
  
  adlb_code ac = xlb_handle_client_notif_work(&resp_hdr.notifs,
                                              to_server_rank);
  ADLB_CHECK(ac);

  return ADLB_SUCCESS;
}

/**
   Allocates fresh memory in subscripts and members
   Caller must free this when done
 */
adlb_code
ADLBP_Enumerate(adlb_datum_id container_id,
                int count, int offset, adlb_refc decr,
                bool include_keys, bool include_vals,
                void** data, int* length, int* records,
                adlb_type_extra *kv_type)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(container_id);

  struct packed_enumerate opts;
  opts.id = container_id;
  // Are we requesting subscripts?
  opts.request_subscripts = include_keys;
  // Are we requesting members?
  opts.request_members = include_vals;
  opts.count = count;
  opts.offset = offset;
  opts.decr = decr;

  struct packed_enumerate_result res;
  IRECV(&res, sizeof(res), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&opts, sizeof(struct packed_enumerate), MPI_BYTE,
       to_server_rank, ADLB_TAG_ENUMERATE);
  WAIT(&request,&status);

  if (res.dc == ADLB_DATA_SUCCESS)
  {
    *records = res.records;
    *length = res.length;
    if (include_keys || include_vals)
    {
      assert(res.length >= 0);
      *data = malloc((size_t)res.length);
      RECV(*data, res.length, MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
    }
    kv_type->valid = true;
    kv_type->CONTAINER.key_type = res.key_type;
    kv_type->CONTAINER.val_type = res.val_type;
    return ADLB_SUCCESS;
  }
  else
    return ADLB_ERROR;
}

adlb_code
ADLBP_Unique(adlb_datum_id* result)
{
  MPI_Status status;
  MPI_Request request;

  TRACE("ADLBP_Unique()...");

  // This is just something to send, it is ignored by the server
  static int msg = 0;
  int to_server_rank = get_next_server();
  IRECV(result, 1, MPI_ADLB_ID, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&msg, 1, MPI_INT, to_server_rank, ADLB_TAG_UNIQUE);
  WAIT(&request, &status);

  if (result == ADLB_DATA_ID_NULL)
    return ADLB_ERROR;
  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Typeof(adlb_datum_id id, adlb_data_type* type)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(id);
  IRECV(type, 1, MPI_INT, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&id, 1, MPI_ADLB_ID, to_server_rank, ADLB_TAG_TYPEOF);
  WAIT(&request, &status);

  DEBUG("ADLB_Typeof <%"PRId64">=>%i", id, *type);

  if (*type == -1)
    return ADLB_ERROR;
  return ADLB_SUCCESS;
}

adlb_code
ADLBP_Container_typeof(adlb_datum_id id, adlb_data_type* key_type,
                                 adlb_data_type* val_type)
{
  MPI_Status status;
  MPI_Request request;
  // DEBUG("ADLB_Container_typeof: %li", id);

  int to_server_rank = ADLB_Locate(id);
  adlb_data_type types[2];
  IRECV(types, 2, MPI_INT, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&id, 1, MPI_ADLB_ID, to_server_rank, ADLB_TAG_CONTAINER_TYPEOF);
  WAIT(&request, &status);

  DEBUG("ADLB_Container_typeof <%"PRId64">=>(%i,%i)", id, types[0], types[1]);

  if (types[0] == -1 || types[1] == -1)
    return ADLB_ERROR;

  *key_type = types[0];
  *val_type = types[1];
  return ADLB_SUCCESS;
}

/**
   @param work_type work type to receive notification as
   @param subscribed output: false if data is already closed
                             or ADLB_ERROR on error
 */
adlb_code
ADLBP_Subscribe(adlb_datum_id id, adlb_subscript subscript,
                int work_type, int* subscribed)
{
  int to_server_rank;
  MPI_Status status;
  MPI_Request request;

  to_server_rank = ADLB_Locate(id);
  
  char *xfer_pos = xfer;
  MSG_PACK_BIN(xfer_pos, work_type);
  xfer_pos += xlb_pack_id_sub(xfer_pos, id, subscript);

  int req_length = (int)(xfer_pos - xfer);
  assert(req_length > 0);

  struct pack_sub_resp result;
  IRECV(&result, sizeof(result), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(xfer, req_length, MPI_BYTE, to_server_rank, ADLB_TAG_SUBSCRIBE);
  WAIT(&request, &status);

  if (result.dc == ADLB_DATA_SUCCESS)
  {
    *subscribed = result.subscribed;
    if (!adlb_has_sub(subscript))
    {
      DEBUG("ADLB_Subscribe: <%"PRId64"> => %i", id, *subscribed);
    }
    else
    {
      // TODO: support binary subscript
      DEBUG("ADLB_Subscribe: <%"PRId64">[\"%.*s\"] => %i", id,
            (int)subscript.length, (const char*)subscript.key, *subscribed);
    }
    return ADLB_SUCCESS;
  }
  else if (result.dc == ADLB_DATA_ERROR_NOT_FOUND)
  {
    DEBUG("ADLB_Subscribe: <%"PRId64"> not found", id);
    return ADLB_DATA_ERROR_NOT_FOUND;
  }
  else
  {
    if (!adlb_has_sub(subscript))
    {
      DEBUG("ADLB_Subscribe: <%"PRId64"> => error", id);
    }
    else
    {
      // TODO: support binary subscript
      DEBUG("ADLB_Subscribe: <%"PRId64">[\"%.*s\"] => error", id,
            (int)subscript.length, (const char*)subscript.key);
    }
    return ADLB_ERROR;
  }
}

/**
   This consumes a read reference count to the container
   @return false in subscribed if data is already closed
 */
adlb_code
ADLBP_Container_reference(adlb_datum_id id, adlb_subscript subscript,
            adlb_datum_id ref_id, adlb_subscript ref_subscript,
            adlb_data_type ref_type, adlb_refc transfer_refs)
{
  MPI_Status status;
  MPI_Request request;

  char *xfer_pos = xfer;

  MSG_PACK_BIN(xfer_pos, ref_type);
  xfer_pos += xlb_pack_id_sub(xfer_pos, id, subscript);
  xfer_pos += xlb_pack_id_sub(xfer_pos, ref_id, ref_subscript);
  MSG_PACK_BIN(xfer_pos, transfer_refs);

  int to_server_rank = ADLB_Locate(id);

  struct packed_cont_ref_resp resp;

  IRECV(&resp, sizeof(resp), MPI_BYTE, to_server_rank, ADLB_TAG_RESPONSE);

  assert(xfer_pos - xfer <= INT_MAX);
  int length = (int)(xfer_pos - xfer);

  SEND(xfer, length, MPI_CHAR, to_server_rank,
       ADLB_TAG_CONTAINER_REFERENCE);
  WAIT(&request, &status);

  // Check for error before processing notification
  ADLB_DATA_CHECK(resp.dc);

  adlb_code ac = xlb_handle_client_notif_work(&resp.notifs, to_server_rank);
  ADLB_CHECK(ac);

  // TODO: support binary subscript
  DEBUG("ADLB_Container_reference: <%"PRId64">[%.*s] => "
        "<%"PRId64">[%.*s] (%i)",
        id, (int)subscript.length, (const char*)subscript.key, ref_id,
        (int)ref_subscript.length, (const char*)ref_subscript.key, ref_type);

  return ADLB_SUCCESS;
}

/** Return ADLB_ERROR and size=-1 if container is not closed */
adlb_code
ADLBP_Container_size(adlb_datum_id container_id, int* size,
                     adlb_refc decr)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(container_id);

  struct packed_size_req req = { .id = container_id, .decr = decr };
  IRECV(size, 1, MPI_INT, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&req, sizeof(req), MPI_BYTE, to_server_rank,
                ADLB_TAG_CONTAINER_SIZE);
  WAIT(&request, &status);

  DEBUG("ADLB_Container_size: <%"PRId64"> => %i",
        container_id, *size);

  if (*size < 0)
    return ADLB_ERROR;
  return ADLB_SUCCESS;
}

/**
   @return result 0->try again, 1->locked
 */
adlb_code
ADLBP_Lock(adlb_datum_id id, bool* result)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(id);

  // c 0->try again, 1->locked, x->failed
  char c;
  IRECV(&c, 1, MPI_CHAR, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&id, 1, MPI_ADLB_ID, to_server_rank, ADLB_TAG_LOCK);
  WAIT(&request, &status);

  if (c == 'x')
    return ADLB_ERROR;

  if (c == '0')
    *result = false;
  else if (c == '1')
    *result = true;
  else
    assert(false);

  return ADLB_SUCCESS;
}

/**
   @return result 0->try again, 1->locked
 */
adlb_code
ADLBP_Unlock(adlb_datum_id id)
{
  MPI_Status status;
  MPI_Request request;

  int to_server_rank = ADLB_Locate(id);

  // c: 1->success, x->failed
  char c;
  IRECV(&c, 1, MPI_CHAR, to_server_rank, ADLB_TAG_RESPONSE);
  SEND(&id, 1, MPI_ADLB_ID, to_server_rank, ADLB_TAG_UNLOCK);
  WAIT(&request, &status);

  if (c == 'x')
    return ADLB_ERROR;

  if (c != '1')
    assert(false);

  return ADLB_SUCCESS;
}

/**
   Is the server at rank idle?

   check_attempt: attempt number from master server of checking for idle
   result: true if idle
   request_counts: must be array large enough to hold ntypes. Filled in
        if idle with # of requests for each type
   untargeted_work_counts: must be array large enough to hold ntypes,
        Filled in if idle with # of tasks for each type
 */
adlb_code
ADLB_Server_idle(int rank, int64_t check_attempt, bool* result,
                 int *request_counts, int *untargeted_work_counts)
{
  MPI_Request request;
  MPI_Status status;
  IRECV(result, sizeof(result), MPI_BYTE, rank, ADLB_TAG_RESPONSE);
  SEND(&check_attempt, sizeof(check_attempt), MPI_BYTE, rank, ADLB_TAG_CHECK_IDLE);
  WAIT(&request, &status);

  if (*result)
  {
    RECV(request_counts, xlb_types_size, MPI_INT, rank, ADLB_TAG_RESPONSE);
    RECV(untargeted_work_counts, xlb_types_size, MPI_INT, rank,
         ADLB_TAG_RESPONSE);
  }
  return ADLB_SUCCESS;
}

/**
   Tell the server that this worker is shutting down
 */
static inline adlb_code
ADLB_Shutdown(void)
{
  TRACE_START;
  SEND_TAG(xlb_my_server, ADLB_TAG_SHUTDOWN_WORKER);
  TRACE_END;
  return ADLB_SUCCESS;
}

static void free_hostmap(void);

adlb_code
ADLBP_Finalize()
{
  TRACE_START;

  int rc;
  int flag;
  MPI_Finalized(&flag);
  CHECK_MSG(!flag,
            "ERROR: MPI_Finalize() called before ADLB_Finalize()\n");

#ifdef XLB_ENABLE_XPT
  // Finalize checkpoints before shutting down data
  ADLB_Xpt_finalize();
#endif

  adlb_data_code dc = xlb_data_finalize();
  if (dc != ADLB_DATA_SUCCESS)
    xlb_server_fail(1);

  if (xlb_comm_rank >= xlb_master_server_rank)
  {
    // Server:
    ; // print_final_stats();
  }
  else
  {
    // Worker:
    if (!got_shutdown)
    {
      rc = ADLB_Shutdown();
      ADLB_CHECK(rc);
    }
  }

  free_hostmap();
  
  xlb_debug_symbols_finalize();

  bool failed;
  int fail_code;
  xlb_server_failed(&failed, &fail_code);
  if (xlb_comm_rank == xlb_master_server_rank && failed)
  {
    printf("FAILED: EXIT(%i)\n", fail_code);
    exit(fail_code);
  }

  // Get messaging module to clean up state
  xlb_msg_finalize();

  // Clean up communicators (avoid memory leaks if ADLB used within
  // another application, and avoid spurious warnings from leak
  // detectors otherwise)
  if (xlb_am_server)
    MPI_Comm_free(&adlb_server_comm);
  else
    MPI_Comm_free(&adlb_worker_comm);
  MPI_Group_free(&adlb_group);

  free(xlb_types);
  xlb_types = NULL;
  
  xlb_data_types_finalize();

  return ADLB_SUCCESS;
}

static void
free_hostmap()
{
  if (disable_hostmap) return;
  for (int i = 0; i < hostmap.capacity; i++)
  {
    table_entry *head = &hostmap.array[i];
    if (table_entry_valid(head))
    {
      table_entry *e, *next;
      bool is_head;
      
      for (e = head, is_head = true; e != NULL;
           e = next, is_head = false)
      {
        next = e->next; // get next pointer before freeing

        char* name = e->key;
        struct list_i* L = e->data;
        free(name);
        list_i_free(L);
        
        if (!is_head)
        {
          // Free unless inline in array
          free(e);
        }
      }
    }
  }
  table_release(&hostmap);
}

adlb_code
ADLB_Fail(int code)
{
  printf("ADLB_Fail(%i)\n", code);

  SEND(&code, 1, MPI_INT, xlb_master_server_rank, ADLB_TAG_FAIL);

  // give servers a chance to shut down
  sleep(1);

  return ADLB_SUCCESS;
}

void
ADLB_Abort(int code)
{
  printf("ADLB_Abort(%i)\n", code);
  printf("MPI_Abort(%i)\n", code);
  MPI_Abort(MPI_COMM_WORLD, code);
}

static void
print_proc_self_status()
{
  int val;
  char input_line[1024], key[100], mag[100];
  FILE *statsfile;

  statsfile = fopen("/proc/self/status", "r");
  if (statsfile)
  {
    printf("values from: /proc/self/status:\n");
    while (fgets(input_line,100,statsfile) != NULL)
    {
      if (strncmp(input_line,"VmRSS:",6)  == 0 ||
          strncmp(input_line,"VmHWM:",6)  == 0 ||
          strncmp(input_line,"VmPeak:",7) == 0 ||
          strncmp(input_line,"VmSize:",7) == 0)
      {
        sscanf(input_line,"%s %d %s",key,&val,mag);
        printf("    %s %d %s\n",key,val,mag);
      }
    }
    fclose(statsfile);
  }
}



//static int get_server_idx(int);
//static int get_server_rank(int);

/*
static int
get_server_idx(int server_rank)
{
  return server_rank - master_server_rank;
}
*/

/*
static int
get_server_rank(int server_idx)
{
  return master_server_rank + server_idx;
}
*/

void
adlb_exit_handler()
{
  printf("adlb_exit_handler:\n");
  print_proc_self_status();
}
