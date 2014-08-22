/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef __LLVM_CFG_H
#define __LLVM_CFG_H

#include <set>
#include <map>
#include "llvm/Instructions.h"
#include "llvm/BasicBlock.h"

using namespace llvm;

class GraphNode;
class CodeInfo;

typedef set< GraphNode* > NodeSet;
typedef vector< GraphNode* > NodeVec;
typedef set< pair<GraphNode*,GraphNode*> > NPSet;
typedef map< target_ulong, GraphNode* > GuestNodeMap;
typedef map< GraphNode*, int > NodeMap;
typedef map< GraphNode*, GraphNode* > Node2Map;
typedef map< GraphNode*, BasicBlock* > NodeBBMap;
typedef vector< pair< Instruction*, GraphNode* > > BranchVec;

typedef vector< CodeInfo* > CodeVec;
typedef set< CodeInfo* > CodeSet;
typedef map< CodeInfo*, CodeVec > SuccMap;
typedef pair< CodeInfo*, CodeVec > SuccPair;

/*
 * CompactInfo is used to format a CFG in a compact form. A compact CFG is the
 * CFG without duplicate node (i.e. two identical nodes are merged as one).
 */
struct CompactInfo
{
    GuestNodeMap GNMap;
    NPSet NPair;
};

/*
 * GraphNode is used to describe the information of one single node in a CFG.
 * The information includes the address information of a block and its
 * outlinks, and such information can be used to build a successor CFG.
 */
class GraphNode
{
private:
    TranslationBlock *TB;  /* The basic block information */
    target_ulong PC;       /* PC of this node */
    target_ulong CS;       /* CS of this node */
    GraphNode *EntryNode;  /* The root of this CFG */
    NodeVec Successor;     /* The successor nodes */
    
public:
    GraphNode(TranslationBlock *tb) : 
        TB(tb), PC(tb->pc), CS(tb->cs_base), EntryNode(this) {}
    GraphNode(target_ulong pc, target_ulong cs) : 
        TB(NULL), PC(pc), CS(cs), EntryNode(this) {}
    ~GraphNode() { Successor.clear(); }

    TranslationBlock *getTB()           { return TB; }
    target_ulong getGuestPC()           { return PC; }
    target_ulong getGuestCS()           { return CS; }
    GraphNode *getEntryNode()           { return EntryNode; }
    NodeVec &getSuccessor()             { return Successor; }
    
    void setEntryNode(GraphNode *Node)  { EntryNode = Node; }

    /* setSuccessor - Add one successor node. */
    void setSuccessor(GraphNode *Node)
    {
        Successor.push_back(Node);
        Node->setEntryNode(EntryNode);
    }

    /* setSuccessor - Add a list of successor nodes. */
    void setSuccessor(NodeVec &Nodes)
    {
        while (!Nodes.empty())
        {
            GraphNode *Node = Nodes.back();
            Nodes.pop_back();
            Successor.push_back(Node);
            Node->setEntryNode(EntryNode);
        }
    }
    void removeSuccessor() { Successor.clear(); }
};

/*
 * ControlFlowGraph is used to build the whole program control flow graph (CFG).
 * GlobalCFG uses this structure to maintain a whole program CFG connected by
 * direct branches.
 */
class ControlFlowGraph
{
public:
    ControlFlowGraph()  { llvm_lock_init(&Lock); }
    ~ControlFlowGraph() {}

    llvm_lock_t Lock;  /* The lock of this graph */
    map<target_ulong, TranslationBlock*> Nodes; /* Mapping of PC and block */
    map<TranslationBlock*, vector<TranslationBlock*> > SuccCFG;
    map<TranslationBlock*, vector<TranslationBlock*> > IBSuccCFG;

    /* reset - Reset to an empty graph. */
    void reset()
    {
        Nodes.clear();
        SuccCFG.clear();
        IBSuccCFG.clear();
    }

    /* addEdge - Add one direct link to this graph. */
    void addEdge(TranslationBlock *Src, TranslationBlock *Dst)
    {
        SuccCFG[Src].push_back(Dst);
    }

    /* addIBEdge - Add one indirect link to this graph. */
    void addIBEdge(TranslationBlock *Src, TranslationBlock *Dst)
    {
        vector<TranslationBlock*> &IBSucc = IBSuccCFG[Src];
        if (IBSucc.size() == 8)
            IBSucc.erase(IBSucc.begin());
        IBSucc.push_back(Dst);
    }

    /* hasNode - Determine if this graph consists of the given node. */
    bool hasNode(TranslationBlock *TB)
    {
        return (Nodes.count(TB->pc) == 0) ? false : true;
    }

    vector<TranslationBlock*> &getSuccessor(TranslationBlock *TB)
    {
        return SuccCFG[TB];
    }
    vector<TranslationBlock*> &getIBSuccessor(TranslationBlock *TB)
    {
        return IBSuccCFG[TB];
    }
};

/* Return an copy of the input CFG. */
GraphNode *CloneCFG(GraphNode *Node, Node2Map &NodeMapping);

/* Return an copy of the input CFG in a compact form. */
GraphNode *CloneCompactCFG(GraphNode *Node, Node2Map &NodeMapping,
        CompactInfo &CI);

/* Remove one CFG. */
void DeleteCFG(GraphNode *Root);
void DeleteCFG(NodeMap &CFGMap);

/* Build one new CFG by cloning the input CFGs and then connecting them. The 
 * original CFG is not changed. */
void BuildCFG(CodeInfo *Parent, SuccMap &SMap, Node2Map &NodeMapping,
        int &Version);

/* Build one new compact CFG by cloning the input CFGs and then connecting
 * them. The original CFG is not changed. */
void BuildCompactCFG(CodeInfo *Parent, SuccMap &SMap, Node2Map &NodeMapping,
        CompactInfo &CI, int &Version);

/* Connecting multiple compact CFGs. No copy of original CFG is performed and
 * the original CFGs are changed. */
void LinkCompactCFG(CompactInfo &CI);

/* Search for the loop header of the input CFG. */
void FindLoopHeader(SuccMap &CodeSucc, CodeInfo *Code, CodeSet &Result);

/* Collect all the regions that can be reached from the region `Code'. */
void FindLinkedNodes(SuccMap &SMap, CodeInfo *Code, CodeSet &Result);

/* Encode a CFG in an buffer. */
int EncodeCFG(char *buf, GraphNode *CFG, NodeVec &CFGVec);

/* Decode a CFG from an buffer. */
int DecodeCFG(char *buf, target_ulong cs_base, GraphNode **CFG, int &Count,
        target_ulong **PCs, NodeMap &CFGMap);

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
