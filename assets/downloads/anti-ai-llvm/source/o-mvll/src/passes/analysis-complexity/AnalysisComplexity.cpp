// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "omvll/passes/analysis-complexity/AnalysisComplexity.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"
#include <cstdint>
#include <string>

using namespace llvm;
namespace omvll {
namespace {
constexpr unsigned MaxFunctions = 64, MaxReturns = 16;
constexpr unsigned MaxSites = 16, MaxHelpers = 128, MaxNodes = 24, MaxLeaves = 32;
constexpr StringLiteral Marker = "omvll.analysis-complexity";
SmallVector<Function *, 16> selectedFunctions(Module &M) {
  SmallVector<Function *, 16> Result;
  auto *Annotations = M.getNamedGlobal("llvm.global.annotations");
  if (!Annotations || !Annotations->hasInitializer()) return Result;
  auto *Entries = dyn_cast<ConstantArray>(Annotations->getInitializer());
  if (!Entries) return Result;
  for (const Use &U : Entries->operands()) {
    auto *Entry = dyn_cast<ConstantStruct>(U.get());
    if (!Entry || Entry->getNumOperands() < 2) continue;
    auto *F = dyn_cast<Function>(Entry->getOperand(0)->stripPointerCasts());
    auto *Text = dyn_cast<GlobalVariable>(Entry->getOperand(1)->stripPointerCasts());
    if (!F || !Text || !Text->hasInitializer()) continue;
    auto *Data = dyn_cast<ConstantDataArray>(Text->getInitializer());
    if (Data && Data->isCString() && Data->getAsCString() == Marker &&
        !llvm::is_contained(Result, F)) Result.push_back(F);
  }
  return Result;
}

bool smallInteger(Type *T) {
  return T->isIntegerTy() && T->getIntegerBitWidth() <= 32;
}

// Pure scalar operations only. Never move loads, stores, calls, PHIs or
// operations across basic-block boundaries. Original instruction flags remain.
bool supported(Instruction *I) {
  if (!smallInteger(I->getType())) return false;
  if (!isa<BinaryOperator, ICmpInst, SelectInst, CastInst>(I)) return false;
  for (Value *V : I->operands())
    if (!smallInteger(V->getType())) return false;
  return true;
}

struct Slice {
  BasicBlock *Block;
  SmallPtrSet<Instruction *, 32> Seen;
  SmallVector<Instruction *, 16> Nodes;
  SmallVector<Value *, 32> Leaves;
  void collect(Value *V, unsigned Depth = 0) {
    auto *I = dyn_cast<Instruction>(V);
    if (I && I->getParent() == Block && supported(I) && Depth < 16 &&
        Seen.size() < MaxNodes) {
      if (!Seen.insert(I).second) return;
      for (Value *Op : I->operands()) collect(Op, Depth + 1);
      Nodes.push_back(I);
    } else if (!llvm::is_contained(Leaves, V)) {
      Leaves.push_back(V);
    }
  }
};

uint32_t nextKey(uint32_t &Seed) {
  Seed = Seed * 1664525u + 1013904223u;
  return Seed | 1u;
}

bool outlineSlice(Instruction *Root, Module &M, uint32_t &Seed, unsigned Index) {
  Slice S{Root->getParent(), {}, {}, {}};
  S.collect(Root);
  if (S.Nodes.empty() || S.Leaves.size() > MaxLeaves) return false;
  auto &C = M.getContext();
  auto *I32 = Type::getInt32Ty(C);
  SmallVector<Type *, 32> Types(S.Leaves.size(), I32);
  auto *FT = FunctionType::get(I32, Types, false);
  auto *H = Function::Create(FT, GlobalValue::InternalLinkage,
                             "__omvll_ac_slice_" + std::to_string(Index), M);
  H->addFnAttr(Attribute::NoInline);
  H->addFnAttr(Attribute::OptimizeNone);
  H->addFnAttr(Attribute::NoUnwind);
  H->addFnAttr(Attribute::WillReturn);
  H->setMemoryEffects(MemoryEffects::none());
  IRBuilder<> HB(BasicBlock::Create(C, "entry", H));
  IRBuilder<> CB(Root);
  DenseMap<Value *, Value *> Values;
  SmallVector<Value *, 32> Args;
  for (unsigned I = 0; I < S.Leaves.size(); ++I) {
    Value *Leaf = S.Leaves[I];
    uint32_t Key = nextKey(Seed);
    // Encoding is a bijection on i32. Smaller scalar types round-trip through
    // zero-extension and truncation, preserving their exact bit patterns.
    Args.push_back(CB.CreateXor(CB.CreateZExtOrTrunc(Leaf, I32), CB.getInt32(Key)));
    Value *Decoded = HB.CreateXor(H->getArg(I), HB.getInt32(Key));
    Values[Leaf] = HB.CreateTruncOrBitCast(Decoded, Leaf->getType());
  }
  for (Instruction *I : S.Nodes) {
    Instruction *Clone = I->clone();
    Clone->setDebugLoc(DebugLoc());
    for (unsigned J = 0; J < I->getNumOperands(); ++J)
      Clone->setOperand(J, Values.lookup(I->getOperand(J)));
    HB.Insert(Clone);
    Values[I] = Clone;
  }
  uint32_t OutKey = nextKey(Seed);
  Value *Result = HB.CreateZExtOrTrunc(Values.lookup(Root), I32);
  HB.CreateRet(HB.CreateXor(Result, HB.getInt32(OutKey)));
  Value *Call = CB.CreateCall(H, Args);
  Value *Decoded = CB.CreateXor(Call, CB.getInt32(OutKey));
  Value *Replacement = CB.CreateTruncOrBitCast(Decoded, Root->getType());
  Root->replaceAllUsesWith(Replacement);
  // This removes the original business expression when no other user needs it;
  // shared expressions remain until their other uses are transformed.
  RecursivelyDeleteTriviallyDeadInstructions(Root);
  return true;
}
}

PreservedAnalyses AnalysisComplexity::run(Module &M, ModuleAnalysisManager &) {
  if (M.getNamedMetadata("omvll.analysis-complexity.applied"))
    return PreservedAnalyses::all();
  unsigned Functions = 0, Helpers = 0;
  uint32_t Seed = 2166136261u;
  for (unsigned char Ch : M.getModuleIdentifier()) Seed = (Seed ^ Ch) * 16777619u;
  for (Function *F : selectedFunctions(M)) {
    if (Functions == MaxFunctions || Helpers == MaxHelpers) break;
    if (F->isDeclaration() || !F->getReturnType()->isIntegerTy(32) ||
        F->hasFnAttribute(Attribute::Naked) ||
        F->hasFnAttribute("omvll.analysis-complexity.applied")) continue;
    bool Unsafe = false;
    unsigned Returns = 0;
    for (Instruction &I : instructions(F)) {
      if (auto *Call = dyn_cast<CallInst>(&I)) Unsafe |= Call->isMustTailCall();
      Returns += isa<ReturnInst>(I);
    }
    if (Unsafe || Returns == 0 || Returns > MaxReturns) continue;
    ++Functions;
    SmallVector<WeakTrackingVH, 32> Roots;
    // Comparisons control real business decisions and receive first priority.
    for (Instruction &I : instructions(F))
      if (isa<ICmpInst>(I) && supported(&I)) Roots.emplace_back(&I);
    for (Instruction &I : instructions(F)) {
      if (!supported(&I) || isa<ICmpInst>(I)) continue;
      bool Boundary = llvm::any_of(I.users(), [&](User *U) {
        auto *UI = dyn_cast<Instruction>(U);
        return !UI || !supported(UI) || UI->getParent() != I.getParent();
      });
      if (Boundary) Roots.emplace_back(&I);
    }
    unsigned Sites = 0;
    for (WeakTrackingVH &Handle : Roots) {
      if (Sites == MaxSites || Helpers == MaxHelpers) break;
      auto *I = dyn_cast_or_null<Instruction>(static_cast<Value *>(Handle));
      if (!I || I->getFunction() != F || !supported(I)) continue;
      if (outlineSlice(I, M, Seed, Helpers)) { ++Sites; ++Helpers; }
    }
    if (Sites) F->addFnAttr("omvll.analysis-complexity.applied");
  }
  if (!Helpers) return PreservedAnalyses::all();
  M.getOrInsertNamedMetadata("omvll.analysis-complexity.applied");
  return PreservedAnalyses::none();
}
}
