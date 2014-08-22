/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_OPC_H
#define __LLVM_OPC_H

#include <set>
#include <map>
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm-def.h"
#include "llvm-debug.h"
#include "llvm-macro.h"
#include "llvm-cfg.h"

//#define ASSERT
//#define ENABLE_STATS

using namespace llvm;
using namespace std;

class CodeInfo;
class PatchInfo;
class IRFactory;
class OptimizationInfo;
class IBChainInfo;
class TranslatedCode;
class LLVMTranslator;

typedef pair< target_ulong, target_ulong > CodeKey;
typedef map< TranslationBlock*, TranslatedCode* > TCodeMap;
typedef map< uintptr_t, string > HFuncMap;
typedef map< uintptr_t, string > GlobalMap;
typedef map< CONST Type*, pair<int,CONST Type*> > SPRegMap;

typedef map< size_t, pair<CONST Type*,int> > StateTypeMap;
typedef vector< Instruction *> IVec;
typedef vector< BasicBlock *> BBVec;
typedef set< const BasicBlock* > BBSet;
typedef map< const BasicBlock*, int > BBMap;
typedef map< target_ulong, vector<uintptr_t> > TraceChainMap;
typedef vector< tcg_target_ulong > ChainVec;
typedef vector< IBChainInfo* > IBChainVec;

typedef int (IRFactory::*FuncPtr)(const TCGArg *);

enum
{
    OP_ADD = 0,
    OP_SUB,
    OP_MUL,
    OP_OR,
    OP_XOR,
    OP_AND,
    OP_ANDN,
    OP_LSHR,
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
};

enum
{
    PATCH_DUMMY = 0,
    PATCH_EXIT_TB,
    PATCH_DIRECT_JUMP,
    PATCH_INDIRECT_JUMP,
    PATCH_TRACE_BLOCK_CHAINING,
    PATCH_CALL,
    PATCH_IBCHAIN,
};

/* Status of the register. */
enum 
{
    VAL_REV   = 0x1,   /* Register is reserved */
    VAL_REG   = 0x2,   /* Register is promoted */
    VAL_MEM   = 0x4,   /* Register is in CPUState memory */
    VAL_TMP   = 0x8,   /* Register is a temporary register */
    VAL_LOCAL = 0x10,  /* Register is a local register */
};

/* Pack of basic types for passing among different translator components. */
struct BasicType
{
    CONST Type *VoidTy;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *Int128Ty;
    CONST IntegerType *IntPtrTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int16PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;
    CONST Type *FloatTy;
    CONST Type *DoubleTy;
    CONST Type *FP80Ty;
    CONST Type *FP128Ty;
    CONST PointerType *FloatPtrTy;
    CONST PointerType *DoublePtrTy;
};

/* 
 * TranslatedCode describe the translated code information in the optimized
 * code cache. This information is used for both block-level and region-level
 * translation.
 */
class TranslatedCode
{
public:
    TranslatedCode() {}
    ~TranslatedCode();

    int Count;                 /* Block count */
    TranslationBlock *HeadTB;  /* The entry block of the region */
    target_ulong PC;           /* Start PC of the entry block */
    target_ulong CSBase;       /* CS of the entry block */
    set<target_ulong> PCs;     /* Start PC of all blocks */
    CodeKey CKey;              /* The pair of PC and CS */
    uint16_t Size;             /* Size of the translated host code */
    uint8_t *Code;             /* Start PC of the translated host code */
    Function *Func;            /* LLVM Function of this region */
    CodeInfo *Info;            /* Link to CodeInfo used during translation */
    PatchInfo *Patch;          /* Patch rules of this translated code */
    bool isPatched;            /* Patched or not */
};

/* 
 * CallBackInfo is used to passing information between LLVMTranslator,
 * IRFactory and the JIT listener.
 */
struct CallBackInfo
{
    int OPCIdx;            /* Index to the TCG opcode */
    TranslationBlock *TB;  /* Entry block of this translation unit */
    JITSyncData *JSData;   /* Sync data to be set by the JIT listener */
    Function *Func;        /* LLVM Function of this translation unit */
    CodeInfo *CI;          /* CodeInfo of this translation unit */
    uint16_t Size;         /* Size of the translated host code */
    uint8_t *Code;         /* Start PC of the translated host code */
};

/*
 * VectorFactory is used to convert the SIMD helper functions which are
 * implemented by scalar operations by using LLVM vector primitives.
 */
class VectorFactory
{
private:
    LLVMContext &Context;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;

    CONST Type *VectorTy;   /* The vector type to operate */
    Value *In1, *In2;       /* The pointers to operand 1 and 2 */
    Value *Vec1, *Vec2;     /* The values of operand 1 and 2 */
    Value *VecMinusOne;     /* The vector value of {-1} */
    Instruction *LastInst;  /* Insert position */
    
public:
    VectorFactory(LLVMContext &CTX, BasicType &BT);
    
    void Init(Function *F, CONST Type *ElementType, int NumElements);
    void Create(Function *F, int Op, CONST Type *ElementType, int NumElements);
    void Create(Function *F, int Op, CONST Type *ElementType, int NumElements,
            int Idx);
    void addOp(int Op);
    void setConstantVec2(CONST Type *Ty, int NumElements, int V);
    inline void Finalize();
    inline Value *getConstantVector(vector<Constant *> &V);
};

/*
 * ExecProfile records the execution statistics.
 */
struct ExecProfile
{
    ExecProfile() : Block(0), Trace(0), Other(0), NumLoads(0), NumStores(0){}

    uint64_t Block;      /* The percentage of time in the block code cache */
    uint64_t Trace;      /* The percentage of time in the trace code cache */
    uint64_t Other;      /* The percentage of time in the rest of components */
    uint64_t NumLoads;   /* The number of load instructions retired */
    uint64_t NumStores;  /* The number of store instructions retired */
};

/*
 * BaseRegister is used to describe the `reserved' registers by QEMU TCG.
 * Ex: R14 for the x86 host and R7 for the ARM host.
 */
struct BaseRegister
{
    char *Name;      /* The name string of the register */
    InlineAsm *Reg;  /* Inline assembly to move value from the register */
    CallInst *Base;  /* CallInst for the register movement */
};

/*
 * Register is used to describe the pseudo registers used by QEMU TCG op.
 */
struct Register
{
    int Mode;                 /* Status of the register */
    int Base;                 /* Base regiser # */
    int Size;                 /* Register size */
    CONST IntegerType *Type;  /* Register type in LLVM */
    tcg_target_long Off;      /* Register offset of CPUState */
    string Name;              /* Name string of this register */
    Value *Data;              /* Data value if this regisrer is promoted */
    bool Dirty;               /* This register is updated or not */
    bool Allocated;           /* This register is initialized or not */
    bool Global;              /* This register is TCG global register or not */

    Register() : Mode(0), Size(0), Off((unsigned)-1), Data(NULL), Dirty(false),
        Allocated(false), Global(false) {}

    void reset(int size, CONST IntegerType *type)
    {
        Data = NULL;
        Dirty = false;
        Allocated = false;
        setType(size, type);
    }

    /* Promote - This function is invoked when the register state is loaded
     * from CPUState in memory to the LLVM pseudo register. */
    void Promote()              { Mode |= VAL_REG;  }

    /* Demote - This function is invokded when the reigster state is stored
     * back to CPUState in memory from the LLVM pseudo register. */
    void Demote()               { Mode &= ~VAL_REG; }

    Value *getData()            { return Data;      }
    void setMode(int mode)      { Mode = mode;      }
    void setBase(int base)      { Base = base;      }
    void setOff(tcg_target_long off) { Off = off;   }
    void setName(string name)   { Name = name;      }
    void setGlobal(bool global) { Global = global;  }
    void setDirty()             { Dirty = true;     }
    void setAllocated()         { Allocated = true; }
    void setType(int size, CONST IntegerType *type)
    {
        Size = size;
        Type = type;
    }
    void setData(Value *data, bool dirty = false)
    {
        Data = data;
        Dirty = dirty;
        Promote();
    }
    bool isRev()        { return Mode & VAL_REV;   }
    bool isReg()        { return Mode & VAL_REG;   }
    bool isMem()        { return Mode & VAL_MEM;   }
    bool isTmp()        { return Mode & VAL_TMP;   }
    bool isLocal()      { return Mode & VAL_LOCAL; }
    bool isDirty()      { return Dirty;            }
    bool isAllocated()  { return Allocated;        }
    bool isGlobal()     { return Global;           }
};

/* 
 * ArgInfo describes the weight to calculate the function inlining cost.
 */
struct ArgInfo
{
public:
    unsigned ConstantWeight;  /* Weight if the argument is a constant */
    unsigned AllocaWeight;    /* Weight if the argument is a alloca */
    
    ArgInfo(unsigned CWeight, unsigned AWeight)
        : ConstantWeight(CWeight), AllocaWeight(AWeight) {}
};

/*
 * Information of helper function (defined in llvm-helper.h) which requires
 * special handling. 
 */
struct HelperInfo
{
    HelperInfo()
    {
        isInitialized = false;
        States.clear();
        mayConflict = false;
        mayConflictArg = false;
        hasNestedCall = false;
#if defined(TARGET_I386)
        hasFP = false;
#endif
    }

    Function *Func;          /* Function symbol to be inlined */
    Function *FuncNoInline;  /* Function symbol not to be inlined */
    set<size_t> StateUse;
    set<size_t> StateDef;
    map<size_t,int> States;  /* CPU states manipulated by this function */
    CodeMetrics Metrics;     /* Inlining metrics */
    vector<ArgInfo> ArgumentWeights;  /* Weight of the function arguments */

    bool isInitialized;
    bool mayConflict;        /* States conflict with state mapping or not */
    bool mayConflictArg;     /* Arguments conflict with state mapping or not */
    bool hasNestedCall;      /* This function has nested function or not */
#if defined(TARGET_I386)
    bool hasFP;              /* This function has floating-point ops or not */
#endif

    /* insertState - Set the registers' constraint that are manipulated by this
     * helper function. */
    inline void insertState(size_t Off, bool IsClobber)
    {
        if (IsClobber)
            StateDef.insert(Off);
        else
            StateUse.insert(Off);
    }
    inline void insertState(struct HelperInfo *toAdd)
    {
        StateUse.insert(toAdd->StateUse.begin(), toAdd->StateUse.end());
        StateDef.insert(toAdd->StateDef.begin(), toAdd->StateDef.end());
    }
};

/*
 * Address information in the code cache for block/region chaining.
 */
struct ChainInfo
{
    uint32_t Off;          /* Chaining offset to the start address of code */
    int NextOff;           /* Next offset relative to Off */
    int JmpOff;            /* Jump offset relative to Off */
    tcg_target_ulong Key;  /* Unique ID to this chaining point */
    target_ulong PC;       /* Target address (guest) of this chaining */

    ChainInfo() {}
    ChainInfo(uint32_t off, int next_off=0, int jmp_off=0,
            unsigned long key=0, target_ulong pc=0) :
        Off(off), NextOff(next_off), JmpOff(jmp_off),
        Key(key), PC(pc) {}
};

/* 
 * PatchInfo describes the rules for patching translated code.
 */
class PatchInfo
{
public:
    vector<uint32_t> toExit;    /* Offsets going to dispatch */
    vector<ChainInfo> toExec;   /* ChainInfo going to another code region */
    vector< pair<uint32_t,int> > ExternalSymbol; /* Offsets of external
                                                    symbols */
    vector< pair<uint32_t,int> > ConstPool;  /* Offsets of constant pools */
    int ConstPoolSize;          /* Host code size of all constant pools */
};

/*
 * IBChainInfo is used to implement the indirect branch chaining proposed in
 * the Pin paper published in PLDI'05.
 *   Chi-Keung Luk, Robert Cohn, Robert Muth, Harish Patil, Artur Klauser,
 *   Geoff Lowney, Steven Wallace, Vijay J. Reddi and Kim Hazelwood, "Pin: 
 *   Building Customized Program Analysis Tools with Dynamic Instrumentation,"
 *   Proceedings of the 2005 ACM SIGPLAN Conference on Programming Language
 *   Design and Implementation, pages 190-200, 2005.
 *
 * Different to the Pin paper, we do not take special handling for the return
 * instructions (this is, we treat the return instructions the same as other
 * indirect branches. Moreover, as the chain list achieves the maximum size,
 * we force the target translation to look up a global IBTC hash table, instead
 * of jumping to another hashed comparison chain used by Pin.
 */
class IBChainInfo
{
private:
    tcg_target_ulong PatchAddr;     /* Address of the IB chain head */
    vector<uint8_t*> Chain;         /* Pointer to each chain element */
    vector<uint16_t> ChainOffset;   /* Offset of the jump instruction to the
                                       next chain element */
    vector<target_ulong> ChainedPC; /* PC of each chain element */

public:
    IBChainInfo() {}

    inline int getChainSize()         { return Chain.size(); }
    inline uint8_t *getChain(int Idx) { return Chain[Idx];   }
    inline tcg_target_ulong getPatchAddr(int Idx)
    {
        return (Idx < 0) ? PatchAddr : (unsigned long)Chain[Idx] + ChainOffset[Idx];
    }
    inline void setPatchAddr(tcg_target_ulong addr) { PatchAddr = addr; }
    inline void addChain(uint8_t *ChainAddr, uint16_t PatchOffset, target_ulong PC)
    {
        Chain.push_back(ChainAddr);
        ChainOffset.push_back(PatchOffset);
        ChainedPC.push_back(PC);
    }
    bool isPCChained(target_ulong pc)
    {
        for (int i = 0, e = ChainedPC.size(); i < e; i++)
        {
            if (pc == ChainedPC[i])
                return true;
        }
        return false;
    }
};

/*
 * Code Information (Block and Region)
 */
#define STATE_NONE          (uint32_t)0
#define STATE_LOOPHEADER    ((uint32_t)1 << 0)
#define STATE_MERGED        ((uint32_t)1 << 1)
class CodeInfo
{
public:
    bool isBlock;           /* The translation unit is block or not */
    bool Committed;         /* Set if the code has completed the translation */
    int Version;            /* Version of the region */
    target_ulong GuestPC;   /* Start PC of the entry block */
    uint16_t GuestSize;     /* Aggregate guest size of all blocks */
    uint32_t GuestICount;   /* Aggregate guest icount of all blocks */
    unsigned long HostPC;   /* Start PC of the translated host code */
    uint32_t HostSize;      /* Size of the translated code */
    GraphNode *CFG;         /* CFG of this region */
    map<BasicBlock*,Instruction*> InsertPos;  /* Position to store dirty 
                                                 states */
    map<long,int> CallPos;  /* Position of TLBMiss helper calls */
    set<target_ulong> Link; /* Target PCs (guest) this region jumps to */
    set<target_ulong> IBLink; /* IB target PCs (guest) this region jumps to */
    map<target_ulong, NodeVec> LinkMap; /* Map of target PC to CFGNode */
    set<target_ulong> PCSet;  /* PCs of the guest block of this region */
    uint32_t State;         /* Set to 1 if this code is selected for merging */
    uint32_t MonitorID;     /* DBO monitoring ID of this region */
    Function *Func;         /* LLVM function of this translation */

    /* getInsertPos - Given a BB, return its insertion point or NULL if it
     * cannot be found. */
    Instruction *getInsertPos(BasicBlock *BB)
    {
        return (InsertPos.find(BB) == InsertPos.end()) ? NULL : InsertPos[BB];
    }
    /* getLink - Give a target PC, return the node jumping to this target PC */
    NodeVec &getLink(target_ulong pc)   { return LinkMap[pc];            }
    map<long,int> &getCallPos()         { return CallPos;                }

    void setInsertPos(Instruction *I)   { InsertPos[I->getParent()] = I; }
    void setLink(target_ulong pc)       { Link.insert(pc);               }
    void setLink(target_ulong pc, GraphNode *Node)
    {
        Link.insert(pc);
        LinkMap[pc].push_back(Node);
    }
    void setIBLink(target_ulong pc)
    {
        if (IBLink.count(pc))
            return;
        IBLink.insert(pc);
        setLink(pc, CFG);
    }
};

/* Code information of a single block. */
class BlockInfo : public CodeInfo
{
public:
    BlockInfo()
    {
        Committed = false;
        isBlock = true;
        Version = 0;
        State = STATE_NONE;
    }
    ~BlockInfo() { DeleteCFG(CFG); }

    void Commit(TranslationBlock *TB, int host_size)
    {
        GuestPC = TB->pc;
        GuestICount = TB->icount;
        GuestSize = TB->size;
        HostPC = (unsigned long)TB->tc_ptr;
        HostSize = host_size;

        PCSet.insert(TB->pc);
        CFG = new GraphNode(TB);
        for (int i = 0; i < 2; i++)
        {
            if (TB->jmp_pc[i] != (target_ulong)-1)
            {
                setLink(TB->jmp_pc[i], CFG);
                if (TB->jmp_pc[i] == TB->pc)
                    CFG->setSuccessor(CFG);
            }
        }
        Committed = true;
    }
};

/* Code information of a region. */
class TraceInfo : public CodeInfo
{
public:
    TraceInfo(int version, uint32_t id)
    {
        Committed = false;
        isBlock = false;
        Version = version;
        State = STATE_NONE;
        ID = id;

        GuestICount = GuestSize = HostSize = 0;
        NumBlock = NumExit = NumIndirectBranch = 0;
        LoopHeader.clear();
        Exit = NULL;
    }
    ~TraceInfo() { DeleteCFG(CFG); }
    
    uint32_t ID;              /* Unique region ID */
    BBSet LoopHeader;         /* Set of the loop head blocks */
    int NumBlock;             /* Number of guest blocks */
    int NumLoop;              /* Number of loops */
    int NumExit;              /* Number of side exits */
    int NumIndirectBranch;    /* Number of indirect branches */
    vector<target_ulong> PC;  /* List of start PCs of guest blocks */
    vector<TranslationBlock*> TB;  /* List of guest blocks */
    uint64_t **Exit;          /* Counters for edge profile */
    NodeVec CFGVec;           /* List of CFG nodes */
    NodeMap CFGMap;           /* Map of CFG nodes */

    void Commit(unsigned long pc, int size)
    {
        HostPC = pc;
        HostSize = size;
        Committed = true;
    }
    NodeMap &getCFGMap()           { return CFGMap;           }
    void setID(uint32_t id)        { ID = id;                 }
    void setExit(uintptr_t exit)   { Exit = (uint64_t**)exit; }
    void setCFGMap(NodeMap &CMap)  { CFGMap = CMap;           }
    void addGuestICount(int Count) { GuestICount += Count;    }
    void addGuestSize(int Size)    { GuestSize += Size;       }
};

/*
 * TraceBuilder provides the facilities to build a region in IRFactory.
 */
class TraceBuilder
{
private:
    TraceInfo *Trace;     /* The region to build */
    GraphNode *CFG;       /* CFG of the region */
    GraphNode *CurrNode;  /* The current CFG node to process */
    NodeMap Depth;        /* Depth of the CFG node */
    BBMap BBDepth;        /* Map of CFG node and its depth */
    BranchVec Branches;   /* Branches which jump to the region internally */
    NodeBBMap Node2BB;    /* Map of CFG node and LLVM BasicBlock */
    NPSet Links;          /* Internal links of CFG nodes */
    map<CodeKey, GraphNode*> UniqNode;  /* Map of (PC,CS) and CFG node */
    NodeVec NodeQueue;    /* Queued CFG node */

public:
    TraceBuilder() : Trace(NULL) {}
    ~TraceBuilder() {}

    /* Init - Initialization of the region before building it. */
    CodeInfo *Init(GraphNode *cfg, int version);

    inline void reset()
    {
        Depth.clear();
        BBDepth.clear();
        Branches.clear();
        Node2BB.clear();
        Links.clear();
        UniqNode.clear();
        NodeQueue.clear();
    }

    /* lookupNode - Determine if the given PC:CS is in one of the nodes. */
    inline GraphNode *lookupNode(target_ulong pc, target_ulong cs_base)
    {
        CodeKey Key = CodeKey(pc, cs_base);
        return UniqNode.count(Key) ? UniqNode[Key] : NULL;
    }

    TraceInfo *getTrace()     { return Trace;           }
    NodeMap &getDepth()       { return Depth;           }
    int getCurrDepth()        { return Depth[CurrNode]; }
    int getBBDepth(const BasicBlock *BB) { return BBDepth[BB]; }
    GraphNode *getEntryNode() { return CFG;             }
    GraphNode *getCurrNode()  { return CurrNode;        }
    NodeBBMap &getNode2BB()   { return Node2BB;         }
    BranchVec &getBranches()  { return Branches;        }
    NPSet &getLinks()         { return Links;           }

    /* getNextNode - Get the next node to be translated. */
    GraphNode *getNextNode()
    {
        if (NodeQueue.empty())
            return NULL;
        GraphNode *Node = NodeQueue.back();
        NodeQueue.pop_back();
        return Node;
    }

    /* setNode - Bind the CFG node to BasicBlock. */
    void setNode(GraphNode *Node, BasicBlock *BB)
    {
        CurrNode = Node;
        Node2BB[Node] = BB;
        BBDepth[BB] = Depth[Node];

        Trace->TB.push_back(Node->getTB());
        Trace->PC.push_back(Node->getGuestPC());
        Trace->PCSet.insert(Node->getGuestPC());
    }
    void setExit(target_ulong PC) { Trace->setLink(PC, CurrNode); }

    /* setSuccessor - Set the internal link of two CFG nodes. If the target
     * node is not translated yet, add it to the node queue. */
    void setSuccessor(Instruction *I, GraphNode *Node)
    {
        Branches.push_back(make_pair(I, Node));
        if (Node2BB.count(Node) == 0)
        {
            Node2BB[Node] = NULL;
            NodeQueue.push_back(Node);
        }
        Links.insert(make_pair(CurrNode, Node));
    }

    /* setUniqueNode - Set the list of unique CFG nodes. That is, two CFG nodes
     * with the same PC:CS have only one record. */
    inline void setUniqueNode(GraphNode *Node)
    {
        CodeKey Key = CodeKey(Node->getGuestPC(), Node->getGuestCS());
        if (UniqNode.count(Key))
            return;
        UniqNode[Key] = Node;
    }
};

enum
{
    STAGE_ONE = 0,
    STAGE_TWO,
    STAGE_THREE,
    NUM_STAGE,
};

/*
 * IRFactory is the main component to perform QEMU TCG opcodes to LLVM IR
 * conversion.
 */
class IRFactory
{
private:
    /* Basic types */
    CONST Type *VoidTy;
    CONST IntegerType *Int8Ty;
    CONST IntegerType *Int16Ty;
    CONST IntegerType *Int32Ty;
    CONST IntegerType *Int64Ty;
    CONST IntegerType *IntPtrTy;
    CONST PointerType *Int8PtrTy;
    CONST PointerType *Int16PtrTy;
    CONST PointerType *Int32PtrTy;
    CONST PointerType *Int64PtrTy;

    LLVMContext &Context;    /* Translator local context */
    Module *Mod;             /* The LLVM module */
    ExecutionEngine *EE;     /* The JIT compiler */
    CallBackInfo *CallBack;  /* Info to pass among translator and JIT */
    const TargetData *TD;    /* Target data */
    int BaseRegNo;           /* Base register # to hold the CPUState pointer */
    char *BaseRegStr;        /* Name of the base register */
    CONST Type *BaseRegType; /* LLVM Type of the base register */
    map<string, HelperInfo*> &HelperFuncs; /* Helper with special handling */
    FunctionPassManager **Passes;   /* Optimization passes */

    Function *Func;          /* The container of LLVM IR to be translated */
    BasicBlock *InitBB;      /* BasicBlock for variable decalaration */
    BasicBlock *CurrBB;      /* Current BasicBlock to insert LLVM IR */
    BasicBlock *ExitBB;      /* Temp BasicBlock as the exit-function stub */
    BranchInst *LastInst;    /* Position to insert LLVM IR */
    Instruction *InitLastInst; /* Position to declare variables */
    Instruction *InitGEPInst;  /* Position to declare GEP instruction */

    int Mode;          /* Optimization mode */
    uint32_t UID;      /* IRFactory's internal unique ID */
    int Pass;          /* Pass stage */
    uint32_t CallIdx;  /* CallTable index */
    int nb_globals;    /* The number of TCG global register */
    int nb_temps;      /* The number of TCG temp register */
    BaseRegister BaseReg[TCG_TARGET_NB_REGS]; /* Base register */
    BasicBlock *Labels[TCG_MAX_LABELS];       /* Labels as TCG label */
    tcg_target_long FrameStart;  /* Offset of frame for register spilling */
    Register *FrameReg;          /* Base register for the frame space */
    Register *Reg;               /* Record of TCG pseudo registers */
    CodeInfo *Code;              /* Information of current translated code */
    IVec Separator;              /* Separators to guest assembly */
    IVec toErase;                /* List of Instructions to be removed */

    /* Cache of pointers and Types to CPUState registers */
    map<tcg_target_long, GetElementPtrInst*> AddrCache;
    map<CONST Type*, map<Value*, Value*> > TypeCache;

    IVec InlineCalls;    /* Helpers to be inlined */
    IVec NoInlineCalls;  /* Helpers not to be inlined */

    /* InitBasicTypes - Initialize basic types used during IR conversion. */
    void InitBasicTypes();

    /* InitContext - Initialize TCG's context including frame info and pseudo
     * registers. */
    void InitContext(TCGContext *s);

    /* InitFunction - Prepare the LLVM Function and arrange the initial
     * BasicBlocks and variable declaration. */
    void InitFunction(target_ulong pc, int mode);

    /* LookupAddr - Get or insert the pointer to the CPU register in the
     * AddrCache. */
    Value *LookupAddr(Register *reg, CONST Type *type);
    Value *LookupAddr(Value *Base, tcg_target_ulong Off, CONST Type *type);

    /* LoadInput - Load the value of CPU register from the CPUState in memory
     * or return from the LLVM pseudo register if it has been promoted. */
    Value *LoadInput(Register *reg);

    /* SaveGlobals - Store dirty states back to CPUState in memory. */
    void SaveGlobals(int coherence_level, Instruction *InsertBefore);

    /* AnalyzeInlineCost - Analyze a helper function to determine if it will be
     * inlined or not. */
    int AnalyzeInlineCost(CallSite CS);

    /* ProcessInline - Perform helper function inlining. */
    bool ProcessInline(IVec &Calls, bool NeedAnalysis);

    /* InsertPatchIBChain - Reserve space in the code cache for IB chaining. */
    inline void InsertPatchIBChain(MDNode *MD, Instruction *InsertBefore);

    /* InsertPatchJmp - Reserve space in the code cache for trace chaining. */
    inline void InsertPatchJmp(MDNode *MD, Instruction *InsertBefore);

    /* InsertPatchReturn - Reserve space in the code cache for jumping back
     * to the dispatcher. */
    inline void InsertPatchReturn(MDNode *MD, unsigned long RetVal,
            Instruction *InsertBefore);

    /* InsertLookupIBTC - Insert code for IBTC hash table lookup. */
    inline void InsertLookupIBTC(Value *GuestEIP, Instruction *InsertBefore);

    /* CreateBSwap - Insert bswap intrinsic instruction. */
    inline Value *CreateBSwap(CONST Type *Ty, Value *V,
            Instruction *InsertPos);

    /* QEMULoad - Load data from the guest memory. */
    Value *QEMULoad(Value *base, tcg_target_long offset, int sizeop,
            int mem_index);

    /* QEMUStore - Store data to the guest memory. */
    void QEMUStore(Value *data, Value *base, tcg_target_long offset,
            int sizeop, int mem_index);

    /* ReformatStoreEIP - For the guest register size longer than the host,
     * replace the multiple store-EIP instructions to one single store-EIP
     * instruction. */
    StoreInst *ReformatStoreEIP(Instruction *Inst);

    /* TraceLink - Perform internal linking of basic blocks to form a region. */
    int TraceLink(StoreInst *SI);

    /* TraceLinkPHI - Perform internal linking of basic blocks that have common
     * successor. */
    int TraceLinkPHI();

    /* TraceLinkDirectJump - Link basic blocks of direct branch. */
    void TraceLinkDirectJump(GraphNode *Node, StoreInst *SI, int mode);

    /* TraceLinkIndirectJump - Link basic blocks of indirect branch. */
    void TraceLinkIndirectJump(GraphNode *Node, Value *GuessPC);

    /* TraceCheckStoreEIP - Early check if the last instruction is a store-EIP
     * instruction. */
    void TraceCheckStoreEIP(StoreInst *SI);

    /* SaveTraceEIP - Insert store-EIP instruction in the IR container. */
    void SaveTraceEIP(Instruction *InsertBefore);

    /* TraceRenameBB - Rename the name of one LLVM BasicBlock. */
    inline void TraceRenameBB(Instruction *I, string Append);

#if defined(TARGET_SPARC)
    /* Special routines for the SPARC guest. */
    void TraceLinkDirectJump(GraphNode *Node, StoreInst *SI, StoreInst *SI2,
            int mode);
    void TraceLinkIndirectJump(GraphNode *Node, Value *GuessPC,
            Value *GuessNPC);
    inline void InsertLookupIBTC(Value *GuestEIP, Value *GuestEIP2,
            Instruction *InsertBefore);
#endif

    /* runPasses - Perform optimization passes. */
    void runPasses();

    /* getPointerTy - Given the size, return its PointerType. */
    CONST PointerType *getPointerTy(int size);

    /* getUID - Get a unique ID. */
    Constant *getUID();

    /* setInsertPos - Set the insertion position for storing dirty states. */
    void setInsertPos(Instruction *I) { Code->setInsertPos(I); }

    /* setSuccessor - Set the jump from the instruction BI in the predecessor
     * block to the successor block BB. */
    inline void setSuccessor(BranchInst *BI, BasicBlock *BB);

public:
    IRFactory(TCGContext *s, LLVMTranslator *Trans);
    ~IRFactory();

    LLVMTranslator *Translator;  /* Uplink to the LLVMTranslator instance */
    Instruction *CPU;            /* Base register with (char*) type */
    Instruction *CPUStruct;      /* Base register with (struct CPUState*) type */
    Value *CPUStructAsm;         /* Inline asm of base register with
                                    `struct CPUState*' type. */
    TraceBuilder *TBuilder;      /* The trace builder */

    /* QEMU TCG IR to LLVM IR converion routines. */
#define DEF(name, oargs, iargs, cargs, flags) int op_ ## name(const TCGArg *);
#include "tcg-opc.h"
#undef DEF

    /* addNoInlineCall - Add the helper function to the non-inline list. */
    void addNoInlineCall(Instruction *I) { NoInlineCalls.push_back(I); }

    /* InsertSeparator - Insert the seperator indicating the last opcode of the
     * guest instruction. */
    void InsertSeparator();
    void InsertSeparator(Instruction *InsertPos);

    /* BlockSetContext - Initialize the context for a new translation with the
     * block-level translation. */
    void BlockSetContext(int mode, TCGContext *s, target_ulong pc);

    /* TraceSetContext - Initialize the context for a new translation with the
     * region-level translation. */
    void TraceSetContext(int mode, TCGContext *s, GraphNode *Node);

    /* TraceInit - Reset the trace builder for a new region formation. */
    void TraceInit(target_ulong PC, int Count, GraphNode *CFG, int Version);

    /* TraceFinalize - Finalize the translation of a region. */
    void TraceFinalize();

    /* PreProcess - Legalize LLVM IR before running the pre-defined passes. */
    void PreProcess();

    /* PostProcess - Legalize LLVM IR after running the pre-defined passes. */
    void PostProcess();

    /* Compile - This function is called to start the LLVM JIT compilation. */
    void Compile();

    int getPass()              { return Pass;                 }
    int getMode()              { return Mode;                 }
    Module *getModule()        { return Mod;                  }
    Function *getFunction()    { return Func;                 }
    LLVMContext &getContext()  { return Context;              }
    BaseRegister *getBaseReg() { return BaseReg;              }
    CodeInfo *getCodeInfo()    { return Code;                 }
    TraceInfo *getTrace()      { return TBuilder->getTrace(); }
    IVec &getSeparator()       { return Separator;            }

    /* getOpcFn - Get the function pointer of the IR converion routines. */
    void *getOpcFn();
};

/* Determine if the offset is the EIP register. */
bool isStoreEIP(size_t Off);

/* Get the real PC by combining PC:CS. */
void ResolvePC(target_ulong &PC, target_ulong &CS);

/* Perform instruction removal from the parent container. */
void ProcessErase(IVec &toErase);

/* Remove an instruction and its dependent instructions if they are also dead. */
void DeleteDeadInstructions(Instruction *I);

/* Return the exact base pointer and offset for the given address Ptr. */
Value *getBaseWithConstantOffset(const TargetData *TD, Value *Ptr,
        size_t &Offset);

/* Return the possible base pointer and offset for the give address Ptr. */
Value *getBaseWithPossibleOffset(const TargetData *TD, Value *Ptr,
        size_t &Offset, Value **LastPtr);

/* Return a metadata node. */
MDNode *CreateMDNode(LLVMContext &Context, Value **M, int count);

/* Return a CallInst. */
CallInst *CreateCall(Value *Fn, const char *Name, Instruction *InsertPos);
CallInst *CreateCall(Value *Fn, SmallVector<Value*,4> &Params, const char *Name,
        Instruction *InsertPos);

/* Return a PHINode. */
inline PHINode *CreatePHI(CONST Type *Ty, int Count, Instruction *InsertPos);

extern "C"
{
/* Copy the global TCG context to a thread-local context. */
void copy_tcg_context(CPUState *env);
/* Reset the thread-local context. */
void reset_tcg_context();
int llvm_tb_alloc();
int llvm_tb_alloc_size(int size);
int llvm_regen_trace(GraphNode *CFG, int count, int version);
}

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
