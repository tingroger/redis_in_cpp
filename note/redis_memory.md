
# zmalloc与Redis的内存管理

大三那年，我面试过阿里提前批的实习生内推。二面的时候面试官问到自定义内存管理函数、以及如何处理8字节对齐等问题。
当时语塞，挂掉了面试。在那过后的一个月，因缘际会我开始阅读Redis源码，当读到zmalloc.c时，哑然一笑，这可能正是面试官想要的答案，但逝去的面试再也回不来。当时年少，才疏学浅。而本文的原文初版也是写于那年（2015年），所以源码基于Redis 3.x。

## 1 字长与字节对齐

CPU一次性能读取数据的二进制位数称为字长，也就是我们通常所说的32位系统（字长4个字节）、64位系统（字长8个字节）的由来。
所谓的8字节对齐，就是指变量的起始地址是8的倍数。比如程序运行时（CPU）在读取long型数据的时候，只需要一个总线周期，时间更短，如果不是8字节对齐的则需要两个总线周期才能读完数据。

本文中我提到的8字节对齐是针对64位系统而言的，如果是32位系统那么就是4字节对齐。
实际上Redis源码中的字节对齐是软编码，而非硬编码。里面多用sizeof(long)或sizeof(size_t)来表示。
size_t（gcc中其值为long unsigned int）和long的长度是一样的，long的长度就是计算机的字长。
这样在未来的系统中如果字长（long的大小）不是8个字节了，该段代码依然能保证相应代码可用。

## 2 zmalloc

### 2.1 辅助函数

malloc()
zmalloc_oom_handler【函数指针】
zmalloc_default_oom()【被上面的函数指针所指向】
update_zmalloc_stat_alloc()【函数宏】
update_zmalloc_stat_add()【函数宏】
zmalloc()和malloc()有相同的API（相同参数、返回值）。

### 2.2 zmalloc()源码

```c
void *zmalloc(size_t size) {
    void *ptr = malloc(size+PREFIX_SIZE);
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}
```

参数size是我们需要分配的内存大小。
调用malloc实际分配的大小是 `size+PREFIX_SIZE`

`PREFIX_SIZE`是一个条件编译的宏，不同的平台有不同的结果，可能是：0， sizeof(long long), sizeof(size_t)
当`PREFIX_SIZE = sizeof(size_t)`，多分配一个字长(8个字节)的空间（后面代码可以看到多分配8个字节的目的是用于储存size的值）

如果ptr指针为NULL（内存分配失败），调用 `zmalloc_oom_handler(size)`
该函数实际上是一个函数指针指向函数 `zmalloc_default_oom`，其主要功能就是打印错误信息并终止程序。

```c
// oom即out of memory
static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}
```

接下来是宏的条件编译，我们聚焦在#else的部分。

```c
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
```

1. 在已分配内存的第一个字长（前8个字节）处写入需要分配的字节大小（size）。
1. 调用 `update_zmalloc_stat_alloc()`【宏函数】，更新全局变量 `used_memory`（已分配内存的大小）的值
1. 返回 `(char *) ptr+PREFIX_SIZE` 就是将已分配内存的起始地址向右偏移 `PREFIX_SIZE` （即8个字节），此时得到的新指针指向的内存空间的大小就等于size了

接下来，分析一下update_zmalloc_stat_alloc的源码

### 2.3 update_zmalloc_stat_alloc

```c
#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    atomicIncr(used_memory,__n); \
} while(0)
```

值得注意点是，这个函数宏使用了do{...}while(0)循环来定义，这也是定义函数宏的时候的一个小技巧。

因为 sizeof(long) = 8 【64位系统中】，所以上面的第一个if语句，可以等价于以下代码：
`if(_n&7) _n += 8 - (_n&7);`
增加一点可读性，等价于：
`if (_n&7 != 0) { _n = _n + 8 - (_n&7); }`

这段代码就是判断分配的内存空间的大小是不是8的倍数(_n&7==0)。
如果内存大小不是8的倍数，就加上相应的偏移量使之变成8的倍数。
_n&7 等价于 _n%8，不过位操作的效率显然更高

malloc()本身能够保证所分配的内存是8字节对齐的：
如果你要分配的内存不是8的倍数，那么malloc就会多分配一点，来凑成8的倍数。
所以update_zmalloc_stat_alloc函数真正要实现的功能并不是进行8字节对齐（malloc已经保证了），它的真正目的是使变量used_memory精确的维护实际已分配内存的大小。

`atomicIncr` 表示原子操作，保证线程安全

```c
#define atomicDecr(var,count) __atomic_sub_fetch(&var,(count),__ATOMIC_RELAXED)
```

## 3 zfree

zfree()和free()有相同的API，它负责清除zmalloc()分配的空间。

### 3.1 辅助函数

free()
update_zmalloc_free()【宏函数】
update_zmalloc_sub()【宏函数】
zmalloc_size()

### 3.2 zfree()源码

```c
void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif
    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}
```

重点关注#else后面的代码

`realptr = (char *)ptr - PREFIX_SIZE;`
表示的是ptr指针向前偏移8个字节的长度，即回退到最初malloc返回的地址，这里称为realptr。

`oldsize = *((size_t*)realptr);`
先进行类型转换再取指针所指向的值。
通过zmalloc()函数的分析，可知这里存储着我们最初需要分配的内存大小（zmalloc中的size），这里赋值给oldsize

`update_zmalloc_stat_free(oldsize+PREFIX_SIZE);`
update_zmalloc_stat_free() 也是一个函数宏，和zmalloc中update_zmalloc_stat_alloc()大致相同，唯一不同之处是前者在给变量used_memory减去分配的空间，而后者是加上该空间大小。
最后free(realptr)，清除空间

### 3.3 update_zmalloc_stat_free

```c
#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)
```

其中的函数update_zmalloc_sub与zmalloc()中的update_zmalloc_add相对应，但功能相反，提供线程安全地used_memory减法操作。

```c
#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)
```

## 4 zcalloc

zcalloc()的实现基于calloc()，但是两者API不同。看一下对比：

```c
void *calloc(size_t nmemb, size_t size);
void *zcalloc(size_t size);
```

calloc()的功能是也是分配内存空间，与malloc()的不同之处有两点：

1. 它分配的空间大小是 size * nmemb。比如：
`calloc(10, sizoef(char)); // 分配10个字节`
2. calloc()会对分配的空间做初始化工作（初始化为0），而malloc()不会

### 4.1 辅助函数

calloc()
update_zmalloc_stat_alloc()【宏函数】
update_zmalloc_stat_add()【宏函数】

### 4.2 zcalloc()源码

```c
void *zcalloc(size_t size) {
    void *ptr = calloc(1, size+PREFIX_SIZE);
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}
```

zcalloc()中没有calloc()的第一个函数nmemb。
因为它每次调用calloc()，其第一个参数都是1。
也就是说zcalloc()功能是每次分配 size+PREFIX_SIZE 的空间，并初始化。

其余代码的分析和zmalloc()相同，也就是说：
zcalloc()和zmalloc()具有相同的编程接口，实现功能基本相同，唯一不同之处是zcalloc()会做初始化工作，而zmalloc()不会。

## 5 zrealloc

zrealloc()和realloc()具有相同的API：

```c
void *realloc (void *ptr, size_t size);
void *zrealloc(void *ptr, size_t size);
```

realloc()要完成的功能是给首地址ptr的内存空间，重新分配大小。
如果失败了，则在其它位置新建一块大小为size字节的空间，将原先的数据复制到新的内存空间，并返回这段内存首地址【原内存会被系统自然释放】。
zrealloc()要完成的功能也类似。

### 5.1 辅助函数

zmalloc()
zmalloc_size()
realloc()
zmalloc_oom_handler【函数指针】
update_zmalloc_stat_free()【函数宏】
update_zmalloc_stat_alloc()【函数宏】

### 5.2 zrealloc()源码

```c
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;
    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom_handler(size);
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom_handler(size);
    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}
```

经过前面关于zmalloc()和zfree()的源码解读，相信您一定能够很轻松地读懂zrealloc()的源码，这里我就不赘述了。

## 6 zstrdup

从这个函数名中，很容易发现它是string duplicate的缩写，即字符串复制。
它的代码比较简单。先看一下函数声明：
`char *zstrdup(const char *s);`

功能描述：复制字符串s的内容，到新的内存空间，构造新的字符串【堆区】。并将这段新的字符串地址返回。

### 6.1 zstrdup源码

```c
char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;
    char *p = zmalloc(l);
    memcpy(p,s,l);
    return p;
}
```

首先，先获得字符串s的长度，新闻strlen()函数是不统计'\0'的，所以最后要加1。
然后调用zmalloc()来分配足够的空间，首地址为p。
调用memcpy来完成复制。
然后返回p。

### 6.2 memcpy

memcpy这是标准C，ANSI C 中用于内存复制的函数，在头文件<string.h>中。函数声明如下：
`void *memcpy(void *dest, const void *src, size_t n);`
dest即目的地址，src是源地址。n是要复制的字节数。

## 7 其他函数

### 7.1 zmalloc_enable_thread_safeness

void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}
前文有述，zmalloc_thread_safe是一个标记，它是全局静态变量（static int）。表示是否需要保证线程安全。

### 7.2 zmalloc_used_memory

```c
size_t zmalloc_used_memory(void) {
    size_t um; 
    if (zmalloc_thread_safe) {
#if defined(__ATOMIC_RELAXED) || defined(HAVE_ATOMIC)
        um = update_zmalloc_stat_add(0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    }
    else {
        um = used_memory;
    }
    return um;
}
```

该函数要完成的操作就是返回变量used_memory（已用内存）的值，所以它的功能是查询系统当前为Redis分配的内存大小。本身代码量不大，但是涉及到了线程安全模式下的查询操作。实现线程同步用到了互斥锁（mutex）。关于互斥锁的内容在前文中已经简要介绍过了。总之要记住的是加锁（pthread_mutex_lock）和解锁(pthread_mutex_unlock)。在加了互斥锁之后，就能保证之后的代码同时只能被一个线程所执行。

### 7.3 zmalloc_set_oom_handler

void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}
该函数的功能是给zmalloc_oom_handler赋值。zmalloc_oom_handler是一个函数指针，表示在内存不足（out of memory，缩写oom）的时候所采取的操作，它的类型是void (*) (size_t)。所以zmalloc_set_oom_handler函数的参数也是void (*) (size_t)类型，调用的时候就是传递一个该类型的函数名就可以了。

不过zmalloc_oom_handler在声明的时候初始化了默认值——zmalloc_default_oom()。

### 7.4 zmalloc_size

```c
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif
```

这段代码和zfree()函数中的内容颇为相似。这里再概括一下，zmalloc(size)在分配内存的时候会多申请sizeof(size_t)个字节大小的内存【64位系统中是8字节】，即调用malloc(size+8)，所以一共申请分配size+8个字节，zmalloc(size)会在已分配内存的首地址开始的8字节中存储size的值，实际上因为内存对齐，malloc(size+8)分配的内存可能会比size+8要多一些，目的是凑成8的倍数，所以实际分配的内存大小是size+8+X【(size+8+X)%8==0 (0<=X<=7)】。然后内存指针会向右偏移8个字节的长度。zfree()就是zmalloc()的一个逆操作，而zmalloc_size()的目的就是计算出size+8+X的总大小。

这个函数是一个条件编译的函数，通过阅读zmalloc.h文件，我们可以得知zmalloc_size()依据不同的平台，具有不同的宏定义，因为在某些平台上提供查询已分配内存实际大小的函数，可以直接#define zmalloc_size(p)：

tc_malloc_size(p) 【tcmalloc】
je_malloc_usable_size(p)【jemalloc】
malloc_size(p) 【Mac系统】
当这三个平台都不存在的时候，就自定义，也就是上面的源码。

### 7.5 zmalloc_get_rss（有意思）

获取RSS的大小，是指的Resident Set Size，表示当前进程实际所驻留在内存中的空间大小，即不包括被交换（swap）出去的空间。

了解一点操作系统的知识，就会知道我们所申请的内存空间不会全部常驻内存，系统会把其中一部分暂时不用的部分从内存中置换到swap区（装Linux系统的时候我们都知道有一个交换空间）。如果你使用过top命令，就知道进程状态有两列指标：RSS和SWAP

该函数大致的操作就是在当前进程的 /proc/\<pid>/stat （\<pid>指代当前进程实际id）文件中进行检索。该文件的第24个字段是RSS的信息，它的单位是pages（内存页的数目）

```c
size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x; 
    snprintf(filename,256,"/proc/%d/stat",getpid());
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';
    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
}
```

函数开头：
    int page = sysconf(_SC_PAGESIZE);

通过调用库函数sysconf()【大家可以man sysconf查看详细内容】来查询内存页的大小。

接下来：
    snprintf(filename,256,"/proc/%d/stat",getpid());

getpid()就是获得当前进程的id，所以这个snprintf()的功能就是将当前进程所对应的stat文件的绝对路径名保存到字符数组filename中。【不得不称赞一下类Unix系统中“万物皆文件”的概念】

```c
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
```

以只读模式打开 /proc/\<pid>/stat 文件。然后从中读入4096个字符到字符数组buf中。如果失败就关闭文件描述符fd，并退出（个人感觉因错误退出，还是返回-1比较好吧）。

```c
    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
```

RSS在stat文件中的第24个字段位置，所以就是在第23个空格的后面。观察while循环，循环体中用到了字符串函数strchr()，这个函数在字符串p中查询空格字符，如果找到就把空格所在位置的字符指针返回并赋值给p，找不到会返回NULL指针。p++原因是因为，p当前指向的是空格，在执行自增操作之后就指向下一个字段的首地址了。如此循环23次，最终p就指向第24个字段的首地址了。

```c
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';
```

因为循环结束也可能是p变成了空指针，所以判断一下p是不是空指针。接下来的的几部操作很好理解，就是将第24个字段之后的空格设置为'\0'，这样p就指向一个一般的C风格字符串了。

```c
    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
```

这段代码又用到了一个字符串函数——strtoll()：顾名思义：string to long long。它有三个参数，前面两个参数表示要转换的字符串的起始和终止位置（字符指针类型），NULL和'\0'是等价的。最后一个参数表示的是“进制”，这里就是10进制了。

后面用rss和page相乘并返回，因为rss获得的实际上是内存页的页数，page保存的是每个内存页的大小（单位字节），相乘之后就表示RSS实际的内存大小了。

### 7.6 zmalloc_get_fragmentation_ratio与内存碎片

```c
/* Fragmentation = RSS / allocated-bytes */
float zmalloc_get_fragmentation_ratio(size_t rss) {
    return (float)rss/zmalloc_used_memory();
}
```

这个函数是查询内存碎片率（fragmentation ratio），即RSS与所分配总内存空间的比值。需要用zmalloc_get_rss()获得RSS的值，再以RSS的值作为参数传递进来。

内存碎片分为：内部碎片和外部碎片

内部碎片：是已经被分配出去（能明确指出属于哪个进程）却不能被利用的内存空间，直到进程释放掉，才能被系统利用；
外部碎片：是还没有被分配出去（不属于任何进程），但由于太小了无法分配给申请内存空间的新进程的内存空闲区域。
zmalloc_get_fragmentation_ratio()要获得的显然是内部碎片率。

### 7.7 zmalloc_get_smap_bytes_by_field

```c
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field) {
    char line[1024];
    size_t bytes = 0;
    FILE *fp = fopen("/proc/self/smaps","r");
    int flen = strlen(field); 
    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,field,flen) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line+flen,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
size_t zmalloc_get_smap_bytes_by_field(char *field) {
    ((void) field);
    return 0;
}
#endif
```

一个条件编译的函数，我们当然要聚焦到#if defined的部分。

   FILE *fp = fopen("/proc/self/smaps","r");
用标准C的fopen()以只读方式打开/proc/self/smaps文件。简单介绍一下该文件，前面我们已经说过/proc目录下有许多以进程id命名的目录，里面保存着每个进程的状态信息，而/proc/self目录的内容和它们是一样的，self/ 表示的是当前进程的状态目录。而smaps文件中记录着该进程的详细映像信息，该文件内部由多个结构相同的块组成，看一下其中某一块的内容：

00400000-004ef000 r-xp 00000000 08:08 1305603 /bin/bash
Size: 956 kB
Rss: 728 kB
Pss: 364 kB
Shared_Clean: 728 kB
Shared_Dirty: 0 kB
Private_Clean: 0 kB
Private_Dirty: 0 kB
Referenced: 728 kB
Anonymous: 0 kB
AnonHugePages: 0 kB
Swap: 0 kB
KernelPageSize: 4 kB
MMUPageSize: 4 kB
Locked: 0 kB
VmFlags: rd ex mr mw me dw sd
除去开头和结尾两行，其他的每一行都有一个字段和该字段的值（单位kb）组成【每个字段的具体含义，各位自行百度】。注意这只是smaps文件的一小部分。

```c
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,field,flen) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line+flen,NULL,10) * 1024;
            }
        }
    }
```

利用fgets()逐行读取/proc/self/smaps文件内容

然后strchr()将p指针定义到字符k的位置

然后将p置为'\0'，截断形成普通的C风格字符串

line指向的该行的首字符，line+flen（要查询的字段的长度）所指向的位置就是字段名后面的空格处了，不必清除空格，strtol()无视空格可以将字符串转换成int类型

strol()转换的结果再乘以1024，这是因为smaps里面的大小是kB表示的，我们要返回的是B（字节byte）表示

实际上/proc/self目录是一个符号链接，指向/proc/目录下以当前id命名的目录。我们可以进入该目录下敲几个命令测试一下。

root@X:/proc/self# pwd -P
/proc/4152
root@X:/proc/self# ps aux|grep [4]152
root      4152  0.0  0.0  25444  2176 pts/0    S    09:06   0:00 bash

### 7.8 zmalloc_get_private_dirty

size_t zmalloc_get_private_dirty(void) {
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:");
}
源代码很简单，该函数的本质就是在调用

zmalloc_get_smap_bytes_by_field("Private_Dirty:");

其完成的操作就是扫描 /proc/self/smaps文件，统计其中所有 Private_Dirty字段的和。那么这个Private_Dirty是个什么意思呢？

大家继续观察一下，我在上面贴出的 /proc/self/smaps文件的结构，它有很多结构相同的部分组成。其中有几个字段有如下的关系：

Rss=Shared_Clean+Shared_Dirty+Private_Clean+Private_Dirty
其中：

Shared_Clean:多进程共享的内存，且其内容未被任意进程修改
Shared_Dirty:多进程共享的内存，但其内容被某个进程修改
Private_Clean:某个进程独享的内存，且其内容没有修改
Private_Dirty:某个进程独享的内存，但其内容被该进程修改
主要分为Shared和Private两大类，这里所谓Shared，一般指的就是Unix系统中的共享库（.so文件）的使用，它只有在程序运行时才被装入内存。这时共享库中的代码和数据可能会被多个进程所调用，于是就会产生干净（Clean）与脏（Dirty）的区别了。此外该处所说的共享的内存除了包括共享库以外，还包括System V的IPC机制之一的共享内存段（shared memory）

关于smaps文件，其实也很有了解的必要，大家可以自己搜索一下。
