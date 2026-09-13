#ifndef TOY_PLATFORM_COMPAT_H
#define TOY_PLATFORM_COMPAT_H

/*
 * C library calls the toys use that the Windows CRT lacks. On Linux every
 * name here is plain libc.
 */

#include <stdlib.h>

/* random() returns 31 bits on every platform. RAND_MAX belongs to rand()
   and is only 15 bits on Windows. */
#define POINGO_RANDOM_MAX 0x7fffffffL

#ifdef _WIN32
#include <errno.h>
#include <malloc.h>

/* Aligned blocks need _aligned_free() on Windows, so every block from
   posix_memalign() is released with poingo_aligned_free(). */
static inline int posix_memalign(void **out, size_t alignment, size_t size)
{
    *out = _aligned_malloc(size, alignment);
    return *out ? 0 : ENOMEM;
}

#define poingo_aligned_free _aligned_free

/* xorshift64*: good enough for palettes, shuffles and spawn points. */
static unsigned long long poingo_random_state = 0x853c49e6748fea9bULL;

static inline void srandom(unsigned int seed)
{
    poingo_random_state = (seed + 1ULL) * 0x9e3779b97f4a7c15ULL;
}

static inline long random(void)
{
    unsigned long long x = poingo_random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    poingo_random_state = x;
    return (long)((x * 0x2545f4914f6cdd1dULL) >> 33);
}

/* glibc hands freed heap back to the system on request; the Windows heap
   decides that for itself. */
static inline int malloc_trim(size_t pad)
{
    (void)pad;
    return 0;
}

#define POINGO_NULL_DEVICE "NUL"
#else
#define poingo_aligned_free free
#define POINGO_NULL_DEVICE "/dev/null"
#endif

#endif
