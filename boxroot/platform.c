#define CAML_INTERNALS

#include "platform.h"
#include <stdlib.h>
#include <errno.h>

#if OCAML_MULTICORE

#include <caml/domain.h>
static_assert(Max_domains <= Num_domains,
              "OCaml is configured for a maximum number of domains greater than"
              " Boxroot.");

#endif

pool * boxroot_alloc_uninitialised_pool(size_t size)
{
  void *p = NULL;
  // TODO: portability?
  // Win32: p = _aligned_malloc(size, alignment);
  int err = posix_memalign(&p, size, size);
  assert(err != EINVAL);
  if (err == ENOMEM) return NULL;
  assert(p != NULL);
  return p;
}

void boxroot_free_pool(pool *p) {
    // Win32: _aligned_free(p);
    free(p);
}

#if BOXROOT_USE_MUTEX

int boxroot_initialize_mutex(pthread_mutex_t *mutex)
{
  int err;
  while (EAGAIN == (err = pthread_mutex_init(mutex, NULL))) {};
  return err == 0;
}

void boxroot_mutex_lock(pthread_mutex_t *mutex)
{
  pthread_mutex_lock(mutex);
}

void boxroot_mutex_unlock(pthread_mutex_t *mutex)
{
  pthread_mutex_unlock(mutex);
}

#else

int boxroot_initialize_mutex(int *m) { (void)m; return 1; }
void boxroot_mutex_lock(int *m) { (void)m; }
void boxroot_mutex_unlock(int *m) { (void)m; }

#endif
