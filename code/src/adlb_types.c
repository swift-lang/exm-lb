
#define _GNU_SOURCE // for asprintf()

#include "adlb_types.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

#include <vint.h>

#include "adlb.h"
#include "checks.h"
#include "data_cleanup.h"
#include "data_internal.h"
#include "data_structs.h"
#include "debug.h"
#include "multiset.h"

static char *data_repr_container(const adlb_container *c);

/*
  Whether we pad the vint size to VINT_MAX_BYTES
 */
bool ADLB_pack_pad_size(adlb_data_type type)
{
  return type == ADLB_DATA_TYPE_MULTISET ||
         type == ADLB_DATA_TYPE_CONTAINER;
}

adlb_data_code
ADLB_Pack(const adlb_datum_storage *d, adlb_data_type type,
          const adlb_buffer *caller_buffer,
          adlb_binary_data *result)
{
  adlb_data_code dc;
  switch (type)
  {
    case ADLB_DATA_TYPE_INTEGER:
      return ADLB_Pack_integer(&d->INTEGER, result);
    case ADLB_DATA_TYPE_REF:
      return ADLB_Pack_ref(&d->REF, result);
    case ADLB_DATA_TYPE_FLOAT:
      return ADLB_Pack_float(&d->FLOAT, result);
    case ADLB_DATA_TYPE_STRING:
      return ADLB_Pack_string(&d->STRING, result);
    case ADLB_DATA_TYPE_BLOB:
      return ADLB_Pack_blob(&d->BLOB, result);
    case ADLB_DATA_TYPE_FILE_REF:
      return ADLB_Pack_file_ref(&d->FILE_REF, result);
    case ADLB_DATA_TYPE_STRUCT:
      return ADLB_Pack_struct(d->STRUCT, caller_buffer, result);
    case ADLB_DATA_TYPE_CONTAINER:
    case ADLB_DATA_TYPE_MULTISET: {
      // Use ADLB_Pack_buffer implementation for these compound types
      // since we need to accumulate data at the end of the buffer
      adlb_buffer res;
      bool use_caller_buf;
      if (caller_buffer == NULL)
      {
        res.data = NULL;
        res.length = 0;
        use_caller_buf = false;
      }
      else
      {
        res = *caller_buffer;
        use_caller_buf = true;
      }
      int pos = 0;
      dc = ADLB_Pack_buffer(d, type, false, NULL, &res, &use_caller_buf,
                            &pos);
      DATA_CHECK(dc);
      result->data = result->caller_data = res.data;
      result->length = pos;
      
      return ADLB_DATA_SUCCESS; 
    }
    default:
      verbose_error(ADLB_DATA_ERROR_TYPE,
        "Cannot serialize unknown type %i!\n", type);
  }
  // Unreachable:
  return ADLB_DATA_ERROR_UNKNOWN;
}

adlb_data_code
ADLB_Append_buffer(adlb_data_type type, const void *data, int length,
    bool prefix_len, adlb_buffer *output, bool *output_caller_buffer,
    int *output_pos)
{
  adlb_data_code dc;
  // Check buffer large enough for this member
  int required_size = *output_pos + (prefix_len ? (int)VINT_MAX_BYTES : 0)
                    + length;
  dc = ADLB_Resize_buf(output, output_caller_buffer, required_size);
  DATA_CHECK(dc);

  if (prefix_len)
  {
    // Prefix with length of member
    int vint_len = vint_encode(length, output->data + *output_pos);
    assert(vint_len >= 1);
    *output_pos += vint_len;

    if (ADLB_pack_pad_size(type) && vint_len < (int)VINT_MAX_BYTES)
    {
      // We expect the size to be padded for these
      int padding = VINT_MAX_BYTES - vint_len;
      memset(output->data + *output_pos, 0, padding);
      *output_pos += padding;
    }
  }

  // Copy in data
  assert(length >= 0);
  memcpy(output->data + *output_pos, data, (size_t)length);
  *output_pos += length;
  return ADLB_DATA_SUCCESS;
}


adlb_data_code
ADLB_Pack_buffer(const adlb_datum_storage *d, adlb_data_type type,
        bool prefix_len,
        const adlb_buffer *tmp_buf, adlb_buffer *output,
        bool *output_caller_buffer, int *output_pos)
{
  adlb_data_code dc;

  // Some types are implemented by appending to buffer anyway
  if (ADLB_pack_pad_size(type))
  {
    // Reserve space at front to prefix serialized size in bytes
    int required = *output_pos + (int)VINT_MAX_BYTES;
    dc = ADLB_Resize_buf(output, output_caller_buffer, required);
    DATA_CHECK(dc);

    int start_pos = *output_pos;
    if (prefix_len)
    {
      memset(output->data + start_pos, 0, VINT_MAX_BYTES);
      *output_pos += VINT_MAX_BYTES;
    }
    if (type == ADLB_DATA_TYPE_CONTAINER)
    {
      dc = ADLB_Pack_container(&d->CONTAINER, tmp_buf, output,
                              output_caller_buffer, output_pos);
      DATA_CHECK(dc);
    }
    else
    {
      assert(type == ADLB_DATA_TYPE_MULTISET);
      dc = ADLB_Pack_multiset(d->MULTISET, tmp_buf, output,
                              output_caller_buffer, output_pos);
      DATA_CHECK(dc);
    }

    if (prefix_len)
    {
      // Add in actual size to reserved place
      int serialized_len = *output_pos - start_pos - VINT_MAX_BYTES;
      vint_encode(serialized_len, output->data + start_pos);
    }
    return ADLB_DATA_SUCCESS;
  }

  // Get binary representation of datum
  adlb_binary_data packed;
  dc = ADLB_Pack(d, type, tmp_buf, &packed); 
  DATA_CHECK(dc);

  dc = ADLB_Append_buffer(type, packed.data, packed.length,
            prefix_len, output, output_caller_buffer, output_pos);
  
  // Free any malloced temporary memory
  if (tmp_buf != NULL && packed.data != tmp_buf->data)
    ADLB_Free_binary_data(&packed);

  return dc;
}

adlb_data_code
ADLB_Pack_container(const adlb_container *container,
          const adlb_buffer *tmp_buf, adlb_buffer *output,
          bool *output_caller_buffer, int *output_pos)
{
  int dc;

  const table_bp *members = container->members;
  dc = ADLB_Pack_container_hdr(members->size, container->key_type,
      container->val_type, output, output_caller_buffer, output_pos);
  DATA_CHECK(dc);
  
  int appended = 0;

  TABLE_BP_FOREACH(members, item)
  {
    // append key; append val
    int required = *output_pos + (int)VINT_MAX_BYTES + item->key_len;
    dc = ADLB_Resize_buf(output, output_caller_buffer, required);
    DATA_CHECK(dc);

    dc = ADLB_Append_buffer(ADLB_DATA_TYPE_NULL,
          table_bp_get_key(item), item->key_len,
          true, output, output_caller_buffer, output_pos);
    DATA_CHECK(dc);

    dc = ADLB_Pack_buffer(item->data, container->val_type,
            true, tmp_buf, output, output_caller_buffer, output_pos);
    DATA_CHECK(dc);

    appended++;
  }

  DEBUG("Packed container:  entries: %i, key: %s, val: %s, bytes: %d",
        members->size, ADLB_Data_type_tostring(container->key_type), 
        ADLB_Data_type_tostring(container->val_type), *output_pos);
 
  // Check that the number we appended matches
  assert(appended == members->size);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Pack_container_hdr(int elems, adlb_data_type key_type,
        adlb_data_type val_type, adlb_buffer *output,
        bool *output_caller_buffer, int *output_pos)
{
  int required = *output_pos + (int)VINT_MAX_BYTES * 3;
  adlb_data_code dc = ADLB_Resize_buf(output, output_caller_buffer,
                                      required);
  DATA_CHECK(dc);
  
  // pack key/val types
  int vint_len = vint_encode(key_type, output->data + *output_pos);
  assert(vint_len >= 1);
  *output_pos += vint_len;
  
  vint_len = vint_encode(val_type, output->data + *output_pos);
  assert(vint_len >= 1);
  *output_pos += vint_len;

  vint_len = vint_encode(elems, output->data + *output_pos);
  assert(vint_len >= 1);
  *output_pos += vint_len;
  TRACE("Pack container:  entries: %i, key: %s, val: %s, pos: %d",
        elems, ADLB_Data_type_tostring(key_type), 
        ADLB_Data_type_tostring(val_type), *output_pos);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Pack_multiset(const adlb_multiset_ptr ms,
          const adlb_buffer *tmp_buf, adlb_buffer *output,
          bool *output_caller_buffer, int *output_pos)
{
  adlb_data_code dc;

  int size = xlb_multiset_size(ms);

  dc = ADLB_Pack_multiset_hdr(size, ms->elem_type, output,
                        output_caller_buffer, output_pos);
  DATA_CHECK(dc);

  int appended = 0;
  for (uint i = 0; i < ms->chunk_count; i++)
  {
    xlb_multiset_chunk *chunk = ms->chunks[i];
    uint chunk_len = (i == ms->chunk_count - 1) ?
          ms->last_chunk_elems : XLB_MULTISET_CHUNK_SIZE;
    
    for (uint j = 0; j < chunk_len; j++)
    {
      // append value
      dc = ADLB_Pack_buffer(&chunk->arr[j], ms->elem_type,
              true, tmp_buf, output, output_caller_buffer, output_pos);
      DATA_CHECK(dc);

      appended++;
    }
  }

  // Check that the number we appended matches
  assert(appended == size);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Pack_multiset_hdr(int elems, adlb_data_type elem_type,
    adlb_buffer *output, bool *output_caller_buffer, int *output_pos)
{
  adlb_data_code dc;

  int required = *output_pos + (int)VINT_MAX_BYTES * 2;
  dc = ADLB_Resize_buf(output, output_caller_buffer, required);
  DATA_CHECK(dc);

  int vint_len;
  
  // pack elem type
  vint_len = vint_encode(elem_type, output->data + *output_pos);
  assert(vint_len >= 1);
  *output_pos += vint_len;

  vint_len = vint_encode(elems, output->data + *output_pos);
  assert(vint_len >= 1);
  *output_pos += vint_len;
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Pack_multiset_entry(const adlb_datum_storage *d, adlb_data_type type,
        const adlb_buffer *tmp_buf, adlb_buffer *output,
        bool *output_caller_buffer, int *output_pos)
{
  return ADLB_Pack_buffer(d, type, true, tmp_buf, output,
                       output_caller_buffer, output_pos);
}

adlb_data_code
ADLB_Unpack_buffer(adlb_data_type type,
        const void *buffer, int length, int *pos,
        const void **entry, int *entry_length)
{
  assert(buffer != NULL);
  assert(length >= 0);
  assert(pos != NULL);
  assert(*pos >= 0);
  assert(entry != NULL);
  assert(entry_length != NULL);

  if (*pos >= length)
  {
    return ADLB_DATA_DONE;
  }
  int64_t entry_length64;
  int vint_len = vint_decode(buffer + *pos, length - *pos,
                             &entry_length64);
  check_verbose(vint_len >= 1, ADLB_DATA_ERROR_INVALID,
      "Error decoding entry length when unpacking buffer");
  check_verbose(entry_length64 >= 0, ADLB_DATA_ERROR_INVALID,
       "Packed buffer entry length < 0");
  check_verbose(entry_length64 <= INT_MAX, ADLB_DATA_ERROR_INVALID,
       "Packed buffer encode entry length too long for int: %"PRId64,
       entry_length64);
  
  int vint_padded_len = ADLB_pack_pad_size(type) ? VINT_MAX_BYTES : vint_len;
  int remaining = length - *pos - vint_padded_len;
  check_verbose(entry_length64 <= remaining,
        ADLB_DATA_ERROR_INVALID, "Decoded entry less than remaining data "
        "in buffer: %d remains, length %"PRId64, remaining, entry_length64);
  *entry_length = (int)entry_length64;
  *entry = buffer + *pos + vint_padded_len;
  *pos += vint_padded_len + *entry_length;
  return ADLB_DATA_SUCCESS;
}

adlb_data_code ADLB_Unpack(adlb_datum_storage *d, adlb_data_type type,
                          const void *buffer, int length)
{
  return ADLB_Unpack2(d, type, buffer, length, true);
}

adlb_data_code ADLB_Unpack2(adlb_datum_storage *d, adlb_data_type type,
        const void *buffer, int length, bool init_compound)
{
  switch (type)
  {
    case ADLB_DATA_TYPE_INTEGER:
      return ADLB_Unpack_integer(&d->INTEGER, buffer, length);
    case ADLB_DATA_TYPE_REF:
      return ADLB_Unpack_ref(&d->REF, buffer, length);
    case ADLB_DATA_TYPE_FLOAT:
      return ADLB_Unpack_float(&d->FLOAT, buffer, length);
    case ADLB_DATA_TYPE_STRING:
      // Ok to cast from const buffer since we force it to copy
      return ADLB_Unpack_string(&d->STRING, (void *)buffer, length, true);
    case ADLB_DATA_TYPE_BLOB:
      // Ok to cast from const buffer since we force it to copy
      return ADLB_Unpack_blob(&d->BLOB, (void *)buffer, length, true);
    case ADLB_DATA_TYPE_FILE_REF:
      return ADLB_Unpack_file_ref(&d->FILE_REF, buffer, length);
    case ADLB_DATA_TYPE_STRUCT:
      return ADLB_Unpack_struct(&d->STRUCT, buffer, length);
    case ADLB_DATA_TYPE_CONTAINER:
      return ADLB_Unpack_container(&d->CONTAINER, buffer, length,
                                   init_compound);
    case ADLB_DATA_TYPE_MULTISET:
      return ADLB_Unpack_multiset(&d->MULTISET, buffer, length,
                                  init_compound);
    default:
      printf("data_store(): unknown type: %i\n", type);
      return ADLB_DATA_ERROR_INVALID;
      break;
  }
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Unpack_container(adlb_container *container,
      const void *data, int length, bool init_cont)
{
  assert(container != NULL);
  assert(data != NULL);

  adlb_data_code dc;
  int pos = 0;
  int entries;
  adlb_data_type key_type, val_type;

  dc = ADLB_Unpack_container_hdr(data, length, &pos,
        &entries, &key_type, &val_type);
  DATA_CHECK(dc);

  if (init_cont)
  {
    container->key_type = key_type;
    container->val_type = val_type;
    container->members = table_bp_create(CONTAINER_INIT_CAPACITY);
  }
  else
  {
    assert(container->members != NULL);
    check_verbose(container->key_type == key_type &&
         container->val_type == val_type, ADLB_DATA_ERROR_TYPE,
        "Unpacked container type does not match: expected %s[%s] vs. %s[%s]",
        ADLB_Data_type_tostring(container->val_type),
        ADLB_Data_type_tostring(container->key_type),
        ADLB_Data_type_tostring(val_type), ADLB_Data_type_tostring(key_type));
  }

  for (int i = 0; i < entries; i++)
  {
    // unpack key/value pair and add to container
    const void *key, *val;
    int key_len, val_len;
    dc = ADLB_Unpack_container_entry(key_type, val_type,
          data, length, &pos, &key, &key_len, &val, &val_len);
    DATA_CHECK(dc);

    adlb_datum_storage *d = malloc(sizeof(adlb_datum_storage));
    check_verbose(d != NULL, ADLB_DATA_ERROR_OOM,
                  "error allocating memory");
    dc = ADLB_Unpack(d, val_type, val, val_len);
    DATA_CHECK(dc);

    // TODO: handle case where key already exists
    bool ok = table_bp_add(container->members, key, key_len, d);
    check_verbose(ok, ADLB_DATA_ERROR_OOM, "Error adding to container");
  }

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Unpack_container_hdr(const void *data, int length, int *pos,
        int *entries, adlb_data_type *key_type, adlb_data_type *val_type)
{
  DEBUG("Unpack container: %d/%d", length, *pos);
  int vint_len;
  int64_t key_type64;
  vint_len = vint_decode(data + *pos, length - *pos, &key_type64);
  check_verbose(vint_len >= 1, ADLB_DATA_ERROR_INVALID,
                "Could not decode vint for key type (%i)", vint_len);
  *pos += vint_len;
  *key_type = (adlb_data_type)key_type64; 
  check_verbose(key_type64 == *key_type, ADLB_DATA_ERROR_INVALID,
              "Container key type is out of range: %"PRId64, key_type64);
  
  int64_t val_type64;
  vint_len = vint_decode(data + *pos, length - *pos, &val_type64);
  check_verbose(vint_len >= 1, ADLB_DATA_ERROR_INVALID,
                "Could not decode vint for value type (%i)", vint_len);
  *pos += vint_len;
  *val_type = (adlb_data_type)val_type64; 
  check_verbose(val_type64 == *val_type, ADLB_DATA_ERROR_INVALID,
              "Container val type is out of range: %"PRId64, val_type64);

  int64_t entries64;
  vint_len = vint_decode(data + *pos, length, &entries64);
  check_verbose(vint_len >= 0, ADLB_DATA_ERROR_INVALID,
                "Could not extract multiset entry count (%i)", vint_len);
  check_verbose(entries64 >= 0 && entries64 <= INT_MAX,
      ADLB_DATA_ERROR_INVALID, "Entries out of range: %"PRId64, entries64);
  *entries = (int)entries64;
  *pos += vint_len;
  
  DEBUG("Unpack container:  entries: %i, key: %s, val: %s, pos: %d",
        *entries, ADLB_Data_type_tostring(*key_type), 
        ADLB_Data_type_tostring(*val_type), *pos);
  return ADLB_DATA_SUCCESS;

}

adlb_data_code
ADLB_Unpack_container_entry(adlb_data_type key_type,
          adlb_data_type val_type,
          const void *data, int length, int *pos,
          const void **key, int *key_len,
          const void **val, int *val_len)
{
  // Key data not stored in typed way
  int dc;
  dc = ADLB_Unpack_buffer(ADLB_DATA_TYPE_NULL, data, length,
                  pos, key, key_len);
  DATA_CHECK(dc);
  
  dc = ADLB_Unpack_buffer(val_type, data, length,
                  pos, val, val_len);
  DATA_CHECK(dc);
  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Unpack_multiset(adlb_multiset_ptr *ms,
                const void *data, int length, bool init_ms)
{
  assert(ms != NULL);
  assert(data != NULL);

  adlb_data_code dc;
  int pos = 0;

  int entries;
  adlb_data_type elem_type;
  dc = ADLB_Unpack_multiset_hdr(data, length, &pos,
                                &entries, &elem_type);
  DATA_CHECK(dc);
  
  if (init_ms)
  {
    *ms = xlb_multiset_alloc(elem_type);
  }
  else
  {
    assert(*ms != NULL);
    assert((*ms)->elem_type == elem_type);
    check_verbose((*ms)->elem_type == elem_type, ADLB_DATA_ERROR_TYPE,
        "Unpacked multiset elem type does not match: expected %s vs. %s",
        ADLB_Data_type_tostring((*ms)->elem_type),
        ADLB_Data_type_tostring(elem_type));
  }

  for (int i = 0; i < entries; i++)
  {
    // unpack elem and add it
    const void *elem;
    int elem_len;
    dc = ADLB_Unpack_multiset_entry(elem_type, data, length,
                                    &pos, &elem, &elem_len);
    DATA_CHECK(dc);
    
    dc = xlb_multiset_add(*ms, elem, elem_len, NULL);
    DATA_CHECK(dc);
  }

  return ADLB_DATA_SUCCESS;
}

adlb_data_code
ADLB_Unpack_multiset_hdr(const void *data, int length, int *pos,
                int *entries, adlb_data_type *elem_type)
{
  int vint_len;
  int64_t elem_type64;
  vint_len = vint_decode(data + *pos, length - *pos, &elem_type64);
  assert(vint_len >= 1);
  *pos += vint_len;
  *elem_type = (adlb_data_type)elem_type64; 
  check_verbose(elem_type64 == *elem_type, ADLB_DATA_ERROR_INVALID,
              "Multiset elem type is out of range: %"PRId64, elem_type64);

  int64_t entries64;
  vint_len = vint_decode(data + *pos, length, &entries64);
  check_verbose(vint_len >= 0, ADLB_DATA_ERROR_INVALID,
                "Could not extract multiset entry count");
  check_verbose(entries64 >= 0 && entries64 <= INT_MAX,
      ADLB_DATA_ERROR_INVALID, "Entries out of range: %"PRId64, entries64);
  *entries = (int)entries64;
  *pos += vint_len;
  
  return ADLB_DATA_SUCCESS;
}

/*
  Unpack value from buffer
 */
adlb_data_code
ADLB_Unpack_multiset_entry(adlb_data_type elem_type,
          const void *data, int length, int *pos,
          const void **elem, int *elem_len)
{
  return ADLB_Unpack_buffer(elem_type, data, length,
                            pos, elem, elem_len);
}

/* Free the memory associated with datum contents */
adlb_data_code ADLB_Free_storage(adlb_datum_storage *d, adlb_data_type type)
{
  adlb_data_code dc;
  switch (type)
  {
    case ADLB_DATA_TYPE_STRING:
      free(d->STRING.value);
      break;
    case ADLB_DATA_TYPE_BLOB:
      free(d->BLOB.value);
      break;
    case ADLB_DATA_TYPE_CONTAINER:
    {
      dc = xlb_members_cleanup(&d->CONTAINER, true, ADLB_NO_RC, NO_SCAVENGE);
      DATA_CHECK(dc);
      break;
    }
    case ADLB_DATA_TYPE_MULTISET:
      dc = xlb_multiset_cleanup(d->MULTISET, true, true, ADLB_NO_RC,
                                NO_SCAVENGE);
      DATA_CHECK(dc);
      break;
    case ADLB_DATA_TYPE_STRUCT:
      xlb_free_struct(d->STRUCT, true);
      break;
    // Types with no malloced storage:
    case ADLB_DATA_TYPE_INTEGER:
    case ADLB_DATA_TYPE_FLOAT:
    case ADLB_DATA_TYPE_REF:
    case ADLB_DATA_TYPE_FILE_REF:
      break;
    default:
      check_verbose(false, ADLB_DATA_ERROR_TYPE,
                    "datum_gc(): unknown type %u", type);
      break;
  }
  return ADLB_DATA_SUCCESS;
}

char *ADLB_Data_repr(const adlb_datum_storage *d, adlb_data_type type)
{
  int rc;
  adlb_data_code dc;
  char *tmp;
  switch (type)
  {
    case ADLB_DATA_TYPE_STRING:
    {
      // Allocate with enough room for trailing ...
      tmp = malloc((size_t)d->STRING.length + 5);
      strcpy(tmp, d->STRING.value);
      int pos = 0;
      // Don't return multiple lines of multi-line string
      while (tmp[pos] != '\0')
      {
        if (tmp[pos] == '\n')
        {
          tmp[pos++] = '.';
          tmp[pos++] = '.';
          tmp[pos++] = '.';
          tmp[pos++] = '\0';
          break;
        }
        pos++;
      }
      return tmp;
    }
    case ADLB_DATA_TYPE_INTEGER:
      rc = asprintf(&tmp, "%"PRId64"", d->INTEGER);
      assert(rc >= 0);
      return tmp;
    case ADLB_DATA_TYPE_REF:
      rc = asprintf(&tmp, "<%"PRId64">", d->REF);
      assert(rc >= 0);
      return tmp;
    case ADLB_DATA_TYPE_FLOAT:
      rc = asprintf(&tmp, "%lf", d->FLOAT);
      assert(rc >= 0);
      return tmp;
      break;
    case ADLB_DATA_TYPE_BLOB:
      rc = asprintf(&tmp, "blob (%d bytes)", d->BLOB.length);
      assert(rc >= 0);
      return tmp;
    case ADLB_DATA_TYPE_FILE_REF:
      rc = asprintf(&tmp, "status:<%"PRId64"> filename:<%"PRId64"> mapped:%i",
                    d->FILE_REF.status_id, d->FILE_REF.filename_id,
                    d->FILE_REF.mapped);
      assert(rc >= 0);
      return tmp;
    case ADLB_DATA_TYPE_CONTAINER:
      return data_repr_container(&d->CONTAINER);
    case ADLB_DATA_TYPE_MULTISET:
      dc = xlb_multiset_repr(d->MULTISET, &tmp);
      assert(dc == ADLB_DATA_SUCCESS);
      return tmp;
    case ADLB_DATA_TYPE_STRUCT:
      return xlb_struct_repr(d->STRUCT);
    default:
      rc = asprintf(&tmp, "unknown type: %i\n", type);
      assert(rc >= 0);
      return tmp;
      break;
  }
  return strdup("???");
}

static char *data_repr_container(const adlb_container *c)
{
  adlb_data_code dc;
  table_bp* members = c->members;
  size_t cont_str_len = 1024;
  char *cont_str = malloc(cont_str_len);
  int cont_str_pos = 0;

  const char *kts = ADLB_Data_type_tostring(c->key_type);
  const char *vts = ADLB_Data_type_tostring(c->val_type);
  dc = xlb_resize_str(&cont_str, &cont_str_len, cont_str_pos,
                 strlen(kts) + strlen(vts) + 4);
  assert(dc == ADLB_DATA_SUCCESS);
  cont_str_pos += sprintf(&cont_str[cont_str_pos], "%s=>%s: ", kts, vts);

  TABLE_BP_FOREACH(members, item)
  {
    adlb_container_val v = item->data;
    char *value_s = ADLB_Data_repr(v, c->val_type);
    dc = xlb_resize_str(&cont_str, &cont_str_len, cont_str_pos,
                   (item->key_len - 1) + strlen(value_s) + 7);
    assert(dc == ADLB_DATA_SUCCESS);
    if (c->key_type == ADLB_DATA_TYPE_STRING)
    {
      cont_str_pos += sprintf(&cont_str[cont_str_pos], "\"%s\"={%s} ",
                              (char*)table_bp_get_key(item), value_s);
    }
    else
    {
      // TODO: support binary keys
      cont_str_pos += sprintf(&cont_str[cont_str_pos], "\"%s\"={%s} ",
                              (char*)table_bp_get_key(item), value_s);
    }

    free(value_s);
  }
  cont_str[cont_str_pos] = '\0';
  return cont_str;
}

adlb_data_code
xlb_resize_str(char **str, size_t *curr_size, int pos, size_t needed)
{
  assert(pos >= 0);
  size_t total_needed = ((size_t)pos) + needed + 1;
  if (total_needed > *curr_size)
  {
    size_t new_size = *curr_size + 1024;
    if (new_size < total_needed)
      new_size = total_needed + 1024;

    DATA_REALLOC(*str, new_size);
    *curr_size = new_size;
  }
  return ADLB_DATA_SUCCESS;
}
