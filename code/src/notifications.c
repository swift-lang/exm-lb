#include "notifications.h"

#include "common.h"
#include "handlers.h"
#include "server.h"
#include "sync.h"

#define MAX_NOTIF_PAYLOAD (32+ADLB_DATA_SUBSCRIPT_MAX)

// Returns size of payload including null terminator
static int fill_payload(char *payload, adlb_datum_id id, adlb_subscript subscript)
{
  int strlen;
  if (!adlb_has_sub(subscript))
  {
    strlen = sprintf(payload, "close %"PRId64"", id);
  }
  else
  {
    // TODO: support binary subscript
    strlen = sprintf(payload, "close %"PRId64" %.*s", id,
                     (int)subscript.length, subscript.key);
  }
  return strlen + 1;
}

static adlb_code notify_local(int target, const char *payload, int length)
{
  int answer_rank = -1;
  int work_prio = 1;
  int work_type = 1; // work_type CONTROL
  int rc = xlb_put_targeted_local(work_type, xlb_comm_rank,
               work_prio, answer_rank,
               target, payload, length);
  ADLB_CHECK(rc);
  return ADLB_SUCCESS;
}

static adlb_code notify_nonlocal(int target, int server,
                        const char *payload, int length)
{
  int answer_rank = -1;
  int work_prio = 1;
  int work_type = 1; // work_type CONTROL
  int rc;
  if (xlb_am_server)
  {
    rc = xlb_sync(server);
    ADLB_CHECK(rc);
  }
  rc = ADLB_Put(payload, length, target,
                    answer_rank, work_type, work_prio, 1);
  ADLB_CHECK(rc);
  return ADLB_SUCCESS;
}


void xlb_free_notif(adlb_notif_t *notifs)
{
  xlb_free_ranks(&notifs->close_notify);
  xlb_free_ranks(&notifs->insert_notify);
  xlb_free_datums(&notifs->references);
}

void xlb_free_ranks(adlb_ranks *ranks)
{
  if (ranks->ranks != NULL)
  {
    free(ranks->ranks);
    ranks->ranks = NULL;
  }
}

void xlb_free_datums(adlb_datums *datums)
{
  if (datums->ids != NULL)
  {
    free(datums->ids);
    datums->ids = NULL;
  }
}


/*
   Set references.
   refs: an array of ids, where negative ids indicate that
          the value should be treated as a string, and
          positive indicates it should be parsed to integer
   value: string value to set references to.
 */
adlb_code xlb_set_refs(adlb_datum_id *refs, int refs_count,
                         const char *value, int value_len,
                         adlb_data_type type)
{
  adlb_code rc;
  for (int i = 0; i < refs_count; i++)
  {
    TRACE("Notifying reference %"PRId64"\n", refs[i]);
    rc = xlb_set_ref_and_notify(refs[i], value, value_len, type);
    ADLB_CHECK(rc);
  }
  return ADLB_SUCCESS;
}

adlb_code
xlb_close_notify(adlb_datum_id id, adlb_subscript subscript,
                   int* ranks, int count)
{
  adlb_code rc;
  char payload[MAX_NOTIF_PAYLOAD];
  int length = fill_payload(payload, id, subscript);

  for (int i = 0; i < count; i++)
  {
    int target = ranks[i];
    int server = xlb_map_to_server(target);
    if (xlb_am_server && server == xlb_comm_rank)
    {
      rc = notify_local(target, payload, length);
      ADLB_CHECK(rc);
    }
    else
    {
      rc = notify_nonlocal(target, server, payload, length);
      ADLB_CHECK(rc);
    }
  }
  return ADLB_SUCCESS;
}

adlb_code
xlb_process_local_notif(adlb_datum_id id, adlb_subscript subscript,
                            adlb_ranks *ranks)
{
  assert(xlb_am_server);
  if (ranks->count == 0)
    return ADLB_SUCCESS;

  char payload[MAX_NOTIF_PAYLOAD];
  int length = fill_payload(payload, id, subscript);

  int i = 0;
  while (i < ranks->count)
  {
    int target = ranks->ranks[i];
    int server = xlb_map_to_server(target);
    if (server == xlb_comm_rank)
    {
      // Swap with last and shorten array
      int rc = notify_local(target, payload, length);
      ADLB_CHECK(rc);
      ranks->ranks[i] = ranks->ranks[ranks->count - 1];
      ranks->count--;
    }
    else
    {
      i++;
    }
  }

  // Free memory if we managed to remove some
  if (ranks->count == 0 && ranks->ranks != NULL)
  {
    free(ranks->ranks);
    ranks->ranks = NULL;
  }
  return ADLB_SUCCESS;
}

adlb_code
xlb_set_ref_and_notify(adlb_datum_id id, const void *value, int length,
                         adlb_data_type type)
{
  DEBUG("xlb_set_ref: <%"PRId64">=%p[%i]", id, value, length);

  int rc = ADLB_SUCCESS;
  int server = ADLB_Locate(id);
  if (xlb_am_server && server != xlb_comm_rank)
    rc = xlb_sync(server);
  ADLB_CHECK(rc);

  rc = ADLB_Store(id, ADLB_NO_SUB, type, value, length, ADLB_WRITE_RC);
  ADLB_CHECK(rc);
  TRACE("SET_REFERENCE DONE");
  return ADLB_SUCCESS;
}

adlb_code
xlb_notify_all(const adlb_notif_t *notifs,
           adlb_datum_id id, adlb_subscript subscript,
           const void *value, int value_len,
           adlb_data_type value_type)
{
  adlb_code rc;
  if (notifs->close_notify.count > 0)
  {
    rc = xlb_close_notify(id, ADLB_NO_SUB, notifs->close_notify.ranks,
                 notifs->close_notify.count);
    ADLB_CHECK(rc);
  }
  if (notifs->insert_notify.count > 0)
  {
    assert(adlb_has_sub(subscript));
    rc = xlb_close_notify(id, subscript, notifs->insert_notify.ranks,
                 notifs->insert_notify.count);
    ADLB_CHECK(rc);
  }
  if (notifs->references.count > 0)
  {
    assert(value != NULL);
    // TODO: handle other types
    rc = xlb_set_refs(notifs->references.ids,
                   notifs->references.count, value, value_len,
                   value_type);
    ADLB_CHECK(rc);
  } 
  return ADLB_SUCCESS;
}
