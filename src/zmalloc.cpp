#include <cstdlib>
#include <atomic>
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

std::atomic<size_t> Zmalloc::used_memory(0);

void *Zmalloc::zcalloc(size_t size)
{
    void *ptr = calloc(1, size + PREFIX_SIZE);
    if (!ptr)
        zmalloc_default_oom(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    return (char *)ptr + PREFIX_SIZE;
#endif
}

void *Zmalloc::zmalloc(size_t size)
{
    void *ptr = malloc(size + PREFIX_SIZE);

    if (!ptr)
        zmalloc_default_oom(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    return (char *)ptr + PREFIX_SIZE;
#endif
}

void Zmalloc::zfree(void *ptr)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL)
        return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

void Zmalloc::zmalloc_default_oom(const size_t size)
{
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
            size);
    fflush(stderr);
    abort();
}

inline void Zmalloc::update_zmalloc_stat_alloc(const size_t n)
{
    size_t _n = n;
    if (n & (sizeof(long) - 1))
        _n += sizeof(long) - (n & (sizeof(long) - 1));
    used_memory += _n;
}

inline void Zmalloc::update_zmalloc_stat_free(const size_t n)
{
    size_t _n = n;
    if (n & (sizeof(long) - 1))
        _n += sizeof(long) - (_n & (sizeof(long) - 1));
    used_memory -= _n;
}