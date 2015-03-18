
/*
 * location.c
 *
 *  Created on: Feb 4, 2015
 *      Author: wozniak
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/utsname.h>

#include <list_i.h>
#include <table.h>
#include <table_ip.h>
#include <tools.h>

#include "adlb.h"
#include "checks.h"
#include "common.h"
#include "debug.h"
#include "location.h"

/**
   Maps string hostname to list of int ranks which are running on
   that host
 */
struct xlb_hostmap {
  struct table map;
};

static void
report_ranks(MPI_Comm comm)
{
  int rank;
  MPI_Comm_rank(comm, &rank);

  int debug_ranks;
  getenv_integer("ADLB_DEBUG_RANKS", 0, &debug_ranks);
  if (!debug_ranks) return;

  struct utsname u;
  uname(&u);

  printf("ADLB_DEBUG_RANKS: rank: %i nodename: %s\n",
         rank, u.nodename);
}

adlb_code
xlb_hostnames_gather(MPI_Comm comm, struct xlb_hostnames *hostnames)
{
  int rc;

  report_ranks(comm);

  int comm_size;
  rc = MPI_Comm_size(comm, &comm_size);
  MPI_CHECK(rc);

  struct utsname u;
  uname(&u);

  // Length of nodenames
  hostnames->name_length = sizeof(u.nodename);

  hostnames->my_name = malloc(hostnames->name_length);
  ADLB_MALLOC_CHECK(hostnames->my_name);

  // This prevents valgrind errors:
  memset(hostnames->my_name, 0, hostnames->name_length);
  strcpy(hostnames->my_name, u.nodename);

  hostnames->all_names = malloc((size_t)comm_size *
                hostnames->name_length * sizeof(char));
  ADLB_MALLOC_CHECK(hostnames->all_names);

  rc = MPI_Allgather(
      hostnames->my_name,   (int)hostnames->name_length, MPI_CHAR,
      hostnames->all_names, (int)hostnames->name_length, MPI_CHAR, comm);
  MPI_CHECK(rc);

  return ADLB_SUCCESS;
}

const char *
xlb_hostnames_lookup(const struct xlb_hostnames *hostnames, int rank)
{
  assert(hostnames->all_names != NULL);
  return &hostnames->all_names[(size_t)rank * hostnames->name_length];
}

void
xlb_hostnames_free(struct xlb_hostnames *hostnames) {
  free(hostnames->all_names);
  free(hostnames->my_name);
  hostnames->all_names = NULL;
}

adlb_code
xlb_hostmap_init(const struct xlb_layout *layout,
                 const struct xlb_hostnames *hostnames,
                 struct xlb_hostmap **hostmap)
{
  *hostmap = malloc(sizeof(**hostmap));
  ADLB_MALLOC_CHECK(*hostmap);

  bool debug_hostmap = false;
  char* t = getenv("ADLB_DEBUG_HOSTMAP");
  if (t != NULL && strcmp(t, "1") == 0)
    debug_hostmap = true;

  table_init(&(*hostmap)->map, 1024);

  for (int rank = 0; rank < layout->size; rank++)
  {
    const char* name = xlb_hostnames_lookup(hostnames, rank);

    if (layout->rank == 0 && debug_hostmap)
      printf("HOSTMAP: %s -> %i\n", name, rank);


    if (!table_contains(&(*hostmap)->map, name))
    {
      struct list_i* L = list_i_create();
      table_add(&(*hostmap)->map, name, L);
    }
    struct list_i* L;
    table_search(&(*hostmap)->map, name, (void*) &L);
    list_i_add(L, rank);
  }

  return ADLB_SUCCESS;
}


adlb_code
xlb_get_hostmap_mode(xlb_hostmap_mode *mode)
{
  // Deprecated feature:
  int disable_hostmap;
  bool b = getenv_integer("ADLB_DISABLE_HOSTMAP", 0, &disable_hostmap);
  if (!b)
  {
    printf("Bad integer in ADLB_DISABLE_HOSTMAP!\n");
    return ADLB_ERROR;
  }
  if (disable_hostmap == 1)
  {
    *mode = HOSTMAP_DISABLED;
    return ADLB_SUCCESS;
  }

  char* m = getenv("ADLB_HOSTMAP_MODE");
  if (m == NULL || strlen(m) == 0)
    m = "ENABLED";
  DEBUG("ADLB_HOSTMAP_MODE: %s\n", m);
  if (strcmp(m, "ENABLED") == 0)
    *mode = HOSTMAP_ENABLED;
  else if (strcmp(m, "LEADERS") == 0)
    *mode = HOSTMAP_LEADERS;
  else if (strcmp(m, "DISABLED") == 0)
    *mode = HOSTMAP_DISABLED;
  else
  {
    printf("Unknown setting: ADLB_HOSTMAP_MODE=%s\n", m);
    return ADLB_ERROR;
  }

  return ADLB_SUCCESS;
}

adlb_code
xlb_setup_leaders(struct xlb_layout *layout, struct xlb_hostmap *hosts,
                  MPI_Comm comm, MPI_Comm *leader_comm)
{
  // Cannot be more leaders than hosts
  int max_leaders = hosts->map.size;

  int* leader_ranks = malloc((size_t)(max_leaders) * sizeof(int));
  int leader_rank_count = 0;

  TABLE_FOREACH(&hosts->map, table_item)
  {
    struct list_i *rank_list = table_item->data;
    assert(rank_list->size > 0);

    struct list_i_item *list_item = rank_list->head;

    // Find lowest non-server
    while (list_item != NULL &&
           xlb_is_server(layout, list_item->data))
    {
      list_item = list_item->next;
    }

    if (list_item != NULL)
    {
      int leader_rank = list_item->data;

      leader_ranks[leader_rank_count++] = leader_rank;
      TRACE("leader: %i\n", leader_rank);
      if (leader_rank == layout->rank)
      {
        layout->am_leader = true;
        DEBUG("am leader");
      }
    }
  }

  MPI_Group group_all, group_leaders;
  MPI_Comm_group(comm, &group_all);

  MPI_Group_incl(group_all, leader_rank_count, leader_ranks,
                 &group_leaders);
  MPI_Comm_create(comm, group_leaders, leader_comm);
  MPI_Group_free(&group_leaders);
  MPI_Group_free(&group_all);

  free(leader_ranks);

  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_stats(unsigned int* count, unsigned int* name_max)
{
  CHECK_MSG(xlb_s.hostmap_mode != HOSTMAP_DISABLED,
            "ADLB_Hostmap_stats: hostmap is disabled!");
  struct utsname u;
  *count = (uint)xlb_s.hostmap->map.size;
  *name_max = sizeof(u.nodename);
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_lookup(const char* name, int max,
                    int* output, int* actual)
{
  CHECK_MSG(xlb_s.hostmap_mode != HOSTMAP_DISABLED,
            "ADLB_Hostmap_lookup: hostmap is disabled!");
  struct list_i* L;
  bool b = table_search(&xlb_s.hostmap->map, name, (void*) &L);
  if (!b)
    return ADLB_NOTHING;
  int i = 0;
  for (struct list_i_item* item = L->head; item; item = item->next)
  {
    output[i++] = item->data;
    if (i == max)
      break;
  }
  *actual = i;
  return ADLB_SUCCESS;
}

adlb_code
ADLB_Hostmap_list(char* output, unsigned int max,
                  unsigned int offset, int* actual)
{
  CHECK_MSG(xlb_s.hostmap_mode != HOSTMAP_DISABLED,
            "ADLB_Hostmap_list: hostmap is disabled!");
  // Number of chars written
  int count = 0;
  // Number of hostnames written
  int h = 0;
  // Moving pointer into output
  char* p = output;
  // Counter for offset
  int j = 0;

  TABLE_FOREACH(&xlb_s.hostmap->map, item)
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

void
xlb_hostmap_free(struct xlb_hostmap *hostmap)
{
  if (hostmap == NULL) return;

  for (int i = 0; i < hostmap->map.capacity; i++)
  {
    table_entry *head = &hostmap->map.array[i];
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
  table_release(&hostmap->map);
}