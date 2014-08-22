/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <map>
#include <unistd.h>
#include "llvm.h"
#include "utils.h"

extern LLVMDebug *DM;

/*
 * CloneCFG()
 *  Return an copy of the input CFG.
 */
GraphNode *CloneCFG(GraphNode *Node, Node2Map &NodeMapping)
{
    GraphNode *Parent = new GraphNode(Node->getTB());
    NodeMapping[Node] = Parent;

    NodeVec &Successor = Node->getSuccessor();
    for (int i = 0, e = Successor.size(); i < e; i++)
    {
        GraphNode *Child;
        if (NodeMapping.count(Successor[i]) == 0)
            Child = CloneCFG(Successor[i], NodeMapping);
        else
            Child = NodeMapping[Successor[i]];
        Parent->setSuccessor(Child);
    }

    return Parent;
}

/*
 * DeleteCFG()
 *  Remove one CFG.
 */
void DeleteCFG(GraphNode *Root)
{
    NodeVec VisitStack;
    NodeSet Visited;
    
    VisitStack.push_back(Root);
    while(VisitStack.size())
    {
        GraphNode *Parent = VisitStack.back();
        VisitStack.pop_back();
        
        if (Visited.count(Parent))
            continue;
        
        Visited.insert(Parent);
        NodeVec &Child = Parent->getSuccessor();
        for (int i = 0, e = Child.size(); i < e; i++)
            VisitStack.push_back(Child[i]);
    }

    for (NodeSet::iterator I = Visited.begin(), E = Visited.end(); I != E; I++)
        delete *I;
}

/*
 * DeleteCFG()
 *  Remove one CFG.
 */
void DeleteCFG(NodeMap &CFGMap)
{
    for (NodeMap::iterator I = CFGMap.begin(), E = CFGMap.end(); I != E; I++)
        delete I->first;
}

/*
 * BuildCFG()
 *  Build one new CFG by cloning the input CFGs and then connecting them. The
 *  original CFG is not changed.
 */
void BuildCFG(CodeInfo *Parent, SuccMap &SMap, Node2Map &NodeMapping,
        int &Version)
{
    GraphNode *ChildCFG;
    
    CloneCFG(Parent->CFG, NodeMapping);

    CodeVec &Successor = SMap[Parent];
    int NumSucc = Successor.size();
    if (NumSucc == 0)
        return;

    for (int i = 0; i < NumSucc; i++)
    {
        CodeInfo *Child = Successor[i];
        if (Child->Version > Version)
            Version = Child->Version;

        if (NodeMapping.count(Child->CFG) == 0)
            BuildCFG(Child, SMap, NodeMapping, Version);
        
        ChildCFG = NodeMapping[Child->CFG];

        NodeVec &Links = Parent->getLink(Child->GuestPC);
        int NumLinks = Links.size();
        for (int j = 0; j < NumLinks; j++)
        {
            if (NodeMapping.find(Links[j]) == NodeMapping.end())
               DM->Error("%s: fatal error on %x.\n", __func__, Child->GuestPC);

            GraphNode *LinkNode = NodeMapping[Links[j]];
            LinkNode->setSuccessor(ChildCFG);
        }
    }
}

/*
 * CloneCompactCFG()
 *  Return an copy of the input CFG in a compact form.
 */
GraphNode *CloneCompactCFG(GraphNode *Node, Node2Map &NodeMapping,
        CompactInfo &CI)
{
    target_ulong GuestPC = Node->getGuestPC();

    if (CI.GNMap.count(GuestPC) == 0)
        CI.GNMap[GuestPC] = new GraphNode(Node->getTB());

    GraphNode *Parent = CI.GNMap[GuestPC];
    NodeMapping[Node] = Parent;

    NodeVec &Successor = Node->getSuccessor();
    int NumSucc = Successor.size();
    if (NumSucc == 0)
        return Parent;

    for (int i = 0; i < NumSucc; i++)
    {
        GraphNode *Child;
        if (NodeMapping.count(Successor[i]) == 0)
            Child = CloneCompactCFG(Successor[i], NodeMapping, CI);
        else
            Child = NodeMapping[Successor[i]];

        if (CI.NPair.count(make_pair(Parent,Child)) == 0)
            CI.NPair.insert(make_pair(Parent,Child));
    }

    return Parent;
}

/*
 * BuildCompactCFG()
 *  Build one new compact CFG by cloning the input CFGs and then connecting
 *  them. The original CFG is not changed.
 */
void BuildCompactCFG(CodeInfo *Parent, SuccMap &SMap, Node2Map &NodeMapping,
        CompactInfo &CI, int &Version)
{
    GraphNode *ChildCFG;
    
    CloneCompactCFG(Parent->CFG, NodeMapping, CI);

    CodeVec &Successor = SMap[Parent];
    int NumSucc = Successor.size();
    if (NumSucc == 0)
        return;

    for (int i = 0; i < NumSucc; i++)
    {
        CodeInfo *Child = Successor[i];
        if (Child->Version > Version)
            Version = Child->Version;

        if (NodeMapping.count(Child->CFG) == 0)
            BuildCompactCFG(Child, SMap, NodeMapping, CI, Version);
        
        ChildCFG = NodeMapping[Child->CFG];

        NodeVec &Links = Parent->getLink(Child->GuestPC);
        int NumLinks = Links.size();
        for (int j = 0; j < NumLinks; j++)
        {
            if (NodeMapping.count(Links[j]) == 0)
                DM->Error("%s: fatal error on %x.\n", __func__, Child->GuestPC);

            GraphNode *LinkNode = NodeMapping[Links[j]];
            if (CI.NPair.count(make_pair(LinkNode,ChildCFG)) == 0)
                CI.NPair.insert(make_pair(LinkNode,ChildCFG));
        }
    }
}

/*
 * LinkCompactCFG()
 *  Connecting multiple compact CFGs. No copy of original CFG is performed and
 *  the original CFGs are changed.
 */
void LinkCompactCFG(CompactInfo &CI)
{
    for (NPSet::iterator I = CI.NPair.begin(), E = CI.NPair.end(); I != E; I++)
    {
        GraphNode *Src = I->first;
        GraphNode *Dest = I->second;
        Src->setSuccessor(Dest);
    }
}

/*
 * FindLoopHeader()
 *  Search for the loop header of the input CFG.
 */
void FindLoopHeader(SuccMap &CodeSucc, CodeInfo *Code, CodeSet &Result)
{
    CodeSet Visited;
    CodeSet InStack;
    CodeVec VisitStack;

    Visited.insert(Code);
    InStack.insert(Code);
    VisitStack.push_back(Code);

    do {
        CodeInfo *Parent = VisitStack.back();
        CodeVec &Successor = CodeSucc[Parent];

        CodeInfo *Child = NULL;
        bool FoundNew = false;
        for (int i = 0, e = Successor.size(); i < e; i++)
        {
            Child = Successor[i];
            if (Visited.count(Child) == 0)
            {
                Visited.insert(Child);
                FoundNew = true;
                break;
            }

            /* Successor is in VisitStack, it's a back edge. */
            if (InStack.count(Child))
                Result.insert(Child);
        }

        if (FoundNew)
        {
            /* Go down one level if there is a unvisited successor. */
            InStack.insert(Child);
            VisitStack.push_back(Child);
        }
        else
        {
            /* Go up one level. */
            CodeInfo *Last = VisitStack.back();
            VisitStack.pop_back();
            InStack.erase(Last);
        }
    } while (!VisitStack.empty());
}

/*
 * FindLinkedNodes()
 *  Collect all the regions that can be reached from the region `Code'.
 *  Each region is viewed as one node in the SuccMap.
 */
void FindLinkedNodes(SuccMap &SMap, CodeInfo *Code, CodeSet &Result)
{
    Result.insert(Code);
    CodeVec &Successor = SMap[Code];
    for (int i = 0, e = Successor.size(); i < e; i++)
    {
        if (Result.count(Successor[i]))
            continue;
        FindLinkedNodes(SMap, Successor[i], Result);
    }
}

/*
 * EncodeCFG()
 *  Encode a CFG in an buffer.
 */
int EncodeCFG(char *buf, GraphNode *CFG, NodeVec &CFGVec)
{
    char *p = buf;
    int NumNodes = 0;
    NodeMap CFGMap;
    NodeVec VisitStack;
    NodeSet Visited;

    /* Assign each node a unique number and encode PCs based on this order. */
    VisitStack.push_back(CFG);
    while(VisitStack.size())
    {
        GraphNode *Parent = VisitStack.back();
        VisitStack.pop_back();

        if (CFGMap.count(Parent))
            continue;

        CFGVec.push_back(Parent);
        CFGMap[Parent] = NumNodes++;
        NodeVec &Child = Parent->getSuccessor();
        for (int i = 0, e = Child.size(); i < e; i++)
            VisitStack.push_back(Child[i]);
    }

    encode_int32(p, NumNodes);
    target_ulong *PC = (target_ulong *)p;
    endecode_skip(p, NumNodes * sizeof(target_ulong));
    for (int i = 0, e = CFGVec.size(); i < e; i++)
    {
        GraphNode *Parent = CFGVec[i];
        NodeVec &Child = Parent->getSuccessor();

        encode_int32(p, Child.size());
        for (int j = 0, k = Child.size(); j < k; j++)
            encode_int32(p, CFGMap[Child[j]]);

        PC[i] = Parent->getGuestPC();
    }

    return p - buf;
}

/*
 * DecodeCFG()
 *  Decode a CFG from an buffer.
 */
int DecodeCFG(char *buf, target_ulong cs_base, GraphNode **CFG, int &Count,
        target_ulong **PCs, NodeMap &CFGMap)
{
    char *p = buf;
    int NumNodes;
    target_ulong *PC, *PCList;
    vector<GraphNode*> Nodes;

    decode_int32(p, NumNodes);

    PCList = new target_ulong[NumNodes];
    PC = (target_ulong *)p;
    for (int i = 0; i < NumNodes; i++)
    {
        PCList[i] = PC[i];
        GraphNode *Node = new GraphNode(PC[i], cs_base);
        Nodes.push_back(Node);
        CFGMap[Node] = i;
    }

    endecode_skip(p, NumNodes * sizeof(target_ulong));
    for (int i = 0; i < NumNodes; i++)
    {
        int NumChild;
        GraphNode *Parent = Nodes[i];
        decode_int32(p, NumChild);
        for (int j = 0; j < NumChild; j++)
        {
            int ChildIdx;
            decode_int32(p, ChildIdx);
            Parent->setSuccessor(Nodes[ChildIdx]);
        }
    }

    *CFG = Nodes[0];
    *PCs = PCList;
    Count = NumNodes;

    return p - buf;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */

