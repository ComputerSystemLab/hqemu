//===-- JITMemoryManager.cpp - Memory Allocator for JIT'd code ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the DefaultJITMemoryManager class.
//
//===----------------------------------------------------------------------===//
#ifndef __JITMEMORYMANAGER_H
#define __JITMEMORYMANAGER_H

#include <sys/mman.h>
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm-def.h"
#include "llvm-target.h"

using namespace llvm;

#define DEFAULT_GLOBAL_SIZE     (64 * 1024)
#define MIN_CODE_CACHE_SIZE     (16 * 1024)
#define DEFAULT_CODE_CACHE_SIZE (32 * 1024 * 1024)
#define DEFAULT_THRESHOLD       (4 * 1024)

#ifdef CONFIG_USER_ONLY
#define USE_STATIC_CODE_GEN_BUFFER
extern "C" uint8_t static_code_gen_buffer[];
extern "C" uint8_t StaticCodeCache[];
#else
extern size_t ram_size;
#endif

extern LLVMDebug *DM;
extern int HybridMode;

extern "C"
{
uint8_t *CodeCache;
uint8_t *CodeGenPtr;
size_t CodeCacheSize;
extern int llvm_flush;
}

JITMemoryManager::~JITMemoryManager() {}

/// DefaultJITMemoryManager - Manage memory for the JIT code generation.
/// This splits a large block of MAP_NORESERVE'd memory into two
/// sections, one for function stubs, one for the functions themselves.  We
/// have to do this because we may need to emit a function stub while in the
/// middle of emitting a function, and we don't know how large the function we
/// are emitting is.
class DefaultJITMemoryManager : public JITMemoryManager {
private:
  uint8_t *StubBase;
  uint8_t *GlobalBase;
  uint8_t *CodeBase;
  int32_t StubSize;
  int32_t GlobalSize, GlobalRemain;
  int32_t CodeSize, CodeRemain;
  int32_t Threshold;
  llvm_lock_t GlobalLock;
  llvm_lock_t CodeLock;

  bool UseTmpCodeCache;
  uint8_t *TmpCodeCache;
  int32_t TmpCodeSize;

public:
  DefaultJITMemoryManager();
  ~DefaultJITMemoryManager();

  void AllocateGOT();

  virtual void *getPointerToNamedFunction(const std::string &Name,
          bool AbortOnFailure = true) {
    const char *NameStr = Name.c_str();
    if (NameStr[0] == 1) ++NameStr;
    void *Ptr = sys::DynamicLibrary::SearchForAddressOfSymbol(NameStr);
    if (Ptr) return Ptr;

    if (NameStr[0] == '_') {
      Ptr = sys::DynamicLibrary::SearchForAddressOfSymbol(NameStr+1);
      if (Ptr) return Ptr;
    }
    if (AbortOnFailure)
      DM->Error("Program used external function '%s' which could not be resolved!\n", Name.c_str());
    return NULL;
  }
  
  virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
          unsigned SectionID) { DM->Error("%s: fixme.\n", __func__); return NULL; }
  virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
          unsigned SectionID) { DM->Error("%s: fixme.\n", __func__); return NULL; }

  // Testing methods.
  size_t GetDefaultCodeSlabSize() { DM->Error("%s: fixme.\n", __func__); return 0; }
  size_t GetDefaultDataSlabSize() { DM->Error("%s: fixme.\n", __func__); return 0; }
  size_t GetDefaultStubSlabSize() { DM->Error("%s: fixme.\n", __func__); return 0; }
  unsigned GetNumCodeSlabs() { DM->Error("%s: fixme.\n", __func__); return 0; }
  unsigned GetNumDataSlabs() { DM->Error("%s: fixme.\n", __func__); return 0; }
  unsigned GetNumStubSlabs() { DM->Error("%s: fixme.\n", __func__); return 0; }

  /// startFunctionBody - When a function starts, allocate a block of free
  /// executable memory, returning a pointer to it and its actual size.
  uint8_t *startFunctionBody(const Function *F, uintptr_t &ActualSize) {
    llvm_spin_lock(&CodeLock);
    if (unlikely(ActualSize > 0)) {
#if defined(CONFIG_HYBRID)
      if (ActualSize > (uintptr_t)TmpCodeSize) {
        TmpCodeSize *= 2;
        uint8_t *NewTmpCodeCache = new uint8_t[TmpCodeSize];
        delete [] TmpCodeCache;
        TmpCodeCache = NewTmpCodeCache;
      }

      DM->Error("%s: fix me.\n", __func__);
      llvm_flush = 1;
      ActualSize = TmpCodeSize;
      UseTmpCodeCache = true;
      return TmpCodeCache;
#else
      DM->Error("%s: fatal error. %d < %d\n", __func__, CodeRemain, Threshold);
#endif
    }
    if (unlikely(CodeRemain < Threshold))
        DM->Error("%s: internal error. %d < %d\n", __func__, CodeRemain, Threshold);

    ActualSize = CodeRemain;
    return CodeGenPtr;
  }

  /// endFunctionBody - The function F is now allocated, and takes the memory
  /// in the range [FunctionStart,FunctionEnd).
  void endFunctionBody(const Function *F, uint8_t *FunctionStart,
                       uint8_t *FunctionEnd) {
    assert(FunctionEnd > FunctionStart);
    if (unlikely(UseTmpCodeCache)) {
      UseTmpCodeCache = false;
      llvm_spin_unlock(&CodeLock);
      return;
    }

    int GenSize = FunctionEnd - FunctionStart;
    if (unlikely(GenSize > CodeRemain))
        DM->Error("%s: exceeds available cache size %d > %d\n", __func__, GenSize, CodeRemain);

    if (HybridMode == HYBRID_SERVER) 
        Flush();
    else {
        uint8_t *OldCodeGenPtr = CodeGenPtr;
        CodeGenPtr = (uint8_t *)(((unsigned long)CodeGenPtr + GenSize + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));
        CodeRemain -= (CodeGenPtr - OldCodeGenPtr);
    }
    llvm_spin_unlock(&CodeLock);
  }

  /// allocateSpace - Allocate a memory block of the given size.  This method
  /// cannot be called between calls to startFunctionBody and endFunctionBody.
  uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) {
    DM->Error("%s: fixme.\n", __func__);
    return NULL;
  }

  /// allocateStub - Allocate memory for a function stub.
  uint8_t *allocateStub(const GlobalValue* F, unsigned StubSize,
                        unsigned Alignment) {
    return allocateGlobal(StubSize, Alignment);
  }

  /// allocateGlobal - Allocate memory for a global.
  uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) {
    llvm_spin_lock(&GlobalLock);
    unsigned MisAligned = ((intptr_t)GlobalBase & (Alignment-1));
    if (MisAligned)
      MisAligned = Alignment - MisAligned;

    GlobalRemain -= (Size + MisAligned);
    if (GlobalRemain < 0)
      DM->Error("%s: exceeds available global size %d\n", __func__, GlobalSize);

    uint8_t *GlobalPtr = GlobalBase + MisAligned;
    GlobalBase = GlobalPtr + Size;
    llvm_spin_unlock(&GlobalLock);

    return GlobalPtr;
  }

  /// startExceptionTable - Use startFunctionBody to allocate memory for the
  /// function's exception table.
  uint8_t* startExceptionTable(const Function* F, uintptr_t &ActualSize) {
    return startFunctionBody(F, ActualSize);
  }

  /// endExceptionTable - The exception table of F is now allocated,
  /// and takes the memory in the range [TableStart,TableEnd).
  void endExceptionTable(const Function *F, uint8_t *TableStart,
                         uint8_t *TableEnd, uint8_t* FrameRegister) {
    endFunctionBody(F, TableStart, TableEnd);
  }

  uint8_t *getGOTBase() const { return NULL; }

  /// deallocateFunctionBody - Deallocate all memory for the specified
  /// function body.
  void deallocateFunctionBody(void *Body) {}

  /// deallocateExceptionTable - Deallocate memory for the specified
  /// exception table.
  void deallocateExceptionTable(void *ET) {}

  /// setMemoryWritable - When code generation is in progress,
  /// the code pages may need permissions changed.
  void setMemoryWritable() {}
  /// setMemoryExecutable - When code generation is done and we're ready to
  /// start execution, the code pages may need permissions changed.
  void setMemoryExecutable() {}

  /// setPoisonMemory - Controls whether we write garbage over freed memory.
  ///
  void setPoisonMemory(bool poison) {}

  void *getCodeCacheAddr()  { return CodeCache; }
  size_t getCodeCacheSize() { return CodeCacheSize - DEFAULT_GLOBAL_SIZE; }
  size_t getCodeSize()      { return CodeGenPtr - CodeCache; }
  int isSizeAvailable() { return (CodeRemain >= Threshold) ? 1 : 0; }
  int isSizeAvailable(int Size) { return (CodeRemain >= Threshold && CodeRemain >= Size) ? 1 : 0; }
  void Flush();

  void map_exec(void *addr, long size)
  {
      unsigned long start, end, page_size;
      
      page_size = getpagesize();
      start = (unsigned long)addr;
      start &= ~(page_size - 1);
      
      end = (unsigned long)addr + size;
      end += page_size - 1;
      end &= ~(page_size - 1);
      
      mprotect((void *)start, end - start,
              PROT_READ | PROT_WRITE | PROT_EXEC);
  }
};

void DefaultJITMemoryManager::Flush()
{
    CodeGenPtr = CodeBase;
    CodeRemain = CodeSize;
}

DefaultJITMemoryManager::DefaultJITMemoryManager()
{
#ifdef USE_STATIC_CODE_GEN_BUFFER
    CodeCache = StaticCodeCache;
    map_exec(CodeCache, CodeCacheSize);
#else
    if (CodeCacheSize == 0)
    {
#if defined(CONFIG_USER_ONLY)
        /* in user mode, phys_ram_size is not meaningful */
        CodeCacheSize = DEFAULT_CODE_CACHE_SIZE;
#else
        CodeCacheSize = (unsigned long)(ram_size / 4);
#endif
    }
    if (CodeCacheSize < MIN_CODE_GEN_BUFFER_SIZE)
        CodeCacheSize = MIN_CODE_GEN_BUFFER_SIZE;
    /* The code gen buffer location may have constraints depending on
       the host cpu and OS */
#if defined(__linux__) 
    {
        int flags;
        void *start = NULL;

        flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        flags |= MAP_32BIT;
        /* Cannot map more than that */
        if (CodeCacheSize > (800 * 1024 * 1024))
            CodeCacheSize = (800 * 1024 * 1024);
#elif defined(__sparc_v9__)
        // Map the buffer below 2G, so we can use direct calls and branches
        flags |= MAP_FIXED;
        start = (void *) 0x60000000UL;
        if (CodeCacheSize > (512 * 1024 * 1024))
            CodeCacheSize = (512 * 1024 * 1024);
#elif defined(__arm__)
        /* Map the buffer below 32M, so we can use direct calls and branches */
        flags |= MAP_FIXED;
        start = (void *) 0x01000000UL;
        if (CodeCacheSize > 16 * 1024 * 1024)
            CodeCacheSize = 16 * 1024 * 1024;
#elif defined(__s390x__)
        /* Map the buffer so that we can use direct calls and branches.  */
        /* We have a +- 4GB range on the branches; leave some slop.  */
        if (CodeCacheSize > (3ul * 1024 * 1024 * 1024))
        {
            CodeCacheSize = 3ul * 1024 * 1024 * 1024;
        }
        start = (void *)0x90000000UL;
#endif
        CodeCache = (uint8_t *)mmap(start, CodeCacheSize,
                PROT_WRITE | PROT_READ | PROT_EXEC,
                flags, -1, 0);
        if (CodeCache == MAP_FAILED)
            DM->Error("%s: Could not allocate dynamic translator buffer\n");
    }
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
    {
        int flags;
        void *addr = NULL;
        flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        /* FreeBSD doesn't have MAP_32BIT, use MAP_FIXED and assume
         * 0x40000000 is free */
        flags |= MAP_FIXED;
        addr = (void *)0x40000000;
        /* Cannot map more than that */
        if (CodeCacheSize > (800 * 1024 * 1024))
            CodeCacheSize = (800 * 1024 * 1024);
#endif
        CodeCache = (uint8_t *)mmap(addr, CodeCacheSize,
                PROT_WRITE | PROT_READ | PROT_EXEC, 
                flags, -1, 0);
        if (CodeCache == MAP_FAILED)
            DM->Error("%s: Could not allocate dynamic translator buffer\n");
    }
#else
    CodeCache = new uint8_t[CodeCacheSize];
    map_exec(CodeCache, CodeCacheSize);
#endif

#endif /* !USE_STATIC_CODE_GEN_BUFFER */

    llvm_lock_init(&CodeLock);
    llvm_lock_init(&GlobalLock);

    GlobalBase = CodeCache;
    GlobalSize = GlobalRemain = DEFAULT_GLOBAL_SIZE;

    Threshold = DEFAULT_THRESHOLD;
    CodeRemain = CodeCacheSize - DEFAULT_GLOBAL_SIZE;
    StubBase = GlobalBase + GlobalSize;
    StubSize = gen_target_stub((void *)StubBase);
    if (StubSize >= CodeRemain)
        DM->Error("Code cache size too small (%d)\n", CodeCacheSize);

    CodeRemain -= StubSize;
    if (CodeRemain <= Threshold)
        DM->Error("Code cache size too small (%d)\n", CodeCacheSize);

    uint8_t *OldCodeBase;
    CodeBase = OldCodeBase = StubBase + StubSize;
    CodeBase = (uint8_t *)(((unsigned long)CodeBase + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));
    CodeRemain -= (CodeBase - OldCodeBase);

    CodeGenPtr = CodeBase;
    CodeSize = CodeRemain;

    TmpCodeSize = MIN_CODE_CACHE_SIZE;
    TmpCodeCache = new uint8_t[TmpCodeSize];
    UseTmpCodeCache = false;
}

void DefaultJITMemoryManager::AllocateGOT() { DM->Error("%s: fixme.\n", __func__); }

DefaultJITMemoryManager::~DefaultJITMemoryManager()
{
    delete [] TmpCodeCache;
}

JITMemoryManager *JITMemoryManager::CreateDefaultMemManager() {
  return new DefaultJITMemoryManager();
}

DefaultJITMemoryManager *createMemoryManager(size_t tb_size=0) {
#ifdef USE_STATIC_CODE_GEN_BUFFER
  size_t size = tb_size;
  size_t max_size = DEFAULT_CODE_CACHE_SIZE * 2 - (StaticCodeCache - static_code_gen_buffer);
  CodeCacheSize = (size >= MIN_CODE_CACHE_SIZE && size < max_size) ?
      size : max_size;
#endif

  return new DefaultJITMemoryManager();
}

#endif

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
