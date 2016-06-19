#include "polli/FunctionCloner.h"
#include "polli/ModuleExtractor.h"
#include "polli/Schema.h"
#include "polli/ScopMapper.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Transforms/IPO.h"

using namespace llvm;
#define DEBUG_TYPE "polyjit"

STATISTIC(Instrumented, "Number of instrumented functions");
STATISTIC(MappedGlobals, "Number of global to argument redirections");
STATISTIC(UnmappedGlobals, "Number of argument to global redirections");

namespace polli {
char ModuleExtractor::ID = 0;

using ModulePtrT = std::unique_ptr<Module>;

static ModulePtrT copyModule(Module &M) {
  auto NewM = ModulePtrT(new Module(M.getModuleIdentifier(), M.getContext()));
  NewM->setDataLayout(M.getDataLayout());
  NewM->setTargetTriple(M.getTargetTriple());
  NewM->setMaterializer(M.getMaterializer());
  NewM->setModuleInlineAsm(M.getModuleInlineAsm());

  return NewM;
}

void ModuleExtractor::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ScopMapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
}

void ModuleExtractor::releaseMemory() { InstrumentedFunctions.clear(); }

/**
 * @brief Convert a module to a string.
 *
 * @param M the module to convert
 *
 * @return a string containing the LLVM IR
 */
static std::string moduleToString(Module &M) {
  std::string ModStr;
  llvm::raw_string_ostream os(ModStr);
  ModulePassManager PM;
  PrintModulePass PrintModuleP(os);

  PM.addPass(PrintModuleP);
  PM.run(M);

  os.flush();
  return ModStr;
}

using ExprList = SetVector<Instruction *>;
using GlobalList = SetVector<const GlobalValue *>;

/**
 * @brief Get the pointer operand to this Instruction, if possible
 *
 * @param I the Instruction we fetch the pointer operand from, if it has one.
 *
 * @return the pointer operand, if it exists.
 */

/**
 * @brief Get the pointer operand to this Instruction, if possible.
 *
 * @param I The Instruction we fetch the pointer operand from, if it has one.
 * @return llvm::Value* The pointer operand we found.
 */
static Value *getPointerOperand(Instruction &I) {
  Value *V = nullptr;

  if (LoadInst *L = dyn_cast<LoadInst>(&I))
    V = L->getPointerOperand();

  if (StoreInst *S = dyn_cast<StoreInst>(&I))
    V = S->getPointerOperand();

  if (GetElementPtrInst *G = dyn_cast<GetElementPtrInst>(&I))
    V = G->getPointerOperand();

  return V;
}

/**
 * @brief Set the pointer operand for this instruction to a new value.
 *
 * This is done by creating a new (almost identical) instruction that replaces
 * the new one.
 *
 * @param I The instruction we set a new pointer operand for.
 * @param V The value we set as new pointer operand.
 * @return void
 */
static void setPointerOperand(Instruction &I, Value &V) {
  IRBuilder<> Builder = IRBuilder<>(&I);

  Value *NewV;
  if (isa<LoadInst>(&I)) {
    NewV = Builder.CreateLoad(&V);
  } else if (StoreInst *S = dyn_cast<StoreInst>(&I)) {
    NewV = Builder.CreateStore(S->getValueOperand(), &V);
  } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    SmallVector<Value *, 4> Indices;
    for (GetElementPtrInst::const_op_iterator I = GEP->idx_begin(),
                                              E = GEP->idx_end();
         I != E; ++I) {
      Value *V = (*I).get();
      Indices.push_back(V);
    }
    NewV = Builder.CreateGEP(&V, Indices);
  } else {
    return;
  }

  I.replaceAllUsesWith(NewV);
}

/**
 * @brief Get the number of globals we carry within this function signature.
 *
 * @param F The Function we want to cound the globals on.
 * @return size_t The number of globals we carry with this function signature.
 */
static inline size_t getGlobalCount(Function *F) {
  size_t n = 0;
  if (F->hasFnAttribute("polyjit-global-count"))
    if (!(std::stringstream(
              F->getFnAttribute("polyjit-global-count").getValueAsString()) >>
          n))
      n = 0;
  return n;
}

#ifdef DEBUG
static void dumpUsers(Value &V) {
  for (const auto &U : V.users()) {
    U->print(outs().indent(2));
    outs() << "\n";
  }
  llvm::outs() << "====\n";
}
#endif

using InstrList = SmallVector<Instruction *, 4>;
/**
 * @brief Convert a ConstantExpr pointer operand to an Instruction Value.
 *
 * This is used in conjunction with the apply function.
 *
 * @param I The Instruction we want to convert the operand in.
 * @param Converted A list of Instructions where we keep track of all found
 *                  Instructions so far.
 * @return void
 */
static inline void constantExprToInstruction(Instruction &I,
                                             InstrList &Converted) {
  Value *V = getPointerOperand(I);
  if (V) {
    if (ConstantExpr *C = dyn_cast<ConstantExpr>(V)) {
      Instruction *Inst = C->getAsInstruction();
      Inst->insertBefore(&I);

      DEBUG(llvm::outs() << "I: " << I << "\nInst: " << *Inst << "\n";
            llvm::outs() << "Users:\n";
            dumpUsers(*C));
      setPointerOperand(I, *Inst);
      Converted.push_back(&I);
    }
  }
}

/**
 * @brief Collect all global variables used within this Instruction.
 *
 * We need to keep track of global vars, when extracting prototypes.
 * This is used in conjunction with the apply function.
 *
 * @param I The Instruction we collect globals from.
 * @param Globals A list of globals we collected so far.
 * @return void
 */
static inline void selectGV(Instruction &I, GlobalList &Globals) {
  Value *V = getPointerOperand(I);

  if (V) {
    if (GlobalValue *GV = dyn_cast<GlobalValue>(V))
      Globals.insert(GV);

    if (ConstantExpr *C = dyn_cast<ConstantExpr>(V)) {
      Instruction *Inst = C->getAsInstruction();
      selectGV(*Inst, Globals);
    }
  }
}

/**
 * @brief Apply a selector function on the function body.
 *
 * This is a little helper function that allows us to scan over all instructions
 * within a function, collecting arbitrary stuff on the way.
 *
 * @param T The type we track our state in.
 * @param F The Function we operate on.
 * @param I The Instruction the selector operates on next.
 * @param L The state the SelectorF operates with.
 * @param SelectorF The selector function we apply to all instructions in the
 *                  function.
 * @return T
 */
template <typename T>
static T apply(Function &F,
               std::function<void(Instruction &I, T &L)> SelectorF) {
  T L;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      SelectorF(I, L);

  return L;
}

/**
 * @brief Get all globals variable used in this function.
 *
 * @param SrcF The function we collect globals from.
 * @return polli::GlobalList
 */
static GlobalList getGVsUsedInFunction(Function &SrcF) {
  return apply<GlobalList>(SrcF, selectGV);
}

using ArgListT = SmallVector<Type *, 4>;
/**
 * @brief Collect referenced globals as pointer arguments.
 *
 * This adds global variables referenced in the function body to the
 * function signature via pointer arguments.
 */
struct AddGlobalsPolicy {
  /**
   * @brief Map the arguments from the source function to the target function.
   *
   * In the presence of global variables a correct mapping needs to make sure
   * that we keep track of the mapping between global variables and function
   * arguments.
   *
   * We do this by appending all referenced global variables of the source
   * function to the function signature of the target function.
   *
   * The ValueToValueMap provides the mechanism to actually change the
   * references in the target function during cloning.
   *
   * @param VMap Keeps track of the Argument/GlobalValue mappings.
   * @param From Mapping source Function.
   * @param To Mapping target Function.
   * @return void
   */
  void MapArguments(ValueToValueMapTy &VMap, Function *From,
                           Function *To) {
    Function::arg_iterator NewArg = To->arg_begin();
    for (Argument &Arg : From->args()) {
      NewArg->setName(Arg.getName());
      VMap[&Arg] = &*(NewArg++);
    }

    GlobalList ReqGlobals = getGVsUsedInFunction(*From);
    for (const GlobalValue *GV : ReqGlobals) {
      AttrBuilder Builder;
      // It's actually a global variable, so we guarantee that this pointer
      // is not null.
      Builder.addAttribute(Attribute::NonNull);

      NewArg->addAttr(AttributeSet::get(To->getContext(), 1, Builder));
      /* FIXME: We rely heavily on the name later on.
       * The problem is that we do not keep track of mappings between
       * different invocations of the FunctionCloner.
       */
      NewArg->setName(GV->getName());
      VMap[GV] = &*(NewArg++);
      MappedGlobals++;
    }
  }

  /**
   * @brief Create a new Function that is able to keep track of GlobalValues.
   *
   * The Function remaps all references to GlobalValues in the body to function
   * arguments.
   * The number of GlobalValues we keep track of is annotated as function
   * attribute: "polyjit-global-count".
   *
   * @param From Source function.
   * @param To Target function.
   * @return llvm::Function*
   */
  Function *Create(Function *From, Module *To) {
    GlobalList ReqGlobals = getGVsUsedInFunction(*From);
    ArgListT Args;

    for (auto &Arg : From->args())
      Args.push_back(Arg.getType());

    for (const GlobalValue *GV : ReqGlobals)
      Args.push_back(GV->getType());

    FunctionType *FType = FunctionType::get(From->getReturnType(), Args, false);
    Function *F =
        Function::Create(FType, From->getLinkage(), From->getName(), To);

    // Set the global count attribute in the _source_ function, because
    // the function cloner will copy over all attributes from SrcF to
    // TgtF afterwards.
    From->addFnAttr("polyjit-global-count",
                    fmt::format("{:d}", ReqGlobals.size()));
    return F;
  }
};

/**
 * @brief Remove global variables from argument signature.
 *
 * This is used in conjunction with AddGlobalsPolicy. The number of global
 * variables we need to remove is determined by the 'polyjit-global-count'
 * function attribute.
 */
struct RemoveGlobalsPolicy {
  /**
   * @brief Remove function arguments that are available as globals.
   *
   * This is the inverse policy to the AddGlobalsPolicy. It takes n function
   * arguments from the back of the signature, where n is the number of tracked
   * GlobalValues in the source, and remaps them to GlobalValues of the same
   * name in the target module.
   * The mapping is recorded in a ValueToValueMap which is used during cloning
   * later on.
   *
   * @param VMap Keeps track of the Argument/GlobalValue mappings.
   * @param From Mapping source Function.
   * @param To Mapping target Function.
   * @return void
   */
  void MapArguments(ValueToValueMapTy &VMap, Function *From,
                           Function *To) {
    size_t FromArgCnt = From->arg_size() - getGlobalCount(From);
    Module &ToM = *To->getParent();
    Function::arg_iterator ToArg = From->arg_begin();

    size_t i = 0;
    for (auto &FromArg : From->args())
      if (i++ >= FromArgCnt) {
        if (GlobalValue *GV = ToM.getGlobalVariable(FromArg.getName(), true)) {
          VMap[&FromArg] = GV;
          UnmappedGlobals++;
        }
      } else {
        VMap[&FromArg] = &*(ToArg++);
      }
  }

  /**
   * @brief Create a new Function that does not track GlobalValues anymore.
   *
   * @param From Source function.
   * @param To Target function.
   * @return llvm::Function*
   */
  Function *Create(Function *From, Module *ToM) {
    ArgListT Args;
    size_t ArgCount = From->arg_size() - getGlobalCount(From);

    size_t i = 0;
    for (auto &Arg : From->args())
      if (i++ < ArgCount) {
        Args.push_back(Arg.getType());
      }

    return Function::Create(
        FunctionType::get(From->getReturnType(), Args, false),
        From->getLinkage(), From->getName(), ToM);
  }
};

/**
 * @brief Extract a module containing a single function as a prototype.
 *
 * The function is copied into a new module using the AddGlobalsPolicy.
 * It is important to avoid using the DestroySource policy here as long as
 * the module extraction is done within a FunctionPass.
 *
 * @param VMap ValueToValue mappings collected from previous FunctionCloner runs
 * @param F The Function we extract as prototype.
 * @param M The Module we copy the function to.
 * @return llvm::Function* The prototype Function in the new Module.
 */
static Function *extractPrototypeM(ValueToValueMapTy &VMap, Function &F,
                                   Module &M) {
  using ExtractFunction =
      FunctionCloner<AddGlobalsPolicy, IgnoreSource, IgnoreTarget>;

  DEBUG(dbgs() << fmt::format("Source to Prototype -> {:s}\n",
                              F.getName().str()));
  // Prepare the source function.
  // We need to substitute all instructions that use ConstantExpressions.
  InstrList Converted = apply<InstrList>(F, constantExprToInstruction);

  for (Instruction *I : Converted) {
    I->eraseFromParent();
  }

  // First create a new prototype function.
  ExtractFunction Cloner(VMap, &M);
  return Cloner.setSource(&F).start(true);
}

std::vector<PHINode*> findPhiNodes(LoopInfo* LI, BasicBlock& BB) {
  std::vector<PHINode*> RetVal;
  for(Instruction &I : BB) {
    if(auto Phi = dyn_cast<PHINode>(&I)) {
      RetVal.push_back(Phi);
    }
  }
  return RetVal;
}

BasicBlock* getLoopHeader(LoopInfo *LI, Function &F) {
  // We scan for the header of the outermost loop in the function.
  // There should only be one outermost loop in the SCoP function.
  for(auto& BB : F) {
    if(LI->isLoopHeader(&BB)) {
      auto ContainerLoop = LI->getLoopFor(&BB);
      if(ContainerLoop->getLoopDepth() == 1) {
        return &BB;
      }
    }
  }
  
  // TODO: put an unreachable here
  return NULL;
}
/**
 * @brief Parametrize lower bounds of function.
 */
struct AddLowerBoundsParametersPolicy {
  private:
    LoopInfo *LI;

  public:
  void MapArguments(ValueToValueMapTy &VMap, Function *From,
                           Function *To) {
    Function::arg_iterator NewArg = To->arg_begin();
    for (Argument &Arg : From->args()) {
      NewArg->setName(Arg.getName());
      VMap[&Arg] = &*(NewArg++);
    }

  }

  Function *Create(Function *From, Module *To) {
    BasicBlock *loopHeader = getLoopHeader(LI, *From);
    std::vector<PHINode*> phiNodes = findPhiNodes(LI, *loopHeader);
    ArgListT Args;

    for (auto &Arg : From->args())
      Args.push_back(Arg.getType());

    for (const PHINode *PV : phiNodes)
      Args.push_back(PV->getType());

    FunctionType *FType = FunctionType::get(From->getReturnType(), Args, false);
    Function *F =
        Function::Create(FType, From->getLinkage(), From->getName(), To);

    return F;
  }

  void setLoopInfo(LoopInfo *LI) { this->LI = LI; }
};


/**
 * @brief Replace the lower bounds of the loop with parameters.
 */
struct ParametrizeLowerBounds {
  private:
    LoopInfo *LI1;
  public:
    std::vector<Value*> initialValues;
    void setLoopInfo1(LoopInfo *LI) { LI1 = LI; }
  void Apply(Function *From, Function *To, ValueToValueMapTy &VMap) {
    assert(From && "No source function!");
    assert(To && "No target function!");

    if (To->isDeclaration())
      return;

    llvm::outs() << "To function: " << *To << "\n";

    auto loopHeader = getLoopHeader(LI1, *From);
    auto loop = LI1->getLoopFor(loopHeader);
    auto phiNodes = findPhiNodes(LI1, *loopHeader);

    // First skip the parameters that already exist in the source function.
    Function::arg_iterator NewArg = To->arg_begin();
    for (Argument &Arg : From->args()) {
      NewArg++;
    }

    // Now replace the lower bound of each phi node with the remaining arguments.
    for (auto phiNode : phiNodes) {
      for(unsigned int i=0; i < phiNode->getNumIncomingValues(); i++) {
        if(!loop->contains(phiNode->getIncomingBlock(i))) {
          // Remember the original incoming value in initialValues
          initialValues.push_back(phiNode->getIncomingValue(i));
          auto phiNodeInTarget = dyn_cast<PHINode>(VMap[phiNode]);
          phiNodeInTarget->setIncomingValue(i, &*NewArg);
          break;
        }
      }
      NewArg++;
    }

  }
};
/**
 * @brief Endpoint policy that instruments the target Function for PolyJIT
 *
 * Instrumentation in the sense of PolyJIT means that the function is replaced
 * with an indirection that calls the JIT with a pointer to a prototype Function
 * string and the parameters in the form of an array of pointers.
 *
 */
struct InstrumentEndpoint {

  /**
   * @brief The prototype function we pass into the JIT callback.
   *
   * @param Prototype A prototype value that gets passed to the JIT as string.
   * @return void
   */
  void setPrototype(Function *PrototypeFunction, Value *PrototypeValue) { PrototypeF = PrototypeFunction; PrototypeV = PrototypeValue; }

  /**
   * @brief Setter for a fallback function that will be called.
   *
   * The fallback function is the function that will be called when the JIT
   * reports that it cannot fullfill a request in time.
   *
   * This automatically forces the client to execute the fallback in parallel
   * to the JIT' request.
   *
   * @param F The function we use as fallback when the JIT is not ready.
   */
  void setFallback(Function *F) { FallbackF = F; }

  void setInitialValues(std::vector<Value*> IV) { initialValues = IV; }

  /**
   * @brief Apply the JIT indirection to the target Function.
   *
   * 1. Create a JIT callback function signature, in the form of:
   *    void pjit_main(char *Prototype, int argc, char *argv)
   * 2. Empty the target function.
   * 3. Allocate an array with length equal to the number of arguments in the
   *    source Function.
   * 4. Place pointer to the source Functions arguments in the array.
   * 5. Call pjit_main with the prototype and the source functions arguments.
   *
   * @param From Source Function.
   * @param To Target Function.
   * @param VMap ValueToValueMap that carries all previous mappings.
   * @return void
   */
  void Apply(Function *From, Function *To, ValueToValueMapTy &VMap) {
    assert(From && "No source function!");
    assert(To && "No target function!");
    assert(FallbackF && "No fallback function!");

    if (To->isDeclaration())
      return;

    Module *M = To->getParent();
    assert(M && "TgtF has no parent module!");

    LLVMContext &Ctx = M->getContext();

    Function *PJITCB = cast<Function>(M->getOrInsertFunction(
        "pjit_main", Type::getInt1Ty(Ctx), Type::getInt8PtrTy(Ctx),
        Type::getInt32Ty(Ctx), Type::getInt8PtrTy(Ctx), NULL));
    PJITCB->setLinkage(GlobalValue::ExternalLinkage);

    To->deleteBody();
    To->setLinkage(GlobalValue::WeakAnyLinkage);

    BasicBlock *BB = BasicBlock::Create(Ctx, "polyjit.entry", To);
    IRBuilder<> Builder(BB);
    Builder.SetInsertPoint(BB);

    /* Create a generic IR sequence of this example C-code:
     *
     * void foo(int n, int A[42]) {
     *  void *params[2];
     *  params[0] = &n;
     *  params[1] = A;
     *
     *  pjit_callback("foo", 2, params);
     * }
     */

    /* Store each parameter as pointer in the params array */
    int i = 0;
    Value *Size1 = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
    Value *Idx0 = ConstantInt::get(Type::getInt32Ty(Ctx), 0);

    /* Prepare a stack array for the parameters. We will pass a pointer to
     * this array into our callback function. */
    int argc = To->arg_size() + initialValues.size() + getGlobalCount(PrototypeF);
    Value *ParamC = ConstantInt::get(Type::getInt32Ty(Ctx), argc);
    ArrayType *StackArrayT = ArrayType::get(Type::getInt8PtrTy(Ctx), argc);
    Value *Params = Builder.CreateAlloca(StackArrayT, Size1, "params");

    for (Argument &Arg : To->args()) {
      llvm::outs() << "Argument: " << Arg << "\n";
      /* Get the appropriate slot in the parameters array and store
       * the stack slot in form of a i8*. */
      Value *ArrIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i++);

      Value *Slot;
      if (Arg.getType()->isPointerTy()) {
        Slot = &Arg;
      } else {
        /* Allocate a slot on the stack for the i'th argument and store it */
        Slot = Builder.CreateAlloca(Arg.getType(), Size1);
        Builder.CreateStore(&Arg, Slot, "pjit.stack.param");
      }

      Value *Dest = Builder.CreateGEP(Params, {Idx0, ArrIdx});
      Builder.CreateStore(
          Builder.CreateBitCast(Slot, StackArrayT->getArrayElementType()),
          Dest);
    }

    Function::arg_iterator GlobalArgs = PrototypeF->arg_begin();
    for (int j = 0; j < i; j++)
      GlobalArgs++;
    // Append lower bounds.
    for (auto LowerBound : initialValues) {
      auto& Arg = *GlobalArgs;
      Value *Slot;
      Slot = Builder.CreateAlloca(Arg.getType(), Size1);
      Builder.CreateStore(LowerBound, Slot, "pjit.stack.param_lowerbound");
      Value *ArrIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i++);
      Value *Dest = Builder.CreateGEP(Params, {Idx0, ArrIdx});
      Builder.CreateStore(Builder.CreateBitCast(Slot, StackArrayT->getArrayElementType()), Dest);
      GlobalArgs++;
    }

    // Append required global variables.
    for (; i < argc; i++) {
      StringRef Name = (GlobalArgs++)->getName();
      if (GlobalVariable *GV =
              M->getGlobalVariable(Name, /*AllowInternals*/ true)) {
        /* Get the appropriate slot in the parameters array and store
         * the stack slot in form of a i8*. */
        Value *ArrIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i);
        Value *Dest = Builder.CreateGEP(Params, {Idx0, ArrIdx});

        Builder.CreateStore(
            Builder.CreateBitCast(GV, StackArrayT->getArrayElementType()),
            Dest);
      }
    }

    SmallVector<Value *, 3> Args;
    Args.push_back((PrototypeV) ? PrototypeV
                                : Builder.CreateGlobalStringPtr(To->getName()));
    Args.push_back(ParamC);
    Args.push_back(Builder.CreateBitCast(Params, Type::getInt8PtrTy(Ctx)));

    BasicBlock *JitReady = BasicBlock::Create(Ctx, "polyjit.ready", To);
    BasicBlock *JitNotReady = BasicBlock::Create(Ctx, "polyjit.not.ready", To);
    BasicBlock *Exit = BasicBlock::Create(Ctx, "polyjit.exit", To);
    CallInst *ReadyCheck = Builder.CreateCall(PJITCB, Args);

    Builder.CreateCondBr(ReadyCheck, JitReady, JitNotReady);
    Builder.SetInsertPoint(JitReady);
    Builder.CreateBr(Exit);
    Builder.SetInsertPoint(JitNotReady);

    // Checkpoint the Fallback Function
    
    ValueToValueMapTy VMap2;
    FunctionCloner<CopyCreator, IgnoreSource, IgnoreTarget> PlainFunctionCloner(VMap2, M);
    PlainFunctionCloner.setSource(FallbackF);
    Function *FallbackCopy = PlainFunctionCloner.start();
    
    auto loopHeaderO = getLoopHeader(LI, *FallbackF);
    auto loopHeader = llvm::dyn_cast<BasicBlock>(VMap2[loopHeaderO]);
    auto loop = LI->getLoopFor(loopHeader);
    auto phiNodes = findPhiNodes(LI, *loopHeader);
    
    // We will insert the checkpoint as an alternative to the loop header.
    // Scan for the predecessor of the loop header that's within the loop.
    llvm::BasicBlock *LastBlock;
    // Assumption: The loop header has a predecessor within the block.
    for(auto PI = pred_begin(loopHeaderO), E = pred_end(loopHeaderO); PI != E; PI++) {
      LastBlock = *PI;
      if(loop->contains(LastBlock)) {
        break;
      }
    }

    LastBlock = dyn_cast<BasicBlock>(VMap2[LastBlock]);

    BasicBlock *CheckJitReady = BasicBlock::Create(Ctx, "polyjit.checkready", FallbackCopy);
    BasicBlock *OnJitReady = BasicBlock::Create(Ctx, "polyjit.onready", FallbackCopy);
    IRBuilder<> Builder2(CheckJitReady);
    Builder2.SetInsertPoint(CheckJitReady);
    ReadyCheck = Builder.CreateCall(PJITCB, Args);

    Builder2.CreateCondBr(ReadyCheck, OnJitReady, loopHeader);

    // Replace the branch to the loop header with a branch to CheckJitReady
    auto TerminatorInst = LastBlock->getTerminator();
    for(unsigned i=0; i < TerminatorInst->getNumSuccessors(); i++) {
      auto Successor = TerminatorInst->getSuccessor(i);
      if(Successor == loopHeader) {
        TerminatorInst->setSuccessor(i, CheckJitReady);
      }
    }

    // Just hand the args from the function down to the source function.
    SmallVector<Value *, 3> ToArgs;
    for (auto &Arg: To->args()) {
      ToArgs.push_back(&Arg);
    }
    Builder2.SetInsertPoint(OnJitReady);
    Builder2.CreateCall(FallbackF, ToArgs);
    Builder2.CreateRetVoid();
  }

  void setLoopInfo(LoopInfo *V) { LI = V; }

private:
  std::vector<Value*> initialValues;
  Function *PrototypeF;
  Value *PrototypeV;
  Function *FallbackF;
  LoopInfo *LI;
};

static inline void collectRegressionTest(const std::string Name,
                                         const std::string &ModStr) {
  if (!opt::CollectRegressionTests) {
    return;
  }
  using namespace db;

  auto T = std::shared_ptr<Tuple>(new RegressionTest(Name, ModStr));
  db::Session S;
  S.add(T);
  S.commit();
}

static void clearFunctionLocalMetadata(Function *F) {
  if (!F)
    return;

  SmallVector<Instruction *, 4> DeleteInsts;
  for (auto &I : instructions(F)) {
    if (DbgInfoIntrinsic *DI = dyn_cast_or_null<DbgInfoIntrinsic>(&I)) {
      DeleteInsts.push_back(DI);
    }
  }

  for (auto *I : DeleteInsts) {
      I->removeFromParent();
  }
}

using InstrumentingFunctionCloner =
    FunctionCloner<CopyCreator, IgnoreSource, InstrumentEndpoint>;

/**
 * @brief Extract all SCoP regions in a function into a new Module.
 *
 * This extracts all SCoP regions that are marked for extraction by
 * the ScopMapper pass into a new Module that gets stored as a prototype in
 * the original module. The original function is then replaced with a
 * new version that calls an indirection called 'pjit_main' with the
 * prototype function and original function's arguments as parameters.
 *
 * From there, the PolyJIT can begin working.
 *
 * @param F The Function we extract all SCoPs from.
 * @return bool
 */
bool ModuleExtractor::runOnFunction(Function &F) {
  SetVector<Function *> Functions;
  bool Changed = false;

  if (F.isDeclaration())
    return false;
  if (F.hasFnAttribute("polyjit-jit-candidate"))
    return false;

  DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  ScopMapper &SM = getAnalysis<ScopMapper>();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

  // Extract all regions marked for extraction into an own function and mark it
  // as 'polyjit-jit-candidate'.
  for (const Region *R : SM.regions()) {
    CodeExtractor Extractor(DT, *(R->getNode()), /*AggregateArgs*/ false);
    if (Extractor.isEligible()) {
      if (Function *ExtractedF = Extractor.extractCodeRegion()) {
        ExtractedF->setLinkage(GlobalValue::WeakAnyLinkage);
        ExtractedF->setName(ExtractedF->getName() + ".pjit.scop");
        ExtractedF->addFnAttr("polyjit-jit-candidate");

        Functions.insert(ExtractedF);
        Changed |= true;
      }
    }
  }

  // Instrument all extracted functions.
  for (Function *F : Functions) {
    if (F->isDeclaration())
      continue;

    Module *M = F->getParent();
    ValueToValueMapTy VMap3;
    FunctionCloner<AddLowerBoundsParametersPolicy, IgnoreSource, ParametrizeLowerBounds> ParametrizeCloner(VMap3, M);
    ParametrizeCloner.setSource(F);
    ParametrizeCloner.setLoopInfo(LI);
    ParametrizeCloner.setLoopInfo1(LI);
    auto ParametrizedF = ParametrizeCloner.start();
    llvm::outs() << "Before parametrization: " << *F << "\n";
    llvm::outs() << "After parametrization: " << *ParametrizedF << "\n";

    ValueToValueMapTy VMap;
    StringRef ModuleName = F->getParent()->getModuleIdentifier();
    StringRef FromName = F->getName();
    ModulePtrT PrototypeM = copyModule(*M);

    PrototypeM->setModuleIdentifier((ModuleName + "." + FromName).str() +
                                    ".prototype");
    auto ProtoF = extractPrototypeM(VMap, *ParametrizedF, *PrototypeM);

    llvm::legacy::PassManager MPM;
    MPM.add(llvm::createStripSymbolsPass(true));
    MPM.run(*PrototypeM);

    clearFunctionLocalMetadata(F);

    // Make sure that we do not destroy the function before we're done
    // using the IRBuilder, otherwise this will end poorly.
    IRBuilder<> Builder(&*(F->begin()));
    const std::string ModStr = moduleToString(*PrototypeM);
    Value *Prototype =
        Builder.CreateGlobalStringPtr(ModStr, FromName + ".prototype");

    // Persist the resulting prototype for later reuse.
    // A separate tool should then try to generate a LLVM-lit test that
    // tries to detect that again.
    collectRegressionTest(FromName, ModStr);

    InstrumentingFunctionCloner InstCloner(VMap, M);
    InstCloner.setInitialValues(ParametrizeCloner.initialValues);
    InstCloner.setSource(F);
    InstCloner.setPrototype(ProtoF, Prototype);
    InstCloner.setFallback(F);

    Function *InstF = InstCloner.start(/* RemapCalls */ false);
    llvm::outs() << "Original function: " << *F << "\n";
    llvm::outs() << "Instrumented function: " << *InstF << "\n";
    InstF->addFnAttr(Attribute::OptimizeNone);
    InstF->addFnAttr(Attribute::NoInline);

    InstrumentedFunctions.insert(InstF);
    VMap.clear();
    Instrumented++;

    F->replaceAllUsesWith(InstF);
    llvm::outs() << "Module after: " << *InstF->getParent() << "\n";
    llvm::outs() << "Prototype Module: " << *PrototypeM << "\n";
  }

  return Changed;
}

void ModuleExtractor::print(raw_ostream &os, const Module *M) const {
  int i = 0;
  for (const Function *F : InstrumentedFunctions) {
    os << fmt::format("{:d} {:s} ", i++, F->getName().str());
    F->print(os);
    os << "\n";
  }
}

static RegisterPass<ModuleExtractor>
    X("polli-extract-scops", "PolyJIT - Move extracted SCoPs into new modules");
} // namespace polli
