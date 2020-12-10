#ifndef _ZMALLOC_H_
#define _ZMALLOC_H_

#if defined(USE_TCMALLOC)
#define ZMALLOC_LIB ("tcmalloc-" __xstr(TC_VERSION_MAJOR) "." __xstr(TC_VERSION_MINOR))
#include <google/tcmalloc.h>
#if (TC_VERSION_MAJOR == 1 && TC_VERSION_MINOR >= 6) || (TC_VERSION_MAJOR > 1)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) tc_malloc_size(p)
#else
#error "Newer version of tcmalloc required"
#endif

#elif defined(USE_JEMALLOC)
#define ZMALLOC_LIB ("jemalloc-" __xstr(JEMALLOC_VERSION_MAJOR) "." __xstr(JEMALLOC_VERSION_MINOR) "." __xstr(JEMALLOC_VERSION_BUGFIX))
#include <jemalloc/jemalloc.h>
#if (JEMALLOC_VERSION_MAJOR == 2 && JEMALLOC_VERSION_MINOR >= 1) || (JEMALLOC_VERSION_MAJOR > 2)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) je_malloc_usable_size(p)
#else
#error "Newer version of jemalloc required"
#endif

#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_size(p)
#endif

#ifndef ZMALLOC_LIB
#define ZMALLOC_LIB "libc"
#ifdef __GLIBC__
#include <malloc.h>
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_usable_size(p)
#endif
#endif

class Zmalloc
{
public:
    static void *zmalloc(size_t size);
    static void *zcalloc(size_t size);
    static void zfree(void *ptr);

private:
    static std::atomic<size_t> used_memory;                //已用内存大小
    static void zmalloc_default_oom(const size_t size);    //内存分配失败处理函数
    static void update_zmalloc_stat_alloc(const size_t n); //更新实际分配空间的大小(考虑内存对齐)
    static void update_zmalloc_stat_free(const size_t n);  //更新实际释放空间的大小(考虑内存对齐)
};

#endif  //_ZMALLOC_H_