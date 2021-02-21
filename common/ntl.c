#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "ntl.h"

void **
ntl_malloc_init (size_t nelems,  size_t elem_size, void (*init)(void * elem_p))
{
  char * p = (char *)malloc((nelems + 1) * sizeof(void *) // indices
                            +  nelems * elem_size); // elements

  char * elem_start = p + (nelems + 1) * sizeof(void *);
  void ** array = (void **)p;
  size_t i;
  for (i = 0; i < nelems; i++) {
    if (init)
      init(elem_start);

    array[i] = (void *)elem_start;
    elem_start += elem_size;
  }
  array[i] = 0;
  return array;
}

void **
ntl_malloc (size_t nelems,  size_t elem_size)
{
  return ntl_malloc_init(nelems, elem_size, NULL);
}

void **
ntl_calloc_init (size_t nelems,  size_t elem_size, void (*init)(void * elem_p))
{
  void ** p = ntl_malloc_init(nelems, elem_size, NULL);
  char * elem_start = (char *)p + ((nelems + 1) * sizeof(void *));
  memset(elem_start, 0, nelems * elem_size);
  if (init) {
    for (int i = 0; p[i]; i++)
      init(p[i]);
  }
  return p;
}

void **
ntl_calloc (size_t nelems,  size_t elem_size)
{
  return ntl_calloc_init(nelems, elem_size, NULL);
}

void **
ntl_realloc_init(void **p, size_t new_nelems, size_t elem_size,
                 void (*init)(void * elem_p))
{
  void ** new_p = ntl_calloc_init(new_nelems, elem_size, NULL);
  int i = 0;
  for (i = 0; p[i]; i++)
    memcpy(new_p[i], p[i], elem_size);

  if (init) {
    while (new_p[i]) {
      init(new_p[i]);
      i++;
    }
  }

  free(p);
  return new_p;
}


void
ntl_free(void **p, void (*free_elem)(void *p))
{
  size_t i;
  for (i = 0; p[i]; i++)
    (*free_elem)(p[i]);
  free(p);
}

size_t
ntl_length (void **p)
{
  size_t i;
  for (i = 0; p[i]; i++) /* empty body */;

  return i;
}


void **
ntl_dup (void ** p, size_t elem_size)
{
  void ** o =  ntl_calloc(ntl_length(p), elem_size);
  for (size_t i = 0; p[i]; i++)
    memcpy(o[i], p[i], elem_size);
  return o;
}


void
ntl_apply(void **p, void (*f)(void *p))
{
  size_t i;
  for (i = 0; p[i]; i++)
    (*f)(p[i]);
}

/*
 *
 */
int
ntl_to_buf(char *buf, size_t size, void **p, struct ntl_str_delimiter * d,
           ntl_elem_serializer * x)
{
  static struct ntl_str_delimiter dx = { '[', ",", "", ']' };
  if (!d) d = &dx;

  const char * start = buf;
  size_t i, tsize = 0;
  int psize;

  if (start) {
    buf[0] = d->start_delimiter;
    buf ++;
  }
  tsize ++;

  for(i = 0; p[i]; i++) {
    bool is_last = (NULL == p[i+1]);
    psize = (*x)(buf, size, p[i]);
    if (psize < 0) // error happens
      return -1;

    if(start) {
      buf += psize; // move to next available byte
    }
    tsize += psize;
    if (is_last) {
      psize = strlen (d->last_element_delimiter);
      if (start) {
        memcpy(buf, d->last_element_delimiter, psize);
        buf += psize;
      }
    }
    else {
      psize = strlen (d->element_delimiter);
      if (start) {
        memcpy(buf, d->element_delimiter, psize);
        buf += psize;
      }
    }
    tsize += psize;
  }

  if (start) {
    buf[0] = d->end_delimiter;
    buf ++;
  }
  tsize ++;
  return tsize;
}

int
ntl_to_abuf(char ** buf_p, void **p, struct ntl_str_delimiter * d,
           ntl_elem_serializer * x)
{
  int s = ntl_to_buf(NULL, 0, p, d, x);
  if (s < 0)
    return -1;

  *buf_p = (char *)malloc(s);
  return ntl_to_buf(*buf_p, s, p, d, x);
}

void **
ntl_fmap(void ** from_list, size_t to_elem_size, ntl_converter * f)
{
  void ** to_list = ntl_calloc(ntl_length(from_list), to_elem_size);
  if (f) {
    size_t i;
    for (i = 0; from_list[i]; i++)
      (*f)(from_list[i], to_list[i]);
  }
  return to_list;
}


void **
ntl_append(void ** p, size_t elem_size, void * added_elem)
{
  size_t len = ntl_length(p);
  void ** o = ntl_malloc(len + 1, elem_size);
  size_t i;
  for (i = 0; p[i]; i++)
    memcpy(o[i], p[i], elem_size);

  memcpy(o[i], added_elem, elem_size);
  return o;
}

int
ntl_from_buf(char *buf, size_t len, struct ntl_deserializer * deserializer)
{
  struct sized_buffer **elem_bufs = NULL;
  int ret = (*deserializer->partition_as_sized_bufs)(buf, len, &elem_bufs);
  if (0 == ret) {
    *deserializer->ntl_recipient_p = NULL;
    return 0;
  }

  size_t nelems = ntl_length((void **)elem_bufs);
  void ** new_ntl =
    ntl_calloc_init(nelems, deserializer->elem_size, deserializer->init_elem);

  for (size_t i=0; elem_bufs[i]; ++i)
    (*deserializer->elem_from_buf)(
      elem_bufs[i]->start,
      elem_bufs[i]->size,
      new_ntl[i]);

  free(elem_bufs);
  *deserializer->ntl_recipient_p = new_ntl;
  return nelems;
}


int
ntl_is_a_member (void ** p , void * addr)
{
  for (size_t i = 0; p[i]; i++) {
    if (p[i] == addr)
      return 1;
  }
  return 0;
}
