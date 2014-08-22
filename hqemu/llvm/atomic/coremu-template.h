/* The following code may be included multiple times in a single file. */

#if DATA_BITS == 64
#  define DATA_TYPE uint64_t
#  define SUFFIX q
#elif DATA_BITS == 32
#  define DATA_TYPE uint32_t
#  define SUFFIX l
#elif DATA_BITS == 16
#  define DATA_TYPE uint16_t
#  define SUFFIX w
#elif DATA_BITS == 8
#  define DATA_TYPE uint8_t
#  define SUFFIX b
#else
#error unsupported data size
#endif

static __inline__ void coremu_glue(atomic_inc, SUFFIX)(DATA_TYPE *p) {
    asm volatile(
        LOCK_PREFIX "inc"coremu_str(SUFFIX)" %0"
        : "+m"(*p)
        :
        : "cc");
}

static __inline__ void coremu_glue(atomic_dec, SUFFIX)(DATA_TYPE *p) {
    asm volatile(
        LOCK_PREFIX "dec"coremu_str(SUFFIX)" %0"
        : "+m"(*p)
        :
        : "cc");
}

static __inline__ void coremu_glue(atomic_add, SUFFIX)(DATA_TYPE* addr,
        DATA_TYPE val) {
    asm volatile(
        LOCK_PREFIX "add"coremu_str(SUFFIX)" %1, %0"
        : "+m"(*addr)
        : "a"(val)
        : "cc");
}

/* swap the value VAL and *p.
 * Return the value swapped out from memory. */
static inline DATA_TYPE coremu_glue(atomic_exchange, SUFFIX)(
        DATA_TYPE *p, DATA_TYPE val)
{
    DATA_TYPE out;
    __asm __volatile(
            "lock; xchg"coremu_str(SUFFIX)" %1,%2 \n\t"
            : "=a" (out), "+m" (*p)
            : "a" (val)
            );
    return out;
}
/* Return previous value in addr. So if the return value is the same as oldval,
 * swap occured. */
static __inline__ DATA_TYPE coremu_glue(atomic_compare_exchange, SUFFIX)(DATA_TYPE *addr,
        DATA_TYPE oldval, DATA_TYPE newval) {
    asm volatile(
        LOCK_PREFIX "cmpxchg"coremu_str(SUFFIX)" %2, %1"
        : "+a"(oldval), "+m"(*addr)
        : "q"(newval)
        : "cc");

    return oldval;
}

static __inline__ void coremu_glue(atomic_and, SUFFIX)(DATA_TYPE *addr,
        DATA_TYPE mask) {
    asm volatile(
        LOCK_PREFIX "and"coremu_str(SUFFIX)" %1, %0"
        : "+m"(*addr)
        : "r"(mask)
        : "cc");
}

static __inline__ void coremu_glue(atomic_or, SUFFIX)(DATA_TYPE *addr,
        DATA_TYPE mask) {
    asm volatile(
        LOCK_PREFIX "or"coremu_str(SUFFIX)" %1, %0"
        : "+m"(*addr)
        : "r"(mask)
        : "cc");
}

static __inline__ DATA_TYPE coremu_glue(atomic_xadd, SUFFIX)(
        DATA_TYPE* addr, DATA_TYPE val) {
    asm volatile(
        LOCK_PREFIX "xadd"coremu_str(SUFFIX)" %0, %1"
        : "+a"(val), "+m"(*addr)
        :
        : "cc");

    return val;
}

#undef DATA_BITS
#undef DATA_TYPE
#undef SUFFIX
