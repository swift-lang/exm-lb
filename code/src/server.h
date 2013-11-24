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
 * server.h
 *
 *  Created on: Jun 14, 2012
 *      Author: wozniak
 */

#ifndef SERVER_H
#define SERVER_H

/** Time of last activity: used to determine shutdown */
extern double xlb_time_last_action;

extern int64_t xlb_idle_check_attempt;

/** Are we currently trying to sync with another server?
    Prevents nested syncs, which we do not support */
extern bool xlb_server_sync_in_progress;

/** Did we just get rejected when attempting to server sync? */
extern bool server_sync_retry;

// Number of workers associated with this server
extern int xlb_my_workers;

adlb_code xlb_server_init(void);

// ADLB_Server prototype is in adlb.h

/**
   This process has accepted a sync from a calling server
   Handle the actual RPC here
 */
adlb_code xlb_serve_server(int source);

adlb_code xlb_shutdown_worker(int worker);

/*
  master: if we're the master server
  check_attempt: if not master, the check_attempt number received
 */
bool xlb_server_check_idle_local(bool master, int64_t check_attempt);

extern bool xlb_server_shutting_down;

adlb_code xlb_server_shutdown(void);

adlb_code xlb_server_fail(int code);

/**
   Did we fail?  If so, obtain fail code.
   Given code may be NULL if caller does not require the code
 */
adlb_code xlb_server_failed(bool* aborted, int* code);

/** Get approximate time, updated frequently by server loop */
double xlb_approx_time(void);

/**
  Try to steal and handle any necessary matching.
 */
adlb_code xlb_steal_match();

/**
   @param rank rank of worker belonging to this server
   @return unique number for each of my workers, e.g. to use in array.
           Does not validate that rank is valid
 */
static inline int
xlb_my_worker_ix(int rank)
{
  return rank / xlb_servers;
}

static inline bool
xlb_is_server(int rank)
{
  if (rank > xlb_comm_size - xlb_servers)
    return true;
  return false;
}

/**
   @param rank of worker
   @return rank of server for this worker rank
 */
static inline int
xlb_map_to_server(int rank)
{
  if (xlb_is_server(rank))
    return rank;
  valgrind_assert(rank >= 0);
  valgrind_assert(rank < xlb_comm_size);
  int w = rank % xlb_servers;
  return w + xlb_workers;
}


#endif
