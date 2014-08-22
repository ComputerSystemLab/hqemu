/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_DBO_H
#define __LLVM_DBO_H

#if defined(CONFIG_DBO)

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <sys/poll.h>
#include <map>
#include "perf.h"
#include "llvm-debug.h"
#include "llvm-opc.h"

using namespace std;

#define SMPL_LAST   0
#define SMPL_TOTAL  1

class SmplProfile;

typedef void (*signal_handler_t)(int, siginfo_t *, void *);
typedef map< target_ulong, CodeInfo* > GuestCodeMap;
typedef map< unsigned long, CodeInfo* > HostCodeMap;
typedef vector< uint64_t > IPVec;
typedef vector< SmplProfile* > ProfileVec;
typedef map< uint32_t, SmplProfile* > ProfileMap;

/*
 * WindowManager is the profiling window to track the last N windows of sampled
 * regions and determine if the execution has entered the stable state. The
 * windows are operated like an cyclic queue.
 */
template <class T> class WindowManager
{
public:
    WindowManager(int size)
    {
        Size = size;
        CurrIdx = 0;
        Window = new T[size];
    }

    int Size;     /* N: the number of windows */
    int CurrIdx;  /* The index to the last window */
    T *Window;    /* The queue of windows */

    int getSize()       { return Size; }
    int getCurrIdx()    { return CurrIdx; }
    T *getWindowList()  { return Window; }
    T &getCurrWindow()  { return Window[CurrIdx]; }
    void moveNext()     { CurrIdx = (CurrIdx + 1) % Size; }
};

/*
 * SmplData records the sample profiles which include the program counter (PC)
 * and values from branch target buffer (BTB).
 */
class SmplData
{
public:
    IPVec IPList;      /* The list of sampled PC */
    IPVec LBRSrcList;  /* The source PC of the branch */
    IPVec LBRDstList;  /* The target PC of the branch */

    /* PushIP - Insert a sampled PC value. */
    inline void PushIP(uint64_t ip)
    {
        IPList.push_back(ip);
    }

    /* PushLBR - Insert a BTB branch. */
    inline void PushLBR(uint64_t src, uint64_t dst)
    {
        LBRSrcList.push_back(src);
        LBRDstList.push_back(dst);
    }
};

/*
 * SmplProfile keeps the histogram of one monitored region.
 */
class SmplProfile
{
private:
    CodeInfo *Region;     /* The region to be monitored */
    uint64_t LastCount;   /* The sample count during the last interval */
    uint64_t TotalCount;  /* The sample count during the whole lifetime */

public:
    SmplProfile(CodeInfo *code, uint64_t InitCount)
    {
        Region = code;
        LastCount = 0;
        TotalCount = InitCount;
    }
    ~SmplProfile() {}

    inline CodeInfo *getRegion() { return Region; }
    inline uint64_t getCount(int Mode)
    {
        return (Mode == SMPL_LAST) ? LastCount : TotalCount;
    }
    inline uint64_t getNormalizedCount(int Mode)
    {
        return getCount(Mode) / Region->GuestICount;
    }
    inline void setCount(uint64_t Count)
    {
        TotalCount = Count;
    }
    inline void addCount(IPVec &IPList)
    {
        LastCount = IPList.size();
        TotalCount += LastCount;
    }

    static inline bool Cmp(SmplProfile *a, SmplProfile *b)
    {
        return a->TotalCount > b->TotalCount;
    }
    static inline bool CmpTotal(SmplProfile *a, SmplProfile *b)
    {
        return a->TotalCount * b->Region->GuestICount > b->TotalCount * a->Region->GuestICount;
    }
    static inline bool CmpLast(SmplProfile *a, SmplProfile *b)
    {
        return a->LastCount * b->Region->GuestICount > b->LastCount * a->Region->GuestICount;
    }

    /* sort - Sort the regions in decending order based on the non-normalized
     * total count. */
    static inline void sort(ProfileVec &SmpltoSort)
    {
        std::sort(SmpltoSort.begin(), SmpltoSort.end(), Cmp);
    }

    /* sort - Sort the regions in decending order based on the normalized
     * count. */
    static inline void sort(ProfileVec &SmpltoSort, int Mode)
    {
        std::sort(SmpltoSort.begin(), SmpltoSort.end(),
                (Mode == SMPL_LAST) ? CmpLast : CmpTotal);
    }
};

/*
 * ThreadInfo keeps the history profiles of the thread.
 */
class ThreadInfo
{
private:
    int CPUIndex;              /* The CPUID of this thread */
    ProfileMap CollectedProf;  /* The history profiles of monitored regions */
    WindowManager<ProfileVec> *ProfileWindow;  /* The profiling window */

    /* LookupProfileWithIP - Return the region profile where the IP locates. */
    SmplProfile *LookupProfileWithIP(uint64_t IP);

    /* CommitProfile - Associate the sampled restuls to the regions. */
    inline void CommitProfile(IPVec &IPs, ProfileVec &Profs);

    /* getSortedProfile - Get the sorted regions in decending order. */
    inline void getSortedProfile(ProfileVec &Profs, int Mode,
            ProfileVec &SortedProf, uint64_t &Count);

    /* getTopProfile - Get the top regions based on the `Threshold' (ratio). */
    inline void getTopProfile(ProfileVec &Profs, int Mode, double Threshold,
            ProfileVec &TopProf);

    /* ComputeCandidateRegions - Find the candidate regions to be merged. */
    void ComputeCandidateRegions(ProfileVec &Profs, int Mode, CodeVec &Regions);

    /* TryConnectRegions - Determine if the given regions can be connected. */
    void TryConnectRegions(CodeVec &Regions);

    void ExpandRegion();

public:
    ThreadInfo(int cpu_index, perf_event_desc_t *perf_evd);
    ~ThreadInfo();

    perf_event_desc_t *evd;  /* The handle to the HPM events */
    ExecProfile Exec;        /* The execution statistics */

    pid_t getCPUIndex()   { return CPUIndex;      }
    ProfileMap &getProf() { return CollectedProf; }
    void ProcessIPSmpl(SmplData *Smpl);
    void ProcessLBRSmpl(SmplData *Smpl);
    void RecordBreakdown(IPVec &IPs);

    void Print();
};

/*
 * DBOManager is the main component of the HPM-based dynamic binary optimizer
 * (DBO). DBOManager provides the information for the feedback-directed
 * optimization.
 */
class DBOManager
{
private:
    DBOManager();

    int signum;               /* The signal number for the signal handler */
    pthread_t MonitorThread;  /* The DBO thread */
    pid_t MonitorThreadID;    /* The DBO Thread ID */
    uint32_t MonitorID;       /* Unique ID for each monitored region */

    /* Information of the monitored threads */
    map<int, ThreadInfo*> ThreadRegistry;  /* The thread to be monitored */
    map<pid_t, int> ThreadMap;  /* Mapping of thead and HPM handle */
    vector< pair<int,ProfileMap> > ThreadProf; /* The thread profiles */

    perf_event_desc_t *setupEvent(pid_t tid);
    perf_event_desc_t *setupLBREvent(pid_t tid);
    inline void startMonitoring(perf_event_desc_t *evd);
    inline uint32_t getMonitorID();

public:
    ~DBOManager();

    GuestCodeMap GuestCodes;  /* The guest regions to be monitored */
    HostCodeMap HostCodes;    /* The host regions to be monitored */
    CodeVec RetiredCodes;     /* The retired regions after region merging. */
    GuestCodeMap RetiredMap;

    /* MonitorStart and MonitorEnd provie the envelop of the monitored
     * regions. They are used to fast filter out the sample results
     * outside the sample regions. */
    vector<unsigned long> MonitorStart;
    vector<unsigned long> MonitorEnd;

    ThreadInfo *getThreadInfo(int fd);

    /* RegisterThread - Attach a thread for DBO monitoring. */
    void RegisterThread(int cpu_index);

    /* UnregisterThread - Detach a thread from DBO monitoring. */
    void UnregisterThread();

    /* RegisterMonitoring - Attach a region for DBO monitoring. */
    void RegisterMonitoring(CodeInfo *Code);

    /* UnregisterMonitoring - Detach a region from DBO monitoring. */
    void UnregisterMonitoring(CodeInfo *Code);

    /* resetMonitoring - Reset DBO monitroing. */
    void resetMonitoring();

    /* PrintProfile - Display the hottest M regions of the whole execution
     * time. The default value of M is 5. */
    void PrintProfile();

    /* fork_start - Stop DBO before performing fork. */
    void fork_start();

    /* fork_end - Restart DBO after performing fork. */
    void fork_end(int child);

    /* createDBOManager - Entry function to create the DBO instance. */
    static void createDBOManager();
};

extern DBOManager *DBO;

extern "C"
{
int dbo_init(void);
int dbo_finalize(void);
int dbo_register_region(CodeInfo *Code);
int dbo_reset_monitoring(void);
void dbo_fork_start(void);
void dbo_fork_end(int child);
}
#else

#define dbo_init()              do {} while(0)
#define dbo_finalize()          do {} while(0)
#define dbo_register_region(a)  do {} while(0)
#define dbo_reset_monitoring()  do {} while(0)
#define dbo_fork_start()        do {} while(0)
#define dbo_fork_end(a)         do {} while(0)

#endif /* CONFIG_DBO */
#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

