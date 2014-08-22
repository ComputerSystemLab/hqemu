/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __UTILS_H
#define __UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "list.h"

#ifndef __PRI64_PREFIX
#  if __WORDSIZE == 64
#    define __PRI64_PREFIX          "l"
#  else
#    define __PRI64_PREFIX          "ll"
#  endif
#endif

#define PRId64      __PRI64_PREFIX "d"
#define PRIu64      __PRI64_PREFIX "u"

#if defined(__i386__) || defined(__x86_64__)
#define LOCK_FREE
#endif

/*
 * Atomic and Lock Utilities
 */
#if defined(__arm__)
#if defined(__ARM_ARCH_7__) ||  \
    defined(__ARM_ARCH_7A__) || \
    defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_7M__) || \
    defined(__ARM_ARCH_7R__)
#define USE_ARMV7_INSTRUCTIONS
#endif

#if defined(USE_ARMV7_INSTRUCTIONS) || \
    defined(__ARM_ARCH_6J__) || \
    defined(__ARM_ARCH_6K__) || \
    defined(__ARM_ARCH_6T2__) || \
    defined(__ARM_ARCH_6Z__) || \
    defined(__ARM_ARCH_6ZK__)
#define USE_ARMV6_INSTRUCTIONS
#endif

/* Spinlock and Pthread mutex wrapper. */
#define LLVM_LOCK_INITIALIZER   PTHREAD_MUTEX_INITIALIZER
#define llvm_lock_t             pthread_mutex_t
#define llvm_lock_init(lock)    pthread_mutex_init(lock, NULL)
#define llvm_spin_lock(lock)    pthread_mutex_lock(lock)
#define llvm_spin_unlock(lock)  pthread_mutex_unlock(lock)
#else
#define LLVM_LOCK_INITIALIZER   0
#define llvm_lock_t             volatile int
#define llvm_lock_init(lock)    do { *lock = LLVM_LOCK_INITIALIZER; } while(0)
#define llvm_spin_lock(lock)        \
    do {                            \
        while (llvm_testandset(lock));   \
    } while(0)

#define llvm_spin_unlock(lock)      \
    do {                            \
        *(lock) = 0;                \
    } while(0)
#endif

/*
 * atomic inc/dec
 */
#if defined(__i386__) || defined(__x86_64__)
static inline void atomic_inc(volatile int *p)
{
    asm volatile("lock; incl %0"
            : "+m" (*p));
}

static inline int atomic_inc_return(volatile int *p)
{
    int i = 1;
    asm volatile("lock; xaddl %0, %1"
            : "+r"(i), "+m" (*p)
            :
            : "memory");
    return i + 1;
}

static inline int llvm_testandset(volatile int *p)
{
    int ret = 0;
    asm volatile("lock; cmpxchgl %2, %0"
            : "+m" (*p), "+a" (ret)
            : "r" (1)
            : "cc");
    return ret;
}

#elif defined(_ARCH_PPC)
#define __stringify_in_c(...) #__VA_ARGS__
#define stringify_in_c(...)   __stringify_in_c(__VA_ARGS__) " "

#if defined(TCG_TARGET_PPC)
#    define LWSYNC sync
#elif defined(TCG_TARGET_PPC64)
#    define LWSYNC lwsync
#endif
#define PPC_ATOMIC_ENTRY_BARRIER "\n" stringify_in_c(LWSYNC) "\n"
#define PPC_ATOMIC_EXIT_BARRIER  "\n" stringify_in_c(sync) "\n"

static inline void atomic_inc(volatile int *p)
{
    int ret;

    asm volatile(
    "1:     lwarx   %0,0,%2         # atomic_inc\n\
            addic   %0,%0,1\n"
    "       stwcx.  %0,0,%2 \n\
            bne-    1b"
            : "=&r" (ret), "+m" (*p)
            : "r" (p)
            : "cc", "xer");
}

static inline int atomic_inc_return(volatile int *p)
{
    int ret;
    
    asm volatile(
            PPC_ATOMIC_ENTRY_BARRIER
    "1:     lwarx   %0,0,%1         # atomic_inc_return\n\
            addic   %0,%0,1\n"
    "       stwcx.  %0,0,%1 \n\
            bne-    1b"
            PPC_ATOMIC_EXIT_BARRIER
            : "=&r" (ret)
            : "r" (p)
            : "cc", "xer", "memory");

    return ret;
}

static inline int llvm_testandset(volatile int *p)
{
    int ret = 0;
    asm volatile(
            "      lwarx %0,0,%1\n"
            "      xor. %0,%3,%0\n"
            "      bne $+12\n"
            "      stwcx. %2,0,%1\n"
            "      bne- $-16\n"
            : "=&r" (ret)
            : "r" (p), "r" (1), "r" (0)
            : "cr0", "memory");
    return ret;
}

#elif defined(__arm__)

#ifdef USE_ARMV6_INSTRUCTIONS
#define raw_local_irq_save(x)                                   \
        ({                                                      \
        __asm__ __volatile__(                                   \
        "mrs    %0, cpsr                @ local_irq_save\n"     \
        "cpsid  i"                                              \
        : "=r" (x) : : "memory", "cc");                         \
        })
#else
#define raw_local_irq_save(x)                                   \
        ({                                                      \
                unsigned long temp;                             \
                (void) (&temp == &x);                           \
        __asm__ __volatile__(                                   \
        "mrs    %0, cpsr                @ local_irq_save\n"     \
"       orr     %1, %0, #128\n"                                 \
"       msr     cpsr_c, %1"                                     \
        : "=r" (x), "=r" (temp)                                 \
        :                                                       \
        : "memory", "cc");                                      \
        })
#endif

#define raw_local_irq_restore(x)                                \
	__asm__ __volatile(                                     \
	"msr    cpsr_c, %0              @ local_irq_restore\n"  \
	:                                                       \
	: "r" (x)                                               \
	: "memory", "cc")

static inline int llvm_testandset(volatile int *p)
{
    int ret = 0;
    asm volatile(
            "swp %0, %1, [%2]"
            : "=r"(ret)
            : "0"(1), "r"(p));
    return ret;
}

#ifdef USE_ARMV6_INSTRUCTIONS
static inline void atomic_inc(volatile int *p)
{                                               
    unsigned long tmp;                          
    int result;                                 
    asm volatile("@ atomic_add\n"
            "1:     ldrex   %0, [%2]\n"         
            "       add     %0, %0, #1\n"       
            "       strex   %1, %0, [%2]\n"     
            "       teq     %1, #0\n"           
            "       bne     1b"                 
            : "=&r" (result), "=&r" (tmp)       
            : "r" (p)                           
            : "cc");                            
}                                               

static inline int atomic_inc_return(volatile int *p)
{
    unsigned long tmp;
    int result;

    barrier();
    asm volatile("@ atomic_add_return\n"
            "1:     ldrex   %0, [%3]\n"
            "       add     %0, %0, #1\n"
            "       strex   %1, %0, [%3]\n"
            "       teq     %1, #0\n"
            "       bne     1b"
            : "=&r" (result), "=&r" (tmp), "+Qo" (p)
            : "r" (p)
            : "cc");
    barrier();
    
    return result;
}
#else

static inline void atomic_inc(volatile int *p)
{
    unsigned long flags;
    raw_local_irq_save(flags);
    *p += 1;
    raw_local_irq_restore(flags);
}

static inline int atomic_inc_return(volatile int *p)
{
    int ret;
    unsigned long flags;
    raw_local_irq_save(flags);
    ret = *p + 1;
    *p = ret;
    raw_local_irq_restore(flags);
    return ret;
}
#endif

/* FIXME: add atomic inc/dec byte, word and quadword. */

#else
#error "unsupported processor type"
#endif


/*
 * Queue
 */
#ifdef LOCK_FREE
/* Look-free MS-queue */
struct pointer_t
{
    struct node_t *ptr;
    unsigned long int count;
};

struct node_t
{
    struct pointer_t next;
    void *value;
};

struct queue_t
{
    struct pointer_t head;
    struct pointer_t tail;
};

struct queue_t *create_queue(void);
void reset_queue(struct queue_t *queue);
void enqueue(struct queue_t *queue, void *data);
void *dequeue(struct queue_t *queue);

#else
#define USE_FIFO_QUEUE
struct node_t 
{
    struct node_t *next;
    void *value;
};

struct queue_t 
{
    struct node_t *head;
    struct node_t *tail;
    llvm_lock_t lock;
};

struct queue_t *create_queue(void);
void reset_queue(struct queue_t *queue);
void enqueue(struct queue_t *queue, void *data);
void *dequeue(struct queue_t *queue);
#endif

/* Wrapper to mmap. */
void *llvm_mmap(int size);

/* Wrapper to malloc. */
void *llvm_malloc(int size);

/* Wrapper to free. */
void llvm_free(void *ptr);

/* Get a globally unique ID. */
uint64_t uuid_gen(void);

/* Get the thread ID. */
pid_t gettid(void);

/* Get the processor frequency. */
unsigned long get_cpu_speed(void);

/* Patch a direct jump from patch_addr to addr. */
void patch_jmp(volatile unsigned long patch_addr, volatile unsigned long addr);

/* Memory fence. */
void memory_barrier(void);

#ifdef __cplusplus
}
#endif
#endif
/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

