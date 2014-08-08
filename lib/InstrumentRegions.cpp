//===- InstrumentRegions.cpp - Instrument Regions PAPI ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// 
// This takes a region and instruments the Entry and Exit blocks with calls
// to the PAPI library.
//
// The initial implementation only instruments the region with timing calls
// to measure the runtime that was spent inside the region.
//
// We need to make sure that the instrumentation is placed one a single entry
// edge and a single exit edge. Therefore, it is necessary to transform the
// regions before we perform any instrumentation.
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "polyjit"
#include <stddef.h>                     // for NULL
#include <stdint.h>                     // for uint64_t
#include <set>                          // for _Rb_tree_const_iterator, etc
#include <string>                       // for allocator, string, etc
#include <vector>                       // for vector
#include "llvm/ADT/Statistic.h"         // for Statistic, STATISTIC
#include "llvm/ADT/StringRef.h"         // for StringRef
#include "llvm/ADT/ilist.h"             // for ilist_iterator
#include "llvm/Analysis/RegionInfo.h"   // for Region, RegionInfo
#include "llvm/IR/Argument.h"           // for Argument
#include "llvm/IR/BasicBlock.h"         // for BasicBlock, etc
#include "llvm/IR/CFG.h"                // for pred_iterator, PredIterator, etc
#include "llvm/IR/Constant.h"           // for Constant
#include "llvm/IR/Constants.h"          // for ConstantInt
#include "llvm/IR/DerivedTypes.h"       // for IntegerType, PointerType
#include "llvm/IR/Function.h"           // for Function, etc
#include "llvm/IR/IRBuilder.h"          // for IRBuilder
#include "llvm/IR/InstrTypes.h"         // for CastInst
#include "llvm/IR/Instruction.h"        // for Instruction, etc
#include "llvm/IR/Instructions.h"       // for CallInst, AllocaInst
#include "llvm/IR/Module.h"             // for Module
#include "llvm/IR/Type.h"               // for Type
#include "llvm/PassAnalysisSupport.h"   // for Pass::getAnalysis, etc
#include "llvm/PassSupport.h"           // for INITIALIZE_PASS_BEGIN, etc
#include "llvm/Transforms/Utils/BasicBlockUtils.h" // for SplitEdge
#include "llvm/Support/Casting.h"       // for isa
#include "llvm/Support/Debug.h"         // for dbgs, DEBUG
#include "llvm/Support/raw_ostream.h"   // for raw_ostream, errs
#include "papi.h"                       // for PAPI_VER_CURRENT
#include "polli/InstrumentRegions.h"    // for PapiCScopProfiling, etc
#include "polli/JitScopDetection.h"  // for JitScopDetection, etc
#include "polly/ScopDetection.h"        // for ScopDetection, etc
namespace llvm { class LLVMContext; }  // lines 49-49
namespace llvm { class Value; }  // lines 50-50

using namespace llvm;
using namespace polli;
using namespace polly;

STATISTIC(InstrumentedRegions, "Number of instrumented regions");
STATISTIC(InstrumentedJITScops, "Number of instrumented JIT SCoPs");
STATISTIC(MoreEntries, "Number of regions with more than one entry edge");
STATISTIC(MoreExits, "Number of regions with more than one exit edge");

static void PapiRegionEnterSCoP(Instruction *InsertBefore, Module *M, uint64_t id,
                                std::string dbgStr = "") {
  LLVMContext &Context = M->getContext();
  IRBuilder<> Builder(Context);
  std::vector<Value *> Args(2);
  Constant *PapiScopEnterFn = M->getOrInsertFunction(
      "papi_region_enter_scop", Builder.getVoidTy(), Type::getInt64Ty(Context),
      Builder.getInt8PtrTy(), NULL);

  Builder.SetInsertPoint(InsertBefore);
  Args[0] = ConstantInt::get(Type::getInt64Ty(Context), id, false);
  Args[1] = Builder.CreateGlobalStringPtr(dbgStr);

  Builder.CreateCall(PapiScopEnterFn, Args);
}

static void PapiRegionExitSCoP(Instruction *InsertBefore, Module *M, uint64_t id,
                               std::string dbgStr = "") {
  LLVMContext &Context = M->getContext();
  IRBuilder<> Builder(Context);
  std::vector<Value *> Args(2);
  Constant *PapiScopExitFn = M->getOrInsertFunction(
      "papi_region_exit_scop", Builder.getVoidTy(), Type::getInt64Ty(Context),
      Builder.getInt8PtrTy(), NULL);

  Builder.SetInsertPoint(InsertBefore);
  Args[0] = ConstantInt::get(Type::getInt64Ty(Context), id, false);
  Args[1] = Builder.CreateGlobalStringPtr(dbgStr);

  Builder.CreateCall(PapiScopExitFn, Args);
}

void PapiRegionEnter(Instruction *InsertBefore, Module *M, uint64_t id) {
  LLVMContext &Context = M->getContext();
  IRBuilder<> Builder(Context);

  Constant *PapiScopEnterFn =
      M->getOrInsertFunction("papi_region_enter", Builder.getVoidTy(),
                             Type::getInt64Ty(Context), NULL);
  Builder.SetInsertPoint(InsertBefore);
  Builder.CreateCall(
      PapiScopEnterFn,
      ConstantInt::get(Type::getInt64Ty(Context), id, false));
}

void PapiRegionExit(Instruction *InsertBefore, Module *M, uint64_t id) {
  LLVMContext &Context = M->getContext();
  IRBuilder<> Builder(Context);

  Constant *PapiScopEnterFn = M->getOrInsertFunction(
      "papi_region_exit", Builder.getVoidTy(), Type::getInt64Ty(Context), NULL);
  Builder.SetInsertPoint(InsertBefore);
  Builder.CreateCall(
      PapiScopEnterFn,
      ConstantInt::get(Type::getInt64Ty(Context), id, false));
}

static void PapiCreateInit(Function *F) {
  LLVMContext &Context = F->getContext();
  Module *M = F->getParent();
  IRBuilder<> Builder(Context);
  Constant *PapiLibInitFn = M->getOrInsertFunction(
      "PAPI_library_init", Builder.getInt32Ty(), Builder.getInt32Ty(), NULL);

  Instruction *Insert = F->getEntryBlock().getFirstInsertionPt();
  Builder.SetInsertPoint(Insert);
  Builder.CreateCall(PapiLibInitFn, Builder.getInt32(PAPI_VER_CURRENT),
                     "papi.lib.init");
}

static void InsertProfilingInitCall(Function *MainFn) {
  LLVMContext &Context = MainFn->getContext();
  Module &M = *MainFn->getParent();

  // Skip over any allocas in the entry block.
  BasicBlock *Entry = MainFn->begin();
  BasicBlock::iterator InsertPos = Entry->begin();
  while (isa<AllocaInst>(InsertPos))
    ++InsertPos;

  Type *ArgVTy = PointerType::getUnqual(Type::getInt8PtrTy(Context));
  Constant *PapiSetup =
      M.getOrInsertFunction("papi_region_setup", Type::getVoidTy(Context),
                            IntegerType::getInt32Ty(Context), ArgVTy, (Type *)0);

  std::vector<Value *> Args(2);
  Args[0] = Constant::getNullValue(Type::getInt32Ty(Context));
  Args[1] = Constant::getNullValue(ArgVTy);

  CallInst *InitCall = CallInst::Create(PapiSetup, Args, "", InsertPos);

  // If argc or argv are not available in main, just pass null values in.
  Function::arg_iterator AI;
  switch (MainFn->arg_size()) {
  default:
  case 2:
    AI = MainFn->arg_begin();
    ++AI;
    if (AI->getType() != ArgVTy) {
      Instruction::CastOps opcode =
          CastInst::getCastOpcode(AI, false, ArgVTy, false);
      InitCall->setArgOperand(
          1, CastInst::Create(opcode, AI, ArgVTy, "argv.cast", InitCall));
    } else {
      InitCall->setArgOperand(1, AI);
    }
  /* FALL THROUGH */
  case 1:
    AI = MainFn->arg_begin();
    // If the program looked at argc, have it look at the return value of the
    // init call instead.
    if (!AI->getType()->isIntegerTy(32)) {
      Instruction::CastOps opcode =
          CastInst::getCastOpcode(AI, true, Type::getInt32Ty(Context), true);
      InitCall->setArgOperand(
          0, CastInst::Create(opcode, AI, Type::getInt32Ty(Context),
                              "argc.cast", InitCall));
    } else
      InitCall->setArgOperand(0, AI);
  case 0:
    break;
  }
}

//-----------------------------------------------------------------------------
//
// PapiCScopProfilingInitPasss
//
//-----------------------------------------------------------------------------
bool PapiCScopProfilingInit::runOnModule(Module &M) {
  DEBUG(dbgs() << "PapiCScop $ Initializing module\n");
  Function *Main = M.getFunction("main");
  if (Main == 0) {
    errs() << "WARNING: cannot insert papi profiling into a module"
           << " with no main function!\n";
    return false; // No main, no instrumentation!
  }

  // Just place our atexit call and initialize papi library calls.
  InsertProfilingInitCall(Main);
  PapiCreateInit(Main);

  return true;
}

//-----------------------------------------------------------------------------
//
// PapiCScopProfilingPass
//
//-----------------------------------------------------------------------------
bool PapiCScopProfiling::runOnFunction(Function &F) {
  SD = &getAnalysis<ScopDetection>();
  NSD = getAnalysisIfAvailable<JitScopDetection>();
  RI = &getAnalysis<RegionInfo>();

  for (ScopDetection::iterator It = SD->begin(), SE = SD->end(); It != SE;
       ++It) {
    if (processRegion(*It))
      ++InstrumentedRegions;
  }

  if (NSD) {
    for (ScopSet::iterator It = NSD->jit_begin(),
                                          SE = NSD->jit_end();
         It != SE; ++It) {
      if (processRegion(*It)) {
        ++InstrumentedRegions;
        ++InstrumentedJITScops;
      }
    }
  }

  return true;
}

bool PapiCScopProfiling::processRegion(const Region *R) {
  BasicBlock *Entry, *Exit;
  Function *F = R->getEntry()->getParent();
  std::string baseName = F->getName().str() + "::";

  std::string entryName = baseName + R->getEntry()->getName().str();
  std::string exitName = baseName + R->getExit()->getName().str();

  Entry = R->getEntry();
  Exit = R->getExit();

  std::vector<BasicBlock *> EntrySplits;
  std::vector<BasicBlock *> ExitSplits;
  BasicBlock *SplitBB;
  // Iterate over all predecessors and split the edge, if BB is not
  // contained in the region.
  for (pred_iterator BB = pred_begin(Entry), BE = pred_end(Entry); BB != BE;
       ++BB) {
    BasicBlock *PredBB = *BB;
    if (!R->contains(PredBB))
      if ((SplitBB = SplitEdge(PredBB, Entry, this)))
        EntrySplits.push_back(SplitBB);
  }

  for (pred_iterator BB = pred_begin(Exit), BE = pred_end(Exit); BB != BE;
       ++BB) {
    BasicBlock *PredBB = *BB;
    if (R->contains(PredBB))
      if ((SplitBB = SplitEdge(PredBB, Exit, this)))
        ExitSplits.push_back(SplitBB);
  }

  if (EntrySplits.size() > 1) {
    DEBUG(dbgs() << "Entries: ";
    for (auto &Entry : EntrySplits) {
      dbgs() << Entry->getName().str() << " ; ";
    }
    dbgs() << "\n");
    ++MoreEntries;
  }
  if (ExitSplits.size() > 1) {
    DEBUG(dbgs() << "Exits: ";
    for (auto &Exit : ExitSplits) {
      dbgs() << Exit->getName().str() << " ; ";
    }
    dbgs() << "\n");
    ++MoreExits;
  }
  /* Use the curent Region-Exit, we will chose an appropriate place
   * for a PAPI counter later. */
  Module *M = Entry->getParent()->getParent();
  instrumentRegion(M, EntrySplits, ExitSplits, R, entryName, exitName);
  return true;
}

static uint64_t EvID = 1;
void PapiCScopProfiling::instrumentRegion(Module *M,
                                          std::vector<BasicBlock *> &EntryBBs,
                                          std::vector<BasicBlock *> &ExitBBs,
                                          const Region *R, std::string entryName, std::string exitName) {
  BasicBlock::iterator InsertPos;
  for (auto &BB : EntryBBs) {
    InsertPos = BB->getFirstNonPHIOrDbgOrLifetime();
    // Adjust insertion point for landing pads / allocas
    if (BB->isLandingPad())
      ++InsertPos;
    while (isa<AllocaInst>(InsertPos))
      ++InsertPos;
    /* Preserve the correct order for stack tracing.
     * This will make us "sneak" past a previously entered
     * call to ExitSCoP.*/
    while (isa<CallInst>(InsertPos))
      ++InsertPos;

    PapiRegionEnterSCoP(InsertPos, M, EvID, entryName);
  }

  for (auto &BB : ExitBBs) {
    InsertPos = BB->getFirstNonPHIOrDbgOrLifetime();
    // Adjust insertion point for landing pads / allocas
    if (BB->isLandingPad())
      ++InsertPos;
    while (isa<AllocaInst>(InsertPos))
      ++InsertPos;

    PapiRegionExitSCoP(InsertPos, M, EvID, exitName);
  }

  ++EvID;
}

char PapiCScopProfiling::ID = 0;
char PapiCScopProfilingInit::ID = 0;

INITIALIZE_PASS_BEGIN(PapiCScopProfilingInit, "pprof-init",
                      "PAPI CScop Profiling (Initialization)", false, false);
INITIALIZE_PASS_END(PapiCScopProfilingInit, "pprof-init",
                      "PAPI CScop Profiling (Initialization)", false, false);

INITIALIZE_PASS_BEGIN(PapiCScopProfiling, "pprof-caddy",
                      "PAPI CScop Profiling", false, false);
INITIALIZE_PASS_DEPENDENCY(ScopDetection);
INITIALIZE_PASS_END(PapiCScopProfiling, "pprof-caddy",
                      "PAPI CScop Profiling", false, false);
