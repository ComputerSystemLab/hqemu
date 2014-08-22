/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __PERF_H
#define __PERF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <perfmon/pfmlib.h>
#if defined(LIBPFM_VERSION)
#  define CONFIG_PERF_EVENTS
#elif defined(PFMLIB_VERSION)
#  undef CONFIG_PERF_EVENTS
#else
#  error "cannot detect perfmon version"
#endif

#ifdef CONFIG_PERF_EVENTS   /* Linux Perf Event */
#include <perfmon/pfmlib_perf_event.h>

#define PERF_COUNT_HW_CPU_CYCLES            "PERF_COUNT_HW_CPU_CYCLES"
#define PERF_COUNT_HW_INSTRUCTIONS          "PERF_COUNT_HW_INSTRUCTIONS"
#define PERF_COUNT_HW_BRANCH_INSTRUCTIONS   "PERF_COUNT_HW_BRANCH_INSTRUCTIONS"
#define PERF_MEM_LOAD_RETIRED               NULL
#define PERF_MEM_STORE_RETIRED              NULL

#define pfm_start(fd,flag)  ioctl(fd, PERF_EVENT_IOC_ENABLE,  1)
#define pfm_restart(fd)     ioctl(fd, PERF_EVENT_IOC_ENABLE,  1)
#define pfm_stop(fd)        ioctl(fd, PERF_EVENT_IOC_DISABLE, 1)

#else   /* Perfmon2 */
#include <perfmon/perfmon.h>
#include <perfmon/perfmon_dfl_smpl.h>
#include <perfmon/pfmlib_intel_nhm.h>

#define PERF_COUNT_HW_CPU_CYCLES            "UNHALTED_CORE_CYCLES"
#define PERF_COUNT_HW_INSTRUCTIONS          "INSTRUCTIONS_RETIRED"
#define PERF_COUNT_HW_BRANCH_INSTRUCTIONS   "br_inst_retired:all_branches"
#define PERF_MEM_LOAD_RETIRED               "MEM_INST_RETIRED:LOADS"
#define PERF_MEM_STORE_RETIRED              "MEM_INST_RETIRED:STORES"

#define PFM_SUCCESS     PFMLIB_SUCCESS

#define LBPL        6
#define BPL         (sizeof(uint64_t) << 3)
#define pfm_terminate()
#endif

struct perf_event_desc_t
{
    pid_t pid;
    int num_events;
    void *events;
    int threshold;
    int *fd;
    const char **name;
    int *id;
    int num_pages;
    size_t bufsize;
    void *buf;
    size_t smpl_size;
    void *smpl;
    int num_smpl_processed;
};
typedef struct perf_event_desc_t perf_event_desc_t;

int perf_setup_event_desc(perf_event_desc_t *evd, pid_t id, int num_events, int threshold, int num_pages);
int perf_setup_signal(perf_event_desc_t *evd, pid_t id, int signum);
int perf_map_buffer(perf_event_desc_t *evd);

/* general event setup */
int perf_setup_events(perf_event_desc_t *evd, const char **gen_events);
int perf_read_buffer(perf_event_desc_t *evd);

/* lbr */
int perf_setup_lbr_events(perf_event_desc_t *evd);
int perf_read_lbr_buffer(perf_event_desc_t *evd);

#ifdef __cplusplus
}
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

