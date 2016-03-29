#include "polli/NonAffineSCEVs.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Support/Debug.h"
#include <vector>

using namespace llvm;
using namespace polli;

#define _TYPE "polli-scev-validator"

namespace polli {
namespace SCEVType {
/// @brief The type of a SCEV
///
/// To check for the validity of a SCEV we assign to each SCEV a type. The
/// possible types are INT, PARAM, IV and INVALID. The order of the types is
/// important. The subexpressions of SCEV with a type X can only have a type
/// that is smaller or equal than X.
enum TYPE {
  // An integer value.
  INT,

  // An expression that is constant during the execution of the Scop,
  // but that may depend on parameters unknown at compile time.
  PARAM,

  // An expression that may change during the execution of the SCoP.
  IV,

  // An invalid expression.
  INVALID
};
}

/// @brief The result the validator returns for a SCEV expression.
class ValidatorResult {
  /// @brief The type of the expression
  SCEVType::TYPE Type;

  /// @brief The set of Parameters in the expression.
  std::vector<const SCEV *> Parameters;

public:
  /// @brief The copy constructor
  ValidatorResult(const ValidatorResult &Source) {
    Type = Source.Type;
    Parameters = Source.Parameters;
  }

  /// @brief Construct a result with a certain type and no parameters.
  ValidatorResult(SCEVType::TYPE Type) : Type(Type) {
    assert(Type != SCEVType::PARAM && "Did you forget to pass the parameter");
  }

  /// @brief Construct a result with a certain type and a single parameter.
  ValidatorResult(SCEVType::TYPE Type, const SCEV *Expr) : Type(Type) {
    Parameters.push_back(Expr);
  }

  /// @brief Get the type of the ValidatorResult.
  SCEVType::TYPE getType() { return Type; }

  /// @brief Is the analyzed SCEV constant during the execution of the SCoP.
  bool isConstant() { return Type == SCEVType::INT || Type == SCEVType::PARAM; }

  /// @brief Is the analyzed SCEV valid.
  bool isValid() { return Type != SCEVType::INVALID; }

  /// @brief Is the analyzed SCEV of Type IV.
  bool isIV() { return Type == SCEVType::IV; }

  /// @brief Is the analyzed SCEV of Type INT.
  bool isINT() { return Type == SCEVType::INT; }

  /// @brief Is the analyzed SCEV of Type PARAM.
  bool isPARAM() { return Type == SCEVType::PARAM; }

  /// @brief Get the parameters of this validator result.
  std::vector<const SCEV *> getParameters() { return Parameters; }

  /// @brief Add the parameters of Source to this result.
  void addParamsFrom(const ValidatorResult &Source) {
    Parameters.insert(Parameters.end(), Source.Parameters.begin(),
                      Source.Parameters.end());
  }

  /// @brief Merge a result.
  ///
  /// This means to merge the parameters and to set the Type to the most
  /// specific Type that matches both.
  void merge(const ValidatorResult &ToMerge) {
    Type = std::max(Type, ToMerge.Type);
    addParamsFrom(ToMerge);
  }

  void print(raw_ostream &OS) {
    switch (Type) {
    case SCEVType::INT:
      OS << "SCEVType::INT";
      break;
    case SCEVType::PARAM:
      OS << "SCEVType::PARAM";
      break;
    case SCEVType::IV:
      OS << "SCEVType::IV";
      break;
    case SCEVType::INVALID:
      OS << "SCEVType::INVALID";
      break;
    }
  }
};

raw_ostream &operator<<(raw_ostream &OS, class ValidatorResult &VR) {
  VR.print(OS);
  return OS;
}

class NonAffSCEVValidator
    : public SCEVVisitor<NonAffSCEVValidator, class ValidatorResult> {
private:
  const Region *R;
  Loop *Scope;
  ScalarEvolution &SE;
  const Value *BaseAddress;
  polly::InvariantLoadsSetTy *ILS;

public:
  NonAffSCEVValidator(const Region *R, Loop *Scope, ScalarEvolution &SE,
                      const Value *BaseAddress, polly::InvariantLoadsSetTy *ILS)
      : R(R), Scope(Scope), SE(SE), BaseAddress(BaseAddress), ILS(ILS) {}

  class ValidatorResult visitConstant(const SCEVConstant *Constant) {
    return ValidatorResult(SCEVType::INT);
  }

  class ValidatorResult visitTruncateExpr(const SCEVTruncateExpr *Expr) {
    ValidatorResult Op = visit(Expr->getOperand());

    switch (Op.getType()) {
    case SCEVType::INT:
    case SCEVType::PARAM:
      // We currently do not represent a truncate expression as an affine
      // expression. If it is constant during Scop execution, we treat it as a
      // parameter.
      return ValidatorResult(SCEVType::PARAM, Expr);
    case SCEVType::IV:
      dbgs() << "INVALID: Truncation of SCEVType::IV expression";
      return ValidatorResult(SCEVType::INVALID);
    case SCEVType::INVALID:
      return Op;
    }

    llvm_unreachable("Unknown SCEVType");
  }

  class ValidatorResult visitZeroExtendExpr(const SCEVZeroExtendExpr *Expr) {
    ValidatorResult Op = visit(Expr->getOperand());

    switch (Op.getType()) {
    case SCEVType::INT:
    case SCEVType::PARAM:
      // We currently do not represent a truncate expression as an affine
      // expression. If it is constant during Scop execution, we treat it as a
      // parameter.
      return ValidatorResult(SCEVType::PARAM, Expr);
    case SCEVType::IV:
      dbgs() << "INVALID: ZeroExtend of SCEVType::IV expression";
      return ValidatorResult(SCEVType::INVALID);
    case SCEVType::INVALID:
      return Op;
    }

    llvm_unreachable("Unknown SCEVType");
  }

  class ValidatorResult visitSignExtendExpr(const SCEVSignExtendExpr *Expr) {
    // We currently allow only signed SCEV expressions. In the case of a
    // signed value, a sign extend is a noop.
    //
    // TODO: Reconsider this when we add support for unsigned values.
    return visit(Expr->getOperand());
  }

  class ValidatorResult visitAddExpr(const SCEVAddExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));
      Return.merge(Op);

      // Early exit.
      if (!Return.isValid())
        break;
    }

    // TODO: Check for NSW and NUW.
    return Return;
  }

  class ValidatorResult visitMulExpr(const SCEVMulExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    bool HasMultipleParams = false;

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (Op.isINT())
        continue;

      if (Op.isPARAM() && Return.isPARAM()) {
        HasMultipleParams = true;
        continue;
      }

      if ((Op.isIV() || Op.isPARAM()) && !Return.isINT()) {
        dbgs() << "INVALID: More than one non-int operand in MulExpr\n"
               << "\tExpr: " << *Expr << "\n"
               << "\tPrevious expression type: " << Return << "\n"
               << "\tNext operand (" << Op << "): " << *Expr->getOperand(i)
               << "\n";

        return ValidatorResult(SCEVType::INVALID);
      }

      Return.merge(Op);
    }

    if (HasMultipleParams && Return.isValid())
      return ValidatorResult(SCEVType::PARAM, Expr);

    // TODO: Check for NSW and NUW.
    return Return;
  }

  class ValidatorResult visitUDivExpr(const SCEVUDivExpr *Expr) {
    ValidatorResult LHS = visit(Expr->getLHS());
    ValidatorResult RHS = visit(Expr->getRHS());

    // We currently do not represent an unsigned division as an affine
    // expression. If the division is constant during Scop execution we treat it
    // as a parameter, otherwise we bail out.
    if (LHS.isConstant() && RHS.isConstant())
      return ValidatorResult(SCEVType::PARAM, Expr);

    // As long as unsigned division is not represented as an affine expression,
    // the JIT can compute the result from the params at run-time.
    if (LHS.isPARAM() || RHS.isPARAM())
      return ValidatorResult(SCEVType::PARAM, Expr);

    dbgs() << "INVALID: unsigned division of non-constant expressions";
    return ValidatorResult(SCEVType::INVALID);
  }

  class ValidatorResult visitAddRecExpr(const SCEVAddRecExpr *Expr) {
    if (!Expr->isAffine()) {
      dbgs() << "INVALID: AddRec is not affine";
      return ValidatorResult(SCEVType::INVALID);
    }

    ValidatorResult Start = visit(Expr->getStart());
    ValidatorResult Recurrence = visit(Expr->getStepRecurrence(SE));

    if (!Start.isValid())
      return Start;

    if (!Recurrence.isValid())
      return Recurrence;

    auto *L = Expr->getLoop();
    if (R->contains(L) && (!Scope || !L->contains(Scope))) {
      dbgs() << "INVALID: AddRec out of a loop whose exit value is not "
                "synthesizable";
      return ValidatorResult(SCEVType::INVALID);
    }

    if (R->contains(L)) {
      if (Recurrence.isINT()) {
        ValidatorResult Result(SCEVType::IV);
        Result.addParamsFrom(Start);
        return Result;
      }

      // That will be polynomial, no no
      const SCEV *StepRecurr = Expr->getStepRecurrence(SE);
      if (isa<SCEVAddRecExpr>(StepRecurr))
        return ValidatorResult(SCEVType::INVALID);

      if (Recurrence.isPARAM()) {
        ValidatorResult Result(SCEVType::PARAM, StepRecurr);
        Result.addParamsFrom(Start);
        dbgs() << "VALID: AddRec within scop has parametrized"
               << " recurrence part\n";
        return Result;
      }

      dbgs() << "INVALID: AddRec within scop has non-int"
                "recurrence part";
      return ValidatorResult(SCEVType::INVALID);
    }

    assert(Start.isConstant() && Recurrence.isConstant() &&
           "Expected 'Start' and 'Recurrence' to be constant");

    // Directly generate ValidatorResult for Expr if 'start' is zero.
    if (Expr->getStart()->isZero())
      return ValidatorResult(SCEVType::PARAM, Expr);

    // Translate AddRecExpr from '{start, +, inc}' into 'start + {0, +, inc}'
    // if 'start' is not zero.
    const SCEV *ZeroStartExpr = SE.getAddRecExpr(
        SE.getConstant(Expr->getStart()->getType(), 0),
        Expr->getStepRecurrence(SE), Expr->getLoop(), Expr->getNoWrapFlags());

    ValidatorResult ZeroStartResult =
        ValidatorResult(SCEVType::PARAM, ZeroStartExpr);
    ZeroStartResult.addParamsFrom(Start);

    return ZeroStartResult;
  }

  class ValidatorResult visitSMaxExpr(const SCEVSMaxExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (!Op.isValid())
        return Op;

      Return.merge(Op);
    }

    return Return;
  }

  class ValidatorResult visitUMaxExpr(const SCEVUMaxExpr *Expr) {
    // We do not support unsigned operations. If 'Expr' is constant during Scop
    // execution we treat this as a parameter, otherwise we bail out.
    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (!Op.isConstant()) {
        dbgs() << "INVALID: UMaxExpr has a non-constant operand";
        return ValidatorResult(SCEVType::INVALID);
      }
    }

    return ValidatorResult(SCEVType::PARAM, Expr);
  }

  ValidatorResult visitGenericInst(Instruction *I, const SCEV *S) {
    if (R->contains(I)) {
      dbgs() << "INVALID: UnknownExpr references an instruction "
                "within the region\n";
      dbgs() << *I << "\n";
      //return ValidatorResult(SCEVType::INVALID);
      return ValidatorResult(SCEVType::PARAM, S);
    }

    return ValidatorResult(SCEVType::PARAM, S);
  }

  ValidatorResult visitLoadInstruction(Instruction *I, const SCEV *S) {
    if (R->contains(I) && ILS) {
      ILS->insert(cast<LoadInst>(I));
      return ValidatorResult(SCEVType::PARAM, S);
    }

    return visitGenericInst(I, S);
  }

  ValidatorResult visitSDivInstruction(Instruction *SDiv, const SCEV *S) {
    assert(SDiv->getOpcode() == Instruction::SDiv &&
           "Assumed SDiv instruction!");

    auto *Divisor = SDiv->getOperand(1);
    auto *CI = dyn_cast<ConstantInt>(Divisor);
    if (!CI)
      return visitGenericInst(SDiv, S);

    auto *Dividend = SDiv->getOperand(0);
    auto *DividendSCEV = SE.getSCEV(Dividend);
    return visit(DividendSCEV);
  }

  ValidatorResult visitSRemInstruction(Instruction *SRem, const SCEV *S) {
    assert(SRem->getOpcode() == Instruction::SRem &&
           "Assumed SRem instruction!");

    auto *Divisor = SRem->getOperand(1);
    auto *CI = dyn_cast<ConstantInt>(Divisor);
    if (!CI)
      return visitGenericInst(SRem, S);

    auto *Dividend = SRem->getOperand(0);
    auto *DividendSCEV = SE.getSCEV(Dividend);
    return visit(DividendSCEV);
  }

  ValidatorResult visitUnknown(const SCEVUnknown *Expr) {
    Value *V = Expr->getValue();

    // TODO: FIXME: IslExprBuilder is not capable of producing valid code
    //              for arbitrary pointer expressions at the moment. Until
    //              this is fixed we disallow pointer expressions completely.
    if (Expr->getType()->isPointerTy()) {
      dbgs() << "INVALID: UnknownExpr is a pointer type [FIXME]";
      return ValidatorResult(SCEVType::INVALID);
    }

    if (!Expr->getType()->isIntegerTy()) {
      dbgs() << "INVALID: UnknownExpr is not an integer";
      return ValidatorResult(SCEVType::INVALID);
    }

    if (isa<UndefValue>(V)) {
      dbgs() << "INVALID: UnknownExpr references an undef value";
      return ValidatorResult(SCEVType::INVALID);
    }

    if (BaseAddress == V) {
      dbgs() << "INVALID: UnknownExpr references BaseAddress\n";
      return ValidatorResult(SCEVType::INVALID);
    }

    if (Instruction *I = dyn_cast<Instruction>(Expr->getValue())) {
      switch (I->getOpcode()) {
      case Instruction::Load:
        return visitLoadInstruction(I, Expr);
      case Instruction::SDiv:
        return visitSDivInstruction(I, Expr);
      case Instruction::SRem:
        return visitSRemInstruction(I, Expr);
      default:
        return visitGenericInst(I, Expr);
      }
    }

    return ValidatorResult(SCEVType::PARAM, Expr);
  }
};

bool isNonAffineExpr(const Region *R, llvm::Loop *Scope, const SCEV *Expr,
                     ScalarEvolution &SE, const Value *BaseAddress,
                     polly::InvariantLoadsSetTy *ILS) {
  dbgs() << "\n";
  dbgs() << "Expr: " << *Expr << "\n";
  if (isa<SCEVCouldNotCompute>(Expr))
    return false;

  NonAffSCEVValidator Validator(R, Scope, SE, BaseAddress, ILS);
  {
    dbgs() << "Region: " << R->getNameStr() << "\n";
    dbgs() << " -> ";
  };

  ValidatorResult Result = Validator.visit(Expr);

  {
    if (Result.isValid())
      dbgs() << "VALID\n";
    dbgs() << "\n";
  };

  return Result.isValid();
}

std::vector<const SCEV *> getParamsInNonAffineExpr(const Region *R, Loop *Scope,
                                                   const SCEV *Expr,
                                                   ScalarEvolution &SE,
                                                   const Value *BaseAddress) {
  if (isa<SCEVCouldNotCompute>(Expr))
    return std::vector<const SCEV *>();

  polly::InvariantLoadsSetTy ILS;
  NonAffSCEVValidator Validator(R, Scope, SE, BaseAddress, &ILS);
  ValidatorResult Result = Validator.visit(Expr);
  assert(Result.isValid() && "Requested parameters for an invalid SCEV!");

  return Result.getParameters();
}
}