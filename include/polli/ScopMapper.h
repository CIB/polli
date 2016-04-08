//===-- ScopMapper.h - Class definition for the ScopMapper ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//
#ifndef POLLI_SCOP_MAPPER_H
#define POLLI_SCOP_MAPPER_H

#include "llvm/Pass.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/ScalarEvolution.h"

namespace polli {
class JITScopDetection;

/// @brief Extract SCoPs from the host function into a separate function.
///
/// This extracts all SCoPs of a function into separate functions and
/// replaces the SCoP with a call to the extracted function.
class ScopMapper : public llvm::FunctionPass {
public:
  using RegionSet = llvm::SetVector<const llvm::Region *>;
  using ParamList = std::vector<const llvm::SCEV *>;

  llvm::iterator_range<RegionSet::iterator> regions() {
    return llvm::iterator_range<RegionSet::iterator>(MappableRegions.begin(),
                                                     MappableRegions.end());
  }

  static char ID;
  explicit ScopMapper() : FunctionPass(ID) {}

  /// @name FunctionPass interface
  //@{
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  void releaseMemory() override { MappableRegions.clear(); }
  bool runOnFunction(llvm::Function &F) override;

  ParamList getRequiredParams(const Region* R);
  //@}
private:
  //===--------------------------------------------------------------------===//
  // DO NOT IMPLEMENT
  ScopMapper(const ScopMapper &);
  // DO NOT IMPLEMENT
  const ScopMapper &operator=(const ScopMapper &);

  RegionSet MappableRegions;
  polli::JITScopDetection *JSD;
  DominatorTreeWrapperPass *DTP;
};
}
#endif // POLLI_SCOP_MAPPER_H
