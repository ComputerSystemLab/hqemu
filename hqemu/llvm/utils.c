/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <math.h>
#include "utils.h"
#include "exec-all.h"
#include "tcg-target.h"

/*
 * atomic compare and swap
 */
static inline char CAS(volatile uint32_t *m, uint32_t old, uint32_t new)
{
    char flag = 0;

#if defined(__i386__) || defined(__x86_64__)
    asm volatile("lock; cmpxchg %3,%0; setz %1;"
            : "=m" (*m), 
              "=q" (flag)
            : "a" (old), 
              "r" (new)
            : "memory", "cc");
#endif

    return flag;
}

#ifdef LOCK_FREE
/*  Lock-free FIFO queue algorithm of Michael and Scott (MS-queue).
 *  The code is based on the paper published in PODC'96:
 *      Maged M. Michael and Michael L. Scott, "Simple, Fast, and Practical
 *      Non-Blocking and Blocking Concurrent Queue Algorithms," Proc. 15th ACM
 *      Symp. on Principles of Distributed Computing, pages 267-275, 1996.
 */

static inline char CAS2(volatile struct pointer_t *ptr,
        struct pointer_t old,
        struct pointer_t new)
{
    char flag;

#if defined(__i386__)
    asm volatile("lock; cmpxchg8b %0; setz %1;"
            : "=m" (*ptr), 
              "=q" (flag)
            : "d" (old.count), 
              "a" (old.ptr), 
              "c" (new.count), 
              "b" (new.ptr)
            : "memory", "cc");
#elif defined(__x86_64__)
    asm volatile("lock; cmpxchg16b %0; setz %1;"
            : "=m" (*ptr), 
              "=q" (flag)
            : "d" (old.count), 
              "a" (old.ptr), 
              "c" (new.count), 
              "b" (new.ptr)
            : "memory", "cc");
#elif defined(__arm__)
#  ifdef USE_ARMV6_INSTRUCTIONS
    unsigned long oldval, res;
    
    asm volatile("@ atomic_cmpxchg\n"
            "ldrex  %1, [%3]\n"
            "mov    %0, #0\n"
            "teq    %1, %4\n"
            "strexeq %0, %5, [%3]\n"
            : "=&r" (res), "=&r" (oldval), "+Qo" (*ptr->ptr)
            : "r" (ptr->ptr), "Ir" (old.ptr), "r" (new.ptr)
            : "cc");

    flag = !res;
#  else
    unsigned long irq;
    struct node_t *ret;
    
    raw_local_irq_save(irq);
    ret = ptr->ptr;
    if (likely(ret == old.ptr))
    {
        ptr->ptr = new.ptr;
        flag = 1;
    }
    else
        flag = 0;
    raw_local_irq_restore(irq);
#  endif
#elif defined(_ARCH_PPC)
#else
#error "unsupported processor type"
#endif

    return flag;
}

static inline struct node_t *new_node(void *value)
{
    struct node_t *node;

    node = (struct node_t *)llvm_malloc(sizeof(struct node_t));
    node->next.ptr = NULL;
    node->value = value;

    return node;
}

struct queue_t *create_queue(void)
{
    struct queue_t *queue;
    struct node_t *dummy;

    queue = (struct queue_t *)llvm_malloc(sizeof(struct queue_t));

    dummy = new_node(NULL);
    queue->head.ptr = queue->tail.ptr = dummy;
    queue->head.count = queue->tail.count = 0;

    return queue;
}

void reset_queue(struct queue_t *queue)
{
    while (1)
    {
        void *data = dequeue(queue);
        if (data == NULL)
            break;
        llvm_free(data);
    }
}

void enqueue(struct queue_t *queue, void *data)
{
    struct node_t *node;
    struct pointer_t tail, next, insert;

    node = new_node(data);
    insert.ptr = node;

    for (;;)
    {
        tail = queue->tail;
        next = tail.ptr->next;
        
        /* If Tail is consistent (addresses and versions are not changed),
           continue to enqueue. */
        if (CAS2(&queue->tail, tail, queue->tail))
        {
            /* If Tail is pointing to the last node, continue to enqueue.
               Otherwise, try to advance Tail because it might be pointing
               to the second last node. */
            if (next.ptr == NULL)   /* Last node */
            {
                /* Try to insert node at the end of the linked list.
                   if it succeeds, exit the loop. */
                insert.count = next.count + 1;
                if (CAS2(&(tail.ptr->next), next, insert))
                    break;
            }
            else
            {
                next.count = tail.count + 1;
                CAS2(&queue->tail, tail, next);
            }
        }
    }
    
    /* Enqueue is done, try to swing Tail to the inserted node. */
    insert.count = tail.count + 1;
    CAS2(&queue->tail, tail, insert);
}

void *dequeue(struct queue_t *queue)
{
    struct pointer_t head, tail, next;
    void *data;

    for (;;)
    {
        head = queue->head;
        tail = queue->tail;
        next = head.ptr->next;
        
        /* If Head is consistent (addresses and versions are not changed),
           continue to dequeue. */
        if (CAS2(&queue->head, head, queue->head))
        {
            /* If Queue is empty, stop dequeueing. If Tail falling behind, 
               try to advance it. Otherwise, continue to dequeue. */
            if (head.ptr == tail.ptr)
            {
                if (next.ptr == NULL) /* Queue is empty */
                    return NULL;
                
                /* Tail is falling behand, try to advance it. */
                next.count = tail.count + 1;
                CAS2(&queue->tail, tail, next);
            }
            else
            {
                /* We must read value before CAS, otherwise another dequeue 
                   might free the next node. */
                data = next.ptr->value;
                next.count = head.count + 1;
                if (CAS2(&queue->head, head, next))
                    break;
            }
        }
    }
    
    /* Dequeue succeeded. It is safe to free the dummy node.
       Node pointed by Head becomes the new dummy node */
    llvm_free(head.ptr);
    
    return data;
}
#else
static inline struct node_t *new_node(void *value)
{
    struct node_t *node;

    node = (struct node_t *)llvm_malloc(sizeof(struct node_t));
    node->next = NULL;
    node->value = value;

    return node;
}

struct queue_t *create_queue(void)
{
    struct queue_t *queue;
    struct node_t *dummy;

    queue = (struct queue_t *)llvm_malloc(sizeof(struct queue_t));

    dummy = new_node(NULL);
    queue->head = queue->tail = dummy;
    llvm_lock_init(&queue->lock);

    return queue;
}

void reset_queue(struct queue_t *queue)
{
    struct node_t *node, *current, *dummy;

    llvm_spin_lock(&queue->lock);
    node = queue->head;
    while (node)
    {
        current = node;
        node = node->next;
        llvm_free(current);
    }

    dummy = new_node(NULL);
    queue->head = queue->tail = dummy;
    llvm_spin_unlock(&queue->lock);
}

void enqueue(struct queue_t *queue, void *data)
{
    struct node_t *node;

    node = new_node(data);

    llvm_spin_lock(&queue->lock);
#ifdef USE_FIFO_QUEUE
    queue->tail->next = node;
    queue->tail = node;
#else   /* LIFO queue */
    node->next = queue->head->next;
    queue->head->next = node;
#endif
    llvm_spin_unlock(&queue->lock);
}

void *dequeue(struct queue_t *queue)
{
    struct node_t *node, *new_head;
    void *data;

    llvm_spin_lock(&queue->lock);
    node = queue->head;
    new_head = node->next;
    if (new_head == NULL)
    {
        llvm_spin_unlock(&queue->lock);
        return NULL;
    }

    data = new_head->value;
    queue->head = new_head;
    llvm_spin_unlock(&queue->lock);

    llvm_free(node);
    return data;
}

#endif

/*
 * llvm_mmap()
 *   Wrapper to mmap.
 */
void *llvm_mmap(int size)
{
    void *m;
    int flags = MAP_ANONYMOUS|MAP_PRIVATE;
#if defined(__x86_64__)
    flags |= MAP_32BIT;
#endif
    
    if (size <= 0)
    {
        fprintf(stderr, "%s: invalid size %d\n", __func__, size);
        exit(0);
    }

    m = mmap(NULL, size, PROT_READ|PROT_WRITE, flags, -1, 0);
    if (m == NULL)
    {
        fprintf(stderr, "%s: not enough memory.\n", __func__);
        exit(0);
    }

    return m;
}

/*
 * llvm_malloc()
 *  Wrapper to malloc.
 */
void *llvm_malloc(int size)
{
    void *m;

    if (size <= 0)
    {
        fprintf(stderr, "%s: invalid size %d\n", __func__, size);
        exit(0);
    }

    m = malloc(size);
    if (m == NULL)
    {
        fprintf(stderr, "%s: not enough memory.\n", __func__);
        exit(0);
    }

    return m;
}

/*
 * llvm_free()
 *  Wrapper to free.
 */
void llvm_free(void *ptr)
{
    if (ptr == NULL)
    {
        fprintf(stderr, "%s: null pointer.\n", __func__);
        exit(0);
    }

    free(ptr);
}


/*
 * uuid_gen()
 *  Get a DBT-universe unique ID.
 */
uint64_t uuid_gen(void)
{
    static uint64_t uuid = 0;
    uint64_t id;
    
#if defined(__x86_64__)
    uint64_t i = 1;
    asm volatile("lock; xaddq %0, %1"
            : "+r" (i), "+m" (uuid)
            :
            : "memory");
    return i + 1;
#else
#if defined(__arm__) && !defined(USE_ARMV6_INSTRUCTIONS)
    unsigned long flags;
    raw_local_irq_save(flags);
    id = uuid++;
    raw_local_irq_restore(flags);
#else
    static llvm_lock_t uuid_lock = LLVM_LOCK_INITIALIZER;

    llvm_spin_lock(&uuid_lock);
    id = uuid;
    uuid++;
    llvm_spin_unlock(&uuid_lock);
#endif
#endif
    
    return id;
}

/*
 * get_cpu_speed()
 *  Get the processor frequency.
 */
unsigned long get_cpu_speed(void)
{
    FILE *fp;
    unsigned long freq = 0;
    char buffer[128], *p, *value;
    
    memset(buffer, 0, sizeof(buffer));
    
    fp = fopen("/proc/cpuinfo", "r");
    if (fp == NULL)
    {
        fprintf(stderr, "%s: cannot open file /proc/cpuinfo.\n", __func__);
        exit(0);
    }
    
    for (;;)
    {
        buffer[0] = '\0';
        
        p  = fgets(buffer, 127, fp);
        if (p == NULL)
            break;
        
        /* Skip blank lines. */
        if (*p == '\n') continue;
        
        p = strchr(buffer, ':');
        if (p == NULL)
            break;
        
        /*
         * p+2: +1 = space, +2= firt character
         * strlen()-1 gets rid of \n
         */
        *p = '\0';
        value = p + 2;
        
        value[strlen(value)-1] = '\0';
        
        if (!strncmp("cpu MHz", buffer, 7))
        {
            float f;
            sscanf(value, "%f", &f);
            freq = lroundf(f);
            break;
        }
    }
    fclose(fp);

    return freq;
}

/*
 * gettid()
 *  Get the thread ID.
 */
pid_t gettid(void)
{
#ifdef SYS_gettid
    return (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    return (pid_t)syscall(__NR_gettid);
#else
#error "Unable to implement gettid."
#endif
}

#if defined(__arm__)
#ifdef USE_ARMV7_INSTRUCTIONS
#define isb() asm volatile("isb" : : : "memory")
#define dsb() asm volatile("dsb" : : : "memory")
#define dmb() asm volatile("dmb" : : : "memory")
#else
#define isb() asm volatile("" : : : "memory")
#define dsb() asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory")
#define dmb() asm volatile("" : : : "memory")
#endif
#endif

/*
 * patch_jmp()
 *  Patch a direct jump from patch_addr to addr.
 */
void patch_jmp(volatile unsigned long patch_addr, volatile unsigned long addr)
{
    static llvm_lock_t patch_lock = LLVM_LOCK_INITIALIZER;

    memory_barrier();

    llvm_spin_lock(&patch_lock);

#if defined(__i386__) || defined(__x86_64__)
    tb_set_jmp_target1(patch_addr+1, addr);
#elif defined(__arm__)
    *(uint32_t *)patch_addr = addr;
#elif defined(_ARCH_PPC)
    ppc_tb_set_jmp_target(patch_addr, addr);
#endif

    llvm_spin_unlock(&patch_lock);

    memory_barrier();
}

/*
 * memory_barrier()
 *  Memory fence.
 */
inline void memory_barrier(void)
{
#if defined(__arm__)
    dsb();
    dmb();
#elif defined(__i386__) || defined(__x86_64__)
    barrier();
#endif
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

