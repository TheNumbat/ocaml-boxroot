

#include <caml/version.h>

#if OCAML_VERSION < 50000

#include <stdint.h>

#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
#define POSIX_CLOCK
#include <time.h>
#else
#include <sys/time.h>
#endif

int64_t caml_time_counter(void)
{
#if defined(POSIX_CLOCK)
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return
    (int64_t)t.tv_sec  * (int64_t)1000000000 +
    (int64_t)t.tv_nsec;
#else
  struct timeval t;
  gettimeofday(&t, 0);
  return
    (int64_t)t.tv_sec  * (int64_t)1000000000 +
    (int64_t)t.tv_usec * (int64_t)1000;
#endif
}

#endif
