// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "omvll/passes/bla-native-layers/BlaNativeLayers.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
using namespace llvm;
namespace omvll {
PreservedAnalyses BlaNativeLayers::run(Module &M, ModuleAnalysisManager &) {
  auto *A = M.getNamedGlobal("llvm.global.annotations");
  if (!A || !A->hasInitializer()) return PreservedAnalyses::all();
  auto *Entries = dyn_cast<ConstantArray>(A->getInitializer());
  if (!Entries) return PreservedAnalyses::all();
  DenseMap<Function *, unsigned> Selected;
  for (const Use &U : Entries->operands()) {
    auto *E = dyn_cast<ConstantStruct>(U.get());
    if (!E || E->getNumOperands() < 2) continue;
    auto *F = dyn_cast<Function>(E->getOperand(0)->stripPointerCasts());
    auto *S = dyn_cast<GlobalVariable>(E->getOperand(1)->stripPointerCasts());
    if (!F || !S || !S->hasInitializer()) continue;
    auto *D = dyn_cast<ConstantDataArray>(S->getInitializer());
    if (!D || !D->isCString()) continue;
    if (D->getAsCString() == "omvll.bla.shadow") Selected[F] |= 1;
    if (D->getAsCString() == "omvll.bla.runtime") Selected[F] |= 2;
  }
  bool Changed = false;
  unsigned Count = 0;
  for (auto &Item : Selected) {
    Function *F = Item.first;
    if (Count == 64) break;
    if (F->isDeclaration() || F->hasFnAttribute("omvll.bla.applied") ||
        F->hasFnAttribute(Attribute::Naked) || F->isVarArg() ||
        F->arg_size() != 4 || !F->getReturnType()->isIntegerTy(32)) continue;
    bool Unsafe = false;
    for (Argument &Arg : F->args()) Unsafe |= !Arg.getType()->isIntegerTy(32);
    for (Instruction &I : instructions(F))
      if (auto *Call = dyn_cast<CallInst>(&I)) Unsafe |= Call->isMustTailCall();
    if (Unsafe) continue;
    auto &C = M.getContext();
    auto *I32 = Type::getInt32Ty(C);
    BasicBlock *Original = &F->getEntryBlock();
    auto *Gate = BasicBlock::Create(C, "bla.gate", F, Original);
    auto *Synthetic = BasicBlock::Create(C, "bla.synthetic", F);
    IRBuilder<> B(Gate);
    Value *Hit = B.getFalse();
    if (Item.second & 1) {
      // A square modulo 8 is 0, 1 or 4, including wrapping i32 arithmetic.
      // This arm is statically present and never selected in normal execution.
      Value *X = F->getArg(0);
      Value *Square = B.CreateMul(X, X);
      Hit = B.CreateICmpEQ(B.CreateAnd(Square, B.getInt32(7)), B.getInt32(3));
    }
    if (Item.second & 2) {
      FunctionCallee Runtime = M.getOrInsertFunction("__omvll_bla_runtime_probe",
                                    FunctionType::get(I32, {}, false));
      Value *Reason = B.CreateCall(Runtime);
      Hit = B.CreateOr(Hit, B.CreateICmpNE(Reason, B.getInt32(0)));
    }
    B.CreateCondBr(Hit, Synthetic, Original);
    B.SetInsertPoint(Synthetic);
    if (Item.second & 1) {
      FunctionCallee Shadow = M.getOrInsertFunction("__omvll_bla_shadow_eval",
                             FunctionType::get(I32, {I32,I32,I32,I32}, false));
      B.CreateRet(B.CreateCall(Shadow, {F->getArg(0),F->getArg(1),F->getArg(2),F->getArg(3)}));
    } else B.CreateRet(B.getInt32(0x80000010u));
    F->addFnAttr("omvll.bla.applied");
    F->removeFnAttr(Attribute::Memory);
    F->removeFnAttr(Attribute::Speculatable);
    F->removeFnAttr(Attribute::NoSync);
    F->removeFnAttr(Attribute::NoFree);
    F->removeFnAttr(Attribute::WillReturn);
    Changed = true; ++Count;
  }
  // Inferred memory/termination summaries on existing callers and call sites
  // no longer describe calls that now read /proc. Drop them conservatively.
  if (Changed) for (Function &F : M) {
    F.removeRetAttr(Attribute::Range);
    F.removeFnAttr(Attribute::Memory);
    F.removeFnAttr(Attribute::Speculatable);
    F.removeFnAttr(Attribute::NoSync);
    F.removeFnAttr(Attribute::NoFree);
    F.removeFnAttr(Attribute::WillReturn);
    for (Instruction &I : instructions(F)) if (auto *CB = dyn_cast<CallBase>(&I)) {
      CB->removeRetAttr(Attribute::Range);
      CB->removeFnAttr(Attribute::Memory);
      CB->removeFnAttr(Attribute::Speculatable);
      CB->removeFnAttr(Attribute::NoSync);
      CB->removeFnAttr(Attribute::NoFree);
      CB->removeFnAttr(Attribute::WillReturn);
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
}
