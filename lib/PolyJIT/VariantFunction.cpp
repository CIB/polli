#include "polli/VariantFunction.h"
#include "polli/Utils.h"

#include <cxxabi.h>
#include <stdlib.h>

#define DEBUG_TYPE "polyjit"
#include "llvm/Support/Debug.h"

#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace polli {
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Param &P) {
  return OS << P.Val->getUniqueInteger();
}

void VariantFunction::printVariants(raw_ostream &OS) {
    for (VariantsT::iterator I = Variants.begin(), IE = Variants.end(); I != IE;
         ++I) {
      const FunctionKey K = I->first;
      Function *VarFun = I->second;
      Module *M = VarFun->getParent();
      OS.indent(4) << K << M->getModuleIdentifier() << "\n";
    }
    OS << "\n";
}

void VariantFunction::printHeader(llvm::raw_ostream &OS) {
  OS << "Source Function::Base Function:: Variants; Calls; MFLOPS [MFLOPs/s]; "
     << "FLOPs [#]; Real Time [s]; Virtual Time [s]\n\n";
}

static std::string demangle(const std::string &Name) {
  char *demangled;
  size_t size = 0;
  int status;

  demangled = abi::__cxa_demangle(Name.c_str(), nullptr, &size, &status);

  if (demangled) {
    log(Info) << " Content: " << demangled;
  }

  if (status != 0) {
    free((void *)demangled);
    return Name;
  }

  return std::string(demangled);
}

void VariantFunction::print(llvm::raw_ostream &OS) {
  std::string Message;

  OS << demangle(SourceF->getName()) << " :: "
     << demangle(BaseF->getName()) << " :: "
     << Variants.size() << "; "
     << S.ExecCount << "; "
     << S.MFLOPS << "; "
     << S.flpops << "; "
     << S.RealTime << "; "
     << S.ProcTime << "\n";

  DEBUG(printVariants(OS));
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                              const ParamVector<Param> &Params) {
  out << "[";
  for (size_t i = 0; i < Params.size(); ++i) {
    out << Params[i];
    if (!(i == Params.size() - 1))
      out << " ";
  }

  out << "]";
  return out;
}
}