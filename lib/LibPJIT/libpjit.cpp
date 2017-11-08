//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2014 Andreas Simbürger <simbuerg@fim.uni-passau.de>
//
//===----------------------------------------------------------------------===//
//
// This tool implements a just-in-time compiler for LLVM, allowing direct
// execution of LLVM bitcode in an efficient manner.
//
//===----------------------------------------------------------------------===//
#include <likwid.h>

#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Triple.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include "llvm/IRReader/IRReader.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/DynamicLibrary.h"

#include "polli/Caching.h"
#include "polli/Compiler.h"
#include "polli/Jit.h"
#include "polli/Options.h"
#include "polli/RunValues.h"
#include "polli/RuntimeOptimizer.h"
#include "polli/RuntimeValues.h"
#include "polli/Stats.h"
#include "polli/Tasks.h"
#include "polli/VariantFunction.h"
#include "polli/log.h"
#include "polly/RegisterPasses.h"
#include "pprof/Tracing.h"

#define DEBUG_TYPE "polyjit"

using namespace llvm;
using namespace polli;

REGISTER_LOG(console, DEBUG_TYPE);

static ManagedStatic<PolyJIT> JitContext;
static ManagedStatic<SpecializingCompiler> Compiler;

static void wait_for_threads();
struct ThreadPoolCreator {
  static void *call() {
    std::atexit(&wait_for_threads);
    return new llvm::ThreadPool(1);
  }
};

static ManagedStatic<llvm::ThreadPool, ThreadPoolCreator> Pool;
static void wait_for_threads() {
  Pool->wait();
}

struct PolyjitShutdownObject {
  ~PolyjitShutdownObject() {
    Pool->wait();
    llvm_shutdown();
  }
};
static PolyjitShutdownObject Shutdown;

namespace polli {
using MainFnT = std::function<void(int, char **)>;

static void DoCreateVariant(const SpecializerRequest Request, CacheKey K) {
  if (JitContext->find(K) != JitContext->end()) {
    JitContext->increment(JitRegion::CACHE_HIT);
    return;
  }
  JitContext->increment(JitRegion::VARIANTS);

  const Module &PM = Request.prototypeModule();
  Function &Prototype = Request.prototype();
  RunValueList Values = runValues(Request);
  std::string FnName;

  auto Variant = createVariant(Prototype, Values, FnName);
  assert(Variant && "Failed to get a new variant.");
  auto MaybeModule = Compiler->addModule(std::move(Variant));

  console->error_if(!MaybeModule,
                    "Adding the module failed!");
  assert(MaybeModule && "Adding the module failed!");

  llvm::JITSymbol FPtr = Compiler->findSymbol(FnName, PM.getDataLayout());
  auto Addr = FPtr.getAddress();
  console->error_if(!Addr, "Could not get the address of the JITSymbol.");
  assert((bool)Addr && "Could not get the address of the JITSymbol.");

  if (auto [CacheIt, inserted] =
          JitContext->insert(std::make_pair(K, std::move(FPtr)));
      inserted) {
    llvm_unreachable("Key collision in function cace, abort.");
  }
  if (JitContext->CheckpointPtr.find(K) != JitContext->CheckpointPtr.end()) {
    *(JitContext->CheckpointPtr[K]) = (void*) *Addr;
  }
  DEBUG(printRunValues(Values));
}

static void
GetOrCreateVariantFunction(const SpecializerRequest Request,
                           uint64_t ID, CacheKey K) {
  auto Ctx = Compiler->getContext(ID);
  Ctx->RunInCS(DoCreateVariant, Request, K);
}

extern "C" {
void pjit_trace_fnstats_entry(uint64_t Id) {
  JitContext->enter(Id, papi::PAPI_get_real_usec());
}

void pjit_trace_fnstats_exit(uint64_t Id) {
  JitContext->exit(Id, papi::PAPI_get_real_usec());
}

/**
 * @brief Runtime callback for PolyJIT.
 *
 * All calls to the PolyJIT runtime will land here.
 *
 * @param fName The function name we want to call.
 * @retFunctionPtr The optimized version of the function will be placed here once completed.
 *                 Until then, the value will be initialized to 0.
 * @param paramc number of arguments of the function we want to call
 * @param params arugments of the function we want to call.
 */
void pjit_main(const char *fName, void **retFunctionPtr, uint64_t ID,
                unsigned paramc, char **params) {
  // 1. JitContext.
  pjit_trace_fnstats_entry(JitRegion::CODEGEN);
  std::hash<std::string_view> FnHash;

  // 2. Compiler.
  auto [M, CacheHit] = Compiler->getModule(ID, fName);
  SpecializerRequest Request(FnHash(fName), paramc, params, M);

  if (!CacheHit) {
    llvm::Function &F = Request.prototype();
    JitContext->addRegion(F.getName().str(), ID);
  }

  CacheKey K{ID, runValues(Request).hash()};

  if (retFunctionPtr == nullptr) {
    // This is a special value to signify that the existing stack pointer should be cleared.
    JitContext->CheckpointPtr.erase(K);
    return;
  }

  auto CacheResult = JitContext->CheckpointPtr.find(K);
  if (CacheResult == JitContext->CheckpointPtr.end()) {
    JitContext->CheckpointPtr.insert(std::make_pair(K, retFunctionPtr)).first;
    *retFunctionPtr = 0;
    auto FutureFn =
      Pool->async(GetOrCreateVariantFunction, Request, ID, K);
  } else {
    *retFunctionPtr = CacheResult->second;
  }
  pjit_trace_fnstats_exit(JitRegion::CODEGEN);

  return;
}

/**
 * @brief Runtime callback for PolyJIT.
 *
 * This entry-point will just return false and invoke the non-optimized
 * version of the scop we want to jit.
 *
 * @param fName The function name we want to call.
 * @param paramc number of arguments of the function we want to call
 * @param params arugments of the function we want to call.
 */
void *pjit_main_no_recompile(const char *fName, void *ptr, uint64_t ID,
                             unsigned paramc, char **params) {
  pjit_trace_fnstats_entry(JitRegion::CODEGEN);
  std::hash<std::string_view> FnHash;

  auto [M, CacheHit] = Compiler->getModule(ID, fName);
  SpecializerRequest Request(FnHash(fName), paramc, params, M);

  if (!CacheHit) {
    llvm::Function &F = Request.prototype();
    JitContext->addRegion(F.getName().str(), ID);
  }
  pjit_trace_fnstats_exit(JitRegion::CODEGEN);
  return ptr;
}
} /* extern "C" */
} // namespace polli
