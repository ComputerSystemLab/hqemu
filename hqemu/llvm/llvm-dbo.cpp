/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 *
 *  This file implements the HPM-based dynamic binary optimizer (DBO).
 */

#include <unistd.h>
#include "llvm.h"
#include "llvm-profile.h"
#include "llvm-dbo.h"
#include "utils.h"

#define MAX_PRINT_TRACE 5
#define USE_INSTRUCTION_RETIRED

#define SMPL_PERIOD     1e6
#if defined(CONFIG_HPM)
#define NUM_WINDOWS     1
#else
#define NUM_WINDOWS     4
#endif

extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern ProfileFactory *PF;
extern int HybridMode;

DBOManager *DBO;

static llvm_lock_t dbo_lock;
static bool MonitorInitialized;

void *dbo_thread_func(void *args);
void dbo_default_handler(int signum, siginfo_t *siginfo, void *data);
void dbo_lbr_default_handler(int signum, siginfo_t *siginfo, void *data);

static int EnableMem, LoadIdx, StoreIdx;

/*
 * DBOManager
 */
DBOManager::DBOManager()
{
    int ret;

    llvm_lock_init(&dbo_lock);

    ThreadRegistry.clear();

    if (HybridMode == HYBRID_MULTIPLE || HybridMode == HYBRID_HPM)
    {
        /* Create the monitoring thread. We wait here until the monitoring
         * thread registers its thread ID because we need to bind the thread
         * ID to the signal handler. */
        ret = pthread_create(&MonitorThread, NULL, dbo_thread_func,
                (void *)&MonitorThreadID);
        if (ret != 0)
            DM->Error("%s: failed to create dbo monitor thread.\n", __func__);
        
        for (;;)
        {
            llvm_spin_lock(&dbo_lock);
            if (MonitorInitialized == true)
            {
                llvm_spin_unlock(&dbo_lock);
                break;
            }
            llvm_spin_unlock(&dbo_lock);
            usleep(1000);
        }
    }
    else
    {
        MonitorThreadID = gettid();
        MonitorThread = (pthread_t)-1;
    }

    /* Install the signal handler. */
    struct sigaction act;
    signum = SIGIO;
    memset(&act, 0, sizeof(act));
#ifdef USE_LBR
    act.sa_sigaction = (signal_handler_t)dbo_lbr_default_handler;
#else
    act.sa_sigaction = (signal_handler_t)dbo_default_handler;
#endif
    act.sa_flags = SA_SIGINFO;
    ret = sigaction(signum, &act, 0);
    if (ret != 0)
        DM->Error("%s: failed to register signal handler.\n", __func__);

    /* Initialize the range of memory address to monitor. */
    MonitorID = 0;
    for (int i = 0; i < 2; i++)
    {
        MonitorStart.push_back((unsigned long)-1);
        MonitorEnd.push_back(0);
    }

    /* Initialize HPM library. */
    ret = pfm_initialize();
    if (ret != PFM_SUCCESS)
        DM->Error("%s: pfm_initialize failed.\n", __func__);

    DM->Debug(DEBUG_DBO, "Sample period %ld, buffer size %d pages.\n",
            (unsigned long)SMPL_PERIOD, NUM_WINDOWS);
    DM->Debug(DEBUG_DBO, "%s initialized.\n", __func__);
}

DBOManager::~DBOManager()
{
    pfm_terminate();

    if (MonitorThread != (pthread_t)-1)
        pthread_cancel(MonitorThread);

    if (PF->getProfile() & PROFILE_DBO)
        PrintProfile();
}

/*
 * createDBOManager()
 *  Entry function to create DBO manager.
 */
void DBOManager::createDBOManager()
{
    if (DBO)
        return;

    DBO = new DBOManager();
}

/*
 * fork_start()
 *  Stop DBO manager before performing fork.
 */
void DBOManager::fork_start()
{
    if (HybridMode == HYBRID_MULTIPLE || HybridMode == HYBRID_HPM)
    {
        pfm_terminate();

        llvm_spin_lock(&dbo_lock);
        if (MonitorThread != (pthread_t)-1)
        {
        pthread_cancel(MonitorThread);
        MonitorThread = (pthread_t)-1;
        }
        llvm_spin_unlock(&dbo_lock);
    }
}

/*
 * fork_end()
 *  Restart DBO manager after performing fork.
 */
void DBOManager::fork_end(int child)
{
    if (child == 1)
    {
        llvm_lock_init(&dbo_lock);
        MonitorThread = (pthread_t)-1;
    }
}

/*
 * dbo_thread_func()
 *  Monitor thread's routine. The monitor thread is forced to sleep forever in
 *  this function and is 'waked up' only through the signal handler as Linux
 *  kernel triggers a buffer overflow.
 */
void *dbo_thread_func(void *args)
{
    pid_t *MonitorThreadID = (pid_t *)args;
    *MonitorThreadID = gettid();

    llvm_spin_lock(&dbo_lock);
    MonitorInitialized = true;
    llvm_spin_unlock(&dbo_lock);

    for(;;)
        sleep(100);

    pthread_exit(0);
}

/*
 * getMonitorID()
 *  Return a unique ID for the monitored region.
 */
inline uint32_t DBOManager::getMonitorID()
{
    return MonitorID++;
}

/*
 * RegisterThread()
 *  Attach a thread for DBO monitoring. The thread has to call this function
 *  directly because we use gettid() to get its thread ID.
 */
void DBOManager::RegisterThread(int cpu_index)
{
    int fd;
    perf_event_desc_t *evd;
    pid_t tid = gettid();

    llvm_spin_lock(&dbo_lock);

    /* Setup the monitoring event. */
#ifdef USE_LBR
    evd = setupLBREvent(tid);
#else
    evd = setupEvent(tid);
#endif

    ThreadInfo *info = new ThreadInfo(cpu_index, evd);

    fd = evd->fd[0];
    ThreadMap[tid] = fd;
    ThreadRegistry[fd] = info;

    startMonitoring(evd);

    DM->Debug(DEBUG_DBO, "Start monitoring thread %d with event:\n", tid);
    for (int i = 0; i < evd->num_events; i++)
        DM->Debug(DEBUG_DBO, "\t%s\n", evd->name[i]);

    llvm_spin_unlock(&dbo_lock);
}

/*
 * UnregisterThread()
 *  Detach a thread from DBO monitoring.
 */
void DBOManager::UnregisterThread()
{
    pid_t tid = gettid();

    llvm_spin_lock(&dbo_lock);
    DM->Debug(DEBUG_DBO, "Stop monitoring thread %d\n", tid);
    ThreadInfo *info = ThreadRegistry[ThreadMap[tid]];
    ThreadProf.push_back(make_pair(info->getCPUIndex(), info->getProf()));
    ThreadRegistry.erase(ThreadMap[tid]);
    delete info;
    llvm_spin_unlock(&dbo_lock);
}

/* 
 * add_events()
 *  Insert the monitoring events.
 */
static int add_events(const char **events)
{
    int num_events = 0;

#ifdef USE_INSTRUCTION_RETIRED
    events[num_events] = PERF_COUNT_HW_INSTRUCTIONS;
#else
    events[num_events] = PERF_COUNT_HW_CPU_CYCLES;
#endif
    num_events++;

    if (PF->getProfile() & PROFILE_MEM)
    {
#ifdef CONFIG_PERF_EVENTS
        EnableMem = 0;
#else
        EnableMem = 1;
        LoadIdx = num_events;
        StoreIdx = num_events + 1;
        events[num_events++] = PERF_MEM_LOAD_RETIRED;
        events[num_events++] = PERF_MEM_STORE_RETIRED;
#endif
    }

    return num_events;
}

/*
 * setupEvent()
 *  Setup events to be monitored and return the event descriptor.
 */
perf_event_desc_t *DBOManager::setupEvent(pid_t tid)
{
    perf_event_desc_t *evd;
    static const char *gen_events[64];
    int num_events;

    num_events = add_events(gen_events);
    if (num_events == 0)
        DM->Error("%s: no event specified.\n", __func__);

    evd = (perf_event_desc_t *)llvm_malloc(sizeof(perf_event_desc_t));

    perf_setup_event_desc(evd, tid, num_events, SMPL_PERIOD, NUM_WINDOWS);
    perf_setup_events(evd, gen_events);
    perf_map_buffer(evd);
    perf_setup_signal(evd, MonitorThreadID, signum);

    return evd;
}

/*
 * setupLBREvent()
 *  Setup LBR to be monitored and return the event descriptor.
 */
perf_event_desc_t *DBOManager::setupLBREvent(pid_t tid)
{
    perf_event_desc_t *evd;

    evd = (perf_event_desc_t *)llvm_malloc(sizeof(perf_event_desc_t));

    perf_setup_event_desc(evd, tid, 1, SMPL_PERIOD, NUM_WINDOWS);
    perf_setup_lbr_events(evd);
    perf_map_buffer(evd);
    perf_setup_signal(evd, MonitorThreadID, signum);

    return evd;
}

/*
 * startMonitoring()
 *  Enable performance monitoring.
 */
inline void DBOManager::startMonitoring(perf_event_desc_t *evd)
{
    for (int i = 0; i < evd->num_events; i++)
    {
        if (evd->fd[i] == -1)
            break;
        if (pfm_start(evd->fd[i], NULL))
            DM->Error("%s: pfm_start failed.\n", __func__);
    }
}

/*
 * RegisterMonitoring()
 *  Attach a region for DBO monitoring.
 */
void DBOManager::RegisterMonitoring(CodeInfo *Code)
{
    unsigned long Start = Code->HostPC;
    unsigned long End = Start + Code->HostSize;
    int RangeIdx = (Code->isBlock) ? 0 : 1;
    target_ulong GuestPC = Code->GuestPC;

    llvm_spin_lock(&dbo_lock);

    /* If the region of guest PC is already in the monitoring list, this is
     * because a new merged region is going to replace it. Thus, detach the
     * old region. */
    if (GuestCodes.count(GuestPC))
    {
        CodeInfo *Retired = GuestCodes[GuestPC];
        RetiredCodes.push_back(Retired);
        RetiredMap[GuestPC] = Retired;
    }

    Code->MonitorID = getMonitorID();
    GuestCodes[GuestPC] = Code;
    HostCodes[Code->HostPC] = Code;

    /* Update the range of the monitoring space. */
    if (Start < MonitorStart[RangeIdx])
        MonitorStart[RangeIdx] = Start;
    if (End > MonitorEnd[RangeIdx])
        MonitorEnd[RangeIdx] = End;

    llvm_spin_unlock(&dbo_lock);
}

/*
 * UnregisterMonitoring()
 *  Detach a region from DBO monitoring.
 */
void DBOManager::UnregisterMonitoring(CodeInfo *Code)
{
    DM->Error("%s: fix me.\n", __func__);
}

/*
 * resetMonitoring()
 *  Reset DBO monitroing.
 */
void DBOManager::resetMonitoring()
{
    DM->Debug(DEBUG_DBO, "%s: reset monitoring codes.\n", __func__);

    llvm_spin_lock(&dbo_lock);

    MonitorID = 0;
    GuestCodes.clear();
    HostCodes.clear();
    RetiredCodes.clear();
    RetiredMap.clear();

    for (int i = 0; i < 2; i++)
    {
        MonitorStart[i] = (unsigned long)-1;
        MonitorEnd[i] = 0;
    }

    llvm_spin_unlock(&dbo_lock);
}

/*
 * PrintTopProf()
 *  Display the information and LLVM IR of the hottest regions.
 */
static void PrintTopProf(int Num, ProfileMap &CollectedProf)
{
    uint64_t TotalCount = 0;

    ProfileVec TotalProf;
    for (ProfileMap::iterator I = CollectedProf.begin(),
            E = CollectedProf.end(); I != E; I++)
    {
        SmplProfile *Prof = I->second;
        TotalProf.push_back(Prof);
        TotalCount += Prof->getCount(SMPL_TOTAL);
    }
    SmplProfile::sort(TotalProf);

    fprintf(stderr, "total sample count %ld.\n", TotalCount);
    for (int i = 0, e = TotalProf.size(); i < e && i < Num; i++)
    {

        SmplProfile *Prof = TotalProf[i];
        CodeInfo *Code = Prof->getRegion();
        if (Code->isBlock == true)
            continue;

        TraceInfo *Trace = static_cast<TraceInfo*>(Code);
        uint64_t Count = Prof->getCount(SMPL_TOTAL);
        fprintf(stderr, "Top %d: smpl=%"PRId64"(%.1f%%) version=%d block=%d "
                "loop=%d\n", i+1, Count, (double)Count*100/TotalCount,
                Trace->Version, Trace->NumBlock, Trace->NumLoop);
        fprintf(stderr, "  GuestPC: ");
        for (int j = 0, k = Trace->PC.size(); j < k; j++)
            fprintf(stderr, "0x%x ", Trace->PC[j]);
        fprintf(stderr, "\n");

        set<target_ulong> &Link = Code->Link;
        fprintf(stderr, "  OutLink: ");
        for (set<target_ulong>::iterator LI = Link.begin(), LE = Link.end();
                LI != LE; LI++)
            fprintf(stderr, "0x%x ", *LI);
        fprintf(stderr, "\n");

#if 0
        for (int j = 0, k = Trace->TB.size(); j < k; j++)
        {
            TranslationBlock *TB = Trace->TB[j];
            int disas_flags = 0;

#if defined(TARGET_I386)
#if defined(TARGET_X86_64)
            if ((TB->flags >> HF_CS64_SHIFT) & 1)
                disas_flags = 2;
            else
#endif
                disas_flags = !((TB->flags >> HF_CS32_SHIFT) & 1);
#else
            CPUState env;
            void *backup = tcg_copy_state(&env, TB);
            disas_flags = env.thumb;
            tcg_restore_state(&env, backup, 1);
#endif

            fprintf(stderr, "----------------\n");
            fprintf(stderr, "IN: [size=%d]\n", TB->size);
            target_disas(stderr, TB->pc, TB->size, disas_flags);
            fprintf(stderr, "\n");
        }
#endif

        /* Dump LLVM IR and translated host code of this region. */
        Code->Func->dump();
        fprintf(stderr, "OUT: [size=%d]\n", Code->HostSize);
        disas(stderr, (void *)Code->HostPC, Code->HostSize);
        fprintf(stderr, "\n");
    }
}

/*
 * PrintProfile()
 *  Display the hottest M regions of the whole execution time for each threads.
 *  The default value of M is 5.
 */
void DBOManager::PrintProfile()
{
    llvm_spin_lock(&dbo_lock);
    for (int i = 0, e = ThreadProf.size(); i < e; i++)
    {
        ProfileMap &CollectedProf = ThreadProf[i].second;
        fprintf(stderr, "-------------------------\n");
        fprintf(stderr, "Thread %d: ", ThreadProf[i].first);
        PrintTopProf(MAX_PRINT_TRACE, CollectedProf);
    }
    llvm_spin_unlock(&dbo_lock);
}

/*
 * getThreadInfo()
 *  Return thread information according to the file handle.
 */
ThreadInfo *DBOManager::getThreadInfo(int fd)
{
    return ThreadRegistry.count(fd) ? ThreadRegistry[fd] : NULL;
}

/*
 * dbo_default_handler()
 *  Buffer overflow handler of hardware performance samples.
 */
void dbo_default_handler(int signum, siginfo_t *siginfo, void *data)
{
    int ret, fd;
    ThreadInfo *info = NULL;
    perf_event_desc_t *evd;
    SmplData Smpl;
    uint64_t *p;

    llvm_spin_lock(&dbo_lock);

    fd = siginfo->si_fd;
    info = DBO->getThreadInfo(fd);
    if (info == NULL)
        return;

    evd = info->evd;
    evd->num_smpl_processed = 0;
    p = (uint64_t *)evd->smpl;

    /* Read all sampled results from the buffer. */
    for (;;)
    {
        ret = perf_read_buffer(evd);
        if (ret)
            break;

        Smpl.PushIP(*p);

        if (EnableMem == 1)
        {
            info->Exec.NumLoads += p[LoadIdx];
            info->Exec.NumStores += p[StoreIdx];
        }
    }

    info->ProcessIPSmpl(&Smpl);

    if (pfm_restart(fd))
        DM->Error("%s: pfm_restart failed: %s.\n", __func__, strerror(errno));

    llvm_spin_unlock(&dbo_lock);
}

/*
 * dbo_lbr_default_handler()
 *  Buffer overflow handler of hardware performance samples.
 */
void dbo_lbr_default_handler(int signum, siginfo_t *siginfo, void *data)
{
    int ret, fd;
    ThreadInfo *info = NULL;
    perf_event_desc_t *evd;
    uint64_t *ip;
    SmplData Smpl;

    llvm_spin_lock(&dbo_lock);

    fd = siginfo->si_fd;
    info = DBO->getThreadInfo(fd);
    evd = info->evd;
    evd->num_smpl_processed = 0;
    ip = (uint64_t *)evd->smpl;

    /* Read all sampled results from the buffer. */
    for (;;)
    {
        ret = perf_read_lbr_buffer(evd);
        if (ret)
            break;

        Smpl.PushIP(*ip);
    }

    info->ProcessLBRSmpl(&Smpl);

    if (pfm_restart(fd))
    {
        if (errno != EBUSY)
            DM->Error("%s: pfm_restart failed: %s.\n", __func__, strerror(errno));
    }
    llvm_spin_unlock(&dbo_lock);
}

/*
 * Thread Information
 */
ThreadInfo::ThreadInfo(int cpu_index, perf_event_desc_t *event_desc)
{
#if defined(CONFIG_HPM)
    int Size = 2;
#else
    int Size = 8;
#endif

    CPUIndex = cpu_index;
    evd = event_desc;

    ProfileWindow = new WindowManager<ProfileVec>(Size);
    ProfileVec *WindowList = ProfileWindow->getWindowList();
    for (int i = 0; i < Size; i++)
        WindowList[i].clear();
}

ThreadInfo::~ThreadInfo()
{
    for(int i = 0; i < evd->num_events; i++)
    {
        if (evd->fd[i] == -1)
            break;
        pfm_stop(evd->fd[i]);
    }

    for(int i = 0; i < evd->num_events; i++)
    {
        if (evd->fd[i] == -1)
            break;
        close(evd->fd[i]);
    }

    PF->setExecProfile(CPUIndex, Exec);
}

/*
 * LookupProfileWithIP()
 *  Return the region profile where the IP locates.
 */
SmplProfile *ThreadInfo::LookupProfileWithIP(uint64_t IP)
{
    HostCodeMap::iterator MI = DBO->HostCodes.upper_bound(IP);
    CodeInfo *Code = (--MI)->second;
    
    if (CollectedProf.count(Code->MonitorID) == 0)
    {
        /* If this region is derived from the previous one (from region
         * merging), the initail sample count of the new region is set to the
         * value of the old region and the sample count of the old region is
         * reset so it will not affect the formation of the next 90% cover
         * set for future trace merging. */
        target_ulong GuestPC = Code->GuestPC;
        uint64_t InitCount = 0;
        if (DBO->RetiredMap.count(GuestPC))
        {
            uint32_t RetiredID = DBO->RetiredMap[GuestPC]->MonitorID;
            if (CollectedProf.count(RetiredID))
            {
                SmplProfile *RetiredProf = CollectedProf[RetiredID];
                InitCount = RetiredProf->getCount(SMPL_TOTAL);
                RetiredProf->setCount(0);
            }
        }

        CollectedProf[Code->MonitorID] = new SmplProfile(Code, InitCount);
    }

    return CollectedProf[Code->MonitorID];
}

/*
 * getSortedProfile()
 *  Get the sorted regions in decending order.
 */
inline void ThreadInfo::getSortedProfile(ProfileVec &Profs, int Mode,
        ProfileVec &SortedProf, uint64_t &Count)
{
    Count = 0;

    for (int i = 0, e = Profs.size(); i < e; i++)
    {
        SmplProfile *Prof = Profs[i];
        SortedProf.push_back(Prof);
        Count += Prof->getNormalizedCount(Mode);
    }

    SmplProfile::sort(SortedProf, Mode);
}

/*
 * getTopProfile()
 *  Get the top regions based on the `Threshold' (ratio).
 */
inline void ThreadInfo::getTopProfile(ProfileVec &Profs, int Mode,
        double Threshold, ProfileVec &TopProf)
{
    ProfileVec SortedProf;
    uint64_t TotalCount = 0;
    uint64_t Count;

    getSortedProfile(Profs, Mode, SortedProf, Count);
    Count *= Threshold;
    for (int i = 0, e = SortedProf.size(); i < e; i++)
    {
        TopProf.push_back(SortedProf[i]);
        TotalCount += SortedProf[i]->getNormalizedCount(SMPL_TOTAL);
        if (TotalCount > Count)
            return;
    }
}

/*
 * RecordBreakdown()
 *  Calculate the breakdown of time in terms of the block code trace (Block),
 *  the optimized code cache (Trace) and other (Other).
 */
void ThreadInfo::RecordBreakdown(IPVec &IPs)
{
    uint64_t BlockStart, BlockEnd;
    uint64_t TraceStart, TraceEnd;

    BlockStart = (uint64_t)qemu_get_cache_address();
    BlockEnd = BlockStart + qemu_get_cache_size();
    TraceStart = (uint64_t)Env->getCodeCacheAddr();
    TraceEnd = TraceStart + Env->getCodeCacheSize();

    for (int i = 0, e = IPs.size(); i < e; i++)
    {
        uint64_t IP = IPs[i];
        if (IP >= BlockStart && IP < BlockEnd)
            Exec.Block++;
        else if (IP >= TraceStart && IP < TraceEnd)
            Exec.Trace++;
        else
            Exec.Other++;
    }
}

/*
 * CommitProfile()
 *  Associate the sampled restuls to the region profiles.
 */
void ThreadInfo::CommitProfile(IPVec &IPs, ProfileVec &Profs)
{
    int RangeIdx;
    map<SmplProfile*,IPVec> LastProfile;

    for (int i = 0, e = IPs.size(); i < e; i++)
    {
        uint64_t IP = IPs[i];
        for (RangeIdx = 0; RangeIdx < 2; RangeIdx++)
        {
            if (IP >= DBO->MonitorStart[RangeIdx] && 
                    IP < DBO->MonitorEnd[RangeIdx])
                break;
        }
        if (RangeIdx == 2)
            continue;

        SmplProfile *Prof = LookupProfileWithIP(IP);
        LastProfile[Prof].push_back(IP);
    }
    
    for (map<SmplProfile*,IPVec>::iterator I = LastProfile.begin(),
            E = LastProfile.end(); I != E; I++)
    {
        SmplProfile *Prof = I->first;
        Prof->addCount(I->second);
        Profs.push_back(Prof);
    }
}

/*
 * ComputeCandidateRegions()
 *  Find the candidate regions to be merged. A candidate region has three
 *  criteria: (1) it must be in the `stable' state; (2) it must be `hot';
 *  (3) its sample count must be larger than a threshold. 
 *  A region is in the stable state if it appears in ALL the last N
 *  monitoring windows.
 */
void ThreadInfo::ComputeCandidateRegions(ProfileVec &Profs, int Mode,
        CodeVec &Regions)
{
    int Size = ProfileWindow->getSize();
    ProfileVec *WindowList = ProfileWindow->getWindowList();
    ProfileVec &CurrWindow = ProfileWindow->getCurrWindow();
    ProfileWindow->moveNext();

    CurrWindow.clear();
    CurrWindow = ProfileVec(Profs);

    /* For each region, count how many windows it appears. */
    map<SmplProfile*,int> ProfileUsed;
    for (int i = 0; i < Size; i++)
    {
        ProfileVec &Window = WindowList[i];
        for (int j = 0, e = Window.size(); j < e; j++)
            ProfileUsed[Window[j]]++;
    }

    /* Only select the regions that appears in all windows. */
    ProfileVec SortedProf;
    for (map<SmplProfile*,int>::iterator I = ProfileUsed.begin(),
            E = ProfileUsed.end(); I != E; I++)
    {
        if (I->second == Size)
            SortedProf.push_back(I->first);
    }

    /* Filter out the regions that are under the threshold. */
    SmplProfile::sort(SortedProf, Mode);
    for (ProfileVec::iterator I = SortedProf.begin(), E = SortedProf.end();
            I != E; I++)
    {
        SmplProfile *Prof = *I;
        CodeInfo *Code = Prof->getRegion();
        if (Code->isBlock == false &&
                Prof->getCount(Mode) < (uint64_t)Code->HostSize)
            continue;
        Regions.push_back(Code);
    }
}

/*
 * ProcessIPSmpl()
 *  Process the sampled results in the last interval.
 */
void ThreadInfo::ProcessIPSmpl(SmplData *Smpl)
{
    ProfileVec LastProf;
    ProfileVec TopProf;
    CodeVec Regions;

    if (PF->getProfile() & PROFILE_BREAKDOWN)
        RecordBreakdown(Smpl->IPList);

    /* Add sample data to the sample profile. */
    CommitProfile(Smpl->IPList, LastProf);

    /* Collect the regions within the 90% cover set. */
    getTopProfile(LastProf, SMPL_TOTAL, 0.9, TopProf);

    ComputeCandidateRegions(TopProf, SMPL_TOTAL, Regions);
    if (Regions.size() < 2)
        return;

    TryConnectRegions(Regions);
}

void ThreadInfo::ProcessLBRSmpl(SmplData *Smpl)
{
    ProfileVec LastSrc;
    ProfileVec LastDst;

    CommitProfile(Smpl->LBRSrcList, LastSrc);
    CommitProfile(Smpl->LBRDstList, LastDst);

    DM->Error("%s: LBR not supported.\n", __func__);
}

void PrintLink(CodeVec &Regions)
{
    int NumRegion = Regions.size();

    for (int i = 0; i < NumRegion; i++)
    {
        CodeInfo *Code = Regions[i];
        set<target_ulong> &Link = Code->Link;
        fprintf(stderr, "%3d (0x%08x) %d: ", Code->MonitorID, Code->GuestPC, Code->State);
        for (set<target_ulong>::iterator I = Link.begin(), E = Link.end(); I != E; I++)
            fprintf(stderr, "0x%08x ", *I);
        fprintf(stderr, "\n");
    }
}

bool isSubset(set<target_ulong> &Set1, set<target_ulong> &Set2)
{
    if (includes(Set1.begin(), Set1.end(), Set2.begin(), Set2.end()))
        return true;
    if (includes(Set2.begin(), Set2.end(), Set1.begin(), Set1.end()))
        return true;

    return false;
}

void ThreadInfo::ExpandRegion()
{

}

#if defined(CONFIG_HPM)
void ThreadInfo::TryConnectRegions(CodeVec &Regions)
{
    int NumRegion = Regions.size();
    SuccMap CodeSucc;
    CodeSet MergedHead;

    DM->Debug(DEBUG_DBO, "%d regions have samples\n", NumRegion);

#if 0
    PrintLink(Regions);

    /* Build successor tree of regions. */
    for (int i = 0; i < NumRegion; i++)
    {
        CodeInfo *Pred = Regions[i];
        if (Pred->State != 0)
            continue;
        set<target_ulong> &Link = Pred->Link;
        for (int j = 0; j < NumRegion; j++)
        {
            CodeInfo *Succ = Regions[j];
            if (Succ->State == 0 && Link.count(Succ->GuestPC))
                CodeSucc[Pred].push_back(Succ);
        }
    }

    for (int i = 0; i < NumRegion; i++)
        FindLoopHeader(CodeSucc, Regions[i], MergedHead);
    if (MergedHead.size() == 0)
        for (int i = 0; i < NumRegion; i++)
        MergedHead.insert(Regions[i]);

    for (int i = 0; i < NumRegion; i++)
    {
        if (MergedHead.count(Regions[i]) == 0)
            continue;

        CodeSet LinkedNodes;
        FindLinkedNodes(CodeSucc, Regions[i], LinkedNodes);
        LinkedNodes.erase(Regions[i]);
        for (CodeSet::iterator I = LinkedNodes.begin(), E = LinkedNodes.end();
                I != E; I++)
        {
            if (MergedHead.count(*I) == 0)
                continue;
            MergedHead.erase(*I);
        }
    }

    /* Generate CFG */
    for (CodeSet::iterator I = MergedHead.begin(), E = MergedHead.end();
            I != E; I++)
    {
        CodeInfo *Code = *I;

        if (CodeSucc[Code].empty())
            continue;

        int Version = 0;
        Node2Map NodeMapping;
        CompactInfo CI;

        Code->State = 1;
        BuildCompactCFG(Code, CodeSucc, NodeMapping, CI, Version);
        LinkCompactCFG(CI);

        if (NodeMapping.count(Code->CFG) == 0)
            DM->Error("%s: internal error\n", __func__);

//        ExpandRegion();

//        llvm_regen_trace(Code->CFG, 1, Version+1);
        llvm_regen_trace(NodeMapping[Code->CFG], NodeMapping.size(), Version+1);
    }

    for (CodeSet::iterator I = MergedHead.begin(), E = MergedHead.end();
            I != E; I++)
        fprintf(stderr, "head = 0x%x\n", (*I)->GuestPC);
#endif
}

#else
/*
 * TryConnectRegions()
 *  Determine if the given regions can be connected and try to merge them.
 */
void ThreadInfo::TryConnectRegions(CodeVec &Regions)
{
    int NumRegion;
    SuccMap CodeSucc;
    CodeSet MergedHead;

    NumRegion = Regions.size();
    DM->Debug(DEBUG_DBO, "num = %d\n", NumRegion);

#if 0
    PrintLink(Regions);
#endif

    /* Build successor tree of the regions. */
    for (int i = 0; i < NumRegion; i++)
    {
        CodeInfo *Pred = Regions[i];
        if (Pred->State != 0)
            continue;
        set<target_ulong> &Link = Pred->Link;
        for (int j = 0; j < NumRegion; j++)
        {
            CodeInfo *Succ = Regions[j];
            if (i == j || Succ->State != 0)
                continue;
            if (Link.count(Succ->GuestPC) == 0)
                continue;
            CodeSucc[Pred].push_back(Succ);
        }
    }

    /* Find the loopheader in the successor tree. */
    for (int i = 0; i < NumRegion; i++)
        FindLoopHeader(CodeSucc, Regions[i], MergedHead);

    /* Traverse the successor tree and search for any cyclic path. */
    for (int i = 0; i < NumRegion; i++)
    {
        if (MergedHead.count(Regions[i]) == 0)
            continue;

        /* Remove the regions that are dominated by another region. */
        CodeSet LinkedNodes;
        FindLinkedNodes(CodeSucc, Regions[i], LinkedNodes);
        LinkedNodes.erase(Regions[i]);
        for (CodeSet::iterator I = LinkedNodes.begin(), E = LinkedNodes.end();
                I != E; I++)
        {
            if (MergedHead.count(*I) == 0)
                continue;
            MergedHead.erase(*I);
        }
    }

    /* Merge regions */
    for (CodeSet::iterator I = MergedHead.begin(), E = MergedHead.end(); I != E; I++)
    {
        int Version = 0;
        Node2Map NodeMapping;
        CompactInfo CI;
        CodeInfo *Code = *I;

        /* Skip the region with no successor (i.e. no cyclic path). */
        if (CodeSucc[Code].size() == 0)
            continue;

        Code->State = 1;
        BuildCompactCFG(Code, CodeSucc, NodeMapping, CI, Version);
        LinkCompactCFG(CI);

        if (NodeMapping.count(Code->CFG) == 0)
            DM->Error("%s: internal error\n", __func__);

        /* Put this merged region in the optimization request queue. */
        llvm_regen_trace(NodeMapping[Code->CFG], NodeMapping.size(), Version+1);
    }
}
#endif

void ThreadInfo::Print()
{
    int idx = 0;
    int flag = 0;
    uint64_t TotalCount = 0, NinetyCount = 0;

    ProfileVec TotalProf;
    ProfileVec SortedProf;

    for (ProfileMap::iterator I = CollectedProf.begin(), E = CollectedProf.end(); I != E; I++)
        TotalProf.push_back(I->second);

    getSortedProfile(TotalProf, SMPL_TOTAL, SortedProf, NinetyCount);

    NinetyCount *= 0.9;
    for (ProfileVec::iterator I = SortedProf.begin(), E = SortedProf.end(); I != E; I++)
    {
        uint64_t Count;
        SmplProfile *Prof = *I;

        idx++;
        Count = Prof->getNormalizedCount(SMPL_TOTAL);
        TotalCount += Count;

        CodeInfo *Code = Prof->getRegion();
        set<target_ulong> &Link = Code->Link;
        fprintf(stderr, "%4d (0x%08x) (%"PRId64")(%f): ", 
                Code->MonitorID, Code->GuestPC, Count, (double)Count/Code->HostSize);
        for (set<target_ulong>::iterator LI = Link.begin(), LE = Link.end(); LI != LE; LI++)
            fprintf(stderr, "0x%08x ", *LI);

        fprintf(stderr, "\n");

        if (flag == 0 && TotalCount > NinetyCount)
        {
            fprintf(stderr, "-------------- Num = %d ----------------------\n", idx);
            flag = 1;
        }
    }
}

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * dbo_init()
 *  Wrapper function to create DBO service.
 */
int dbo_init(void)
{
    DBOManager::createDBOManager();
    return 0;
}

/*
 * dbo_finalize()
 *  Wrapper function to destroy the DBO service.
 */
int dbo_finalize(void)
{
    if (DBO)
    {
        delete DBO;
        DBO = NULL;
    }
    return 0;
}

/*
 * dbo_register_thread()
 *  Wrapper function to attach a thread for DBO monitoring.
 */
int dbo_register_thread(int cpu_index)
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->RegisterThread(cpu_index);
    return 0;
}

/*
 * dbo_deregister_thread()
 *  Wrapper function to detach a thread from DBO monitoring.
 */
int dbo_deregister_thread()
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->UnregisterThread();
    return 0;
}

/*
 * dbo_register_region()
 *  Wrapper function to attach a region for DBO monitoring.
 */
int dbo_register_region(CodeInfo *Code)
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->RegisterMonitoring(Code);
    return 0;
}

/*
 * dbo_reset_monitoring()
 *  Wrapper function to reset DBO monitroing.
 */
int dbo_reset_monitoring(void)
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->resetMonitoring();
    return 0;
}

/*
 * dbo_fork_start()
 *  Wrapper function to stop DBO before performing fork.
 */
void dbo_fork_start(void)
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->fork_start();
}

/*
 * dbo_fork_end()
 *  Wrapper function to restart DBO after performing fork.
 */
void dbo_fork_end(int child)
{
    if (DBO == NULL)
        DM->Error("%s: DBOManager not initialized.\n", __func__);

    DBO->fork_end(child);
}

#ifdef __cplusplus
}
#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

