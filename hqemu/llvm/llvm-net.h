/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_NET_H
#define __LLVM_NET_H

#include "tcp.h"
#include "llvm-opc.h"

#define INVALID_TICKET  0
#define DIGEST_LENGTH   16

#define encode_char(p,v)        do { *p = (v); p += 1; } while (0)
#define decode_char(p,v)        do { (v) = *p; p += 1; } while (0)
#define encode_int16(p,v)       do { *(int16_t *)p = (v);  p += 2; } while (0)
#define decode_int16(p,v)       do { (v) = *(int16_t *)p;  p += 2; } while (0)
#define encode_uint16(p,v)      do { *(uint16_t *)p = (v); p += 2; } while (0)
#define decode_uint16(p,v)      do { (v) = *(uint16_t *)p; p += 2; } while (0)
#define encode_int32(p,v)       do { *(int32_t *)p = (v);  p += 4; } while (0)
#define decode_int32(p,v)       do { (v) = *(int32_t *)p;  p += 4; } while (0)
#define encode_uint32(p,v)      do { *(uint32_t *)p = (v); p += 4; } while (0)
#define decode_uint32(p,v)      do { (v) = *(uint32_t *)p; p += 4; } while (0)
#define encode_uint64(p,v)            \
    do {                              \
        uint32_t *t = (uint32_t *)&v; \
        encode_uint32(p, t[0]);       \
        encode_uint32(p, t[1]);       \
    } while(0)
#define decode_uint64(p,v)            \
    do {                              \
        uint32_t *t = (uint32_t *)&v; \
        decode_uint32(p, t[0]);       \
        decode_uint32(p, t[1]);       \
    } while(0)
#define encode_string(p,s)      do { strcpy(p,s);   p += strlen(s) + 1; } while (0)
#define decode_string(p,s)      do { s = p; p += strlen(p) + 1; } while (0)
#define encode_type(p,v)        do { *(uint8_t *)p = (uint8_t)(v);  p += sizeof(uint8_t); } while (0)
#define decode_type(p,v)        do { (v) = (TCGType)(*(uint8_t *)p);  p += sizeof(uint8_t); } while (0)
#define encode_raw(p,v,n)       do { memcpy(p,v,n); p += n; } while(0)
#define decode_raw(p,v,n)       do { memcpy(v,p,n); p += n; } while(0)
#define endecode_skip(p,d)      do { p += d; } while(0)

#define encode_guest_ulong(p,v)     do { *(target_ulong *)p = (v); p += sizeof(target_ulong); } while (0)
#define decode_guest_ulong(p,v)     do { (v) = *(target_ulong *)p; p += sizeof(target_ulong); } while (0)
#define encode_host_long(p,v)       do { *(tcg_target_long *)p = (v); p += sizeof(tcg_target_long); } while (0)
#define decode_host_long(p,v)       do { (v) = *(tcg_target_long *)p; p += sizeof(tcg_target_long); } while (0)
#define encode_host_ulong(p,v)      do { *(tcg_target_ulong *)p = (v); p += sizeof(tcg_target_ulong); } while (0)
#define decode_host_ulong(p,v)      do { (v) = *(tcg_target_ulong *)p; p += sizeof(tcg_target_ulong); } while (0)

typedef map< uintptr_t, string > NestedFuncMap;
typedef map< uintptr_t, string > GlobalMap;
typedef map< string, int > SymbolMap;
typedef vector< tcg_target_long > SymbolVec;
typedef map< CodeKey, TranslatedCode* > NetCodeMap;

class JITSyncData
{
public:
    NestedFuncMap NestedFunc;
    GlobalMap GlobalVariable;
    SymbolMap ExternalSymbolIdx;
    SymbolVec ExternalSymbol;
};

enum
{
    TAG_SYNC = 0,
    TAG_TRACE,
    TAG_SMPL,
    TAG_FLUSH,
};

enum
{
    ARCH_X86 = 0,
    ARCH_X86_64,
    ARCH_ARM,
};

struct ImageInfo
{
    TranslationBlock TB;
    char *Code;
};

struct TraceRequest
{
    uint32_t Ticket;
    uint32_t TraceID;
    int Count;
    target_ulong CSBase;
    target_ulong *PCs;
    NodeMap CFGMap;
    ImageInfo *Images;
};

struct EmulationInfo
{
    int Arch;
    int BaseRegNo;
};

struct SyncHeader
{
    int Arch;
    int BaseRegNo;
    int NumGlobal;
    int NumHelper;
    int NumNestedFunc;
    int NumGlobalVariable;
    int NumSymbol;
    tcg_target_long FrameStart;
    uint64_t GuestBase;
    uint64_t ProfileMode;
    int UseStatic;
    char Digest[DIGEST_LENGTH*2+1];
#if defined(TARGET_I386)
    int X86FPOff[3];
#endif
};

struct TraceHeader
{
    uint32_t Ticket;
    uint32_t Mode;
    uint64_t TraceKey;
    target_ulong CSBase;
};

struct TracePackHeader
{
    uint64_t TraceKey;
    int NumTrace;
};

struct TraceAckHeader
{
    uint32_t TraceID;
    uint32_t GuestICount;
    uint32_t CodeSize;
    uint32_t NumExit;
    uint32_t NumExec;
    uint32_t NumConstPool;
    uint32_t ConstPoolSize;
    tcg_target_ulong MaxKey;
};

struct FlushHeader
{
    uint32_t Ticket;
};

struct ClientTraceInfo
{
    ClientTraceInfo(TranslatedCode *tc) : Cached(true), TC(tc)
    {
        Trace = static_cast<TraceInfo *>(TC->Info);
    }

    bool Cached;
    TranslatedCode *TC;
    TraceInfo *Trace;
};

class PermanentCodeInfo
{
public:
    PermanentCodeInfo(string D) { Digest  = D; }
    ~PermanentCodeInfo() {}

    string Digest;
    NetCodeMap CodeRegions;
    TraceChainMap TraceChain;
    ChainVec ChainPoint;

    vector<ClientTraceInfo*> TraceProfile;
    vector<ClientTraceInfo*> SortedTraceProfile;
};

class PermanentCodeCache
{
public:
    PermanentCodeCache() {}
    ~PermanentCodeCache()
    {
        for (map<string, PermanentCodeInfo*>::iterator I = Program.begin(),
                E = Program.end(); I != E; I++)
            delete I->second;
    }

    map<string, PermanentCodeInfo*> Program;

    PermanentCodeInfo *Lookup(string Digest)
    {
        if (Program.count(Digest) == 0)
            Program[Digest] = new PermanentCodeInfo(Digest);

        return Program[Digest];
    }
};

class ClientData
{
public:
    ClientData(int Hndl)
    {
        Sock = Hndl;
        EnablePrediction = false;
        CodeRegions.clear();
        reset();
    }

    int Sock;
    bool UseStatic;
    PermanentCodeInfo *PCode;

    NetCodeMap CodeRegions;
    NetCodeMap RemoteCodeRegions;
    TraceChainMap TraceChain;
    ChainVec ChainPoint;
    JITSyncData JSData;

    bool EnablePrediction;
    vector<ClientTraceInfo*> TraceProfile;
    vector<ClientTraceInfo*> SortedTraceProfile;

    inline void reset()
    {
        RemoteCodeRegions.clear();

        vector<ClientTraceInfo*> &P = (UseStatic == 0) ? TraceProfile : PCode->TraceProfile;
        for (int i = 0, e = TraceProfile.size(); i < e; i++)
            P[i]->Cached = false;
    }

    inline uint32_t getTraceID()
    {
        if (UseStatic)
            return PCode->TraceProfile.size();
        return TraceProfile.size();
    }
    void setTrace(TranslatedCode *TC)
    {
        CodeKey CKey = CodeKey(TC->PC, TC->CSBase);
        if (UseStatic == 0)
        {
            CodeRegions[CKey] = TC;
            TraceProfile.push_back(new ClientTraceInfo(TC));
            SortedTraceProfile.push_back(TraceProfile.back());
        }
        else
        {
            PCode->CodeRegions[CKey] = TC;
            PCode->TraceProfile.push_back(new ClientTraceInfo(TC));
            PCode->SortedTraceProfile.push_back(PCode->TraceProfile.back());
        }
        RemoteCodeRegions[CKey] = TC;
    }

    inline bool LookupTrace(CodeKey &CKey)
    {
        return (UseStatic == 0) ? CodeRegions.count(CKey) : PCode->CodeRegions.count(CKey);
    }

    inline TranslatedCode *getCachedTrace(CodeKey &CKey)
    {
        return (UseStatic == 0) ? CodeRegions[CKey] : PCode->CodeRegions[CKey];
    }

    inline void CacheTrace(CodeKey &CKey, TranslatedCode *TC)
    {
        if (UseStatic)
        {
            CacheTraceStatic(CKey, TC);
            return;
        }

        TraceInfo *Trace = static_cast<TraceInfo*>(TC->Info);
        if (TraceProfile[Trace->ID]->Cached == false)
        {
            RemoteCodeRegions[CKey] = TC;
            TraceProfile[Trace->ID]->Cached = true;
        }
    }

    inline void CacheTraceStatic(CodeKey &CKey, TranslatedCode *TC)
    {
        TraceInfo *Trace = static_cast<TraceInfo*>(TC->Info);
        if (PCode->TraceProfile[Trace->ID]->Cached == false)
        {
            RemoteCodeRegions[CKey] = TC;
            PCode->TraceProfile[Trace->ID]->Cached = true;
        }
    }

    inline void removeTrace(CodeKey &CKey, TranslatedCode *TC)
    {
        TraceInfo *Trace = static_cast<TraceInfo*>(TC->Info);
        if (UseStatic == 0)
        {
            TraceProfile[Trace->ID]->Cached = false;
            CodeRegions.erase(CKey);
        }
        else
        {
            PCode->TraceProfile[Trace->ID]->Cached = false;
            PCode->CodeRegions.erase(CKey);
        }

        RemoteCodeRegions.erase(CKey);
        delete TC;
    }

    inline bool PredictionEnabled()  { return EnablePrediction; }
};

class PeerInfo
{
public:
    PeerInfo(bool is_server);

    bool isInitialized;
    bool isServer;
    bool UseStatic;
    EmulationInfo Emu;
    CPUState *CPU;

    int Sock;
    char *Buf;
    struct network_fns *Net;
    uint32_t Ticket;

    uint32_t GenTicket();
    bool Sync(int Hndl, int Size = 0);
};

class ServerInfo : public PeerInfo
{
public:
    ServerInfo();
    ~ServerInfo();

    NetCodeMap *CachedCodeRegions;
    PatchInfo *CachedPatch;
    TraceChainMap *CachedTraceChain;
    ChainVec *CachedChainPoint;

    void Daemonize();
    void UpdateTranslator();
    ClientData *CreateTicket(int Hndl, uint32_t &Ack, bool isStatic, char *Digest);
    int ProcessFlush();
    int ProcessTrace(int Size);
    int SendTraceCached(ClientData *Client, GraphNode *CFG, TraceRequest &Req);
    int SendTraceEmitted(ClientData *Client, vector<TranslatedCode*> &SendTC, uint64_t TraceKey);
};

class ClientInfo : public PeerInfo
{
private:
    char *TraceBuf;

public:
    ClientInfo();
    ~ClientInfo();

    int Connect();
    int SendTrace(CPUState *env, OptimizationInfo *Opt);
    int RecvTrace(int Size = 0);
    int Flush();
    void setTraceBuf(char *buf)   { TraceBuf = buf; }
};

#if defined(CONFIG_NET)
int net_init();
int net_finalize();
int net_gen_trace(CPUState *env, OptimizationInfo *Opt, bool force_regen);
int net_flush();
void net_fork_start();
void net_fork_end(int child);
#else
#define net_init()           do {} while(0)
#define net_finalize()       do {} while(0)
#define net_gen_trace(a,b,c) do {} while(0)
#define net_flush()          do {} while(0)
#define net_fork_start()     do {} while(0)
#define net_fork_end(a)      do {} while(0)
#endif

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

