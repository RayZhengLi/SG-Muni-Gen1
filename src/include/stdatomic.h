/* auto-generated fallback stdatomic.h for uClibc/gcc-4.x */
#pragma once
/* 这里不依赖系统 <stdatomic.h>，直接提供最小原子 API */
typedef int atomic_int;
static inline int  atomic_load(volatile atomic_int *v){ __sync_synchronize(); int x=*v; __sync_synchronize(); return x; }
static inline void atomic_store(volatile atomic_int *v,int x){ __sync_synchronize(); *v=x; __sync_synchronize(); }
static inline int  atomic_fetch_add(volatile atomic_int *v,int x){ return __sync_fetch_and_add(v,x); }
static inline int  atomic_fetch_sub(volatile atomic_int *v,int x){ return __sync_fetch_and_sub(v,x); }
