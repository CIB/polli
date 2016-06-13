//===-- RuntimeOptimizer.h - JIT function optimizer -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a small interface to determine the benefits of optimizing
// a given function at run time. If the benefit exceeds a threshold the
// optimization should be executed, e.g. with Polly.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "polyjit"
#include "polli/RuntimeOptimizer.h"
#include "polli/Utils.h"
#include "polli/LikwidMarker.h"
#include "polli/Options.h"
#include "polli/BasePointers.h"

#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#include "llvm/Pass.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/PassRegistry.h"
#include "llvm/PassSupport.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "polly/LinkAllPasses.h"
#include "polly/RegisterPasses.h"
#include "polly/Options.h"

#include <unistd.h>

namespace llvm {
class Function;
} // namespace llvm

using namespace llvm;
using namespace llvm::legacy;
using namespace polly;

namespace polli {
static void registerPolly(const llvm::PassManagerBuilder &Builder,
                          llvm::legacy::PassManagerBase &PM) {
  PM.add(polly::createScopDetectionPass());
  PM.add(polly::createScopInfoRegionPassPass());
  PM.add(polly::createIslScheduleOptimizerPass());
  PM.add(polly::createCodeGenerationPass());
  // FIXME: This dummy ModulePass keeps some programs from miscompiling,
  // probably some not correctly preserved analyses. It acts as a barrier to
  // force all analysis results to be recomputed.
  PM.add(createBarrierNoopPass());
}

static PassManagerBuilder getBuilder() {
  PassManagerBuilder Builder;

  Builder.VerifyInput = false;
  Builder.VerifyOutput = false;
  Builder.OptLevel = 3;
  Builder.addGlobalExtension(PassManagerBuilder::EP_EarlyAsPossible,
                             registerPolly);

  return Builder;
}

Function &OptimizeForRuntime(Function &F) {
  static PassManagerBuilder Builder = getBuilder();
  Module *M = F.getParent();
#ifdef POLLI_STORE_OUTPUT
  opt::GenerateOutput = true;
#endif
  polly::opt::PollyParallel = true;

  legacy::FunctionPassManager PM = legacy::FunctionPassManager(M);

  Builder.populateFunctionPassManager(PM);
#ifdef POLLI_ENABLE_BASE_POINTERS
  PM.add(polli::createBasePointersPass());
#endif
  PM.doInitialization();
  PM.run(F);
  PM.doFinalization();

#ifdef POLLI_ENABLE_PAPI
  if (opt::havePapi()) {
    legacy::PassManager MPM;
    Builder.populateModulePassManager(MPM);
    MPM.add(polli::createTraceMarkerPass());
    MPM.run(*M);
  }
#endif

#ifdef POLLI_ENABLE_LIKWID
  if (opt::haveLikwid()) {
    legacy::PassManager MPM;
    Builder.populateModulePassManager(MPM);
    MPM.add(polli::createLikwidMarkerPass());
    MPM.run(*M);
  }
#endif

#ifdef POLLI_STORE_OUTPUT
  DEBUG(StoreModule(*M, M->getModuleIdentifier() + ".after.polly.ll"));
  opt::GenerateOutput = false;
#endif

  return F;
}
} // namespace polli
