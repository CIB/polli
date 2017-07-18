#include "polli/Jit.h"
#include "polli/Db.h"
#include "polli/Options.h"
#include "polli/RuntimeOptimizer.h"
#include "polli/log.h"
#include "pprof/pprof.h"

#include "llvm/IR/Function.h"
namespace papi {
#include <papi.h>
}

using namespace llvm;

REGISTER_LOG(console, "jit");

namespace polli {

VariantFunctionTy PolyJIT::getOrCreateVariantFunction(Function *F) {
  // We have already specialized this function at least once.
  if (VariantFunctions.count(F))
    return VariantFunctions.at(F);

  // Create a variant function & specialize a new variant, based on key.
  VariantFunctionTy VarFun = std::make_shared<VariantFunction>(*F);

  VariantFunctions.insert(std::make_pair(F, VarFun));
  return VarFun;
}

void PolyJIT::setup() {
  tracing::setup_tracing();
  enter(0, papi::PAPI_get_real_usec());

  /* CACHE_HIT */
  enter(3, 0);

  Regions[JitRegion::START] = "START";
  Regions[JitRegion::CODEGEN] = "CODEGEN";
  Regions[JitRegion::VARIANTS] = "VARIANTS";
  Regions[JitRegion::CACHE_HIT] = "CACHE_HIT";

  SetOptimizationPipeline(opt::runtime::PipelineChoice);
  opt::ValidateOptions();
  db::ValidateOptions();
}

void PolyJIT::tearDown() {
  exit(JitRegion::START, papi::PAPI_get_real_usec());
  db::StoreRun(Events, Entries, Regions);
}

void PolyJIT::UpdatePrefixMap(uint64_t Prefix, const llvm::Function *F) {
  PrefixToFnMap[Prefix] = F;
}
}
