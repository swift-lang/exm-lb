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
 * sync.c
 *
 *  Created on: Aug 20, 2012
 *      Author: wozniak
 */

#include <assert.h>

#include <mpi.h>

#include "backoffs.h"
#include "common.h"
#include "debug.h"
#include "messaging.h"
#include "mpe-tools.h"
#include "server.h"
#include "steal.h"
#include "sync.h"

static inline adlb_code send_sync(int target, const struct packed_sync *hdr);
static inline adlb_code msg_from_target(int target,
                                  const struct packed_sync *hdr, bool* done);
static inline adlb_code msg_from_other_server(int other_server, 
                  int target, const struct packed_sync *my_hdr,
                  pending_sync *pending_syncs, int *pending_sync_count);
static inline adlb_code msg_shutdown(bool* done);

// Number of pending sync requests to store before rejecting
#define PENDING_SYNC_BUFFER_SIZE 1024

// IDs of servers with pending sync requests
pending_sync xlb_pending_syncs[PENDING_SYNC_BUFFER_SIZE];
int xlb_pending_sync_count = 0;

adlb_code
xlb_sync(int target)
{
  struct packed_sync *hdr = malloc(PACKED_SYNC_SIZE);
  memset(hdr, 0, PACKED_SYNC_SIZE);
  hdr->mode = ADLB_SYNC_REQUEST;
  adlb_code code = xlb_sync2(target, hdr);
  free(hdr);
  return code;
}

/*
   While attempting a sync, one of three things may happen:
   1) The target responds.  It either accepts or rejects the sync
      request.  If it rejects, this process retries
   2) Another server interrupts this process with a sync request.
      This process either accepts and serves the request; stores the
      request in xlb_pending_syncs to process later, or rejects it
   3) The master server tells this process to shut down
   These numbers correspond to the variables in the function
 */
adlb_code
xlb_sync2(int target, const struct packed_sync *hdr)
{
  TRACE_START;
  DEBUG("\t xlb_sync() target: %i", target);
  int rc = ADLB_SUCCESS;

  MPE_LOG(xlb_mpe_dmn_sync_start);

  MPI_Status status1, status2, status3;
  /// MPI_Request request1, request2;
  int flag1 = 0, flag2 = 0, flag3 = 0;

  assert(!xlb_server_sync_in_progress);
  xlb_server_sync_in_progress = true;

  // When true, break the loop
  bool done = false;

  // Send initial request:
  send_sync(target, hdr);

  while (!done)
  {
    TRACE("xlb_sync: loop");

    IPROBE(target, ADLB_TAG_SYNC_RESPONSE, &flag1, &status1);
    if (flag1)
    {
      msg_from_target(target, hdr, &done);
      if (done) break;
    }

    IPROBE(MPI_ANY_SOURCE, ADLB_TAG_SYNC_REQUEST, &flag2, &status2);
    if (flag2)
      msg_from_other_server(status2.MPI_SOURCE, target, hdr,
                            xlb_pending_syncs, &xlb_pending_sync_count);

    IPROBE(MPI_ANY_SOURCE, ADLB_TAG_SHUTDOWN_SERVER, &flag3,
           &status3);
    if (flag3)
    {
      msg_shutdown(&done);
      rc = ADLB_SHUTDOWN;
      // TODO: break here?
    }

    if (!flag1 && !flag2 && !flag3)
    {
      // Nothing happened, don't poll too aggressively
      xlb_backoff_sync();
    }
  }

  xlb_server_sync_in_progress = false;
  TRACE_END;
  MPE_LOG(xlb_mpe_dmn_sync_end);

  return rc;
}

static inline adlb_code
send_sync(int target, const struct packed_sync *hdr)
{
  SEND(hdr, PACKED_SYNC_SIZE, MPI_BYTE, target, ADLB_TAG_SYNC_REQUEST);
  return ADLB_SUCCESS;
}

/**
   @return adlb_code
 */
static inline adlb_code
msg_from_target(int target, const struct packed_sync *hdr, bool* done)
{
  MPI_Status status;
  TRACE_START;
  int response;
  RECV(&response, 1, MPI_INT, target, ADLB_TAG_SYNC_RESPONSE);
  if (response)
  {
    // Accepted
    DEBUG("server_sync: [%d] sync accepted by %d.", xlb_world_rank, target);
    *done = true;
  }
  else
  {
    // Rejected
    DEBUG("server_sync: [%d] sync rejected by %d.  retrying...",
           xlb_world_rank, target);
    send_sync(target, hdr);
  }
  TRACE_END
  return ADLB_SUCCESS;
}

static inline adlb_code msg_from_other_server(int other_server, int target,
                  const struct packed_sync *my_hdr,
                  pending_sync *pending_syncs, int *pending_sync_count)
{
  TRACE_START;
  MPI_Status status;

  struct packed_sync *other_hdr = malloc(PACKED_SYNC_SIZE);
  RECV(other_hdr, PACKED_SYNC_SIZE, MPI_BYTE, other_server, ADLB_TAG_SYNC_REQUEST);

  /* Serve another server
   * We need to avoid the case of circular deadlock, e.g. where A is waiting
   * to serve B, which is waiting to serve C, which is waiting to serve A, 
   * so don't serve lower ranked servers until we've finished our
   * sync request */
  if (other_server > xlb_world_rank)
  {
    // accept incoming sync
    DEBUG("server_sync: [%d] interrupted by incoming sync request from %d",
                        xlb_world_rank, other_server);
    bool server_sync_rejected = false;
    
    handle_accepted_sync(other_server, other_hdr, &server_sync_rejected);

    if (other_server == target && server_sync_rejected)
    {
      // In this case, the interrupting server is our sync target
      // It detected the collision and rejected this process
      // Try again
      DEBUG("server_sync: [%d] sync rejected earlier retrying sync with %d...",
                        xlb_world_rank, other_server);
      xlb_backoff_sync_rejected();
      send_sync(target, my_hdr);
    }
  }
  else
  {
    // Don't handle right away, either defer or reject
    if (*pending_sync_count < PENDING_SYNC_BUFFER_SIZE) {
      // Store sync request so we can later service it
      DEBUG("server_sync: [%d] deferring incoming sync request from %d.  "
            "%d deferred sync requests", xlb_world_rank, other_server, *pending_sync_count);
      pending_syncs[*pending_sync_count].rank = other_server;
      pending_syncs[*pending_sync_count].hdr = other_hdr;
      other_hdr = NULL; // Lose reference
      (*pending_sync_count)++;
    }
    else
    {
      int response = 0;
      DEBUG("server_sync: [%d] rejecting incoming sync request from %d",
                                        xlb_world_rank, other_server);
      SEND(&response, 1, MPI_INT, other_server,
           ADLB_TAG_SYNC_RESPONSE);
    }
  }
  if (other_hdr != NULL)
    free(other_hdr);
  TRACE_END;
  return ADLB_SUCCESS;
}

/*
  One we have accepted sync, do whatever processing needed to service
 */
adlb_code handle_accepted_sync(int rank, const struct packed_sync *hdr,
                               bool *server_sync_rejected)
{
  static int response = 1;
  SEND(&response, 1, MPI_INT, rank, ADLB_TAG_SYNC_RESPONSE);

  int mode = hdr->mode;
  adlb_code code = ADLB_ERROR;
  if (mode == ADLB_SYNC_REQUEST)
  {
    code = xlb_serve_server(rank, server_sync_rejected);
  }
  else if (mode == ADLB_SYNC_STEAL)
  {
    // Respond to steal
    code = handle_steal(rank, &hdr->steal);
  }
  else
  {
    printf("Invalid sync mode: %d\n", mode);
    return ADLB_ERROR;
  }
  return code;
}

static inline adlb_code
msg_shutdown(bool* done)
{
  TRACE_START;
  DEBUG("server_sync: [%d] cancelled by shutdown!", xlb_world_rank);
  *done = true;
  TRACE_END;
  return ADLB_SUCCESS;
}

