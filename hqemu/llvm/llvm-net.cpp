/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <sys/utsname.h>
#include "llvm-profile.h"
#include "llvm-dbo.h"
#include "llvm-wrapper.h"
#include "llvm-net.h"
#include "JIT.h"
#include "qemu-timer.h"

#define USE_CALLBACK
#define MAX_BUF_SIZE   (1024 * 1024)

uint32_t SendSize;
uint32_t RecvSize;

extern LLVMEnv *Env;
extern LLVMDebug *DM;
extern ProfileFactory *PF;
extern int HybridMode;
extern char digest[129];
extern int llvm_flush;
extern int tb_flush_version;
extern llvm_lock_t llvm_global_lock;
extern uint8_t *llvm_tb_ret_addr;

static LLVMTranslator *Translator;
static PermanentCodeCache PermanentCache;
static map<uint32_t, ClientData*> ClientMgr;
static vector<TraceInfo*> TraceComplete;
static vector<unsigned long> TraceExit;
static NetCodeMap NetCodeRegions;

void *WorkerFunc(void *argv);

static inline int getArch(const char *Arch)
{
    if (Arch[0] == 'i' && Arch[2] == '8' && Arch[3] == '6')
        return ARCH_X86;
    if (!strcmp(Arch, "amd64") || !strcmp(Arch, "x86_64"))
        return ARCH_X86_64;
    if (!strncmp(Arch, "arm", 3) || !strcmp(Arch, "xscale"))
        return ARCH_ARM;

    DM->Error("%s: unsupport architecture %s\n", __func__, Arch);
    return -1;
}

static void ParseConfig(string &Addr, int &Port)
{
    char Path[3][256] = {"", "/etc/", CONFIG_LLVM_BITCODE, };
    static char path[128] = {'\0'};
    FILE *Fd;
    size_t Size;
    struct stat StatBuf;
    char *ServerConfig = NULL;
    char *Buf, *Str, *P;
    const char *DefaultAddr = "127.0.0.1";
    int DefaultPort = 34182;
    
    for (int i = 0; i < 3; i++)
    {
        char *p = strrchr(Path[i], '/');
        if (p)
        {
            snprintf(path, p-Path[i]+1, "%s", Path[i]);
            strcat(path, "/server.conf");
        }
        else
            sprintf(path, "./server.conf");

        if (stat(path, &StatBuf) == 0)
        {
            ServerConfig = path;
            break;
        }
    }

    if (ServerConfig == NULL)
    {
        Addr = "127.0.0.1";
        Port = 34182;
        DM->Warning("Cannot find server.conf. Use default address %s:%d.\n",
                Addr.c_str(), Port);
        return;
    }

    if ((Fd = fopen(ServerConfig, "r+")) == NULL)
        DM->Error("Open config file %s\n", ServerConfig);

    Size = StatBuf.st_size;
    Buf = new char[Size];
    if (fread(Buf, 1, Size, Fd) != Size)
        DM->Error("Read config file.\n");
    fclose(Fd);

    P = Str = new char[Size + 1];
    for (int i = 0; i < (int)Size; i++)
    {
        if (Buf[i] == ' ' || Buf[i] == '\t' || Buf[i] == '\n' || Buf[i] == '\0')
            continue;
        *P++ = Buf[i];
    }
    *P = '\0';

    if ((P = strchr(Str, ':')) == NULL)
        DM->Error("Invalid config format\n");

    *P++ = '\0';

    Addr = (strlen(Str) == 0) ? DefaultAddr : Str;
    Port = (atoi(P) == 0) ? DefaultPort : atoi(P);

    delete [] Buf;
    delete [] Str;
}

string getDigest(unsigned char *buf, int len)
{
    unsigned char D[DIGEST_LENGTH];
    unsigned char *p = D;
    string Digest = "";
    
    memset(D, 0, DIGEST_LENGTH);
    for (int i = 0; i < len; i++)
        D[i % DIGEST_LENGTH] += buf[i];
    
    for (int i = 0; i < DIGEST_LENGTH; i++, p++)
    {
        unsigned char c = (*p >> 4);
        if(c >= 10)
            c = c -10 + 'a';
        else
            c +='0';
        Digest += c;
        
        c = (*p & 0x0F);
        if(c >= 10)
            c = c - 10 + 'a';
        else
            c += '0';
        Digest += c;
    }
    
    return Digest;
}

static inline TraceInfo *SetupTraceInfo(uint64_t mode, int version, GraphNode *CFG)
{
    TraceInfo *Trace = new TraceInfo(version, 0);

    Trace->CFG = CFG;
    Trace->GuestPC = CFG->getGuestPC();
    if (mode & PROFILE_TRACE)
        Trace->Exit = (uint64_t **)llvm_malloc(MAX_PROFILE_THREADS * sizeof(uint64_t *));

    return Trace;
}

int net_recv_callback(int sock, char *buf, int size)
{
    int64_t start_cycle, end_cycle;

    start_cycle = cpu_get_real_ticks();

    ClientInfo *Client = static_cast<ClientInfo*>(Env->Peer);
    Client->setTraceBuf(buf);
    Client->RecvTrace(size);

    end_cycle = cpu_get_real_ticks();
    PF->addTraceCycles(end_cycle - start_cycle);

    return 0;
}

int net_flush_callback(int sock, char *buf, int size)
{
    RecvSize += size;
    return 0;
}

/*
 * Peer
 */
PeerInfo::PeerInfo(bool is_server) : isServer(is_server)
{
    isInitialized = false;

#ifdef USE_CALLBACK
    setup_tcp_callback(isServer, TAG_TRACE, net_recv_callback,
            TAG_FLUSH, net_flush_callback);
#endif

    Translator = Env->getSingleTranslator();

    setup_tcp_module(isServer);
    Net = retrieve_tcp_fns();
    Buf = new char[MAX_BUF_SIZE];
    Emu.Arch = getArch(Translator->getTargetArch().c_str());
    Emu.BaseRegNo = TCG_AREG0;

    Ticket = INVALID_TICKET;
    SendSize = RecvSize = 0;
}

uint32_t PeerInfo::GenTicket()
{
    ++Ticket;
    if (Ticket == INVALID_TICKET)
        ++Ticket;
    return Ticket;
}

bool PeerInfo::Sync(int Hndl, int Size)
{
    TCGContext *s = tcg_ctx_env;
    struct SyncHeader *Header;
    uint32_t Ack = INVALID_TICKET;
    char *p;

    JITSyncData &JSData = Translator->getSyncData();
    NestedFuncMap &NestedFunc = JSData.NestedFunc;
    GlobalMap &GlobalVariable = JSData.GlobalVariable;
    SymbolVec &ExternalSymbol = JSData.ExternalSymbol;

    if (isServer == true)
    {
        ClientData *Client = NULL;

        Header = (struct SyncHeader *)Buf;
        if (Header->Arch == Emu.Arch && Header->BaseRegNo == Emu.BaseRegNo)
        {
            Client = static_cast<ServerInfo*>(this)->CreateTicket(Hndl, Ack,
                    (Header->UseStatic == 0) ? false : true, Header->Digest);
            if (Client == NULL)
                DM->Error("%s: internal error.\n", __func__);
        }

        Net->Send(Hndl, &Ack, sizeof(uint32_t), TAG_SYNC);
        SendSize += sizeof(uint32_t);
        
        if (Ack == INVALID_TICKET)
            return false;

        NestedFunc.clear();
        GlobalVariable.clear();
        ExternalSymbol.clear();

        s->nb_globals = Header->NumGlobal;
        s->nb_helpers = Header->NumHelper;
        s->frame_start = Header->FrameStart;
        PF->setProfile(Header->ProfileMode);

#if defined(CONFIG_USE_GUEST_BASE)
        guest_base = (unsigned long)Header->GuestBase;
#endif

#if defined(TARGET_I386)
        Translator->setX86FPOff(Header->X86FPOff);
#endif
        p = (char *)(Header + 1);

        /* Decode global registers, helper functions, nested functions,
           global variables and external symbols */
        for (int i = 0; i < s->nb_globals; i++)
        {
            char *name;
            TCGTemp *tmp = &s->temps[i];
            decode_char(p, tmp->fixed_reg);
            decode_type(p, tmp->type);
            decode_int32(p, (tmp->fixed_reg) ? tmp->reg : tmp->mem_reg);
            decode_host_long(p, tmp->mem_offset);
            decode_string(p, name);
            if (tmp->name == NULL)
                tmp->name = strdup(name);
        }

        if (s->helpers)
            free(s->helpers);
        s->helpers = (TCGHelperInfo *)llvm_malloc(s->nb_helpers * sizeof(TCGHelperInfo));
        for (int i = 0; i < s->nb_helpers; i++)
        {
            char *name;
            TCGHelperInfo *th = &s->helpers[i];
            decode_host_ulong(p, th->func);
            decode_string(p, name);
            th->name = strdup(name);
        }

        for (int i = 0; i < Header->NumNestedFunc; i++)
        {
            uintptr_t Addr;
            char *Name;
            decode_host_ulong(p, Addr);
            decode_string(p, Name);
            NestedFunc[Addr] = string(Name);
        }

        for (int i = 0; i < Header->NumGlobalVariable; i++)
        {
            uintptr_t Addr;
            char *Name;
            decode_host_ulong(p, Addr);
            decode_string(p, Name);
            GlobalVariable[Addr] = string(Name);
        }

        SymbolVec &ClientExternalSymbol = Client->JSData.ExternalSymbol;
        for (int i = 0; i < Header->NumSymbol; i++)
        {
            tcg_target_ulong Addr;
            decode_host_ulong(p, Addr);
            ExternalSymbol.push_back(Addr);
            ClientExternalSymbol.push_back(Addr);
        }

#ifdef ASSERT
        if (Size != p - Buf)
            DM->Error("%s: inconsistant message size (%d).\n", __func__, isServer);
#endif
    }
    else
    {
        Header = (struct SyncHeader *)Buf;
        Header->Arch = Emu.Arch;
        Header->BaseRegNo = Emu.BaseRegNo;
        Header->NumGlobal = s->nb_globals;
        Header->NumHelper = s->nb_helpers;
        Header->NumNestedFunc = NestedFunc.size();
        Header->NumGlobalVariable = GlobalVariable.size();
        Header->NumSymbol = ExternalSymbol.size();
        Header->FrameStart = s->frame_start;
        Header->ProfileMode = PF->getProfile();
        Header->UseStatic = (UseStatic == false) ? 0 : 1;
        if (UseStatic)
        {
            string Digest = getDigest((unsigned char *)digest + 1, 128);
            strcpy(Header->Digest, Digest.c_str());
        }

#if defined(CONFIG_USE_GUEST_BASE)
        Header->GuestBase = guest_base;
#endif

#if defined(TARGET_I386)
        Header->X86FPOff[0] = offsetof(CPUState, fpstt);
        Header->X86FPOff[1] = offsetof(CPUState, fptags[0]);
        Header->X86FPOff[2] = offsetof(CPUState, fpregs[0]);
#endif
        p = (char *)(Header + 1);

        /* Encode global registers, helper functions, nested functions,
           global variables and external symbols */
        for (int i = 0; i < s->nb_globals; i++)
        {
            TCGTemp *tmp = &s->temps[i];
            encode_char(p, tmp->fixed_reg);
            encode_type(p, tmp->type);
            encode_int32(p, (tmp->fixed_reg) ? tmp->reg : tmp->mem_reg);
            encode_host_long(p, tmp->mem_offset);
            encode_string(p, tmp->name);
        }

        for (int i = 0; i < s->nb_helpers; i++)
        {
            TCGHelperInfo *th = &s->helpers[i];
            encode_host_ulong(p, th->func);
            encode_string(p, th->name);
        }

        for (HFuncMap::iterator I = NestedFunc.begin(), E = NestedFunc.end();
                I != E; I++)
        {
            encode_host_ulong(p, I->first);
            encode_string(p, I->second.c_str());
        }

        for (GlobalMap::iterator I = GlobalVariable.begin(), E = GlobalVariable.end();
                I != E; I++)
        {
            encode_host_ulong(p, I->first);
            encode_string(p, I->second.c_str());
        }

        for (int i = 0, e = Header->NumSymbol; i < e; i++)
            encode_host_ulong(p, ExternalSymbol[i]);

        Status status;
        Net->Send(Hndl, Buf, p - Buf, TAG_SYNC);
        Net->Recv(Hndl, &Ack, sizeof(uint32_t), TAG_SYNC, &status);
        SendSize += p - Buf;
        RecvSize += sizeof(uint32_t);
        
        Ticket = Ack;
        if (Ticket == INVALID_TICKET)
            DM->Error("Server does not support arch %s.\n", TARGET_ARCH);

#ifdef ASSERT
        int RegSize = TCG_TARGET_REG_BITS / 8;
        int SyncSize = sizeof(struct SyncHeader);
        for (int i = 0; i < s->nb_globals; i++) SyncSize += (6 + RegSize + strlen(s->temps[i].name) + 1);
        for (int i = 0; i < s->nb_helpers; i++) SyncSize += (RegSize + strlen(s->helpers[i].name) + 1);
        for (NestedFuncMap::iterator I = NestedFunc.begin(), E = NestedFunc.end(); I != E; I++)
            SyncSize += (RegSize + I->second.size() + 1);
        for (GlobalMap::iterator I = GlobalVariable.begin(), E = GlobalVariable.end(); I != E; I++)
            SyncSize += (RegSize + I->second.size() + 1);
        SyncSize += (RegSize * ExternalSymbol.size());
        if (SyncSize > MAX_BUF_SIZE)
            DM->Error("%s: package size is too large %d (max=%d)\n", __func__, SyncSize, MAX_BUF_SIZE);
        if (SyncSize != p - Buf)
            DM->Error("%s: inconsistant message size (%d).\n", __func__, isServer);
#endif
    }

    return true;
}

/*
 * Server
 */
ServerInfo::ServerInfo() : PeerInfo(true)
{
}

ServerInfo::~ServerInfo()
{
    if (Sock < 0)
        return;

    Net->Close(Sock);
    Net->Finalize();
}

/*
 * UpdateTranslator()
 *  Update address to symbols in execution engine.
 */
void ServerInfo::UpdateTranslator()
{
    Translator->Update(tcg_ctx_env);

    isInitialized = true;
}

static inline string get_default_model()
{

#if defined(cpudef_setup)
    cpudef_setup();
#endif
    string cpu_model;

#if defined(TARGET_I386)
#ifdef TARGET_X86_64
    cpu_model = "qemu64";
#else
    cpu_model = "qemu32";
#endif
#elif defined(TARGET_ARM)
    cpu_model = "any";
#elif defined(TARGET_M68K)
    cpu_model = "any";
#elif defined(TARGET_SPARC)
#ifdef TARGET_SPARC64
    cpu_model = "TI UltraSparc II";
#else
    cpu_model = "Fujitsu MB86904";
#endif
#elif defined(TARGET_MIPS)
#if defined(TARGET_ABI_MIPSN32) || defined(TARGET_ABI_MIPSN64)
    cpu_model = "20Kc";
#else
    cpu_model = "24Kf";
#endif
#elif defined(TARGET_PPC)
#ifdef TARGET_PPC64
    cpu_model = "970fx";
#else
    cpu_model = "750";
#endif
#else
    cpu_model = "any";
#endif

    return cpu_model;
}

void ServerInfo::Daemonize()
{
    int ret;
    string Addr;
    int Port;

    ParseConfig(Addr, Port);
    Sock = Net->Initialize(Addr.c_str(), Port, 0);
    if (Sock < 0)
        DM->Error("%s: initialize server failed.\n", __func__);
    
    DM->Debug(DEBUG_LLVM, "Server listening on %s with port %d\n", Addr.c_str(), Port);

    for (;;)
    {
        Status status;
        Net->Probe(ANY_SOURCE, ANY_TAG, &status);

        int Hndl = status.source;
        int Size = status.count;
        comm_tag_t Tag = status.tag;

#ifdef ASSERT
        if (Size > MAX_BUF_SIZE)
            DM->Error("%s: package size is too large %d (max=%d)\n", __func__,
                    Size, MAX_BUF_SIZE);
#endif
        ret = Net->Recv(Hndl, Buf, Size, Tag, &status);
        if (ret != NET_SUCCESS)
            continue;

        RecvSize += Size;
        switch(Tag)
        {
        case TAG_SYNC:
            ret = Sync(Hndl, Size);
            if (ret == false)
                continue;

            UpdateTranslator();
            break;
        case TAG_TRACE:
            ProcessTrace(Size);
            break;
        case TAG_FLUSH:
            ProcessFlush();
            break;
        default:
            DM->Error("%s: unsupported tag %d\n", __func__, Tag);
            break;
        }
    }
}

ClientData *ServerInfo::CreateTicket(int Hndl, uint32_t &Ack, bool isStatic,
        char *Digest)
{
    Ack = GenTicket();
    ClientData *Client = new ClientData(Hndl);
    ClientMgr[Ack] = Client;

    Client->UseStatic = isStatic;
    if (isStatic)
    {
        Client->PCode = PermanentCache.Lookup(string(Digest));
        Client->reset();
    }

    return Client;
}

static inline ClientData *getClientData(uint32_t Key)
{
    if (ClientMgr.count(Key) == 0)
        DM->Error("%s: internal error.\n", __func__);
    return ClientMgr[Key];
}

int ServerInfo::ProcessFlush()
{
    struct FlushHeader *Header;
    uint32_t Ticket;
    ClientData *Client;

    Header = (struct FlushHeader *)Buf;
    Ticket = Header->Ticket;
    Client = getClientData(Ticket);
    Client->reset();

    Net->Send(Client->Sock, &Ticket, sizeof(uint32_t), TAG_FLUSH);
    SendSize += sizeof(uint32_t);
    return 0;
}

TranslatedCode *LLVMTranslator::GenTrace(TCGContext *tcg_context, CPUState *env, 
        OptimizationInfo *Opt, TraceRequest &Req)
{
    uint64_t DebugMode = DM->getDebug();
    TCGContext *s = tcg_context;
    target_ulong pc = Opt->CFG->getGuestPC();

    IF->TraceInit(pc, Opt->Count, Opt->CFG, Opt->Version);
    TraceInfo *Trace = IF->TBuilder->getTrace();

    Trace->setID(Req.TraceID);
    Trace->setCFGMap(Req.CFGMap);

    /* We backup `guest_base' because we modify it to make TCG fontend decode
     * our code buffer. Restore it after disassembling the code. */
    unsigned long old_guest_base = guest_base;
    for (;;)
    {
        GraphNode *Node = IF->TBuilder->getNextNode();
        if (Node == NULL)
            break;

        ImageInfo &Image = Req.Images[Req.CFGMap[Node]];
        TranslationBlock *tb = &Image.TB;

        tcg_func_start(env, s);

        if (tb->env)
            tcg_restore_state(env, (void *)tb->env, 0);

        env->gen_trace_skip = 0;
        guest_base = (unsigned long)Image.Code - tb->pc;
        gen_intermediate_code(env, tb);
        guest_base = old_guest_base;
        if (env->gen_trace_skip == 1)
            return NULL;
        
        if (*gen_opc_ptr != INDEX_op_end)
            DM->Error("%s: fatal error - cannot find op_end\n", __func__);

        do_liveness_analysis(s);

        if (DebugMode & DEBUG_OP)
            printOp(s);

        int NumOPC = gen_opc_ptr - gen_opc_buf;
        const TCGArg *Args = gen_opparam_buf;

        IF->TraceSetContext(Opt->Mode, s, Node);
        for (int i = 0; i < NumOPC; i++)
        {
            TCGOpcode opc = (TCGOpcode)gen_opc_buf[i];
            Args += (IF->*OpcFn[opc])(Args);
        }

        Trace->addGuestSize(tb->size);
        Trace->addGuestICount(tb->icount);
    }

    IF->TraceFinalize();
    
    /* JIT host code. */
    IF->Compile();

    Trace->Commit((unsigned long)CallBack->Code, CallBack->Size);

    uint8_t *RealCode, *Code;
    int RealSize, ConstPoolSize;
    PatchInfo &Patch = Env->getPatch();

    /* The real emitted code must include constant pools and seems
     * the X86 LLVM backend tends to exclude it from the emitted code.
     * Thus, we need to resolve the `real' emitted code and size. */
    ConstPoolSize = Patch.ConstPoolSize;
    RealCode = CallBack->Code - ConstPoolSize;
    RealSize = CallBack->Size + ConstPoolSize;
    Code = new uint8_t[RealSize];
    memcpy(Code, RealCode, RealSize);

    TranslatedCode *TC = new TranslatedCode;
    TC->Count = Opt->Count;
    TC->PC = pc;
    TC->CSBase = Req.CSBase;
    TC->Code = Code + ConstPoolSize;
    TC->Size = CallBack->Size;
    TC->Func = IF->getFunction();
    TC->Info = Trace;
    TC->Patch = &Patch;

    for (int i = 0; i < Req.Count; i++)
        TC->PCs.insert(Req.PCs[i]);

    return TC;
}

int ServerInfo::ProcessTrace(int Size)
{
    TCGContext *s = tcg_ctx_env;
    struct TraceHeader *Header;
    OptimizationInfo *Opt;
    GraphNode *CFG;
    struct TraceRequest Req;
    int CFGSize;
    int Count;
    char *p;
            
    Header = (TraceHeader *)Buf;
    p = (char *)(Header + 1);

    /* decode CFG */
    CFGSize = DecodeCFG(p, Header->CSBase, &CFG, Req.Count, &Req.PCs, Req.CFGMap);
    endecode_skip(p, CFGSize);
    
    ClientData *Client = getClientData(Header->Ticket);
    if (Header->Mode != LLVM_REGEN_TRACE && SendTraceCached(Client, CFG, Req) == 1)
        return 0;

    Count = Req.Count;
    Opt = new OptimizationInfo(Header->Mode, Count, -1, 0, tb_flush_version, CFG);

    Req.Ticket = Header->Ticket;
    Req.TraceID = Client->getTraceID();
    Req.CSBase = Header->CSBase;
    Req.Images = new ImageInfo[Count];

    for (int i = 0; i < Count; i++)
    {
        TranslationBlock &TB = Req.Images[i].TB;

        TB.cs_base = Req.CSBase;
        decode_guest_ulong(p, TB.pc);
        decode_uint64(p, TB.flags);
        decode_uint16(p, TB.size);
        decode_uint16(p, TB.cflags);
        decode_uint32(p, TB.icount);

        Req.Images[i].Code = p;
        endecode_skip(p, TB.size);

        int StateSize;
        decode_uint32(p, StateSize);
        TB.env = (StateSize == 0) ? NULL : p;
        endecode_skip(p, StateSize);
    }

    if (Client->UseStatic == 0)
    {
        CachedCodeRegions = &Client->CodeRegions;
        CachedTraceChain = &Client->TraceChain;
        CachedChainPoint = &Client->ChainPoint;
    }
    else
    {
        CachedCodeRegions = &Client->PCode->CodeRegions;
        CachedTraceChain = &Client->PCode->TraceChain;
        CachedChainPoint = &Client->PCode->ChainPoint;
    }
    CachedPatch = new PatchInfo();

    vector<TranslatedCode *> SendTC;
    TranslatedCode *TC = Translator->GenTrace(s, CPU, Opt, Req);
    if (TC == NULL)
        goto skip;

    Client->setTrace(TC);

    SendTC.push_back(TC);
    SendTraceEmitted(Client, SendTC, Header->TraceKey);

skip:
    delete [] Req.Images;
    delete Opt;

    return 0;
}

static inline bool ComparePCs(set<target_ulong> PCs, int ReqCount, target_ulong *ReqPCs)
{
    for (int i = 0; i < ReqCount; i++)
    {
        if (PCs.count(ReqPCs[i]) == 0)
            return false;
    }
    return true;
}

int ServerInfo::SendTraceCached(ClientData *Client, GraphNode *CFG, TraceRequest &Req)
{
    struct TraceHeader *Header;
    vector<TranslatedCode *> SendTC;
    
    Header = (struct TraceHeader *)Buf;
    CodeKey CKey = CodeKey(CFG->getGuestPC(), Header->CSBase);
    if (Client->LookupTrace(CKey) == false)
        return 0;

    TranslatedCode *TC = Client->getCachedTrace(CKey);
#if 0
    if (ComparePCs(TC->PCs, Req.Count, Req.PCs) == false)
    {
        Client->removeTrace(CKey, TC);
        return 0;
    }
#endif
    if (Req.Count > TC->Count)
    {
        Client->removeTrace(CKey, TC);
        return 0;
    }

    Client->CacheTrace(CKey, TC);
    SendTC.push_back(TC);

    SendTraceEmitted(Client, SendTC, Header->TraceKey);

    return 1;
}

int ServerInfo::SendTraceEmitted(ClientData *Client, vector<TranslatedCode*> &SendTC, uint64_t TraceKey)
{
    struct TracePackHeader *Header;
    struct TraceAckHeader *SubHeader;
    int NumTrace, NumExit, NumExec, NumConstPool;
    uint64_t ProfileMode = PF->getProfile();
    uint8_t *RealCode;
    int RealSize, ConstPoolSize;
    char *p;
#ifdef ASSERT
    int Size = 0;
#endif

    NetCodeMap &RemoteCodeRegions = Client->RemoteCodeRegions;

    Header = (TracePackHeader *)Buf;
    Header->TraceKey = TraceKey;
    Header->NumTrace = NumTrace = SendTC.size();
    p = (char *)(Header + 1);

    for (int TraceIdx = 0; TraceIdx < NumTrace; TraceIdx++)
    {
        TranslatedCode *TC = SendTC[TraceIdx];

        tcg_target_ulong MaxKey = 0;
        TraceInfo *Trace = static_cast<TraceInfo *>(TC->Info);
        NodeMap &CFGMap = Trace->getCFGMap();
        PatchInfo &Patch = *TC->Patch;
        vector<uint32_t> &toExit = Patch.toExit;
        vector<ChainInfo> &toExec = Patch.toExec;
        vector< pair<uint32_t,int> > &ExternalSymbol = Patch.ExternalSymbol;
        vector< pair<uint32_t,int> > &ConstPool = Patch.ConstPool;
        
        if (ExternalSymbol.size())
            PatchExternalSymbol((uintptr_t)TC->Code, Client->JSData, ExternalSymbol);

        SubHeader = (TraceAckHeader *)p;
        p = (char *)(SubHeader + 1);

        SubHeader->TraceID = Trace->ID;
        SubHeader->GuestICount = Trace->GuestICount;
        SubHeader->CodeSize = TC->Size;
        SubHeader->NumExit = NumExit = toExit.size();
        SubHeader->NumExec = NumExec = toExec.size();
        SubHeader->NumConstPool = NumConstPool = ConstPool.size();
        SubHeader->ConstPoolSize = ConstPoolSize = Patch.ConstPoolSize;

        RealCode = TC->Code - ConstPoolSize;
        RealSize = TC->Size + ConstPoolSize;
        encode_raw(p, RealCode, RealSize);
        
        for (int i = 0; i < NumExit; i++)
            encode_uint32(p, toExit[i]);
        
        for (int i = 0; i < NumExec; i++)
        {
            CodeKey CKey = CodeKey(toExec[i].PC, TC->CSBase);
            int toTrace = RemoteCodeRegions.count(CKey);
            encode_int32(p, toTrace);
            encode_uint32(p, toExec[i].Off);
            encode_int32(p, toExec[i].NextOff);
            encode_int32(p, toExec[i].JmpOff);
            encode_host_ulong(p, toExec[i].Key);
            encode_guest_ulong(p, toExec[i].PC);
            
#if 0
            if (toExec[i].PC == TC->PC)
                DM->Error("%s: internal error.\n", __func__);
#endif
            
            NodeVec &Links = Trace->getLink(toExec[i].PC);
            encode_int32(p, (int)Links.size());
            for (int j = 0, e = Links.size(); j != e; j++)
                encode_int32(p, CFGMap[Links[j]]);
            
            if (toExec[i].Key > MaxKey)
                MaxKey = toExec[i].Key;
        }
        
        for (int i = 0; i < NumConstPool; i++)
        {
            encode_uint32(p, ConstPool[i].first);
            encode_int32(p, ConstPool[i].second);
        }
        
        if (TraceIdx == 0 && (ProfileMode & PROFILE_BASIC))
        {
            encode_int32(p, Trace->NumLoop);
            encode_int32(p, Trace->NumBlock);
            encode_int32(p, Trace->NumExit);
            encode_int32(p, Trace->NumIndirectBranch);
            encode_int16(p, Trace->GuestSize);
        }
        
        SubHeader->MaxKey = MaxKey;

#ifdef ASSERT
        Size += sizeof(struct TraceAckHeader) + RealSize +
            NumExit * 4 + 
            NumExec * (16 + sizeof(tcg_target_ulong) + sizeof(target_ulong)) + 
            NumConstPool * 8;
        for (int i = 0; i < NumExec; i++)
            Size += 4 + Trace->getLink(toExec[i].PC).size() * 4;
        if (TraceIdx == 0 && (ProfileMode & PROFILE_BASIC))
            Size += 18;
#endif
    }
    
#ifdef ASSERT
    Size += sizeof(struct TracePackHeader);
    if (Size > MAX_BUF_SIZE)
        DM->Error("%s: send message size is too large %d (%d).\n", Size, MAX_BUF_SIZE);
    if (Size != p - Buf)
        DM->Error("%s: inconsistant message size.\n", __func__);
#endif

    Net->Send(Client->Sock, Buf, p - Buf, TAG_TRACE);
    SendSize += p - Buf;

    return 1;
}

/*
 * Client
 */
ClientInfo::ClientInfo() : PeerInfo(false)
{
    char *p;
    p = getenv("LLVM_STATIC");
    UseStatic = (p == NULL || strlen(p) == 0) ? false : true;
}

ClientInfo::~ClientInfo()
{
    if (Sock < 0)
        return;

    Net->Close(Sock);
    Net->Finalize();
}

int ClientInfo::Connect()
{
    string Addr;
    int Port;
    
    /* Connect to server. */
    ParseConfig(Addr, Port);
    
    Sock = Net->Connect(Addr.c_str(), Port);
    
    if (Sock <= 0)
        DM->Warning("Cannot connect to %s:%d. Use ClientOnly mode.\n", Addr.c_str(), Port);
    else
    {
        DM->Debug(DEBUG_LLVM, "Connect to %s:%d: Success.\n", Addr.c_str(), Port);
        Sync(Sock);
        isInitialized = true;
    }

    return (isInitialized == true) ? 1 : 0;
}

int ClientInfo::SendTrace(CPUState *env, OptimizationInfo *Opt)
{
    uint64_t DebugMode = DM->getDebug();
    uint64_t ProfileMode = PF->getProfile();
    struct TraceHeader *Header;
    TraceInfo *Trace;
    GraphNode *CFG = Opt->getCFG();
    int CFGSize;
    char *p;
#ifdef ASSERT
    int Size = 0;
#endif

    Header = (TraceHeader *)Buf;
    p = (char *)(Header + 1);

    Trace = SetupTraceInfo(ProfileMode, Opt->Version, CFG);
    NodeVec &CFGVec = Trace->CFGVec;

    Header->Ticket = Ticket;
    Header->Mode = Opt->Mode;
    Header->TraceKey = (uint64_t)Trace;
    Header->CSBase = CFG->getGuestCS();

    CFGSize = EncodeCFG(p, CFG, CFGVec);
    endecode_skip(p, CFGSize);

    for (int i = 0, e = CFGVec.size(); i < e; i++)
    {
        GraphNode *Parent = CFGVec[i];
        TranslationBlock *TB = Parent->getTB();

        encode_guest_ulong(p, TB->pc);
        encode_uint64(p, TB->flags);
        encode_uint16(p, TB->size);
        encode_uint16(p, TB->cflags);
        encode_uint32(p, TB->icount);
        encode_raw(p, laddr(TB->pc), TB->size);

        uint32_t *StateSize = (uint32_t *)p;
        endecode_skip(p, 4);
        *StateSize = tcg_pack_state(p, TB);
        endecode_skip(p, *StateSize);

        if (DebugMode & DEBUG_INASM)
            LLVMTranslator::printAsm(env, TB);

#ifdef ASSERT
        Size += sizeof(target_ulong) + 20 + TB->size + *StateSize;
#endif
    }

#ifdef ASSERT
    Size += sizeof(struct TraceHeader) + CFGSize;
    if (Size > MAX_BUF_SIZE)
        DM->Error("%s: send message size is too large %d (%d).\n", Size, MAX_BUF_SIZE);
    if (Size != p - Buf)
        DM->Error("%s: internal error.\n", __func__);
#endif

#ifndef USE_CALLBACK
    Net->Send(Sock, Buf, p - Buf, TAG_TRACE);
#else
    Request request;
    char *sbuf = new char[p - Buf];
    memcpy(sbuf, Buf, p - Buf);
    Net->Isend(Sock, sbuf, p - Buf, TAG_TRACE, &request);
#endif
    SendSize += p - Buf;

    return 1;
}

/*
 * PatchJmp()
 *  Patch points that jump to this guest pc.
 */
static void PatchJmp(target_ulong guest_pc, unsigned long addr)
{
    TraceChainMap &TraceChain = Env->getTraceChain();

    vector<uintptr_t> &AddrList = TraceChain[guest_pc];
    for (vector<uintptr_t>::iterator I = AddrList.begin(), E = AddrList.end();
            I != E; I++)
    {
        patch_jmp(*I, addr);
    }
}

int ClientInfo::RecvTrace(int Size)
{
    struct TracePackHeader *Header;
    struct TraceAckHeader *SubHeader;
    int NumTrace;
    TargetJITInfo &TJI = ((JIT*)Translator->getExecutionEngine())->getJITInfo();
    uint64_t ProfileMode = PF->getProfile();
    uint64_t TraceKey;
    target_ulong CSBase=0;
    char *p;

    NetCodeMap &CodeRegions = NetCodeRegions;
    TraceChainMap &TraceChain = Env->getTraceChain();
    ChainVec &ChainPoint = Env->getChainPoint();

    llvm_spin_lock(&llvm_global_lock);

#ifndef USE_CALLBACK
    Status status;
    Net->Probe(Sock, TAG_TRACE, &status);
    Size = status.count;
    Net->Recv(Sock, Buf, Size, TAG_TRACE, &status);
    setTraceBuf(Buf);
#endif
    RecvSize += Size;

    Header = (struct TracePackHeader *) TraceBuf;
    p = (char *)(Header + 1);

    TraceKey = Header->TraceKey;
    NumTrace = Header->NumTrace;

    for (int TraceIdx = 0; TraceIdx < NumTrace; TraceIdx++)
    {
        TraceInfo *Trace;
        int GuestICount, NumExit, NumExec, NumConstPool, NumLink;
        uint8_t *RealCode;
        int RealSize;
        uintptr_t Addr, Code, New, ActualSize = 0;
        uint32_t Off, JmpOff, NextOff, Idx, ConstPoolOff;
        target_ulong PC;
        tcg_target_ulong Key;
        GraphNode *CFG;
        set<int> NodeIdx;

        SubHeader = (struct TraceAckHeader *)p;
        p = (char *)(SubHeader + 1);

        RealSize = SubHeader->CodeSize + SubHeader->ConstPoolSize;

        if (llvm_tb_alloc_size((size_t)RealSize) == 0)
        {
            DM->Error("%s: fix me.\n", __func__);
            llvm_flush = 1;
            llvm_spin_unlock(&llvm_global_lock);
            return -1;
        }

        if (TraceIdx == 0)
        {
            Trace = (TraceInfo *)(unsigned long)TraceKey;
            if (SubHeader->TraceID + 1 > TraceComplete.size())
                TraceComplete.resize(SubHeader->TraceID + 1);
            TraceComplete[SubHeader->TraceID] = Trace;
            Trace->setID(SubHeader->TraceID);
        }
        else
        {
            if (SubHeader->TraceID > TraceComplete.size())
                DM->Error("%s: cannot find complete trace %d\n", __func__, SubHeader->TraceID);
            Trace = TraceComplete[SubHeader->TraceID];
        }

        CFG = Trace->CFG;
        CSBase = CFG->getGuestCS();
        GuestICount = SubHeader->GuestICount;
        NumExit = SubHeader->NumExit;
        NumExec = SubHeader->NumExec;
        NumConstPool = SubHeader->NumConstPool;

        RealCode = Env->startFunctionBody(NULL, ActualSize);
        decode_raw(p, RealCode, RealSize);
        Env->endFunctionBody(NULL, RealCode, RealCode + RealSize);

        TranslatedCode *TC = new TranslatedCode;
        TC->PC = CFG->getGuestPC();
        TC->Size = SubHeader->CodeSize;
        TC->Code = RealCode + SubHeader->ConstPoolSize;
        TC->Info = Trace;
        TC->Patch = NULL;

        NodeVec &CFGVec = Trace->CFGVec;
        Code = (uintptr_t)TC->Code;

        if (SubHeader->MaxKey + 1 > ChainPoint.size())
            ChainPoint.resize(SubHeader->MaxKey + 1);

        for (int i = 0; i < NumExit; i++)
        {
            decode_uint32(p, Off);
            Addr = Code + Off;
            PatchCodeShort(TJI, Addr, (uintptr_t)llvm_tb_ret_addr);
        }

        for (int i = 0; i < NumExec; i++)
        {
            int toTrace;
            decode_uint32(p, toTrace);
            decode_uint32(p, Off);
            decode_int32(p, NextOff);
            decode_int32(p, JmpOff);
            decode_host_ulong(p, Key);
            decode_guest_ulong(p, PC);

            Addr = Code + Off;
            ChainPoint[Key] = Addr + JmpOff;
            TraceChain[PC].push_back(Addr + JmpOff);

            CodeKey CKey = CodeKey(PC, CSBase);
            New = (toTrace && CodeRegions.count(CKey)) ? (uintptr_t)CodeRegions[CKey]->Code : Addr + NextOff;

            PatchCode(TJI, Addr, New, JmpOff);

            decode_int32(p, NumLink);
            for (int j = 0; j < NumLink; j++)
            {
                decode_int32(p, Idx);
                Trace->setLink(PC, CFGVec[Idx]);
            }
        }

        for (int i = 0; i < NumConstPool; i++)
        {
            decode_uint32(p, Off);
            decode_int32(p, ConstPoolOff);
            PatchMovConstPool(Code, Off, ConstPoolOff);
        }

        if (TraceIdx == 0)
        {
            if (ProfileMode & PROFILE_BASIC)
            {
                decode_int32(p, Trace->NumLoop);
                decode_int32(p, Trace->NumBlock);
                decode_int32(p, Trace->NumExit);
                decode_int32(p, Trace->NumIndirectBranch);
                decode_int16(p, Trace->GuestSize);
                PF->addTrace(Trace);
            }

            if (ProfileMode & PROFILE_TRACE)
            {
                for (int i = 0; i < MAX_PROFILE_THREADS; i++)
                {
                    Trace->Exit[i] = (uint64_t *)llvm_malloc((Trace->NumBlock + 2) * sizeof(uint64_t));
                    memset(Trace->Exit[i], 0, (Trace->NumBlock + 2) * sizeof(uint64_t));
                }

                if (Trace->ID + 1 > TraceExit.size())
                    TraceExit.resize(Trace->ID + 1);
                TraceExit[Trace->ID] = (unsigned long)Trace->Exit;
            }

            if (DM->getDebug() & DEBUG_OUTASM)
            {
                fprintf(stderr, "OUT: [size=%d]\n", TC->Size);
                disas(stderr, TC->Code, TC->Size);
                fprintf(stderr, "\n");
            }
        }

        Trace->Commit(Code, TC->Size);

        CodeRegions[CodeKey(TC->PC, CSBase)] = TC;

        /* Flush instruction cache and patch previous traces that jump to this trace. */
        flush_icache_range(Code, Code + TC->Size);
        if (TraceIdx == 0)
        {
            Trace->GuestICount = GuestICount;
            TranslationBlock *Head = CFG->getTB();
            patch_jmp(tb_get_jmp_entry(Head), Code);
            Head->mode = 2;
            Head->opt_ptr = TC->Code;
            TC->isPatched = true;
        }
        else
            TC->isPatched = false;
        PatchJmp(TC->PC, Code);

        dbo_register_region(Trace);
    }

#ifdef ASSERT
    if (Size != p - TraceBuf)
        DM->Error("%s: inconsistant message size.\n", __func__);
#endif

    llvm_spin_unlock(&llvm_global_lock);
    
    return 1;
}

int ClientInfo::Flush()
{
    struct FlushHeader *Header;

    Header = (struct FlushHeader *)Buf;
    Header->Ticket = Ticket;

    Net->Send(Sock, Buf, sizeof(struct FlushHeader), TAG_FLUSH);
    SendSize += sizeof(struct FlushHeader);

#ifndef USE_CALLBACK
    uint32_t Ack;
    Status status;
    Net->Recv(Sock, &Ack, sizeof(uint32_t), TAG_FLUSH, &status);
    RecvSize += sizeof(uint32_t);
#endif

    return 1;
}

/*
 * net_initialize()
 *  Initialize network subsystem and synchronize invariant informations.
 */
int net_init()
{
    if (HybridMode == HYBRID_SERVER)
    {
        CPUState *CPU = cpu_init(get_default_model().c_str());
        CPU->gen_trace = 1;
        CPU->gen_opc_buf = new uint16_t[OPC_BUF_SIZE];
        CPU->gen_opparam_buf = new TCGArg[OPPARAM_BUF_SIZE];
        CPU->gen_opc_instr_start = new uint8_t[OPC_BUF_SIZE];

        ServerInfo *Server = new ServerInfo();
        Env->Peer = Server;
        Server->CPU = CPU;
        Server->Daemonize();
    }
    else if (HybridMode == HYBRID_CLIENT)
    {
        ClientInfo *Client = new ClientInfo();
        Env->Peer = Client;
        int ret = Client->Connect();
        if (ret == 0)
        {
            HybridMode = HYBRID_MULTIPLE;
            Env->SpawnThread(1, WorkerFunc);
        }
    }
    else
        DM->Error("%s: invalid client/server mode.\n", __func__);

    return 0;
}

/*
 * net_finalize()
 *  Destroy network subsystem.
 */
int net_finalize()
{
    if (Env->Peer->isServer == true)
    {
        ServerInfo *Server = static_cast<ServerInfo *>(Env->Peer);
        delete Server;
    }
    else 
    {
        ClientInfo *Client = static_cast<ClientInfo *>(Env->Peer);
        delete Client;
    }
    return 0;
}

int net_gen_trace(CPUState *env, OptimizationInfo *Opt, bool force_regen)
{
    int ret;
    int64_t start_cycle=0, end_cycle=0;
    ControlFlowGraph *GlobalCFG = Env->getGlobalCFG();
    ClientInfo *Client = static_cast<ClientInfo*>(Env->Peer);

    if (Client == NULL || Client->isInitialized == false)
        goto skip;

    llvm_spin_lock(&llvm_global_lock);
    if (force_regen == false)
    {
        TranslationBlock *Head = Opt->getCFG()->getTB();
        if (llvm_testandset(&Head->tb_gen_pending) == 1)
        {
            llvm_spin_unlock(&llvm_global_lock);
            goto skip;
        }

        if (GlobalCFG != NULL)
            ExpandTrace(Opt);
    }

    start_cycle = cpu_get_real_ticks();

    ret = Client->SendTrace(env, Opt);
    llvm_spin_unlock(&llvm_global_lock);

    if (ret == 0)
        goto skip;

#ifndef USE_CALLBACK
    Client->RecvTrace();
#endif

    end_cycle = cpu_get_real_ticks();
    PF->addTraceCycles(end_cycle - start_cycle);

skip:
    delete Opt;

    return 1;
}

int net_flush()
{
    int64_t start_cycle, end_cycle;
    uint64_t ProfileMode = PF->getProfile();

    if (Env->Peer->isServer == true || Env->Peer->isInitialized == false)
        return 0;

    if (ProfileMode & PROFILE_BASIC)
        start_cycle = cpu_get_real_ticks();

    ClientInfo *Client = static_cast<ClientInfo*>(Env->Peer);
    Client->Flush();

    if (ProfileMode & PROFILE_BASIC)
    {
        end_cycle = cpu_get_real_ticks();
        PF->addTraceCycles(end_cycle - start_cycle);
    }

    return 1;
}

void net_fork_start(void)
{
    tcp_fork_start();
}

void net_fork_end(int child)
{
    tcp_fork_end(child);

    if (child == 1)
        Env->Peer = NULL;
}

#ifdef __cplusplus
extern "C"
{
#endif
uint64_t **net_get_counter(uint32_t id)
{
    uint64_t **counter;
    llvm_spin_lock(&llvm_global_lock);
    counter = (uint64_t **)TraceExit[id];
    llvm_spin_unlock(&llvm_global_lock);

    return counter;
}

#ifdef __cplusplus
}
#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

