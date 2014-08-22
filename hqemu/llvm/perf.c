/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "utils.h"
#include "perf.h"

//#define DEBUG_PERF
#ifdef DEBUG_PERF
#define perf_debug(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define perf_debug(fmt, ...)
#endif
#define perf_error(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); exit(0); } while (0)

int perf_setup_event_desc(perf_event_desc_t *evd, pid_t pid, int num_events, 
        int threshold, int num_pages)
{
    int i;
    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    memset(evd, 0, sizeof(perf_event_desc_t));
    evd->num_events = num_events;
    evd->pid = pid;
    evd->threshold = threshold;
    evd->num_pages = num_pages;
    evd->bufsize = num_pages * getpagesize();
    evd->fd = (int *)llvm_malloc(num_events * sizeof(int));
    evd->name = (const char **)llvm_malloc(num_events * sizeof(char *));
    evd->id = (int *)llvm_malloc(num_events * sizeof(int));
    evd->num_smpl_processed = 0;

    if (evd->fd == NULL || evd->name == NULL || evd->id == NULL)
        perf_error("%s: not enough memory.\n", __func__);

    for (i = 0; i < num_events; i++)
        evd->fd[i] = -1;

    memset(evd->name, 0, num_events * sizeof(char *));
    memset(evd->id, 0, num_events * sizeof(int));

    return 0;
}

int perf_setup_signal(perf_event_desc_t *evd, pid_t pid, int signum)
{
    int fd, flags;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    fd = evd->fd[0];
    flags = fcntl(fd, F_GETFL, 0);

    if (fcntl(fd, F_SETFL, flags | O_ASYNC) < 0)
        perf_error("%s: fcntl O_ASYNC failed.\n", __func__);
    
    if (fcntl(fd, F_SETOWN, pid) < 0)
        perf_error("%s: fcntl F_SETOWN failed.\n", __func__);
    
    if (fcntl(fd, F_SETSIG, signum) < 0)
        perf_error("%s: fcntl F_SETSIG failed.\n", __func__);

    return 0;
}

#ifdef CONFIG_PERF_EVENTS

#define PERF_FORMAT_SCALE (PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING)

struct smpl_pid_t { uint32_t pid, tid; };
struct smpl_cpu_t { uint32_t cpu, reserved; };

int perf_setup_events(perf_event_desc_t *evd, const char **gen_events)
{
    int i, num_events;
    uint64_t type, fmt;
    size_t smpl_size = 0;
    pfm_perf_encode_arg_t arg;
    struct perf_event_attr *hw;

    if (gen_events == NULL || evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    num_events = evd->num_events;
    hw = (struct perf_event_attr *)llvm_malloc(num_events * sizeof(struct perf_event_attr));
    memset(hw, 0, num_events * sizeof(struct perf_event_attr));
    for (i = 0; i < num_events; i++)
    {
        memset(&arg, 0, sizeof(arg));
        arg.attr = &hw[i];
        if (pfm_get_os_event_encoding(gen_events[i], PFM_PLM0|PFM_PLM3, 
                    PFM_OS_PERF_EVENT_EXT, &arg) != PFM_SUCCESS)
            perf_error("%s: cannot find event %s.\n", __func__, gen_events[i]);
        hw[i].size = sizeof(struct perf_event_attr);
        evd->name[i] = strdup(gen_events[i]);
    }

    for(i = 0; i < num_events; i++)
    {
        hw[i].disabled = !i;
        if (i == 0)
        {
            hw[0].wakeup_watermark = evd->bufsize / 2;
            hw[0].watermark = 1;
#if !defined(__arm__)
            hw[0].exclude_kernel = 1;
#endif
            hw[0].sample_type = PERF_SAMPLE_IP;
            hw[0].sample_period = evd->threshold;
            hw[0].read_format = 0;
            if (num_events > 1)
                hw[0].read_format |= PERF_FORMAT_GROUP|PERF_FORMAT_ID;

#if 0
            hw[0].sample_type |= PERF_SAMPLE_READ;
            hw[0].read_format |= PERF_FORMAT_SCALE;
#endif
        }
        
        evd->fd[i] = perf_event_open(&hw[i], evd->pid, -1, evd->fd[0], 0);
        if (evd->fd[i] == -1)
            perf_error("%s: cannot attach event %s.\n", __func__, gen_events[i]);
    }

    /* Calculate sample size */
    type = hw[0].sample_type;
    fmt = hw[0].read_format;
    if (type & PERF_SAMPLE_IP)          smpl_size += 8;
    if (type & PERF_SAMPLE_TID)         smpl_size += sizeof(struct smpl_pid_t);
    if (type & PERF_SAMPLE_TIME)        smpl_size += 8;
    if (type & PERF_SAMPLE_ADDR)        smpl_size += 8;
    if (type & PERF_SAMPLE_ID)          smpl_size += 8;
    if (type & PERF_SAMPLE_STREAM_ID)   smpl_size += 8;
    if (type & PERF_SAMPLE_CPU)         smpl_size += sizeof(struct smpl_cpu_t);
    if (type & PERF_SAMPLE_PERIOD)      smpl_size += 8;
    if (type & PERF_SAMPLE_READ)
    {
        if (fmt & PERF_FORMAT_GROUP)
        {
            smpl_size += 8;   /* nr */
            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)   smpl_size += 8;
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)   smpl_size += 8;
            size_t sub_smpl_size = 8;   /* group value */
            if (fmt & PERF_FORMAT_ID)
                sub_smpl_size += 8;     /* group id */
            smpl_size += evd->num_events * sub_smpl_size;
        }
        else
        {
            smpl_size += 8;
            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)   smpl_size += 8;
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)   smpl_size += 8;
            if (fmt & PERF_FORMAT_ID)                   smpl_size += 8;
        }
    }
    if (type & PERF_SAMPLE_CALLCHAIN)
        perf_error("%s: currently not support PERF_SAMPLE_CALLCHAIN.\n", __func__);
    if (type & PERF_SAMPLE_RAW)
        perf_error("%s: currently not support PERF_SAMPLE_RAW.\n", __func__);

    if (smpl_size)
    {
        evd->smpl = (void *)llvm_malloc(smpl_size);
        memset(evd->smpl, 0, smpl_size);
    }
    evd->smpl_size = smpl_size;
    evd->events = hw;

    return 0;
}

int perf_setup_lbr_events(perf_event_desc_t *evd)
{
    perf_error("%s: lbr is not suppoted.\n", __func__);
    return 0;
}

int perf_map_buffer(perf_event_desc_t *evd)
{
    void *buf;
    int i, ret, map_size;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    map_size = (evd->num_pages + 1) * getpagesize();
    buf = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED, evd->fd[0], 0);
    if (buf == MAP_FAILED)
        perf_error("%s: cannot mmap buffer.\n", __func__);

    if (evd->num_events > 1)
    {
        struct perf_event_attr *hw = (struct perf_event_attr *)evd->events;
        uint64_t fmt = hw[0].read_format;
        int header[2] = { 1, 0 };
        if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED) header[0]++;
        if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING) header[0]++;
        header[1] = (fmt & PERF_FORMAT_ID) ? 2 : 1;

        size_t sz = (header[0] + header[1] * evd->num_events) * sizeof(uint64_t);
        uint64_t *val = (uint64_t *)llvm_malloc(sz);
        
        ret = read(evd->fd[0], val, sz);
        if (ret == -1)
            perf_error("%s: cannot read id %zu", __func__, sizeof(val));
        
        if (fmt & PERF_FORMAT_ID)
        {
            for(i = 0; i < evd->num_events; i++)
                evd->id[i] = val[header[0] + header[1]*i + 1];
        }
        llvm_free(val);
    }

    evd->buf = buf;

    return 0;
}

static inline uint64_t perf_scale(uint64_t *values)
{
    uint64_t res = 0;
    if (values[2])
        res = (uint64_t)((double)values[0] * values[1]/values[2]);
    return res;
}

static inline const char *perf_id2event(perf_event_desc_t *evd, uint64_t id)
{
    int i;
    static const char *unknown_str = "unknown sample event";
    for(i = 0; i < evd->num_events; i++)
        if (evd->id[i] == id)
            return evd->name[i];
    return unknown_str;
}

static void perf_skip_buffer(struct perf_event_mmap_page *hdr, size_t sz)
{
    if (hdr->data_tail + sz > hdr->data_head)
        sz = hdr->data_head - hdr->data_tail;
    
    hdr->data_tail += sz;
}

#ifdef DEBUG_PERF
static int perf_print_event(perf_event_desc_t *evd)
{
    uint64_t type, fmt, *val;
    struct perf_event_attr *hw;

    hw = (struct perf_event_attr *)evd->events;
    type = hw[0].sample_type;
    fmt = hw[0].read_format;

    val = (uint64_t *)evd->smpl;
    if (type & PERF_SAMPLE_IP) 
    {
        perf_debug("IIP:%#016"PRIx64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_TID)
    {
        struct smpl_pid_t *smpl_pid = (struct smpl_pid_t *)val;
        perf_debug("PID:%d TID:%d ", smpl_pid->pid, smpl_pid->tid);
        val = (uint64_t *)(++smpl_pid);
    }
    if (type & PERF_SAMPLE_TIME)
    {
        perf_debug("TIME:%'"PRIu64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_ADDR)
    {
        perf_debug("ADDR:%#016"PRIx64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_ID)
    {
        perf_debug("ID:%"PRIu64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_STREAM_ID)
    {
        perf_debug("STREAM_ID:%"PRIu64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_CPU)
    {
        struct smpl_cpu_t *smpl_cpu = (struct smpl_cpu_t *)val;
        perf_debug("CPU:%u ", smpl_cpu->cpu);
        val = (uint64_t *)(++smpl_cpu);
    }
    if (type & PERF_SAMPLE_PERIOD)
    {
        perf_debug("PERIOD:%'"PRIu64" ", *val);
        ++val;
    }
    if (type & PERF_SAMPLE_READ)
    {
        uint64_t values[3];
        uint64_t time_enabled=1, time_running=1;
        struct { uint64_t value, id; } grp;
        int idx = 0;
        if (fmt & PERF_FORMAT_GROUP)
        {
            uint64_t nr = *val;
            ++val;

            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)
            {
                time_enabled = *val;
                ++val;
            }
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)
            {
                time_running = *val;
                ++val;
            }

            perf_debug("ENA=%'"PRIu64" RUN=%'"PRIu64" NR=%"PRIu64"\n", time_enabled, time_running, nr);
            values[1] = time_enabled;
            values[2] = time_running;
            while(nr--)
            {
                grp.id = -1;
                values[0] = *val;
                grp.value = perf_scale(values);

                ++val;
                if (fmt & PERF_FORMAT_ID)
                {
                    grp.id = *val;
                    ++val;
                    const char *event_str = perf_id2event(evd, grp.id);
                    perf_debug("\t%s (%"PRIu64") %'"PRIu64"\n", event_str, grp.id, grp.value);
                }
                else
                    perf_debug("\tevent %d: val=%'"PRIu64"\n", idx++, grp.value);
            }
        }
        else
        {
            uint64_t val64 = *val;
            ++val;
            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)
            {
                time_enabled = *val;
                ++val;
            }
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)
            {
                time_running = *val;
                ++val;
            }
            if (fmt & PERF_FORMAT_ID)
            {
                val64 = *val;
                ++val;
            }

            perf_debug("ENA=%'"PRIu64" RUN=%'"PRIu64"\n", time_enabled, time_running);

            values[0] = val64;
            values[1] = time_enabled;
            values[2] = time_running;
            val64 = perf_scale(values);

            perf_debug("\t%s %'"PRIu64"\n", evd->name[0], val64);
        }
    }
    perf_debug("\n");

    return 0;
}
#endif

/* copy_next: copy next data from a ring buffer. */
#define copy_next(rbuf, rsize, sbuf, ssize, pos)                        \
    do {                                                                \
        size_t remain_size = ssize - pos;                               \
        size_t copy_size = remain_size < rsize ? remain_size : rsize;   \
        memcpy((void *)rbuf, (void *)sbuf + pos, copy_size);            \
        if (rsize > copy_size)                                          \
            memcpy((void *)rbuf + copy_size, sbuf, rsize - copy_size);  \
    } while (0)

#define copy_data(rbuf, rsize)                      \
    do {                                            \
        pos = hdr->data_tail & pgmsk;               \
        copy_next(rbuf, rsize, data, bufsize, pos); \
        hdr->data_tail += rsize;                    \
        rbuf += rsize;                              \
    } while(0)
#define copy_data64(rbuf)   copy_data(rbuf, 8)

int perf_read_buffer(perf_event_desc_t *evd)
{
    void *data, *p=NULL;
    unsigned long pos;
    struct perf_event_mmap_page *hdr;
    struct perf_event_header ehdr;
    size_t avail_size, bufsize, pgmsk;
    uint64_t type, fmt;
    struct perf_event_attr *hw;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    hdr = (struct perf_event_mmap_page *)evd->buf;

    avail_size = hdr->data_head - hdr->data_tail;
    if (avail_size < evd->smpl_size)
        return 1;

    bufsize = evd->bufsize;
    pgmsk = bufsize - 1;
    data = (void *)hdr + getpagesize(); /* data points to beginning of buffer payload */

    pos = hdr->data_tail & pgmsk;
    copy_next(&ehdr, sizeof(ehdr), data, bufsize, pos);
    hdr->data_tail += sizeof(ehdr);
    
    avail_size = ehdr.size - sizeof(ehdr);
    switch(ehdr.type)
    {
        case PERF_RECORD_SAMPLE:
            break;
        default:
            perf_skip_buffer(hdr, avail_size);
            return -1;
    }

    if (avail_size != evd->smpl_size)
        return -1;

    hw = (struct perf_event_attr *)evd->events;
    type = hw[0].sample_type;
    fmt = hw[0].read_format;

    p = (void *)evd->smpl;
    if (type & PERF_SAMPLE_IP)          copy_data64(p);
    if (type & PERF_SAMPLE_TID)         copy_data(p, sizeof(struct smpl_pid_t));
    if (type & PERF_SAMPLE_TIME)        copy_data64(p);
    if (type & PERF_SAMPLE_ADDR)        copy_data64(p);
    if (type & PERF_SAMPLE_ID)          copy_data64(p);
    if (type & PERF_SAMPLE_STREAM_ID)   copy_data64(p);
    if (type & PERF_SAMPLE_CPU)         copy_data(p, sizeof(struct smpl_cpu_t));
    if (type & PERF_SAMPLE_PERIOD)      copy_data64(p);
    if (type & PERF_SAMPLE_READ)
    {
        if (fmt & PERF_FORMAT_GROUP)
        {
            uint64_t nr;
            copy_data64(p);
            nr = *((uint64_t *)p-1);
            if (nr != evd->num_events)
                perf_error("%s: internal error.\n", __func__);

            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)   copy_data64(p);
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)   copy_data64(p);
            while(nr--)
            {
                copy_data64(p);
                if (fmt & PERF_FORMAT_ID)
                    copy_data64(p);
            }
        }
        else
        {
            copy_data64(p);
            if (fmt & PERF_FORMAT_TOTAL_TIME_ENABLED)   copy_data64(p);
            if (fmt & PERF_FORMAT_TOTAL_TIME_RUNNING)   copy_data64(p);
            if (fmt & PERF_FORMAT_ID)                   copy_data64(p);
        }
    }

#ifdef DEBUG_PERF
    perf_print_event(evd);
#endif

    return 0;
}

int perf_read_lbr_buffer(perf_event_desc_t *evd)
{
    perf_error("%s: lbr is not supported.\n", __func__);
    return 0;
}

#else

#define LBPL    6
#define BPL     (sizeof(uint64_t) << 3)
typedef pfm_dfl_smpl_hdr_t      smpl_hdr_t;
typedef pfm_dfl_smpl_entry_t    smpl_entry_t;
typedef pfm_dfl_smpl_arg_t      smpl_arg_t;

static inline void pfm_bv_set(uint64_t *bv, uint16_t rnum)
{
    bv[rnum >> LBPL] |= 1UL << (rnum & (BPL - 1));
}

static inline int pfm_bv_isset(uint64_t *bv, uint16_t rnum)
{
    return bv[rnum >> LBPL] & (1UL << (rnum & (BPL - 1))) ? 1 : 0;
}

static inline void pfm_bv_copy(uint64_t *d, uint64_t *j, uint16_t n)
{
    if (n <= BPL)
        *d = *j;
    else
        memcpy(d, j, (n>>LBPL)*sizeof(uint64_t));
}

int perf_setup_events(perf_event_desc_t *evd, const char **gen_events)
{
    int i, fd, num_events;
    unsigned int max_pmd = 0, num_smpl_pmds = 0;
    pfarg_ctx_t *ctx;
    pfarg_load_t *load_arg;
    smpl_arg_t *buf_arg;
    pfmlib_input_param_t *inp;
    pfmlib_output_param_t *outp;
    pfarg_pmc_t *pc;
    pfarg_pmd_t *pd;

    if (gen_events == NULL || evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    ctx = (pfarg_ctx_t *)llvm_malloc(sizeof(pfarg_ctx_t));
    load_arg = (pfarg_load_t *)llvm_malloc(sizeof(pfarg_load_t));
    buf_arg = (smpl_arg_t *)llvm_malloc(sizeof(smpl_arg_t));
    inp = (pfmlib_input_param_t *)llvm_malloc(sizeof(pfmlib_input_param_t));
    outp = (pfmlib_output_param_t *)llvm_malloc(sizeof(pfmlib_output_param_t));

    memset(ctx, 0, sizeof(pfarg_ctx_t));
    memset(load_arg, 0, sizeof(pfarg_load_t));
    memset(buf_arg, 0, sizeof(smpl_arg_t));
    memset(inp, 0, sizeof(pfmlib_input_param_t));
    memset(outp, 0, sizeof(pfmlib_output_param_t));

    /* Locate events. */
    num_events = evd->num_events;
    for (i = 0; i < num_events; i++)
    {
        if (pfm_find_full_event(gen_events[i], &inp->pfp_events[i]) != PFM_SUCCESS)
            perf_error("%s: cannot find event %s.\n", __func__, gen_events[i]);

        evd->name[i] = gen_events[i];
    }

    /* Set input parameters: # counters, privilege mode. */
    inp->pfp_event_count = num_events;
    inp->pfp_dfl_plm = PFM_PLM3;
    inp->pfp_flags = 0;

    /* Let the library figures out the values for the PMCS. */
    if (pfm_dispatch_events(inp, NULL, outp, NULL) != PFM_SUCCESS)
        perf_error("%s: pfm_dispatch_events failed.\n", __func__);

    pc = (pfarg_pmc_t *)llvm_malloc(outp->pfp_pmc_count * sizeof(pfarg_pmc_t));
    pd = (pfarg_pmd_t *)llvm_malloc(outp->pfp_pmd_count * sizeof(pfarg_pmd_t));

    memset(pc, 0, outp->pfp_pmc_count * sizeof(pfarg_pmc_t));
    memset(pd, 0, outp->pfp_pmd_count * sizeof(pfarg_pmd_t));

    /* Prepare the argument to initialize the PMDs and PMCS. */
    for (i = 0; i < (int)outp->pfp_pmc_count; i++)
    {
        pc[i].reg_num = outp->pfp_pmcs[i].reg_num;
        pc[i].reg_value = outp->pfp_pmcs[i].reg_value;
    }
    
    for (i = 0; i < (int)outp->pfp_pmd_count; i++) 
    {
        pd[i].reg_num = outp->pfp_pmds[i].reg_num;

        /* Skip first counter (for sampling period). */
        if (i != 0)
        {
            pfm_bv_set(pd[0].reg_smpl_pmds, pd[i].reg_num);
            if (pd[i].reg_num > max_pmd)
                max_pmd = pd[i].reg_num;
            num_smpl_pmds++;
        }
    }

    /* We are sampling counter overflow and we want to be notified.
     * The notification will come only when the sampling buffer becomes full. */
    pd[0].reg_flags = PFM_REGFL_OVFL_NOTIFY;
    pd[0].reg_value = - evd->threshold;
    pd[0].reg_long_reset = - evd->threshold;
    pd[0].reg_short_reset = - evd->threshold;

    pd[0].reg_random_seed = 5;
    pd[0].reg_random_mask = 0xfff;

    /* Reset the other PMDs on every overflow. If we do not set this, 
     * the non-overflowed counters will be untouched. */
    pfm_bv_copy(pd[0].reg_reset_pmds, pd[0].reg_smpl_pmds, max_pmd);

    /* Create perfmon context. */
    ctx->ctx_flags = PFM_FL_OVFL_NO_MSG;
    buf_arg->buf_size = evd->bufsize;
    fd = pfm_create_context(ctx, (char *)PFM_DFL_SMPL_NAME, buf_arg,
            sizeof(smpl_arg_t));
    if (fd < 0)
    {
        if (errno == ENOSYS)
            perf_error("%s: no performance monitoring support.\n", __func__);
        perf_error("%s pfm_create_context failed.\n", __func__);
    }

    /* Program the registers. */
    if (pfm_write_pmcs(fd, pc, outp->pfp_pmc_count))
        perf_error("%s: pfm_write_pmcs falied.\n", __func__);
    
    if (pfm_write_pmds(fd, pd, outp->pfp_pmd_count))
        perf_error("%s: pfm_write_pmds failed.\n", __func__);
    
    /* Attach context to stopped task. */
    load_arg->load_pid = evd->pid;
    if (pfm_load_context(fd, load_arg) != 0)
        perf_error("%s: pfm_load_context failed.\n", __func__);

    evd->fd[0] = fd;
    evd->num_events = num_smpl_pmds + 1;
    evd->smpl_size = sizeof(smpl_entry_t) + num_smpl_pmds * sizeof(uint64_t);
    evd->smpl = (void *)llvm_malloc(evd->smpl_size);
    evd->events = (void *)llvm_malloc(sizeof(pfarg_pmd_t));
    memset(evd->smpl, 0, evd->smpl_size);
    memcpy(evd->events, &pd[0], sizeof(pfarg_pmd_t));

    llvm_free(ctx);
    llvm_free(load_arg);
    llvm_free(buf_arg );
    llvm_free(inp);
    llvm_free(outp);
    llvm_free(pc);
    llvm_free(pd);

    return 0;
}

int perf_setup_lbr_events(perf_event_desc_t *evd)
{
    int i, fd;
    pfarg_ctx_t *ctx;
    pfarg_load_t *load_arg;
    smpl_arg_t *buf_arg;
    pfmlib_input_param_t *inp;
    pfmlib_output_param_t *outp;
    pfmlib_nhm_input_param_t *mod_inp;
    pfarg_pmc_t *pc;
    pfarg_pmd_t *pd;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    ctx = (pfarg_ctx_t *)llvm_malloc(sizeof(pfarg_ctx_t));
    load_arg = (pfarg_load_t *)llvm_malloc(sizeof(pfarg_load_t));
    buf_arg = (smpl_arg_t *)llvm_malloc(sizeof(smpl_arg_t));
    inp = (pfmlib_input_param_t *)llvm_malloc(sizeof(pfmlib_input_param_t));
    outp = (pfmlib_output_param_t *)llvm_malloc(sizeof(pfmlib_output_param_t));
    mod_inp = (pfmlib_nhm_input_param_t *)llvm_malloc(sizeof(pfmlib_nhm_input_param_t));
    
    memset(ctx, 0, sizeof(pfarg_ctx_t));
    memset(load_arg, 0, sizeof(pfarg_load_t));
    memset(buf_arg, 0, sizeof(smpl_arg_t));
    memset(inp, 0, sizeof(pfmlib_input_param_t));
    memset(outp, 0, sizeof(pfmlib_output_param_t));
    memset(mod_inp, 0, sizeof(pfmlib_nhm_input_param_t));

    /* Locate events. */
    if (pfm_find_full_event(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, &inp->pfp_events[0]) != PFM_SUCCESS)
        perf_error("%s: cannot find LBR event.\n", __func__);
    
    evd->name[0] = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;

    mod_inp->pfm_nhm_lbr.lbr_used = 1;
    mod_inp->pfm_nhm_lbr.lbr_plm = 0;
    mod_inp->pfm_nhm_lbr.lbr_filter = 0;

    /* Set input parameters: # counters, privilege mode. */
    inp->pfp_event_count = 1;
    inp->pfp_dfl_plm = PFM_PLM3;
    inp->pfp_flags = 0;

    /* Let the library figures out the values for the PMCS. */
    if (pfm_dispatch_events(inp, mod_inp, outp, NULL) != PFM_SUCCESS)
        perf_error("%s: pfm_dispatch_events failed.\n", __func__);

    pc = (pfarg_pmc_t *)llvm_malloc(outp->pfp_pmc_count * sizeof(pfarg_pmc_t));
    pd = (pfarg_pmd_t *)llvm_malloc(sizeof(pfarg_pmd_t));
    
    memset(pc, 0, outp->pfp_pmc_count * sizeof(pfarg_pmc_t));
    memset(pd, 0, sizeof(pfarg_pmd_t));

    /* Prepare the argument to initialize the PMDs and PMCS. */
    for (i = 0; i < (int)outp->pfp_pmc_count; i++)
    {
        pc[i].reg_num = outp->pfp_pmcs[i].reg_num;
        pc[i].reg_value = outp->pfp_pmcs[i].reg_value;
    }
    
    /* We are sampling counter overflow and we want to be notified.
     * The notification will come only when the sampling buffer becomes full. */
    pd[0].reg_num = outp->pfp_pmds[0].reg_num;
    pd[0].reg_flags = PFM_REGFL_OVFL_NOTIFY;
    pd[0].reg_value = - evd->threshold;
    pd[0].reg_long_reset = - evd->threshold;
    pd[0].reg_short_reset = - evd->threshold;

    /* Add 2 x 16 LBR entries + LBR_TOS to smpl_pmds */
    for(i = 31; i < 64; i++)
        pfm_bv_set(pd[0].reg_smpl_pmds, i);

    /* Reset LBR after each sample to be able to determine whether or not we get new data.
     * LBR_TOS(PMD31) is read-only, it is not included in reset_pmds. */
    for(i = 32; i < 64; i++)
        pfm_bv_set(pd[0].reg_reset_pmds, i);

    /* Create perfmon context. */
    ctx->ctx_flags = PFM_FL_OVFL_NO_MSG;
    buf_arg->buf_size = evd->bufsize;
    fd = pfm_create_context(ctx, (char *)PFM_DFL_SMPL_NAME, buf_arg,
            sizeof(smpl_arg_t));
    if (fd < 0)
    {
        if (errno == ENOSYS)
            perf_error("%s: no performance monitoring support.\n", __func__);
        perf_error("%s pfm_create_context failed.\n", __func__);
    }

    /* Program the registers. */
    if (pfm_write_pmcs(fd, pc, outp->pfp_pmc_count))
        perf_error("%s: pfm_write_pmcs falied.\n", __func__);
    
    if (pfm_write_pmds(fd, pd, outp->pfp_pmd_count))
        perf_error("%s: pfm_write_pmds failed.\n", __func__);
    
    /* Attach context to stopped task. */
    load_arg->load_pid = evd->pid;
    if (pfm_load_context(fd, load_arg) != 0)
        perf_error("%s: pfm_load_context failed.\n", __func__);

    evd->fd[0] = fd;
    evd->num_events = 33 + 1;
    evd->smpl_size = sizeof(smpl_entry_t) + 33 * sizeof(uint64_t);
    evd->smpl = (void *)llvm_malloc(evd->smpl_size);
    evd->events = (void *)llvm_malloc(sizeof(pfarg_pmd_t));
    memset(evd->smpl, 0, evd->smpl_size);
    memcpy(evd->events, &pd[0], sizeof(pfarg_pmd_t));

    return 0;
}

int perf_map_buffer(perf_event_desc_t *evd)
{
    void *buf;

    if (evd == NULL)
        perf_error("%s: not enough memory.\n", __func__);

    buf = mmap(NULL, evd->bufsize, PROT_READ, MAP_PRIVATE, evd->fd[0], 0);
    if (buf == MAP_FAILED)
        perf_error("%s: cannot mmap buffer.\n", __func__);

    evd->buf = buf;

    return 0;
}

int perf_read_buffer(perf_event_desc_t *evd)
{
    int i, num_events;
    smpl_hdr_t *hdr;
    void *data;
    smpl_entry_t *ent;
    uint64_t *reg, *p = NULL;
    pfarg_pmd_t *pd;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    hdr = (smpl_hdr_t *)evd->buf;
    if (evd->num_smpl_processed >= hdr->hdr_count)
        return -1;

    num_events = evd->num_events - 1;
    pd = (pfarg_pmd_t *)evd->events;
    data = (void *)(hdr + 1);
    ent = (smpl_entry_t *)(data + evd->num_smpl_processed * evd->smpl_size);
    reg = (uint64_t *)(ent + 1);

    p = (void *)evd->smpl;
    *p++ = ent->ip;
    for(i = 0; num_events; i++)
    {
        if (pfm_bv_isset(pd->reg_smpl_pmds, i)) {
            *p++ = *reg;
            reg++;
            num_events--;
        }
    }

    evd->num_smpl_processed++;

    return 0;
}

int perf_read_lbr_buffer(perf_event_desc_t *evd)
{
    smpl_hdr_t *hdr;
    void *data;
    smpl_entry_t *ent;
    uint64_t *reg, *p = NULL;
    uint64_t tos, idx;

    if (evd == NULL)
        perf_error("%s: null pointer.\n", __func__);

    hdr = (smpl_hdr_t *)evd->buf;
    if (evd->num_smpl_processed >= hdr->hdr_count)
        return -1;

    data = (void *)(hdr + 1);
    ent = (smpl_entry_t *)(data + evd->num_smpl_processed * evd->smpl_size);
    reg = (uint64_t *)(ent + 1);

    p = (void *)evd->smpl;
    *p++ = ent->ip;

    tos = reg[0] * 2;
    idx = (tos + 2) % 32;
    while (idx != tos)
    {
        *p++ = reg[1+idx];
        *p++ = reg[1+idx+1];
        idx = (idx + 2) % 32;
    }
    /* Also add the latest pair. */
    tos = (tos + 1) % 32;
    *p++ = reg[tos];
    *p++ = reg[tos+1];

    evd->num_smpl_processed++;

    return 0;
}

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

