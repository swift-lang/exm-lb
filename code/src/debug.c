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


#include <stdlib.h>
#include <string.h>

#include "debug.h"

// NOTE: DEBUG and TRACE are disabled by default by macros:
//       Cf. debug.h
bool xlb_debug_enabled = true;
bool xlb_trace_enabled = true;

void
debug_check_environment()
{
  char* v;

  v = getenv("ADLB_TRACE");
  if (v != NULL)
  {
    if (strcmp(v, "0") == 0)
      xlb_trace_enabled = false;
  }

  v = getenv("ADLB_DEBUG");
  if (v != NULL)
  {
    if (strcmp(v, "0") == 0)
    {
      xlb_debug_enabled = false;
      xlb_trace_enabled = false;
    }
  }
}
