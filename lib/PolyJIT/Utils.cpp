//===-- Utils.cpp -----------------------------------------------*- C++ -*-===//
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
#define DEBUG_TYPE "polyjit"
#include "polli/Utils.h"
#include "polli/Options.h"

#include <string>                      // for string
#include <utility>                     // for pair
#include "llvm/ADT/SmallVector.h"      // for SmallVector
#include "llvm/ADT/Twine.h"            // for Twine
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/LegacyPassManager.h" // for PassManager
#include "llvm/IR/Module.h"            // for Module
#include "llvm/IR/Verifier.h"          // for createVerifierPass
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Pass.h"                   // for FunctionPass
#include "llvm/Support/CommandLine.h"    // for initializer, desc, init, etc
#include "llvm/Support/Debug.h"          // for dbgs, DEBUG
#include "llvm/Support/FileSystem.h"     // for OpenFlags::F_RW
#include "llvm/Support/ToolOutputFile.h" // for tool_output_file
#include "llvm/Support/raw_ostream.h"    // for raw_ostream

#include <cxxabi.h>

using namespace llvm;

SmallVector<char, 255> *DefaultDir;

static bool DirReady = false;

cl::opt<bool>
GenerateOutput("polli-debug-ir",
               cl::desc("Store all IR files inside a unique subdirectory."),
               cl::init(false));

cl::opt<std::string>
ReportFilename("polli-report-file",
               cl::desc("Name of the report file to generate."),
               cl::init("polli.report"));

llvm::raw_ostream &log(const LogType T, const size_t Level) {
  switch (T) {
  case Debug:
    dbgs().changeColor(raw_ostream::Colors::GREEN) << " D ";
    return dbgs().indent(Level).resetColor() << " :: ";
    break;
  case Warning:
    errs().changeColor(raw_ostream::Colors::YELLOW) << " W ";
    return errs().indent(Level).resetColor() << " :: ";
    break;
  case Error:
    errs().changeColor(raw_ostream::Colors::RED) << " E ";
    return errs().indent(Level).resetColor() << " :: ";
    break;
  case Info:
    return errs().indent(Level).resetColor();
    break;
  }
}

void initializeOutputDir() {
  DefaultDir = new SmallVector<char, 255>();
  SmallVector<char, 255> cwd;
  fs::current_path(cwd);

  p::append(cwd, "polli");
  fs::createUniqueDirectory(StringRef(cwd.data(), cwd.size()), *DefaultDir);

  DEBUG(log(Debug) << "Storing results in: "
                   << StringRef(DefaultDir->data(), DefaultDir->size())
                   << "\n");
  DirReady = true;
}

void StoreModule(Module &M, const Twine &Name) {
  if (!GenerateOutput)
    return;

  if (!DirReady)
    initializeOutputDir();

  M.setModuleIdentifier(Name.str());

  SmallVector<char, 255> destPath = *DefaultDir;
  std::error_code ErrorInfo;

  p::append(destPath, Name);

  std::string path = StringRef(destPath.data(), destPath.size()).str();
  DEBUG(log(Debug, 2) << "Storing: " << M.getModuleIdentifier() << "\n");
  std::unique_ptr<tool_output_file> Out(
      new tool_output_file(path.c_str(), ErrorInfo, sys::fs::F_None));

  // Remove all debug info before storing.
  // FIXME: This is just working around bugs.
  // Somewhere we don't fetch all symbols during extraction.
  llvm::StripDebugInfo(M);

  PassManager PM;
  PM.add(new DataLayoutPass());
  PM.add(llvm::createVerifierPass());
  PM.add(createPrintModulePass(Out->os()));
  PM.run(M);

  Out->os().close();
  Out->keep();
}

void StoreModules(ManagedModules &Modules) {
  for (auto &Modules_MI : Modules) {
    ModulePtrT M = (Modules_MI).first;
    StoreModule(*M, M->getModuleIdentifier());
  }
}

std::string demangle(const std::string &Name) {
  char *demangled;
  size_t size = 0;
  int status;

  demangled = abi::__cxa_demangle(Name.c_str(), nullptr, &size, &status);

  if (demangled) {
    log(Info) << " S: " << size;
    log(Info) << "St: " << status << "\n";
    log(Info) << " Content: " << demangled;
  }

  if (status != 0) {
    free((void *)demangled);
    return Name;
  }

  return std::string(demangled);
}