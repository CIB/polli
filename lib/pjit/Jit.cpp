#include "polli/Jit.h"
#include "pprof/pprof.h"

#include "llvm/IR/Function.h"

using namespace llvm;

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
  papi_region_setup();
}

void PolyJIT::UpdatePrefixMap(uint64_t Prefix, const llvm::Function *F) {
  PrefixToFnMap[Prefix] = F;
}
}