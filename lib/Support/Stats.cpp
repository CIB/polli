#include "polli/Stats.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypeBuilder.h"

#include "format.h"

using namespace llvm;
using namespace fmt;

namespace llvm {
template <bool xcompile> class TypeBuilder<polli::Stats, xcompile> {
public:
  static StructType *get(LLVMContext &Context) {
    return StructType::get(TypeBuilder<types::i<64>, xcompile>::get(Context),
                           TypeBuilder<types::i<64>, xcompile>::get(Context),
                           TypeBuilder<types::i<64>, xcompile>::get(Context),
                           TypeBuilder<types::i<1>, xcompile>::get(Context),
                           nullptr);
  }

  enum Fields {
    NUM_CALLS,
    LOOKUP_TIME,
    LAST_RUNTIME,
    JUMP_INTO_JIT };
  };
} // namespace llvm

namespace polli {
Value *registerStatStruct(Function &F, const Twine &NameSuffix) {
  Type *Ty = TypeBuilder<polli::Stats, true>::get(F.getContext());
  Constant *Init = Constant::getNullValue(Ty);
  GlobalVariable *GV = new GlobalVariable(*(F.getParent()), Ty, false,
                                          GlobalValue::PrivateLinkage, Init,
                                          "polyjit.stats." + NameSuffix);
  F.setPrefixData(GV);
  return GV;
}
} // namespace polli