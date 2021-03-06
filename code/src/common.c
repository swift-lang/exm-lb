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
 * common.c
 *
 *  Created on: Jun 7, 2012
 *      Author: wozniak
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpi.h>

#include <tools.h>

#include "common.h"

#include "checks.h"

char xlb_xfer[ADLB_XFER_SIZE];

struct xlb_state xlb_s;

int
xlb_random_server()
{
  int result = random_between(xlb_s.layout.master_server_rank,
                              xlb_s.layout.size);
  return result;
}

double
xlb_wtime(void)
{
  return MPI_Wtime() - xlb_s.start_time;
}

adlb_code xlb_env_long(const char *env_var, long *val)
{
  char *s = getenv(env_var);
  if (s == NULL || strlen(s) == 0)
  {
    // Undefined or empty: leave val untouched
    return ADLB_NOTHING;
  }

  // Try to parse as number
  char *end = NULL;
  long tmp_val = strtol(s, &end, 10);
  CHECK_MSG(end != NULL && end != s && *end == '\0',
        "Invalid env var \"%s\": not a long int", s)

  // Whole string was number
  *val = tmp_val;
  return ADLB_SUCCESS;
}
