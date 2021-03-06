//===a Revectorizer.cpp - A bottom up SLP-style Vectorizer for vector fusion -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-----------------------------------------------------------------------------===//
//
// This pass implements the Bottom Up SLP vectorizer. It detects consecutive
// stores that can be put together into vector-stores. Next, it attempts to
// construct vectorizable tree using the use-def chains. If a profitable tree
// was found, the SLP vectorizer performs vectorization on the tree.
//
// The pass is inspired by the work described in the paper:
//  "Loop-Aware SLP in GCC" by Ira Rosen, Dorit Nuzman, Ayal Zaks.
//
//===-----------------------------------------------------------------------------===//

#include <iostream>

#include "llvm/Transforms/Vectorize/Revectorizer/Revectorizer.h"
#include "llvm/Transforms/Vectorize/Revectorizer/IntrinsicConversion.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Vectorize.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::PatternMatch;
using namespace revectorizer;

#define SV_NAME "revectorizer"
#define DEBUG_TYPE "REVEC"

//#define DEBUG_REDUCTIONS

#ifdef DEBUG_REDUCTIONS
#define REVEC_DEBUG(x) x
#else
#define REVEC_DEBUG(x) LLVM_DEBUG(x)
#endif

STATISTIC(NumVectorInstructions, "Number of vector instructions generated");

static cl::opt<int>
    RevecCostThreshold("revec-threshold", cl::init(0), cl::Hidden,
                     cl::desc("Only vectorize if you gain more than this "
                              "number "));

static cl::opt<bool>
ShouldVectorizeHor("revec-vectorize-hor", cl::init(true), cl::Hidden,
                   cl::desc("Attempt to vectorize horizontal reductions"));

static cl::opt<bool> ShouldStartVectorizeHorAtStore(
    "revec-vectorize-hor-store", cl::init(false), cl::Hidden,
    cl::desc(
        "Attempt to vectorize horizontal reductions feeding into a store"));

static cl::opt<int>
MaxVectorRegSizeOption("revec-max-reg-size", cl::init(128), cl::Hidden,
    cl::desc("Attempt to vectorize for this register size in bits"));

/// Limits the size of scheduling regions in a block.
/// It avoid long compile times for _very_ large blocks where vector
/// instructions are spread over a wide range.
/// This limit is way higher than needed by real-world functions.
static cl::opt<int>
ScheduleRegionSizeBudget("revec-schedule-budget", cl::init(100000), cl::Hidden,
    cl::desc("Limit the size of the SLP scheduling region per block"));

static cl::opt<int> MinVectorRegSizeOption(
    "revec-min-reg-size", cl::init(128), cl::Hidden,
    cl::desc("Attempt to vectorize for this register size in bits"));

static cl::opt<unsigned> RecursionMaxDepth(
    "revec-recursion-max-depth", cl::init(30), cl::Hidden,
    cl::desc("Limit the recursion depth when building a vectorizable tree"));

static cl::opt<unsigned> MinTreeSize(
    "revec-min-tree-size", cl::init(3), cl::Hidden,
    cl::desc("Only vectorize small trees if they are fully vectorizable"));

static cl::opt<unsigned> MaxStoreChunkSize(
    "revec-max-store-chunk-size", cl::init(256), cl::Hidden,
    cl::desc("Limit the number of stores within which Revec conducts a quadratic adjacency search"));

static cl::opt<bool>
    WriteRevecTree("write-revec-tree", cl::Hidden,
                cl::desc("Write out Revectorization SLP trees to files in /tmp"));

static cl::opt<bool>
    ForceRevec("revec-force", cl::Hidden,
            cl::desc("Vectorize the revec tree regardless of the computed cost"));

static cl::opt<bool>
    ExtractInsertGather("revec-ext-ins-gather", cl::Hidden,
            cl::desc("Gather narrow vector values by extracting and inserting each scalar"));

// Limit the number of alias checks. The limit is chosen so that
// it has no negative effect on the llvm benchmarks.
static const unsigned AliasedCheckLimit = 10;

// Another limit for the alias checks: The maximum distance between load/store
// instructions where alias checks are done.
// This limit is useful for very large basic blocks.
static const unsigned MaxMemDepDistance = 160;

/// If the ScheduleRegionSizeBudget is exhausted, we allow small scheduling
/// regions to be handled.
static const int MinScheduleRegionSize = 16;

static const unsigned MaxBundleSize = 32u;

/// \brief Predicate for the element types that the SLP revectorizer supports.
static bool isValidElementType(Type *Ty) {
  // TODO: Should this check that Ty is not a vector of pointers?
  return Ty->isVectorTy();
}


//get the vector type for the fused vector
static VectorType *getVectorType(Type *ElementTy, unsigned numElements) {
  // Get the new vector type, if elements are vectors
  if (VectorType *VecElementTy = dyn_cast<VectorType>(ElementTy)) {
    return VectorType::get(VecElementTy->getElementType(),
						   VecElementTy->getNumElements() * numElements);
  }

  // Else, elements are scalars: get the combined vector type
  return VectorType::get(ElementTy, numElements);
}

// gets the final elements in the fused vector
static unsigned getFusedSize(ArrayRef<Value *> VL) {
  // Get the total number of elements in vectors contained in VL
  VectorType *ty0 = cast<VectorType>(VL[0]->getType());
  unsigned numElements = ty0->getNumElements();

  // Check that all values in the bundle have the same number of elements
  for (Value *value : VL) {
    assert((numElements == value->getType()->getVectorNumElements()) && "Number of elements in bundle values are not the same");
  }

  return numElements * VL.size();
}

/// \return the ID of the intrinsic called by CI, or Intrinsic::not_intrinsic.
static Intrinsic::ID getIntrinsicByCall(CallInst *CI) {
  Function *FI;
  Intrinsic::ID IID = Intrinsic::not_intrinsic;
  if (CI && (FI = CI->getCalledFunction()) && FI->isIntrinsic()) {
    IID = FI->getIntrinsicID();
  }
  return IID;
}

/// \returns the ID of an intrinsic that is the widened equivalent of IID, and the widening factor.
static std::pair<int, Intrinsic::ID> getWidenedIntrinsic(Intrinsic::ID IID, int VF) {
  // TODO: Take into account the available intrinsics on this platform to only return
  //       usable intrinsics if multiple conversions are available.
  unsigned base = static_cast<unsigned>(IID);
  if (intrinsicWideningMap.count(base))
    for (const auto& target : intrinsicWideningMap[base])
      if (target.first == VF)
        return target;

  return std::make_pair(-1, Intrinsic::not_intrinsic);
}

/// \returns an operand index that should remain narrow.
static Optional<unsigned> getPreservedOperand(Intrinsic::ID IID) {
  unsigned base = static_cast<unsigned>(IID);
  if (preservedOperandMap.count(base))
    return Optional<unsigned>(preservedOperandMap[base]);

  return Optional<unsigned>();
}


/// \returns true if all of the instructions in \p VL are in the same block or
/// false otherwise.
static bool allSameBlock(ArrayRef<Value *> VL) {
  Instruction *I0 = dyn_cast<Instruction>(VL[0]);
  if (!I0)
    return false;
  BasicBlock *BB = I0->getParent();
  for (int i = 1, e = VL.size(); i < e; i++) {
    Instruction *I = dyn_cast<Instruction>(VL[i]);
    if (!I)
      return false;

    if (BB != I->getParent())
      return false;
  }
  return true;
}

/// \returns True if all of the values in \p VL are constants.
static bool allConstant(ArrayRef<Value *> VL) {
  for (Value *i : VL)
    if (!isa<Constant>(i))
      return false;
  return true;
}

/// \returns True if all of the values in \p VL are identical.
static bool isSplat(ArrayRef<Value *> VL) {
  for (unsigned i = 1, e = VL.size(); i < e; ++i)
    if (VL[i] != VL[0])
      return false;
  return true;
}

static bool isSplat(ArrayRef<Constant *> VL) {
  for (unsigned i = 1, e = VL.size(); i < e; ++i)
    if (VL[i] != VL[0])
      return false;
  return true;
}

///\returns Opcode that can be clubbed with \p Op to create an alternate
/// sequence which can later be merged as a ShuffleVector instruction.
static unsigned getAltOpcode(unsigned Op) {
  switch (Op) {
  case Instruction::FAdd:
    return Instruction::FSub;
  case Instruction::FSub:
    return Instruction::FAdd;
  case Instruction::Add:
    return Instruction::Sub;
  case Instruction::Sub:
    return Instruction::Add;
  default:
    return 0;
  }
}

static bool isOdd(unsigned Value) {
  return Value & 1;
}

static bool sameOpcodeOrAlt(unsigned Opcode, unsigned AltOpcode,
                            unsigned CheckedOpcode) {
  return Opcode == CheckedOpcode || AltOpcode == CheckedOpcode;
}

/// Chooses the correct key for scheduling data. If \p Op has the same (or
/// alternate) opcode as \p OpValue, the key is \p Op. Otherwise the key is \p
/// OpValue.
static Value *isOneOf(Value *OpValue, Value *Op) {
  auto *I = dyn_cast<Instruction>(Op);
  if (!I)
    return OpValue;
  auto *OpInst = cast<Instruction>(OpValue);
  unsigned OpInstOpcode = OpInst->getOpcode();
  unsigned IOpcode = I->getOpcode();
  if (sameOpcodeOrAlt(OpInstOpcode, getAltOpcode(OpInstOpcode), IOpcode))
    return Op;
  return OpValue;
}

static Constant *getMaskForSwappedOperands(Type *SrcTy, Constant *Mask) {
  SmallVector<Constant *, 32> SwappedMask;

  unsigned NumSrcElts = SrcTy->getVectorNumElements();
  unsigned NumMaskElts = Mask->getType()->getVectorNumElements();
  Type *int32Ty = cast<VectorType>(Mask->getType())->getElementType();
  Constant *Undef = UndefValue::get(int32Ty);

  for (unsigned i = 0; i < NumMaskElts; ++i) {
    Constant *El = Mask->getAggregateElement(i);
    if (El == nullptr || dyn_cast<UndefValue>(El) != nullptr) {
      SwappedMask.push_back(Undef);
    } else {
      int maskVal = El->getUniqueInteger().getLimitedValue();
      if (maskVal < (int) NumSrcElts)
        maskVal += (int) NumSrcElts;
      else
        maskVal -= (int) NumSrcElts;

      SwappedMask.push_back(ConstantInt::get(int32Ty, maskVal));
    }
  }

  return ConstantVector::get(SwappedMask);
}

static void expandLanes(ArrayRef<unsigned> Lanes, unsigned NumEltsVL0, SmallVectorImpl<unsigned> &DstMask) {
  // Expand lane indices into sequential submasks
  for (unsigned Lane : Lanes) {
    for (unsigned i = 0; i < NumEltsVL0; ++i) {
      unsigned maskVal = Lane * NumEltsVL0 + i;
      DstMask.push_back(maskVal);
    }
  }
}

static bool allSameIntrinsic(ArrayRef<Value *> VL) {
  // Check that all calls use the same intrinsic
  CallInst *CI = cast<CallInst>(VL[0]);
  Intrinsic::ID IID = getIntrinsicByCall(CI);

  for (unsigned i = 1; i < VL.size(); ++i) {
    CallInst *CI_i = dyn_cast<CallInst>(VL[i]);
    if (CI_i == nullptr)
      return false;

    if (getIntrinsicByCall(CI_i) != IID)
      return false;
  }

  return true;
}

namespace {

/// Contains data for the instructions going to be vectorized.
struct RawInstructionsData {
  /// Main Opcode of the instructions going to be vectorized.
  unsigned Opcode = 0;

  /// The list of instructions have some instructions with alternate opcodes.
  bool HasAltOpcodes = false;
};

} // end anonymous namespace

/// Checks the list of the vectorized instructions \p VL and returns info about
/// this list. The return value's Opcode property is the opcode of VL[0]
static RawInstructionsData getMainOpcode(ArrayRef<Value *> VL) {
  auto *I0 = dyn_cast<Instruction>(VL[0]);
  if (!I0)
    return {};
  RawInstructionsData Res;
  unsigned Opcode = I0->getOpcode();
  // Walk through the list of the vectorized instructions
  // in order to check its structure described by RawInstructionsData.
  for (unsigned Cnt = 0, E = VL.size(); Cnt != E; ++Cnt) {
    auto *I = dyn_cast<Instruction>(VL[Cnt]);
    if (!I)
      return {};
    if (Opcode != I->getOpcode())
      Res.HasAltOpcodes = true;
  }
  Res.Opcode = Opcode;
  return Res;
}

namespace {

/// Main data required for vectorization of instructions.
struct InstructionsState {
  /// The very first instruction in the list with the main opcode.
  Value *OpValue = nullptr;

  /// The main opcode for the list of instructions.
  unsigned Opcode = 0;

  /// Some of the instructions in the list have alternate opcodes.
  bool IsAltShuffle = false;

  InstructionsState() = default;
  InstructionsState(Value *OpValue, unsigned Opcode, bool IsAltShuffle)
      : OpValue(OpValue), Opcode(Opcode), IsAltShuffle(IsAltShuffle) {}

};

} // end anonymous namespace

#if 1
/// \returns analysis of the Instructions in \p VL described in
/// InstructionsState, the Opcode that we suppose the whole list 
/// could be vectorized even if its structure is diverse.
static InstructionsState getSameOpcode(ArrayRef<Value *> VL) {
  auto Res = getMainOpcode(VL);
  unsigned Opcode = Res.Opcode;
  if (!Res.HasAltOpcodes)
    return InstructionsState(VL[0], Opcode, false);
  auto *OpInst = cast<Instruction>(VL[0]);
  unsigned AltOpcode = getAltOpcode(Opcode);
  // Examine each element in the list instructions VL to determine
  // if some operations there could be considered as an alternative
  // (for example as subtraction relates to addition operation).
  for (int Cnt = 0, E = VL.size(); Cnt < E; Cnt++) {
    auto *I = cast<Instruction>(VL[Cnt]);
    unsigned InstOpcode = I->getOpcode();
    if ((Res.HasAltOpcodes &&
         InstOpcode != (isOdd(Cnt) ? AltOpcode : Opcode)) ||
        (!Res.HasAltOpcodes && InstOpcode != Opcode)) {
      return InstructionsState(OpInst, 0, false);
    }
  }
  return InstructionsState(OpInst, Opcode, Res.HasAltOpcodes);
}
#else
/// \returns analysis of the Instructions in \p VL described in
/// InstructionsState, the Opcode that we suppose the whole list 
/// could be vectorized even if its structure is diverse.
static InstructionsState getSameOpcode(ArrayRef<Value *> VL) {
  auto Res = getMainOpcode(VL);
  unsigned Opcode = Res.Opcode;
  if (Res.HasAltOpcodes) {
    auto *OpInst = cast<Instruction>(VL[0]);
    unsigned AltOpcode = getAltOpcode(Opcode);
    // Examine each element in the list instructions VL to determine
    // if some operations there could be considered as an alternative
    // (for example as subtraction relates to addition operation).
    for (int Cnt = 0, E = VL.size(); Cnt < E; Cnt++) {
      auto *I = cast<Instruction>(VL[Cnt]);
      unsigned InstOpcode = I->getOpcode();
      // if ((Res.HasAltOpcodes &&
      //     InstOpcode != (isOdd(Cnt) ? AltOpcode : Opcode)) ||
      //     (!Res.HasAltOpcodes && InstOpcode != Opcode)) {
      if (InstOpcode != (isOdd(Cnt) ? AltOpcode : Opcode))
        return InstructionsState(OpInst, 0, false);
    }
    return InstructionsState(OpInst, Opcode, Res.HasAltOpcodes);
  }

  // TODO: Check for shufflevector special case:
  //        - merge without widening operands, only the mask
  return InstructionsState(VL[0], Opcode, false);
}
#endif

/// \returns true if all of the values in \p VL have the same type or false
/// otherwise.
static bool allSameType(ArrayRef<Value *> VL) {
  Type *Ty = VL[0]->getType();
  for (int i = 1, e = VL.size(); i < e; i++)
    if (VL[i]->getType() != Ty)
      return false;

  return true;
}

/// \returns True if Extract{Value,Element} instruction extracts element Idx.
static bool matchExtractIndex(Instruction *E, unsigned Idx, unsigned Opcode) {
  assert(Opcode == Instruction::ExtractElement ||
         Opcode == Instruction::ExtractValue);
  if (Opcode == Instruction::ExtractElement) {
    ConstantInt *CI = dyn_cast<ConstantInt>(E->getOperand(1));
    return CI && CI->getZExtValue() == Idx;
  } else {
    ExtractValueInst *EI = cast<ExtractValueInst>(E);
    return EI->getNumIndices() == 1 && *EI->idx_begin() == Idx;
  }
}

/// \returns True if in-tree use also needs extract. This refers to
/// possible scalar operand in vectorized instruction.
static bool InTreeUserNeedToExtract(Value *Scalar, Instruction *UserInst,
                                    TargetLibraryInfo *TLI) {
  unsigned Opcode = UserInst->getOpcode();
  switch (Opcode) {
  case Instruction::Load: {
    LoadInst *LI = cast<LoadInst>(UserInst);
    return (LI->getPointerOperand() == Scalar);
  }
  case Instruction::Store: {
    StoreInst *SI = cast<StoreInst>(UserInst);
    return (SI->getPointerOperand() == Scalar);
  }
  case Instruction::Call: {
    CallInst *CI = cast<CallInst>(UserInst);
    Intrinsic::ID ID = getVectorIntrinsicIDForCall(CI, TLI);
    if (hasVectorInstrinsicScalarOpd(ID, 1)) {
      return (CI->getArgOperand(1) == Scalar);
    }
    LLVM_FALLTHROUGH;
  }
  default:
    return false;
  }
}

/// \returns the AA location that is being access by the instruction.
static MemoryLocation getLocation(Instruction *I, AliasAnalysis *AA) {
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return MemoryLocation::get(SI);
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return MemoryLocation::get(LI);
  return MemoryLocation();
}

/// \returns True if the instruction is not a volatile or atomic load/store.
static bool isSimple(Instruction *I) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->isSimple();
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->isSimple();
  if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(I))
    return !MI->isVolatile();
  return true;
}

/// Adapted from llvm::createUnpackShuffleMask (X86ISelLowering.h)
static bool isUnpackShuffle(VectorType *OpTy, Constant *Mask, bool Lo,
                              bool Unary) {
  Type *Ty = Mask->getType();
  int NumElts = Ty->getVectorNumElements();
  int NumEltsInLane = 128 / OpTy->getScalarSizeInBits();

  for (int i = 0; i < NumElts; ++i) {
    unsigned LaneStart = (i / NumEltsInLane) * NumEltsInLane;
    int Pos = (i % NumEltsInLane) / 2 + LaneStart;
    Pos += (Unary ? 0 : NumElts * (i % 2));
    Pos += (Lo ? 0 : NumEltsInLane / 2);
    Constant *El = Mask->getAggregateElement(i);
    if (Mask->containsUndefElement() || El == nullptr || (int) El->getUniqueInteger().getLimitedValue() != Pos)
      return false;
  }

  return true;
}

static int getSignedSpacing(Constant *Mask, unsigned i, unsigned j) {
  unsigned NumElts = Mask->getType()->getVectorNumElements();
  assert(i < NumElts && j < NumElts && "Indexes out of bounds for Constant");
  Constant *L = Mask->getAggregateElement(i);
  Constant *R = Mask->getAggregateElement(j);
  if (L == nullptr || R == nullptr)
    // FIXME: Inelegant, large poison value
    return NumElts*100;

  int LVal = L->getUniqueInteger().getLimitedValue();
  int RVal = R->getUniqueInteger().getLimitedValue();
  return RVal - LVal;
}

static bool isAlternatingShuffleMask(Constant *Mask) {
  unsigned NumElts = Mask->getType()->getVectorNumElements();
  if (NumElts < 2)
    return false;

  int spacing = getSignedSpacing(Mask, 0, 1);
  for (unsigned i = 2; i < NumElts - 1; i += 2)
    if (getSignedSpacing(Mask, i, i+1) != spacing)
      return false;

  return true;
}

#if 0
static bool canMergeConstants(ArrayRef<Value *> VL) {
  Type *Ty0 = VL[0]->getType();
  VectorType *VecTy0 = dyn_cast<VectorType>(Ty0);

  // Check that all constants have the same vector type
  if (!VecTy0)
    return false;

  for (Value *val : VL) {
    Constant *C = cast<Constant>(val);
    if (C->getType() != Ty0)
      return false;
  }

  // Check that there is only one non-undef value per index


  return true;
}
#endif

static Constant *concatenateTwoConstantVectors(Value *Left, Value *Right) {
  assert(Left->getType() == Right->getType() && "Concatenating values of differing types");

  Constant *C1 = dyn_cast<Constant>(Left);
  Constant *C2 = dyn_cast<Constant>(Right);

  const VectorType *VecTy = dyn_cast<VectorType>(Left->getType());
  if (!VecTy || !C1 || !C2)
    return nullptr;

  const unsigned NumElt = VecTy->getNumElements();
  SmallVector<Constant *, 32> concatenated;
  for (unsigned i = 0; i < NumElt; ++i)
    concatenated.push_back(C1->getAggregateElement(i));
  for (unsigned i = 0; i < NumElt; ++i)
    concatenated.push_back(C2->getAggregateElement(i));

  return ConstantVector::get(concatenated);
}

static Constant *mergeIfConstantVectors(ArrayRef<Value *> Operands) {
  LLVM_DEBUG(
    dbgs() << "Revec: Testing operands for mergeability:\n";
    for (Value *val : Operands)
      dbgs() << "Revec:   " << *val << "\n";
  );

  // Operands must be constants of the same type to be merged
  if (!allConstant(Operands) || !allSameType(Operands)) {
    LLVM_DEBUG(dbgs() << "Revec: Operands not mergable as not all constant or all same type\n");
    return nullptr;
  }

  // All operands must be vectors
  const VectorType *VecTy = dyn_cast<VectorType>(Operands[0]->getType());
  if (!VecTy) {
    LLVM_DEBUG(dbgs() << "Revec: Operands not mergable as not vector type\n");
    return nullptr;
  }

  Type *ElTy = VecTy->getElementType();
  const unsigned NumElt = VecTy->getVectorNumElements();
  LLVM_DEBUG(dbgs() << "Revec:   Merging len " << NumElt << " constant vectors\n");
  assert(NumElt > 0 && "Empty constant to merge");

  // Initialize a vector of UndefValue
  SmallVector<Constant *, 32> merged;
  for (unsigned i = 0; i < NumElt; ++i)
    merged.push_back(UndefValue::get(ElTy));

  for (Value *V : Operands) {
    const Constant *CVec = cast<Constant>(V);

    for (unsigned i = 0; i < NumElt; ++i) {
      Constant *nextScalar = CVec->getAggregateElement(i);
      LLVM_DEBUG(dbgs() << "Revec:   Merging at lane " << i << " with scalar " << *nextScalar << "\n");

      if (dyn_cast<UndefValue>(nextScalar) == nullptr) {
        if (dyn_cast<UndefValue>(merged[i]) == nullptr && merged[i] != nextScalar) {
          // Lane i has multiple, differing non-undef values
          return nullptr;
        }

        merged[i] = nextScalar;
      }
    }
  }

  Constant *Merged = ConstantVector::get(merged);
  LLVM_DEBUG(dbgs() << "Revec:   (in merge method) Merged Op1: " << *Merged << "\n");
  //LLVM_DEBUG(
  //  bool opsContainUndef = false;
  //  for (Value *Op : Operands)
  //    opsContainUndef |= cast<Constant>(Op)->containsUndefElement();
  //  if (opsContainUndef) {
  //    assert(
  //  }
  //);
  return Merged;
}

namespace llvm {

namespace revectorizer {

/// Bottom Up SLP Vectorizer.
class BoUpSLP {
public:
  using ValueList = SmallVector<Value *, 8>;
  using InstrList = SmallVector<Instruction *, 16>;
  using ValueSet = SmallPtrSet<Value *, 16>;
  using StoreList = SmallVector<StoreInst *, 8>;
  using ExtraValueToDebugLocsMap =
      MapVector<Value *, SmallVector<Instruction *, 2>>;

  bool EnableDebug = false;

  BoUpSLP(Function *Func, ScalarEvolution *Se, TargetTransformInfo *Tti,
          TargetLibraryInfo *TLi, AliasAnalysis *Aa, LoopInfo *Li,
          DominatorTree *Dt, AssumptionCache *AC, DemandedBits *DB,
          const DataLayout *DL, OptimizationRemarkEmitter *ORE)
      : F(Func), SE(Se), TTI(Tti), TLI(TLi), AA(Aa), LI(Li), DT(Dt), AC(AC),
        DB(DB), DL(DL), ORE(ORE), Builder(Se->getContext()) {
    CodeMetrics::collectEphemeralValues(F, AC, EphValues);
    // Use the vector register size specified by the target unless overridden
    // by a command-line option.
    // TODO: It would be better to limit the vectorization factor based on
    //       data type rather than just register size. For example, x86 AVX has
    //       256-bit registers, but it does not support integer operations
    //       at that width (that requires AVX2).
    if (MaxVectorRegSizeOption.getNumOccurrences())
      MaxVecRegSize = MaxVectorRegSizeOption;
    else
      MaxVecRegSize = TTI->getRegisterBitWidth(true);

    if (MinVectorRegSizeOption.getNumOccurrences())
      MinVecRegSize = MinVectorRegSizeOption;
    else
      MinVecRegSize = TTI->getMinVectorRegisterBitWidth();
  }

  /// \brief Vectorize the tree that starts with the elements in \p VL.
  /// Returns the vectorized root.
  Value *vectorizeTree();

  /// Vectorize the tree but with the list of externally used values \p
  /// ExternallyUsedValues. Values in this MapVector can be replaced but the
  /// generated extractvalue instructions.
  Value *vectorizeTree(ExtraValueToDebugLocsMap &ExternallyUsedValues);

  bool isNarrowVectorType(const Type *T) const {
    return T->getPrimitiveSizeInBits() < MaxVecRegSize;
  }

  //get the maximum packing factor
  unsigned getMaxPackingFactor(Value * V){
  
    unsigned vectorSize = 0;


    //get the size in bits
    if (auto *Store = dyn_cast<StoreInst>(V))
      vectorSize = DL->getTypeSizeInBits(Store->getValueOperand()->getType());
    else
      vectorSize = DL->getTypeSizeInBits(V->getType());

    return MaxVecRegSize / vectorSize;


  }



  /// \returns the cost incurred by unwanted spills and fills, caused by
  /// holding live values over call sites.
  int getSpillCost();

  /// \returns the vectorization cost of the subtree that starts at \p VL.
  /// A negative number means that this is profitable.
  int getTreeCost();

  /// Construct a vectorizable tree that starts at \p Roots, ignoring users for
  /// the purpose of scheduling and extraction in the \p UserIgnoreLst.
  void buildTree(ArrayRef<Value *> Roots,
                 ArrayRef<Value *> UserIgnoreLst = None);

  /// Construct a vectorizable tree that starts at \p Roots, ignoring users for
  /// the purpose of scheduling and extraction in the \p UserIgnoreLst taking
  /// into account (and updating it, if required) list of externally used
  /// values stored in \p ExternallyUsedValues.
  void buildTree(ArrayRef<Value *> Roots,
                 ExtraValueToDebugLocsMap &ExternallyUsedValues,
                 ArrayRef<Value *> UserIgnoreLst = None);

  /// Clear the internal data structures that are created by 'buildTree'.
  void deleteTree() {
    VectorizableTree.clear();
    ScalarToTreeEntry.clear();
    MustGather.clear();
    ExternalUses.clear();
    NumOpsWantToKeepOrder.clear();

    //ADDED
    NumOpsWantToKeepOriginalOrder = 0;

    for (auto &Iter : BlocksSchedules) {
      BlockScheduling *BS = Iter.second.get();
      BS->clear();
    }
    ShuffleCache.clear();
  }

  unsigned getTreeSize() const { return VectorizableTree.size(); }

  /// \brief Perform LICM and CSE on the newly generated gather sequences.
  void optimizeGatherSequence();
  
  //ADDED
  /// \returns The best order of instructions for vectorization.
  Optional<ArrayRef<unsigned>> bestOrder() const {
    auto I = std::max_element(
        NumOpsWantToKeepOrder.begin(), NumOpsWantToKeepOrder.end(),
        [](const decltype(NumOpsWantToKeepOrder)::value_type &D1,
           const decltype(NumOpsWantToKeepOrder)::value_type &D2) {
          return D1.second < D2.second;
        });
    if (I == NumOpsWantToKeepOrder.end() ||
        I->getSecond() <= NumOpsWantToKeepOriginalOrder)
      return None;

    return makeArrayRef(I->getFirst());
  }



  /// \returns true if it is beneficial to reverse the vector order.
  bool shouldReorder() const {
    return std::accumulate(
               NumOpsWantToKeepOrder.begin(), NumOpsWantToKeepOrder.end(), 0,
               [](int Val1,
                  const decltype(NumOpsWantToKeepOrder)::value_type &Val2) {
                 return Val1 + (Val2.second < 0 ? 1 : -1);
               }) > 0;
  }

  /// \return The vector element size in bits to use when vectorizing the
  /// expression tree ending at \p V. If V is a store, the size is the width of
  /// the stored value. Otherwise, the size is the width of the largest loaded
  /// value reaching V. This method is used by the vectorizer to calculate
  /// vectorization factors.
  unsigned getVectorElementSize(Value *V);

  // \return The total width in bits of elements in a vector type
  unsigned getVectorSize(Value *V);

  // \returns maximum vector register size as set by TTI or overridden by cl::opt.
  unsigned getMaxVecRegSize() const {
    return MaxVecRegSize;
  }

  // \returns minimum vector register size as set by cl::opt.
  unsigned getMinVecRegSize() const {
    return MinVecRegSize;
  }

  /// \brief Check if ArrayType or StructType is isomorphic to some VectorType.
  ///
  /// \returns number of elements in vector if isomorphism exists, 0 otherwise.
  unsigned canMapToVector(Type *T, const DataLayout &DL) const;

  /// \returns True if the VectorizableTree is both tiny and not fully
  /// vectorizable. We do not vectorize such trees.
  bool isTreeTinyAndNotFullyVectorizable();

  OptimizationRemarkEmitter *getORE() { return ORE; }

private:
  struct TreeEntry;

  /// Checks if all users of \p I are the part of the vectorization tree.
  bool areAllUsersVectorized(Instruction *I) const;

  /// \returns the cost of a particular shufflevector instruction based on mask special cases.
  int getShuffleCost(VectorType *Op0, VectorType *Op1, Constant *Mask);

  /// \returns the cost of the vectorizable entry.
  int getEntryCost(TreeEntry *E);

  /// This is the recursive part of buildTree.
  void buildTree_rec(ArrayRef<Value *> Roots, unsigned Depth, int);

  /// \returns True if the ExtractElement/ExtractValue instructions in VL can
  /// be vectorized to use the original vector (or aggregate "bitcast" to a vector).
  bool canReuseExtract(ArrayRef<Value *> VL, Value *OpValue) const;

  /// Vectorize a single entry in the tree.
  Value *vectorizeTree(TreeEntry *E);

  /// Vectorize a single entry in the tree, starting in \p VL.
  Value *vectorizeTree(ArrayRef<Value *> VL);

  /// \returns the scalarization cost for this list of values. Assuming that
  /// this subtree gets vectorized, we may need to extract the values from the
  /// roots. This method calculates the cost of extracting the values.
  int getGatherCost(ArrayRef<Value *> VL);

  /// \brief Set the Builder insert point to one after the last instruction in
  /// the bundle
  void setInsertPointAfterBundle(ArrayRef<Value *> VL, Value *OpValue);

  /// \returns a concatenated vector from a collection of vectors in \p VL.
  Value *Gather(ArrayRef<Value *> VL, VectorType *Ty);

  /// \returns a concatenated vector from a collection of vectors in \p VL.
  /// Emits an ExtractElement and InsertElement IR instruction for each scalar
  /// in each value in VL. Enabled by -revec-extract-insert-gather LLVM flag.
  Value *Gather_extract_insert(ArrayRef<Value *> VL, VectorType *Ty);

  /// \returns a concatenated vector from a collection of vectors in \p VL, ranging
  /// from VL[start] to VL[end - 1].
  Value *Gather_rec(ArrayRef<Value *> VL, VectorType *Ty, int start, int end);

  Value *Gather_two(Value *L, Value *R);

  // void RecordExternalUse(Value *ElementVector, llvm::User *User);

  /// \returns whether the VectorizableTree is fully vectorizable and will
  /// be beneficial even the tree height is tiny.
  bool isFullyVectorizableTinyTree();

  /// \reorder commutative operands in alt shuffle if they result in
  ///  vectorized code.
  void reorderAltShuffleOperands(unsigned Opcode, ArrayRef<Value *> VL,
                                 SmallVectorImpl<Value *> &Left,
                                 SmallVectorImpl<Value *> &Right);

  /// \reorder commutative operands to get better probability of
  /// generating vectorized code.
  void reorderInputsAccordingToOpcode(unsigned Opcode, ArrayRef<Value *> VL,
                                      SmallVectorImpl<Value *> &Left,
                                      SmallVectorImpl<Value *> &Right);
  struct TreeEntry {
    TreeEntry(std::vector<TreeEntry> &Container) : Container(Container) {}

    /// \returns true if the scalars in VL are equal to this entry.
    bool isSame(ArrayRef<Value *> VL) const {
      if (VL.size() == Scalars.size())
        return std::equal(VL.begin(), VL.end(), Scalars.begin());
      return VL.size() == ReuseShuffleIndices.size() &&
             std::equal(
                 VL.begin(), VL.end(), ReuseShuffleIndices.begin(),
                 [this](Value *V, unsigned Idx) { return V == Scalars[Idx]; });
    }

    /// A vector of scalars.
    ValueList Scalars;

    /// The Scalars are vectorized into this value. It is initialized to Null.
    Value *VectorizedValue = nullptr;

    /// Do we need to gather this sequence ?
    bool NeedToGather = false;

    /// Does this sequence require some shuffling?
    SmallVector<unsigned, 4> ReuseShuffleIndices;
    SmallVector<unsigned, 32> ReuseShuffleMask;

    /// Points back to the VectorizableTree.
    ///
    /// Only used for Graphviz right now.  Unfortunately GraphTrait::NodeRef has
    /// to be a pointer and needs to be able to initialize the child iterator.
    /// Thus we need a reference back to the container to translate the indices
    /// to entries.
    std::vector<TreeEntry> &Container;

    /// The TreeEntry index containing the user of this entry.  We can actually
    /// have multiple users so the data structure is not truly a tree.
    SmallVector<int, 1> UserTreeIndices;

    /// A cost used when rendering the revectorization graph
    int CostCached = 99999;
  };

  using OperandIndices = std::bitset<MaxBundleSize>;

  struct ShuffleBundleDecision {
    enum {
      // Gather shufflevectors
      Gather = 0,

      // Collect operands by a bitmask specifying the left bundle
      IndexOp0_IndexOp1_WidenMask = 1,

      // Take Op0 and Op1 from the first shufflevector, and concatenate masks
      // Example:
      //   %a = shufflevector <n x ty> %op0, <n x ty> %op1, <m x ty> <MASK_A>
      //   %b = shufflevector <n x ty> %op0, <n x ty> %op1, <m x ty> <MASK_B>
      //     =>
      //   %ab = shufflevector <n x ty> %op0, <n x ty> %op1, <2m x ty> <concat(MASK_A, MASK_B)>
      FirstOp0_FirstOp1_ConcatenateMask = 2,

      // Take Op0 from the first shufflevector, merge the first operand vertically, and concatenate masks.
      // Example:
      //   %a = shufflevector <8 x i8> %33, <8 x i8> <i8 1, i8 1, i8 undef, i8 undef, i8 1, i8 1, i8 undef, i8 undef>, <8 x i32> <i32 0, i32 8, i32 1, i32 9, i32 2, i32 10, i32 3, i32 11>
      //   %b = shufflevector <8 x i8> %33, <8 x i8> <i8 undef, i8 undef, i8 1, i8 1, i8 undef, i8 undef, i8 1, i8 1>, <8 x i32> <i32 4, i32 12, i32 5, i32 13, i32 6, i32 14, i32 7, i32 15>
      //     =>
      //   %ab = shufflevector <8 x i8> %33, <8 x i8> <i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1>, <16 x i32> <i32 0, i32 8, i32 1, i32 9, i32 2, i32 10, i32 3, i32 11, i32 4, i32 12, i32 5, i32 13, i32 6, i32 14, i32 7, i32 15>
      FirstOp0_MergeOp1_ConcatenateMask = 3,

      // Example:
      //   sv A, C1, M1
      //   sv A, C2, M2
      //   sv B, C3, M3
      //   sv B, C4, M4
      //     =>
      //   sv cat{A, B}, cat{merge<C1, C2>, merge<C3, C4>}, cat{M1, M2, M3 offset by |A|, M4 offset by |A|}
      FirstThirdOp0_DoubleMergeOp1 = 4,

      // If we have a bundle of shuffles that extract consecutive subvectors from the same source vector,
      // delete the shufflevector bundle and use the source operand as the vectorized value.
      FirstOp0 = 5,

      Diagonal_VF2 = 6
    };

    /// Encodes a merger strategy for the bundle of ShuffleVectors
    int MergeMode;
    /// Vector values in this bundle
    SmallVector<Value *, 32> VL;
    unsigned BundleSize = 0;
    /// A mask that specifies the operand of each element of the bundle to merge
    /// into the first operand of the widened shufflevector.
    OperandIndices Op0Indices = 0ULL;
    /// If operand 1 is precomputed in buildTree when making a vectorization decision,
    /// it may be stored here. In particular, this is expected for FirstOp0_MergeOp1_ConcatenateMask and FirstThirdOp0_DoubleMergeOp1
    Value *Op1Value = nullptr;
    /// Store a precomputed mask
    Constant *Mask = nullptr;

  explicit ShuffleBundleDecision(ArrayRef<Value *> VL, int MergeMode, OperandIndices Op0Indices, Value *Op1Value)
    : MergeMode(MergeMode),
      Op0Indices(Op0Indices) {
    this->Op1Value = Op1Value;
    initializeVL(VL);
    initializeMask();
  }

  explicit ShuffleBundleDecision(ArrayRef<Value *> VL, int MergeMode, Value *Op1Value)
    : MergeMode(MergeMode) {
    this->Op1Value = Op1Value;
    initializeVL(VL);
    initializeMask();
  }

  bool matchesBundle(ArrayRef<Value *> AltVL) {
      if (AltVL.size() != BundleSize)
          return false;

      return std::equal(AltVL.begin(), AltVL.end(), this->VL.begin());
  }

protected:
  void initializeVL(ArrayRef<Value *> VL) {
    this->BundleSize = VL.size();
    for (Value *val : VL)
      this->VL.push_back(val);

    assert((this->VL.size() == this->BundleSize) && "Unable to copy all values on ShuffleBundleDecision construction");
  }

  void initializeMask() {
    switch (MergeMode) {
      case ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask:
      case ShuffleBundleDecision::FirstOp0_MergeOp1_ConcatenateMask: {
        SmallVector<Constant *, 32> wideMask;
        for (Value *narrowVec : VL) {
          ShuffleVectorInst *narrowInst = cast<ShuffleVectorInst>(narrowVec);
          Constant *narrowMask = narrowInst->getMask();
          unsigned i = 0;
          while (Constant *scalar = narrowMask->getAggregateElement(i++))
            wideMask.push_back(scalar);
        }

        Mask = ConstantVector::get(wideMask);
        break;
      }
      case ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask: {
        ValueList LeftBundle, RightBundle;
        for (unsigned i = 0, Max = VL.size(); i < Max; ++i) {
          ShuffleVectorInst *Inst = cast<ShuffleVectorInst>(VL[i]);
          LeftBundle.push_back(
              Inst->getOperand(Op0Indices[i] ? 1 : 0));
          RightBundle.push_back(
              Inst->getOperand(Op0Indices[i] ? 0 : 1));
        }

        // Merge shuffle masks. First, determine the offset each original
        // operand will have in the final shufflevector.
        SmallVector<int, 4> Op0Offsets, Op1Offsets;
        int offset = 0;
        for (unsigned i = 0, Max = VL.size(); i < Max; ++i) {
          if (Op0Indices[i])
            Op1Offsets.push_back(offset);
          else
            Op0Offsets.push_back(offset);
          offset += VL[i]->getType()->getVectorNumElements();
        }
        for (unsigned i = 0, Max = VL.size(); i < Max; ++i) {
          if (Op0Indices[i])
            Op0Offsets.push_back(offset);
          else
            Op1Offsets.push_back(offset);
          offset += VL[i]->getType()->getVectorNumElements();
        }

#ifndef NDEBUG
        int totalElements = 0;
        for (Value *val : LeftBundle)
            totalElements += val->getType()->getVectorNumElements();
        for (Value *val : RightBundle)
            totalElements += val->getType()->getVectorNumElements();
#endif

        // Now, find the original offset of the operands of the bundle and
        // update the mask
        Type *int32Ty = nullptr;
        SmallVector<Constant *, 16> MaskVector;
        for (unsigned i = 0, Max = VL.size(); i < Max; ++i) {
          ShuffleVectorInst *inst = cast<ShuffleVectorInst>(VL[i]);

          int maskShift0 = Op0Offsets[i];

          Value *narrowOp0 = inst->getOperand(0);
          int originalOffset1 = narrowOp0->getType()->getVectorNumElements();
          int maskShift1 = Op1Offsets[i] - originalOffset1;

          if (int32Ty == nullptr)
            int32Ty = cast<Constant>(inst->getMask())->getAggregateElement(0U)->getType();

          for (int maskVal : inst->getShuffleMask()) {
            if (maskVal == -1) {
              MaskVector.push_back(UndefValue::get(int32Ty));
              continue;
            }

            assert(maskVal >= 0 && "Negative mask values not permitted");

            if (maskVal < originalOffset1)
              // This mask value indexed operand 0
              maskVal += maskShift0;
            else
              // This mask value indexed operand 1
              maskVal += maskShift1;

            assert(maskVal >= 0 && "Negative mask values not permitted");

#ifndef NDEBUG
            assert(maskVal < totalElements && "Mask value indexes beyond length of shuffle");
#endif
            MaskVector.push_back(ConstantInt::get(int32Ty, maskVal));
          }
        }

        Mask = ConstantVector::get(MaskVector);
        LLVM_DEBUG(dbgs() << "Revec: Created mask for shuffle merge mode IndexOp0_IndexOp1_MergeMask: " << *Mask << "\n");
        break;
      }
			case ShuffleBundleDecision::Diagonal_VF2: {
        // Remap mask 2 (a flip)
        assert(VL.size() == 2);

        ShuffleVectorInst *SI0 = cast<ShuffleVectorInst>(VL[0]);
        Constant *Mask0 = SI0->getMask();
        
        ShuffleVectorInst *SI1 = cast<ShuffleVectorInst>(VL[1]);
        Constant *Mask1 = getMaskForSwappedOperands(
                            SI1->getOperand(0)->getType(),
                            SI1->getMask());
        LLVM_DEBUG(dbgs() << "Revec: Created mask for shuffle merge mode Diagonal_VF2\n");

        SmallVector<Constant *, 32> wideMask;

        unsigned i = 0;
        while (Constant *scalar = Mask0->getAggregateElement(i++))
          wideMask.push_back(scalar);

        i = 0;
        while (Constant *scalar = Mask1->getAggregateElement(i++))
          wideMask.push_back(scalar);

        Mask = ConstantVector::get(wideMask);
        break;
			}
    }
  }
};

//SmallVector<ShuffleBundleDecision, 32> ShuffleCache;
std::vector<ShuffleBundleDecision> ShuffleCache;

ShuffleBundleDecision getShuffleBundleDecision(ArrayRef<Value *> VL) {
  for (ShuffleBundleDecision decision : ShuffleCache)
    if (decision.matchesBundle(VL))
      return decision;

  LLVM_DEBUG(dbgs() << "Revec: Shuffle bundle decision not found for bundle starting with " << *VL[0] << "\n");
#if 0
  OperandIndices dummyIndices;
  ShuffleCache.emplace_back(VL, ShuffleBundleDecision::Gather, dummyIndices, nullptr);
  return ShuffleCache.back();
#else
  assert(false && "Missing key: VL not present in the shuffle bundle decision cache");
#endif
}

//void newShuffleBundleDecision_laneWiden(ArrayRef<Value *> VL) {
//  ShuffleBundleDecision d(VL, 0);
//  d.MergeMode = ShuffleBundleDecision
//}

/// Create a new VectorizableTree entry.
void newTreeEntry(ArrayRef<Value *> VL, bool Vectorized, int &UserTreeIdx,
                  ArrayRef<unsigned> ReuseShuffleIndices = None) {
  VectorizableTree.emplace_back(VectorizableTree);
  int idx = VectorizableTree.size() - 1;
  TreeEntry *Last = &VectorizableTree[idx];

  Last->Scalars.insert(Last->Scalars.begin(), VL.begin(), VL.end());
  Last->NeedToGather = !Vectorized;

  Last->ReuseShuffleIndices.append(ReuseShuffleIndices.begin(),
                                    ReuseShuffleIndices.end());

  if (ReuseShuffleIndices.size()) {
    unsigned NumEltsVL0 = VL[0]->getType()->getVectorNumElements();
    LLVM_DEBUG(
      for (Value *val : VL)
        assert(val->getType()->getVectorNumElements() == NumEltsVL0 && "Unequal bundle types");
    );

    expandLanes(ReuseShuffleIndices, NumEltsVL0, Last->ReuseShuffleMask);
  }

  if (Vectorized) {
    for (int i = 0, e = VL.size(); i != e; ++i) {
      assert(!getTreeEntry(VL[i]) && "Scalar already in tree!");
      ScalarToTreeEntry[VL[i]] = idx;
    }
  } else {
    MustGather.insert(VL.begin(), VL.end());
  }

  if (UserTreeIdx >= 0)
    Last->UserTreeIndices.push_back(UserTreeIdx);
  UserTreeIdx = idx;
}

/// -- Vectorization State --
/// Holds all of the tree entries.
std::vector<TreeEntry> VectorizableTree;

TreeEntry *getTreeEntry(Value *V) {
  auto I = ScalarToTreeEntry.find(V);
  if (I != ScalarToTreeEntry.end())
    return &VectorizableTree[I->second];
  return nullptr;
}

/// Maps a specific scalar to its tree entry.
SmallDenseMap<Value*, int> ScalarToTreeEntry;

/// A list of scalars that we found that we need to keep as scalars.
ValueSet MustGather;

/// This POD struct describes one external user in the vectorized tree.
struct ExternalUser {
  ExternalUser(Value *S, llvm::User *U, int L)
      : Scalar(S), User(U), Lane(L) {}

  // Which scalar in our function.
  Value *Scalar;

  // Which user that uses the scalar.
  llvm::User *User;

  // Which lane does the scalar/vector start at/belong to.
  int Lane;
};
using UserList = SmallVector<ExternalUser, 16>;

/// Checks if two instructions may access the same memory.
///
/// \p Loc1 is the location of \p Inst1. It is passed explicitly because it
/// is invariant in the calling loop.
bool isAliased(const MemoryLocation &Loc1, Instruction *Inst1,
                Instruction *Inst2) {
  // First check if the result is already in the cache.
  AliasCacheKey key = std::make_pair(Inst1, Inst2);
  Optional<bool> &result = AliasCache[key];
  if (result.hasValue()) {
    return result.getValue();
  }
  MemoryLocation Loc2 = getLocation(Inst2, AA);
  bool aliased = true;
  if (Loc1.Ptr && Loc2.Ptr && isSimple(Inst1) && isSimple(Inst2)) {
    // Do the alias check.
    aliased = AA->alias(Loc1, Loc2);
  }
  // Store the result in the cache.
  result = aliased;
  return aliased;
}

using AliasCacheKey = std::pair<Instruction *, Instruction *>;

/// Cache for alias results.
/// TODO: consider moving this to the AliasAnalysis itself.
DenseMap<AliasCacheKey, Optional<bool>> AliasCache;

/// Removes an instruction from its block and eventually deletes it.
/// It's like Instruction::eraseFromParent() except that the actual deletion
/// is delayed until BoUpSLP is destructed.
/// This is required to ensure that there are no incorrect collisions in the
/// AliasCache, which can happen if a new instruction is allocated at the
/// same address as a previously deleted instruction.
void eraseInstruction(Instruction *I) {
  I->removeFromParent();
  I->dropAllReferences();
  DeletedInstructions.emplace_back(I);
}

int getInstructionThroughput(const Instruction *I, int defaultCost) const {
  int cost = TTI->getInstructionCost(I, TargetTransformInfo::TCK_RecipThroughput);
  if (cost < 0) {
    LLVM_DEBUG(dbgs() << "Revec: Unknown cost for " << *I << ", defaulting to cost = " << defaultCost << ".\n");
    return defaultCost;
  }
  return cost;
}

/// Temporary store for deleted instructions. Instructions will be deleted
/// eventually when the BoUpSLP is destructed.
SmallVector<unique_value, 8> DeletedInstructions;

/// A list of values that need to extracted out of the tree.
/// This list holds pairs of (Internal Scalar : External User). External User
/// can be nullptr, it means that this Internal Scalar will be used later,
/// after vectorization.
UserList ExternalUses;

/// Values used only by @llvm.assume calls.
SmallPtrSet<const Value *, 32> EphValues;

/// Holds all of the instructions that we gathered.
SetVector<Instruction *> GatherSeq;

/// A list of blocks that we are going to CSE.
SetVector<BasicBlock *> CSEBlocks;

/// Contains all scheduling relevant data for an instruction.
/// A ScheduleData either represents a single instruction or a member of an
/// instruction bundle (= a group of instructions which is combined into a
/// vector instruction).
struct ScheduleData {
  // The initial value for the dependency counters. It means that the
  // dependencies are not calculated yet.
  enum { InvalidDeps = -1 };

  ScheduleData() = default;

  void init(int BlockSchedulingRegionID, Value *OpVal) {
    FirstInBundle = this;
    NextInBundle = nullptr;
    NextLoadStore = nullptr;
    IsScheduled = false;
    SchedulingRegionID = BlockSchedulingRegionID;
    UnscheduledDepsInBundle = UnscheduledDeps;
    clearDependencies();
    OpValue = OpVal;
  }

  /// Returns true if the dependency information has been calculated.
  bool hasValidDependencies() const { return Dependencies != InvalidDeps; }

  /// Returns true for single instructions and for bundle representatives
  /// (= the head of a bundle).
  bool isSchedulingEntity() const { return FirstInBundle == this; }

  /// Returns true if it represents an instruction bundle and not only a
  /// single instruction.
  bool isPartOfBundle() const {
    return NextInBundle != nullptr || FirstInBundle != this;
  }

  /// Returns true if it is ready for scheduling, i.e. it has no more
  /// unscheduled depending instructions/bundles.
  bool isReady() const {
    assert(isSchedulingEntity() &&
            "can't consider non-scheduling entity for ready list");
    return UnscheduledDepsInBundle == 0 && !IsScheduled;
  }

  /// Modifies the number of unscheduled dependencies, also updating it for
  /// the whole bundle.
  int incrementUnscheduledDeps(int Incr) {
    UnscheduledDeps += Incr;
    return FirstInBundle->UnscheduledDepsInBundle += Incr;
  }

  /// Sets the number of unscheduled dependencies to the number of
  /// dependencies.
  void resetUnscheduledDeps() {
    incrementUnscheduledDeps(Dependencies - UnscheduledDeps);
  }

  /// Clears all dependency information.
  void clearDependencies() {
    Dependencies = InvalidDeps;
    resetUnscheduledDeps();
    MemoryDependencies.clear();
  }

  void dump(raw_ostream &os) const {
    if (!isSchedulingEntity()) {
      os << "/ " << *Inst;
    } else if (NextInBundle) {
      os << '[' << *Inst;
      ScheduleData *SD = NextInBundle;
      while (SD) {
        os << ';' << *SD->Inst;
        SD = SD->NextInBundle;
      }
      os << ']';
    } else {
      os << *Inst;
    }
  }

  Instruction *Inst = nullptr;

  /// Points to the head in an instruction bundle (and always to this for
  /// single instructions).
  ScheduleData *FirstInBundle = nullptr;

  /// Single linked list of all instructions in a bundle. Null if it is a
  /// single instruction.
  ScheduleData *NextInBundle = nullptr;

  /// Single linked list of all memory instructions (e.g. load, store, call)
  /// in the block - until the end of the scheduling region.
  ScheduleData *NextLoadStore = nullptr;

  /// The dependent memory instructions.
  /// This list is derived on demand in calculateDependencies().
  SmallVector<ScheduleData *, 4> MemoryDependencies;

  /// This ScheduleData is in the current scheduling region if this matches
  /// the current SchedulingRegionID of BlockScheduling.
  int SchedulingRegionID = 0;

  /// Used for getting a "good" final ordering of instructions.
  int SchedulingPriority = 0;

  /// The number of dependencies. Constitutes of the number of users of the
  /// instruction plus the number of dependent memory instructions (if any).
  /// This value is calculated on demand.
  /// If InvalidDeps, the number of dependencies is not calculated yet.
  int Dependencies = InvalidDeps;

  /// The number of dependencies minus the number of dependencies of scheduled
  /// instructions. As soon as this is zero, the instruction/bundle gets ready
  /// for scheduling.
  /// Note that this is negative as long as Dependencies is not calculated.
  int UnscheduledDeps = InvalidDeps;

  /// The sum of UnscheduledDeps in a bundle. Equals to UnscheduledDeps for
  /// single instructions.
  int UnscheduledDepsInBundle = InvalidDeps;

  /// True if this instruction is scheduled (or considered as scheduled in the
  /// dry-run).
  bool IsScheduled = false;

  /// Opcode of the current instruction in the schedule data.
  Value *OpValue = nullptr;


};

#ifndef NDEBUG
friend inline raw_ostream &operator<<(raw_ostream &os,
                                      const BoUpSLP::ScheduleData &SD) {
  SD.dump(os);
  return os;
}
#endif

friend struct GraphTraits<BoUpSLP *>;
friend struct DOTGraphTraits<BoUpSLP *>;

/// Contains all scheduling data for a basic block.
struct BlockScheduling {
  BlockScheduling(BasicBlock *BB)
      : BB(BB), ChunkSize(BB->size()), ChunkPos(ChunkSize) {}

  void clear() {
    ReadyInsts.clear();
    ScheduleStart = nullptr;
    ScheduleEnd = nullptr;
    FirstLoadStoreInRegion = nullptr;
    LastLoadStoreInRegion = nullptr;

    // Reduce the maximum schedule region size by the size of the
    // previous scheduling run.
    ScheduleRegionSizeLimit -= ScheduleRegionSize;
    if (ScheduleRegionSizeLimit < MinScheduleRegionSize)
      ScheduleRegionSizeLimit = MinScheduleRegionSize;
    ScheduleRegionSize = 0;

    // Make a new scheduling region, i.e. all existing ScheduleData is not
    // in the new region yet.
    ++SchedulingRegionID;
  }

  ScheduleData *getScheduleData(Value *V) {
    ScheduleData *SD = ScheduleDataMap[V];
    if (SD && SD->SchedulingRegionID == SchedulingRegionID)
      return SD;
    return nullptr;
  }

  ScheduleData *getScheduleData(Value *V, Value *Key) {
    if (V == Key)
      return getScheduleData(V);
    auto I = ExtraScheduleDataMap.find(V);
    if (I != ExtraScheduleDataMap.end()) {
      ScheduleData *SD = I->second[Key];
      if (SD && SD->SchedulingRegionID == SchedulingRegionID)
        return SD;
    }
    return nullptr;
  }

  bool isInSchedulingRegion(ScheduleData *SD) {
    return SD->SchedulingRegionID == SchedulingRegionID;
  }

  /// Marks an instruction as scheduled and puts all dependent ready
  /// instructions into the ready-list.
  template <typename ReadyListType>
  void schedule(ScheduleData *SD, ReadyListType &ReadyList) {
    SD->IsScheduled = true;
    LLVM_DEBUG(dbgs() << "Revec:   schedule " << *SD << "\n");

    ScheduleData *BundleMember = SD;
    while (BundleMember) {
      if (BundleMember->Inst != BundleMember->OpValue) {
        BundleMember = BundleMember->NextInBundle;
        continue;
      }
      // Handle the def-use chain dependencies.
      for (Use &U : BundleMember->Inst->operands()) {
        auto *I = dyn_cast<Instruction>(U.get());
        if (!I)
          continue;
        doForAllOpcodes(I, [&ReadyList](ScheduleData *OpDef) {
          if (OpDef && OpDef->hasValidDependencies() &&
              OpDef->incrementUnscheduledDeps(-1) == 0) {
            // There are no more unscheduled dependencies after
            // decrementing, so we can put the dependent instruction
            // into the ready list.
            ScheduleData *DepBundle = OpDef->FirstInBundle;
            assert(!DepBundle->IsScheduled &&
                    "already scheduled bundle gets ready");
            ReadyList.insert(DepBundle);
            LLVM_DEBUG(dbgs()
                  << "Revec:    gets ready (def): " << *DepBundle << "\n");
          }
        });
      }
      // Handle the memory dependencies.
      for (ScheduleData *MemoryDepSD : BundleMember->MemoryDependencies) {
        if (MemoryDepSD->incrementUnscheduledDeps(-1) == 0) {
          // There are no more unscheduled dependencies after decrementing,
          // so we can put the dependent instruction into the ready list.
          ScheduleData *DepBundle = MemoryDepSD->FirstInBundle;
          assert(!DepBundle->IsScheduled &&
                  "already scheduled bundle gets ready");
          ReadyList.insert(DepBundle);
          LLVM_DEBUG(dbgs() << "Revec:    gets ready (mem): " << *DepBundle
                        << "\n");
        }
      }
      BundleMember = BundleMember->NextInBundle;
    }
  }

  void doForAllOpcodes(Value *V,
                        function_ref<void(ScheduleData *SD)> Action) {
    if (ScheduleData *SD = getScheduleData(V))
      Action(SD);
    auto I = ExtraScheduleDataMap.find(V);
    if (I != ExtraScheduleDataMap.end())
      for (auto &P : I->second)
        if (P.second->SchedulingRegionID == SchedulingRegionID)
          Action(P.second);
  }

  /// Put all instructions into the ReadyList which are ready for scheduling.
  template <typename ReadyListType>
  void initialFillReadyList(ReadyListType &ReadyList) {
    for (auto *I = ScheduleStart; I != ScheduleEnd; I = I->getNextNode()) {
      doForAllOpcodes(I, [&](ScheduleData *SD) {
#if 0
        LLVM_DEBUG(dbgs() << "Revec:    ScheduleData: " << *SD << " Instr: " << *I
                      << "\n\tSD: " << SD
                      << "\n\tSD FirstInBundle: " << SD->FirstInBundle
                      << "\n\tSD UnscheduledDeps: " << SD->UnscheduledDeps
                      << "\n\tSD UnscheduledDepsInBundle: " << SD->UnscheduledDepsInBundle << "\n");
#endif
        if (SD->isSchedulingEntity() && SD->isReady()) {
          ReadyList.insert(SD);
          LLVM_DEBUG(dbgs() << "Revec:    initially in ready list: " << *I << "\n");
        }
      });
    }
  }

  /// Checks if a bundle of instructions can be scheduled, i.e. has no
  /// cyclic dependencies. This is only a dry-run, no instructions are
  /// actually moved at this stage.
  bool tryScheduleBundle(ArrayRef<Value *> VL, BoUpSLP *SLP, Value *OpValue);

  /// Un-bundles a group of instructions.
  void cancelScheduling(ArrayRef<Value *> VL, Value *OpValue);

  /// Allocates schedule data chunk.
  ScheduleData *allocateScheduleDataChunks();

  /// Extends the scheduling region so that V is inside the region.
  /// \returns true if the region size is within the limit.
  bool extendSchedulingRegion(Value *V, Value *OpValue);

  /// Initialize the ScheduleData structures for new instructions in the
  /// scheduling region.
  void initScheduleData(Instruction *FromI, Instruction *ToI,
                        ScheduleData *PrevLoadStore,
                        ScheduleData *NextLoadStore);

  /// Updates the dependency information of a bundle and of all instructions/
  /// bundles which depend on the original bundle.
  void calculateDependencies(ScheduleData *SD, bool InsertInReadyList,
                              BoUpSLP *SLP);

  /// Sets all instruction in the scheduling region to un-scheduled.
  void resetSchedule();

  BasicBlock *BB;

  /// Simple memory allocation for ScheduleData.
  std::vector<std::unique_ptr<ScheduleData[]>> ScheduleDataChunks;

  /// The size of a ScheduleData array in ScheduleDataChunks.
  int ChunkSize;

  /// The allocator position in the current chunk, which is the last entry
  /// of ScheduleDataChunks.
  int ChunkPos;

  /// Attaches ScheduleData to Instruction.
  /// Note that the mapping survives during all vectorization iterations, i.e.
  /// ScheduleData structures are recycled.
  DenseMap<Value *, ScheduleData *> ScheduleDataMap;

  /// Attaches ScheduleData to Instruction with the leading key.
  DenseMap<Value *, SmallDenseMap<Value *, ScheduleData *>>
      ExtraScheduleDataMap;

  struct ReadyList : SmallVector<ScheduleData *, 8> {
    void insert(ScheduleData *SD) { push_back(SD); }
  };

  /// The ready-list for scheduling (only used for the dry-run).
  ReadyList ReadyInsts;

  /// The first instruction of the scheduling region.
  Instruction *ScheduleStart = nullptr;

  /// The first instruction _after_ the scheduling region.
  Instruction *ScheduleEnd = nullptr;

  /// The first memory accessing instruction in the scheduling region
  /// (can be null).
  ScheduleData *FirstLoadStoreInRegion = nullptr;

  /// The last memory accessing instruction in the scheduling region
  /// (can be null).
  ScheduleData *LastLoadStoreInRegion = nullptr;

  /// The current size of the scheduling region.
  int ScheduleRegionSize = 0;

  /// The maximum size allowed for the scheduling region.
  int ScheduleRegionSizeLimit = ScheduleRegionSizeBudget;

  /// The ID of the scheduling region. For a new vectorization iteration this
  /// is incremented which "removes" all ScheduleData from the region.
  // Make sure that the initial SchedulingRegionID is greater than the
  // initial SchedulingRegionID in ScheduleData (which is 0).
  int SchedulingRegionID = 1;
};

/// Attaches the BlockScheduling structures to basic blocks.
MapVector<BasicBlock *, std::unique_ptr<BlockScheduling>> BlocksSchedules;

/// Performs the "real" scheduling. Done before vectorization is actually
/// performed in a basic block.
void scheduleBlock(BlockScheduling *BS);

/// List of users to ignore during scheduling and that don't need extracting.
ArrayRef<Value *> UserIgnoreList;

/// Number of operation bundles that contain consecutive operations - number
/// of operation bundles that contain consecutive operations in reversed
/// order.
DenseMap<unsigned, int> NumOpsWantToKeepOrder;

/// ADDED
/// Number of bundles that do not require reordering.
unsigned NumOpsWantToKeepOriginalOrder = 0;

// Analysis and block reference.
Function *F;
ScalarEvolution *SE;
TargetTransformInfo *TTI;
TargetLibraryInfo *TLI;
AliasAnalysis *AA;
LoopInfo *LI;
DominatorTree *DT;
AssumptionCache *AC;
DemandedBits *DB;
const DataLayout *DL;
OptimizationRemarkEmitter *ORE;

unsigned MaxVecRegSize; // This is set by TTI or overridden by cl::opt.
unsigned MinVecRegSize; // Set by cl::opt (default: 128).

/// Instruction builder to construct the vectorized tree.
IRBuilder<> Builder;
};

} // end namespace revectorizer

template <> struct GraphTraits<BoUpSLP *> {
using TreeEntry = BoUpSLP::TreeEntry;

/// NodeRef has to be a pointer per the GraphWriter.
using NodeRef = TreeEntry *;

/// \brief Add the VectorizableTree to the index iterator to be able to return
/// TreeEntry pointers.
struct ChildIteratorType
    : public iterator_adaptor_base<ChildIteratorType,
                                    SmallVector<int, 1>::iterator> {
  std::vector<TreeEntry> &VectorizableTree;

  ChildIteratorType(SmallVector<int, 1>::iterator W,
                    std::vector<TreeEntry> &VT)
      : ChildIteratorType::iterator_adaptor_base(W), VectorizableTree(VT) {}

  NodeRef operator*() { return &VectorizableTree[*I]; }
};

static NodeRef getEntryNode(BoUpSLP &R) { return &R.VectorizableTree[0]; }

static ChildIteratorType child_begin(NodeRef N) {
  return {N->UserTreeIndices.begin(), N->Container};
}

static ChildIteratorType child_end(NodeRef N) {
  return {N->UserTreeIndices.end(), N->Container};
}

/// For the node iterator we just need to turn the TreeEntry iterator into a
/// TreeEntry* iterator so that it dereferences to NodeRef.
using nodes_iterator = pointer_iterator<std::vector<TreeEntry>::iterator>;

static nodes_iterator nodes_begin(BoUpSLP *R) {
  return nodes_iterator(R->VectorizableTree.begin());
}

static nodes_iterator nodes_end(BoUpSLP *R) {
  return nodes_iterator(R->VectorizableTree.end());
}

static unsigned size(BoUpSLP *R) { return R->VectorizableTree.size(); }
};

template <> struct DOTGraphTraits<BoUpSLP *> : public DefaultDOTGraphTraits {
using TreeEntry = BoUpSLP::TreeEntry;

DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

std::string getNodeLabel(const TreeEntry *Entry, const BoUpSLP *R) {
  std::string Str;
  raw_string_ostream OS(Str);
  if (isSplat(Entry->Scalars)) {
    OS << "<splat> " << *Entry->Scalars[0];
  } else {
    for (auto V : Entry->Scalars) {
      OS << *V;
      if (std::any_of(
              R->ExternalUses.begin(), R->ExternalUses.end(),
              [&](const BoUpSLP::ExternalUser &EU) { return EU.Scalar == V; }))
        OS << " <extract>";
      OS << "\n";
    }
  }
  OS << "Entry cost: " << Entry->CostCached << "\n";
  return Str;
}

static std::string getNodeAttributes(const TreeEntry *Entry,
                                      const BoUpSLP *) {
  if (Entry->NeedToGather)
    return "color=red";
  return "";
}
};

} // end namespace llvm

void BoUpSLP::buildTree(ArrayRef<Value *> Roots,
                      ArrayRef<Value *> UserIgnoreLst) {
ExtraValueToDebugLocsMap ExternallyUsedValues;
buildTree(Roots, ExternallyUsedValues, UserIgnoreLst);
}

void BoUpSLP::buildTree(ArrayRef<Value *> Roots,
                      ExtraValueToDebugLocsMap &ExternallyUsedValues,
                      ArrayRef<Value *> UserIgnoreLst) {
deleteTree();
UserIgnoreList = UserIgnoreLst;
if (!allSameType(Roots))
  return;

// TODO: Refactor, move this to pass initialization
initializeIntrinsicWideningMap();

buildTree_rec(Roots, 0, -1);

// Collect the values that we need to extract from the tree.
for (TreeEntry &EIdx : VectorizableTree) {
  TreeEntry *Entry = &EIdx;

  // No need to handle users of gathered values.
  if (Entry->NeedToGather)
    continue;

  // For each lane:
  for (int Lane = 0, LE = Entry->Scalars.size(); Lane != LE; ++Lane) {
    Value *narrowVec = Entry->Scalars[Lane];
    int FoundLane = Lane;
    if (!Entry->ReuseShuffleIndices.empty()) {
      FoundLane =
          std::distance(Entry->ReuseShuffleIndices.begin(),
                        llvm::find(Entry->ReuseShuffleIndices, FoundLane));
    }

    // Check if the narrow vector is externally used as an extra arg.
    auto ExtI = ExternallyUsedValues.find(narrowVec);
    if (ExtI != ExternallyUsedValues.end()) {
      LLVM_DEBUG(dbgs() << "Revec: Need to extract: Extra arg " << *narrowVec << " from lane " <<
      Lane << ".\n");
      ExternalUses.emplace_back(narrowVec, nullptr, FoundLane);
    }
    for (User *U : narrowVec->users()) {
      LLVM_DEBUG(dbgs() << "Revec: Checking user:" << *U << ".\n");

      Instruction *UserInst = dyn_cast<Instruction>(U);
      if (!UserInst)
        continue;

      // Skip in-tree scalars that become vectors
      if (TreeEntry *UseEntry = getTreeEntry(U)) {
        Value *UseScalar = UseEntry->Scalars[0];
        // FIXME: Is pulling out of Lane 0 correct for revectorization?
        // Some in-tree scalars will remain as scalar in vectorized
        // instructions. If that is the case, the one in Lane 0 will
        // be used.
        if (UseScalar != U ||
            !InTreeUserNeedToExtract(narrowVec, UserInst, TLI)) {
          LLVM_DEBUG(dbgs() << "Revec: \tInternal user will be removed:" << *U
                        << ".\n");
          assert(!UseEntry->NeedToGather && "Bad state");
          continue;
        }
      }

      // Ignore users in the user ignore list.
      if (is_contained(UserIgnoreList, UserInst))
        continue;

      LLVM_DEBUG(dbgs() << "Revec: Need to extract:" << *narrowVec << " from lane " <<
            Lane << " for User " << *U << ".\n");
      ExternalUses.push_back(ExternalUser(narrowVec, U, FoundLane));
    }
  }
}
}

void BoUpSLP::buildTree_rec(ArrayRef<Value *> VL, unsigned Depth,
                          int UserTreeIdx) {
  assert((allConstant(VL) || allSameType(VL)) && "Invalid types!");

  // Ensure that all values are narorw vectors
  for (Value *val : VL) {
    // TODO: What is the return of gep instructions?
    if (dyn_cast<StoreInst>(val) || dyn_cast<GetElementPtrInst>(val)) {
      // StoreInst returns void, but forms a valid bundle. Skip type checks.
      continue;
    }

    Type *Ty = val->getType();
    if (!isValidElementType(Ty)) {
      LLVM_DEBUG(dbgs() << "Revec: This bundle has invalid element type: " << *Ty << " for value: " << *val << "\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }

    if (!isNarrowVectorType(Ty)) {
      LLVM_DEBUG(dbgs() << "Revec: This bundle has invalid element type: " << *Ty << " for value: " << *val << "\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
  }

  InstructionsState S = getSameOpcode(VL);
  if (Depth == RecursionMaxDepth) {
    LLVM_DEBUG(dbgs() << "Revec: Gathering due to max recursion depth.\n");
    newTreeEntry(VL, false, UserTreeIdx);
    return;
  }

  if (StoreInst *SI = dyn_cast<StoreInst>(S.OpValue))
    if (!SI->getValueOperand()->getType()->isVectorTy()) {
      LLVM_DEBUG(dbgs() << "Revec: Gathering due to non-vector store value type.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }

  // If all of the operands are identical or constant we have a simple solution.
  if (allConstant(VL) || isSplat(VL) || !allSameBlock(VL) || !S.Opcode) {
    LLVM_DEBUG(dbgs() << "Revec: Gathering due to C,S,B,O. \n");
    newTreeEntry(VL, false, UserTreeIdx);
    return;
  }

  // We now know that this is a vector of instructions of the same type from
  // the same block.

  // Don't vectorize ephemeral values.
  for (unsigned i = 0, e = VL.size(); i != e; ++i) {
    if (EphValues.count(VL[i])) {
      LLVM_DEBUG(dbgs() << "Revec: The instruction (" << *VL[i] <<
            ") is ephemeral.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
  }

  // Check if this is a duplicate of another entry.
  if (TreeEntry *E = getTreeEntry(S.OpValue)) {
    LLVM_DEBUG(dbgs() << "Revec: \tChecking bundle: " << *S.OpValue << ".\n");
    if (!E->isSame(VL)) {
      LLVM_DEBUG(dbgs() << "Revec: Gathering due to partial overlap.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
    // Record the reuse of the tree node.  FIXME, currently this is only used to
    // properly draw the graph rather than for the actual vectorization.
    E->UserTreeIndices.push_back(UserTreeIdx);
    LLVM_DEBUG(dbgs() << "Revec: Perfect diamond merge at " << *S.OpValue << ".\n");
    return;
  }

  // Check that none of the instructions in the bundle are already in the tree.
  for (unsigned i = 0, e = VL.size(); i != e; ++i) {
    auto *I = dyn_cast<Instruction>(VL[i]);
    if (!I)
      continue;
    if (getTreeEntry(I)) {
      LLVM_DEBUG(dbgs() << "Revec: The instruction (" << *VL[i] <<
            ") is already in tree.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
  }

  // If any of the narrow vectors is marked as a value that needs to stay narrow, then
  // we need to gather the narrow vectors.
  for (unsigned i = 0, e = VL.size(); i != e; ++i) {
    if (MustGather.count(VL[i])) {
      LLVM_DEBUG(dbgs() << "Revec: Gathering due to gathered scalar.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
  }

  // Check that all of the users of the scalars that we want to vectorize are
  // schedulable.
  auto *VL0 = cast<Instruction>(S.OpValue);
  BasicBlock *BB = VL0->getParent();

  if (!DT->isReachableFromEntry(BB)) {
    // Don't go into unreachable blocks. They may contain instructions with
    // dependency cycles which confuse the final scheduling.
    LLVM_DEBUG(dbgs() << "Revec: bundle in unreachable block.\n");
    newTreeEntry(VL, false, UserTreeIdx);
    return;
  }

  // Check that every instruction appears once in this bundle.
  SmallVector<unsigned, 4> ReuseShuffleIndices;
  SmallVector<Value *, 4> UniqueValues;
  DenseMap<Value *, unsigned> UniquePositions;
  for (Value *V : VL) {
    auto Res = UniquePositions.try_emplace(V, UniqueValues.size());
    ReuseShuffleIndices.emplace_back(Res.first->second);
    if (Res.second)
      UniqueValues.emplace_back(V);
  }
  if (UniqueValues.size() == VL.size()) {
    ReuseShuffleIndices.clear();
  } else {
    LLVM_DEBUG(dbgs() << "Revec: Shuffle for reused scalars.\n");
    if (UniqueValues.size() <= 1 || !llvm::isPowerOf2_32(UniqueValues.size())) {
      LLVM_DEBUG(dbgs() << "Revec: Scalar used twice in bundle.\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
    VL = UniqueValues;
  }

  auto &BSRef = BlocksSchedules[BB];
  if (!BSRef)
    BSRef = llvm::make_unique<BlockScheduling>(BB);

  BlockScheduling &BS = *BSRef.get();

  if (!BS.tryScheduleBundle(VL, this, VL0)) {
    LLVM_DEBUG(dbgs() << "Revec: We are not able to schedule this bundle!\n");
    assert((!BS.getScheduleData(VL0) ||
            !BS.getScheduleData(VL0)->isPartOfBundle()) &&
            "tryScheduleBundle should cancelScheduling on failure");
    newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
    return;
  }
  LLVM_DEBUG(dbgs() << "Revec: We are able to schedule this bundle.\n");

  unsigned ShuffleOrOp = S.IsAltShuffle ?
              (unsigned) Instruction::ShuffleVector : S.Opcode;
  switch (ShuffleOrOp) {
    case Instruction::PHI: {
      PHINode *PH = dyn_cast<PHINode>(VL0);

      // Check for terminator values (e.g. invoke).
      for (unsigned j = 0; j < VL.size(); ++j)
        for (unsigned i = 0, e = PH->getNumIncomingValues(); i < e; ++i) {
          TerminatorInst *Term = dyn_cast<TerminatorInst>(
              cast<PHINode>(VL[j])->getIncomingValueForBlock(PH->getIncomingBlock(i)));
          if (Term) {
            // Gather PHINodes where an incoming value is the result of an invoke.
            // We would rather gather the Phi than handle incoming values differently (invokes would
            // need to be gathered regardless)
            LLVM_DEBUG(dbgs() << "Revec: Need to swizzle PHINodes (TerminatorInst use).\n");
            BS.cancelScheduling(VL, VL0);
            newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
            return;
          }
        }

      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of PHINodes.\n");

      for (unsigned i = 0, e = PH->getNumIncomingValues(); i < e; ++i) {
        ValueList Operands;
        // Prepare the operand vector.
        for (Value *j : VL)
          Operands.push_back(cast<PHINode>(j)->getIncomingValueForBlock(
              PH->getIncomingBlock(i)));

        buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      }
      return;
    }
    case Instruction::ExtractValue:
    case Instruction::ExtractElement: {
      // TODO: Handle ExtractValue instructions?
      LLVM_DEBUG(dbgs() << "Revec: Cannot create a tree entry with ExtractValue/ExtractElement instructions (scalars).\n");
      newTreeEntry(VL, false, UserTreeIdx);
      return;
    }
    case Instruction::Load: {
      // Check that a vectorized load would load the same memory as a scalar
      // load. For example, we don't want to vectorize loads that are smaller
      // than 8-bit. Even though we have a packed struct {<i2, i2, i2, i2>} LLVM
      // treats loading/storing it as an i8 struct. If we vectorize loads/stores
      // from such a struct, we read/write packed bits disagreeing with the
      // unvectorized version.
      Type *ScalarTy = VL0->getType();

      if (DL->getTypeSizeInBits(ScalarTy) !=
          DL->getTypeAllocSizeInBits(ScalarTy)) {
        BS.cancelScheduling(VL, VL0);
        newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
        LLVM_DEBUG(dbgs() << "Revec: Gathering loads of non-packed type.\n");
        return;
      }

      // Make sure all loads in the bundle are simple - we can't vectorize
      // atomic or volatile loads.
      for (unsigned i = 0, e = VL.size() - 1; i < e; ++i) {
        LoadInst *L = cast<LoadInst>(VL[i]);
        if (!L->isSimple()) {
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: Gathering non-simple loads.\n");
          return;
        }
      }

      // Check if the loads are consecutive, reversed, or neither.
      // TODO: What we really want is to sort the loads, but for now, check
      // the two likely directions.
      bool Consecutive = true;
      bool ReverseConsecutive = true;
      for (unsigned i = 0, e = VL.size() - 1; i < e; ++i) {
        if (!isConsecutiveAccess(VL[i], VL[i + 1], *DL, *SE)) {
          Consecutive = false;
          break;
        } else {
          ReverseConsecutive = false;
        }
      }

      if (Consecutive) {
        ++NumOpsWantToKeepOrder[S.Opcode];
        newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
        LLVM_DEBUG(dbgs() << "Revec: added a vector of loads.\n");
        return;
      }

      // If none of the load pairs were consecutive when checked in order,
      // check the reverse order.
      if (ReverseConsecutive)
        for (unsigned i = VL.size() - 1; i > 0; --i)
          if (!isConsecutiveAccess(VL[i], VL[i - 1], *DL, *SE)) {
            ReverseConsecutive = false;
            break;
          }

      if (ReverseConsecutive) {
        --NumOpsWantToKeepOrder[S.Opcode];
        newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
        LLVM_DEBUG(dbgs() << "Revec: added a vector of reversed loads.\n");
        return;
      }

      LLVM_DEBUG(dbgs() << "Revec: Gathering non-consecutive loads.\n");
      BS.cancelScheduling(VL, VL0);
      newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
      return;
    }
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::SIToFP:
    case Instruction::UIToFP:
    case Instruction::Trunc:
    case Instruction::FPTrunc:
    case Instruction::BitCast: {
      Type *SrcTy = VL0->getOperand(0)->getType();
      for (unsigned i = 0; i < VL.size(); ++i) {
        Type *Ty = cast<Instruction>(VL[i])->getOperand(0)->getType();
        if (Ty != SrcTy || !isValidElementType(Ty)) {
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: Gathering casts with different src types.\n");
          return;
        }
      }
      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of casts.\n");

      for (unsigned i = 0, e = VL0->getNumOperands(); i < e; ++i) {
        ValueList Operands;
        // Prepare the operand vector.
        for (Value *j : VL)
          Operands.push_back(cast<Instruction>(j)->getOperand(i));

        buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      }
      return;
    }
    case Instruction::ICmp:
    case Instruction::FCmp: {
      // Check that all of the compares have the same predicate.
      CmpInst::Predicate P0 = cast<CmpInst>(VL0)->getPredicate();
      Type *ComparedTy = VL0->getOperand(0)->getType();
      for (unsigned i = 1, e = VL.size(); i < e; ++i) {
        CmpInst *Cmp = cast<CmpInst>(VL[i]);
        if (Cmp->getPredicate() != P0 ||
            Cmp->getOperand(0)->getType() != ComparedTy) {
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: Gathering cmp with different predicate.\n");
          return;
        }
      }

      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of compares.\n");

      for (unsigned i = 0, e = VL0->getNumOperands(); i < e; ++i) {
        ValueList Operands;
        // Prepare the operand vector.
        for (Value *j : VL)
          Operands.push_back(cast<Instruction>(j)->getOperand(i));

        buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      }
      return;
    }
    case Instruction::Select:
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of bin op.\n");

      // Sort operands of the instructions so that each side is more likely to
      // have the same opcode.
      if (isa<BinaryOperator>(VL0) && VL0->isCommutative()) {
        // Note: Compares may commute if the operation is changed
        //       E.g. flip true/false values by flipping the operation (> to <)
        //       However, this would create different operations in a bundle that
        //       cannot be vectorized.
        ValueList Left, Right;
        reorderInputsAccordingToOpcode(S.Opcode, VL, Left, Right);
        buildTree_rec(Left, Depth + 1, UserTreeIdx);
        buildTree_rec(Right, Depth + 1, UserTreeIdx);
        return;
      }

      for (unsigned i = 0, e = VL0->getNumOperands(); i < e; ++i) {
        ValueList Operands;
        // Prepare the operand vector.
        for (Value *j : VL)
          Operands.push_back(cast<Instruction>(j)->getOperand(i));

        buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      }
      return;

    case Instruction::GetElementPtr: {
      // We don't combine GEPs with complicated (nested) indexing.
      for (unsigned j = 0; j < VL.size(); ++j) {
        if (cast<Instruction>(VL[j])->getNumOperands() != 2) {
          LLVM_DEBUG(dbgs() << "Revec: not-vectorizable GEP (nested indexes).\n");
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          return;
        }
      }

      // We can't combine several GEPs into one vector if they operate on
      // different types.
      Type *Ty0 = VL0->getOperand(0)->getType();
      for (unsigned j = 0; j < VL.size(); ++j) {
        Type *CurTy = cast<Instruction>(VL[j])->getOperand(0)->getType();
        if (Ty0 != CurTy) {
          LLVM_DEBUG(dbgs() << "Revec: not-vectorizable GEP (different types).\n");
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          return;
        }
      }

      // We don't combine GEPs with non-constant indexes.
      for (unsigned j = 0; j < VL.size(); ++j) {
        auto Op = cast<Instruction>(VL[j])->getOperand(1);
        if (!isa<ConstantInt>(Op)) {
          LLVM_DEBUG(
              dbgs() << "Revec: not-vectorizable GEP (non-constant indexes).\n");
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          return;
        }
      }

      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of GEPs.\n");
      for (unsigned i = 0, e = 2; i < e; ++i) {
        ValueList Operands;
        // Prepare the operand vector.
        for (Value *j : VL)
          Operands.push_back(cast<Instruction>(j)->getOperand(i));

        buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      }
      return;
    }
    case Instruction::Store: {
      // Check if the stores are consecutive or of we need to swizzle them.
      for (unsigned i = 0, e = VL.size() - 1; i < e; ++i)
        if (!isConsecutiveAccess(VL[i], VL[i + 1], *DL, *SE)) {
          BS.cancelScheduling(VL, VL0);
          newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: Non-consecutive store.\n");
          return;
        }

      newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: added a vector of stores.\n");

      ValueList Operands;
      for (Value *j : VL)
        Operands.push_back(cast<Instruction>(j)->getOperand(0));

      buildTree_rec(Operands, Depth + 1, UserTreeIdx);
      return;
    }
    case Instruction::Call: {
      // Check if the calls are all to the same vectorizable intrinsic.
      CallInst *CI = cast<CallInst>(VL0);
      Intrinsic::ID IID = getIntrinsicByCall(CI);

      if (IID == Intrinsic::not_intrinsic) {
        LLVM_DEBUG(dbgs() << "Revec: Call bundle contains non-intrinsic " << *CI << "\n");
        BS.cancelScheduling(VL, VL0);
        newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
        return;
      }

      if (!allSameIntrinsic(VL)) {
        // TODO: Split the bundle into halves if the halves match.
        LLVM_DEBUG(
          dbgs() << "Revec: Bundle contains different intrinsics.\n";
          for (Value *val : VL)
            dbgs() << "Revec:   " << *val << "\n";
        );

        BS.cancelScheduling(VL, VL0);
        newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
        return;
      }

      LLVM_DEBUG(
        if (llvm::Intrinsic::isOverloaded(IID))
          dbgs() << "Revec: Found intrinsic function overloading: " << IID << "\n";
        else
          dbgs() << "Revec: Found intrinsic function " << IID << ", " << llvm::Intrinsic::getName(IID) << "\n";
      );

      // Find intrinsic conversion and merge factor
      LLVM_DEBUG(dbgs() << "Revec: intrinsic size " << VL.size() << "\n");
      const auto& target = getWidenedIntrinsic(IID, VL.size());
      int VF = target.first;
      Intrinsic::ID alt = target.second;

      if (alt != Intrinsic::not_intrinsic && VF > 0) {
        assert(VF == static_cast<long>(VL.size()) && "getWidenedIntrinsic returned a VF not requested");

        if (llvm::Intrinsic::isOverloaded(alt))
          LLVM_DEBUG(dbgs() << "Revec:   possible conversion (overloading): " << alt
                  << " widening factor: " << VF << ".\n");
        else
          LLVM_DEBUG(dbgs() << "Revec:   possible conversion: " << llvm::Intrinsic::getName(alt)
                  << " widening factor: " << VF << ".\n");

        newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);

        Optional<unsigned> preservedOperand = getPreservedOperand(IID);

        for (unsigned i = 0, e = CI->getNumArgOperands(); i != e; ++i) {
          if (preservedOperand.hasValue() && i == preservedOperand.getValue())
            continue;

          ValueList Operands;
          // Prepare the operand vector.
          for (Value *val : VL) {
            CallInst *CI2 = dyn_cast<CallInst>(val);
            Operands.push_back(CI2->getArgOperand(i));
          }
          buildTree_rec(Operands, Depth + 1, UserTreeIdx);
        }

        return;
      }

      // Intrinsic cannot be widened. Cancel scheduling this tree
      // TODO: emit an optimization missed remark
      LLVM_DEBUG(
        dbgs()
          << "Revec:   no conversion found.\n"
          << "Revec:     First narrow call: " << *CI << "\n"
          << "Revec:     Narrow key: " << static_cast<unsigned>(IID);
        if (!llvm::Intrinsic::isOverloaded(IID))
          dbgs() << " name: " << llvm::Intrinsic::getName(IID) << "\n";
        else
          dbgs() << " (overloading)\n";
      );

      BS.cancelScheduling(VL, VL0);
      newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
      return;
    }
    case Instruction::ShuffleVector: {
      LLVM_DEBUG(dbgs() << "Revec: buldTree encountered shufflevector bundle starting with " << *VL0 << "\n");

      if (S.IsAltShuffle) {
        // This is actually a bundle of binary operations that require a shuffle to be
        // merged together
        newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
        LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for alt shuffle.\n");

        // Reorder operands if reordering would enable vectorization.
        if (isa<BinaryOperator>(VL0)) {
          ValueList Left, Right;
          reorderAltShuffleOperands(S.Opcode, VL, Left, Right);
          buildTree_rec(Left, Depth + 1, UserTreeIdx);
          buildTree_rec(Right, Depth + 1, UserTreeIdx);
          return;
        }

        for (unsigned i = 0, e = VL0->getNumOperands(); i < e; ++i) {
          ValueList Operands;
          // Prepare the operand vector.
          for (Value *j : VL)
            Operands.push_back(cast<Instruction>(j)->getOperand(i));

          buildTree_rec(Operands, Depth + 1, UserTreeIdx);
        }
        return;
      }

      // Check for special cases with an optimal vectorization decision
      ValueList Left, Right;
      SmallVector<Constant *, 8U> Masks;
      for (Value *V : VL) {
        ShuffleVectorInst *I = cast<ShuffleVectorInst>(V);
        Left.push_back(I->getOperand(0));
        Right.push_back(I->getOperand(1));
        Masks.push_back(I->getMask());
      }

      if (isSplat(Left)) {
        if (isSplat(Right)) {
          if (dyn_cast<UndefValue>(Right[0]) != nullptr) {
            // Check for case:
            // a1 = sv A, undef, <0, 1, 2, ..., 7>
            // a2 = sv A, undef, <8, 9, 10, ..., 15>
            //   =>
            // A
            unsigned expected_mask_value = 0;
            bool isSequentialMask = true;
            for (Constant * mask : Masks) {
              LLVM_DEBUG(dbgs() << "Revec: Checking mask " << *mask << " for sequentiality\n");

              for (unsigned i = 0; i < mask->getType()->getVectorNumElements(); ++i) {
                Value *Opi = mask->getAggregateElement(i);
                ConstantInt *CI = dyn_cast<ConstantInt>(Opi);
                LLVM_DEBUG(dbgs() << "Revec:   Mask operand at index " << i << " = " << *Opi << ". Testing if equal to " << expected_mask_value << "\n");
                if (CI == nullptr || !CI->equalsInt(expected_mask_value)) {
                  LLVM_DEBUG(dbgs() << "Revec:     Not equal! Mask is not sequential.\n");
                  isSequentialMask = false;
                  break;
                }

                LLVM_DEBUG(dbgs() << "Revec:     Equal. Mask may be sequential.\n");

                ++expected_mask_value;
              }
            }

            if (isSequentialMask) {
              // TODO: Remove dummyIndices
              OperandIndices dummyIndices;
              ShuffleCache.emplace_back(VL, ShuffleBundleDecision::FirstOp0, dummyIndices, nullptr);
              newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
              LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode FirstOp0\n");
              return;
            }
          }

          // Left and right can be trivially merged by taking operands of VL[0]
          // The masks will be concatenated
          OperandIndices dummyIndices;
          ShuffleCache.emplace_back(VL, ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask, dummyIndices, nullptr);
          newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode FirstOp0_FirstOp1_ConcatenateMask.\n");
          return;
        }

        Constant *MergedOp1 = mergeIfConstantVectors(Right);
        if (MergedOp1 != nullptr) {
          // Left is merged by taking operand 0 from VL[0]
          // Right is merged by taking non-undefined elements from each lane
          OperandIndices dummyIndices;
          ShuffleCache.emplace_back(VL, ShuffleBundleDecision::FirstOp0_MergeOp1_ConcatenateMask, dummyIndices, MergedOp1);
          newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
          LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode "
                                "FirstOp0_MergeOp1_ConcatentateMask.\n");
          LLVM_DEBUG(dbgs() << "Revec:   MergedOp1: " << *(ShuffleCache.back().Op1Value) << "\n");
          LLVM_DEBUG(dbgs() << "Revec:   Mask: " << *(ShuffleCache.back().Mask) << "\n");
          return;
        }
      }

#if 0
      // TODO: Add getEntryCost and vectorizeTree code for this special case
      if (allConstant(Right) && VL.size() == 4) {
        if (Left[0] == Left[1] && Left[2] == Left[3]) {
          SmallVector<Value *, 2> Op1_01, Op1_23;
          Op1_01.push_back(Right[0]);
          Op1_01.push_back(Right[1]);
          Op1_23.push_back(Right[2]);
          Op1_23.push_back(Right[3]);

          Constant *MergedOp1VL01 = mergeIfConstantVectors(Op1_01);
          Constant *MergedOp1VL23 = mergeIfConstantVectors(Op1_23);

          if (MergedOp1VL01 && MergedOp1VL23) {
            Constant *MergedOp1 = concatenateTwoConstantVectors(MergedOp1VL01, MergedOp1VL23);

            if (MergedOp1) {
              OperandIndices dummyIndices;
              ShuffleCache.emplace_back(VL, ShuffleBundleDecision::FirstThirdOp0_DoubleMergeOp1, dummyIndices, MergedOp1);
                newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
                LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode FirstThirdOp0_DoubleMergeOp1.\n");
                return;
              }
            }
          }
        }
#endif

      if (allConstant(Right) || allConstant(Left) || isSplat(Masks) || isSplat(Left)) {
        // Widen operands vertically, and merge shuffle masks
        //
        // Example if isSplat(Masks) (from Simd library, avx2_interleave):
        //   %shuffle.i.i43.i55.us.1 = shufflevector <16 x i16> %47, <16 x i16> %48, <16 x i32> <i32 0, i32 16, i32 1, i32 17, i32 2, i32 18, i32 3, i32 19, i32 8, i32 24, i32 9, i32 25, i32 10, i32 26, i32 11, i32 27>
        //   %shuffle.i.i42.i57.us.1 = shufflevector <16 x i16> %50, <16 x i16> %51, <16 x i32> <i32 0, i32 16, i32 1, i32 17, i32 2, i32 18, i32 3, i32 19, i32 8, i32 24, i32 9, i32 25, i32 10, i32 26, i32 11, i32 27>

        OperandIndices dummyIndices;
        ShuffleCache.emplace_back(VL, ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask, dummyIndices, nullptr);
        newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
        LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode IndexOp0_IndexOp1_WidenMask (lane widening).\n");
        for (Value *val : VL) {
          LLVM_DEBUG(dbgs() << "Revec:   " << val << " = " << *val << "\n");
        }
        assert(ShuffleCache.back().matchesBundle(VL) && "Could not find bundle that was just inserted");
        assert((getShuffleBundleDecision(VL).MergeMode == ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask) && "Failed to properly find just inserted shuffle bundle");
        LLVM_DEBUG(dbgs() << "Revc: buildTree_rec: After adding lane widen shuffle decision, ShuffleCache contains:\n");
        for (unsigned i = 0; i < ShuffleCache.size(); ++i) {
          ShuffleBundleDecision d = ShuffleCache[i];
          LLVM_DEBUG(dbgs() << "Revec:   MM: " << d.MergeMode << ", VL0: " << d.VL[0] << " = " << *d.VL[0] << "\n");
        }
        buildTree_rec(Left, Depth + 1, UserTreeIdx);
        buildTree_rec(Right, Depth + 1, UserTreeIdx);
        return;
      }

      if (VL.size() == 2) {
      	Left.clear();
      	Right.clear();

      	ShuffleVectorInst *I = cast<ShuffleVectorInst>(VL[0]);
      	Left.push_back(I->getOperand(0));
      	Right.push_back(I->getOperand(1));

      	I = cast<ShuffleVectorInst>(VL[1]);
      	Left.push_back(I->getOperand(1));
      	Right.push_back(I->getOperand(0));

      	if (isSplat(Left) && isSplat(Right)) {
      		OperandIndices dummyIndices;
      		ShuffleCache.emplace_back(VL, ShuffleBundleDecision::Diagonal_VF2, dummyIndices, nullptr);
      		newTreeEntry(VL, true, UserTreeIdx, ReuseShuffleIndices);
      		LLVM_DEBUG(dbgs() << "Revec: added a ShuffleVector op for mode Diagonal_VF2.\n");
      		return;
      	}
      }

      // Default to a gather if we haven't encountered a special case.
      // TODO: Search recursively for optimal bundle indexes, with backtracking
      LLVM_DEBUG(dbgs() << "Revec: non-vectorizable ShuffleVector bundle.\n");
      for (Value *val : VL) {
        LLVM_DEBUG(dbgs() << "   " << *val << "\n");
      }

      printf("Revec: non-veectorizable ShuffleVector bundle.\n");
      for (Value *val : VL) {
        outs() << *val << "\n";
      }

      OperandIndices dummyIndices;
      ShuffleCache.emplace_back(VL, ShuffleBundleDecision::Gather, dummyIndices, nullptr);
      BS.cancelScheduling(VL, VL0);
      newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
      return;
    }
    default:
      BS.cancelScheduling(VL, VL0);
      newTreeEntry(VL, false, UserTreeIdx, ReuseShuffleIndices);
      LLVM_DEBUG(dbgs() << "Revec: Gathering unknown instruction.\n");
      return;
  }
}

unsigned BoUpSLP::canMapToVector(Type *T, const DataLayout &DL) const {
  unsigned N;
  Type *EltTy;
  auto *ST = dyn_cast<StructType>(T);
  if (ST) {
    N = ST->getNumElements();
    EltTy = *ST->element_begin();
  } else {
    N = cast<ArrayType>(T)->getNumElements();
    EltTy = cast<ArrayType>(T)->getElementType();
  }
  if (!isValidElementType(EltTy) || !isNarrowVectorType(T))
    return 0;
  uint64_t VTSize = DL.getTypeStoreSizeInBits(getVectorType(EltTy, N));
  if (VTSize < MinVecRegSize || VTSize > MaxVecRegSize || VTSize != DL.getTypeStoreSizeInBits(T))
    return 0;
  if (ST) {
    // Check that struct is homogeneous.
    for (const auto *Ty : ST->elements())
      if (Ty != EltTy)
        return 0;
  }
  return N;
}

bool BoUpSLP::canReuseExtract(ArrayRef<Value *> VL, Value *OpValue) const {
  Instruction *E0 = cast<Instruction>(OpValue);
  assert(E0->getOpcode() == Instruction::ExtractElement ||
         E0->getOpcode() == Instruction::ExtractValue);
  assert((E0->getOpcode() == getSameOpcode(VL).Opcode) && "Invalid opcode");
  // Check if all of the extracts come from the same vector and from the
  // correct offset.
  Value *Vec = E0->getOperand(0);

  // We have to extract from a vector/aggregate with the same number of elements.
  unsigned NElts;
  if (E0->getOpcode() == Instruction::ExtractValue) {
    const DataLayout &DL = E0->getModule()->getDataLayout();
    NElts = canMapToVector(Vec->getType(), DL);
    if (!NElts)
      return false;
    // Check if load can be rewritten as load of vector.
    LoadInst *LI = dyn_cast<LoadInst>(Vec);
    if (!LI || !LI->isSimple() || !LI->hasNUses(VL.size()))
      return false;
  } else {
    NElts = Vec->getType()->getVectorNumElements();
  }

  if (NElts != VL.size())
    return false;

  // Check that all of the indices extract from the correct offset.
  for (unsigned I = 0, E = VL.size(); I < E; ++I) {
    Instruction *Inst = cast<Instruction>(VL[I]);
    if (!matchExtractIndex(Inst, I, Inst->getOpcode()))
      return false;
    if (Inst->getOperand(0) != Vec)
      return false;
  }

  return true;
}

bool BoUpSLP::areAllUsersVectorized(Instruction *I) const {
  return I->hasOneUse() ||
         std::all_of(I->user_begin(), I->user_end(), [this](User *U) {
           return ScalarToTreeEntry.count(U) > 0;
         });
}

int BoUpSLP::getShuffleCost(VectorType *Op0Ty, VectorType *Op1Ty, Constant *Mask) {
  assert(Op0Ty == Op1Ty && "Source operands of shufflevector must be the same");

  Type *Ty = Mask->getType();

  unsigned NumSourceElts = Op0Ty->getVectorNumElements();
  unsigned NumMaskElts = Ty->getVectorNumElements();

  LLVM_DEBUG(dbgs() << "Revec: Getting cost of shuffle with source types " << *Op0Ty << " , " << *Op1Ty << ", mask " << *Mask << "\n");

	// Cost is unknown is the shuffle vector changes length
	// TODO: Identify and add costs for insert/extract subvector, etc.
  if (NumSourceElts != NumMaskElts) {
    LLVM_DEBUG(dbgs() << "Revec:   Changes length\n");
    return NumMaskElts > 32 ? 2 : 1;
  }

  if (ShuffleVectorInst::isIdentityMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Identity mask\n");
    return 0;
  }

  if (ShuffleVectorInst::isReverseMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Reverse mask\n");
    return TTI->getShuffleCost(TargetTransformInfo::SK_Reverse, Ty, 0, nullptr);
  }

  if (ShuffleVectorInst::isSelectMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Select mask\n");
    return TTI->getShuffleCost(TargetTransformInfo::SK_Select, Ty, 0, nullptr);
  }

  if (ShuffleVectorInst::isTransposeMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Transpose mask\n");
    return TTI->getShuffleCost(TargetTransformInfo::SK_Transpose, Ty, 0, nullptr);
  }

  if (ShuffleVectorInst::isZeroEltSplatMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Zero elt splat mask\n");
    return TTI->getShuffleCost(TargetTransformInfo::SK_Broadcast, Ty, 0, nullptr);
  }

  if (ShuffleVectorInst::isSingleSourceMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Single source mask\n");
    return TTI->getShuffleCost(TargetTransformInfo::SK_PermuteSingleSrc, Ty, 0, nullptr);
  }

  if (isUnpackShuffle(Op0Ty, Mask, false, true) || isUnpackShuffle(Op0Ty, Mask, true, true)) {
    LLVM_DEBUG(dbgs() << "Revec:   Unary unpack mask\n");
    // TODO: Better estimate cost than using the PermuteSingleSrc cost
    return TTI->getShuffleCost(TargetTransformInfo::SK_PermuteSingleSrc, Ty, 0, nullptr);
  }

  if (isUnpackShuffle(Op0Ty, Mask, false, false) || isUnpackShuffle(Op0Ty, Mask, true, false)) {
    LLVM_DEBUG(dbgs() << "Revec:   Binary unpack mask\n");
    // TODO: Get cost with TTI->getOpcodeCost rather than this heuristic
    // Estimate the cost of an unpack as 3 for 128 bit unpacks, 4 for 256 bit, and 5 for 256 bit
    return Log2_32(Op0Ty->getPrimitiveSizeInBits()) - 4;
  }

  if (isAlternatingShuffleMask(Mask)) {
    LLVM_DEBUG(dbgs() << "Revec:   Alternating mask\n");
    return Log2_32(Op0Ty->getPrimitiveSizeInBits()) - 4;
  }

  LLVM_DEBUG(dbgs() << "Revec:   Permute two src\n");
  return TTI->getShuffleCost(TargetTransformInfo::SK_PermuteTwoSrc, Ty, 0, nullptr);
}

int BoUpSLP::getEntryCost(TreeEntry *E) {
  ArrayRef<Value*> VL = E->Scalars;

  Type *ElementTy = VL[0]->getType();
  if (StoreInst *SI = dyn_cast<StoreInst>(VL[0]))
    ElementTy = SI->getValueOperand()->getType();
  else if (CmpInst *CI = dyn_cast<CmpInst>(VL[0]))
    ElementTy = CI->getOperand(0)->getType();

  VectorType *VecTy = getVectorType(ElementTy, VL.size());
  unsigned ReuseShuffleNumbers = E->ReuseShuffleIndices.size();
  bool NeedToShuffleReuses = !E->ReuseShuffleIndices.empty();
  int ReuseShuffleCost = 0;
  if (NeedToShuffleReuses) {
    ReuseShuffleCost =
        TTI->getShuffleCost(TargetTransformInfo::SK_PermuteSingleSrc, VecTy);
  }
  // TODO: Check that this gather cost is not too aggressive
  if (E->NeedToGather) {
    if (allConstant(VL)) {
      LLVM_DEBUG(dbgs() << "Revec: Gather all constant VL, 0 cost entry\n");
      return 0;
    }

    if (isSplat(VL)) {
      int splatCost = 0;
      for (unsigned i = 0; i < VL.size(); ++i) {
        int startIndex = i * ElementTy->getVectorNumElements();
        splatCost += TTI->getShuffleCost(TargetTransformInfo::SK_InsertSubvector, VecTy, startIndex, ElementTy);
      }
      LLVM_DEBUG(dbgs() << "Revec: Gather splat VL, cost " << ReuseShuffleCost << " + " << splatCost << "\n");
      return ReuseShuffleCost + splatCost;
    }

    if (getSameOpcode(VL).Opcode == Instruction::ExtractElement &&
        allSameType(VL) && allSameBlock(VL)) {
      assert(false && "getEntryCost asked to estimate cost of gather of ExtractElement returns, which are scalars, not vectors");
    }

    // TODO: Update getGatherCost
    int GatherCost = getGatherCost(VL);
    LLVM_DEBUG(dbgs() << "Revec: Gather VL, cost " << ReuseShuffleCost << " + " << GatherCost << "\n");
    return ReuseShuffleCost + GatherCost;
  }

  InstructionsState S = getSameOpcode(VL);
  assert(S.Opcode && allSameType(VL) && allSameBlock(VL) && "Invalid VL");
  Instruction *VL0 = cast<Instruction>(S.OpValue);
  unsigned ShuffleOrOp = S.IsAltShuffle ?
               (unsigned) Instruction::ShuffleVector : S.Opcode;
  switch (ShuffleOrOp) {
    case Instruction::PHI:
      return 0;

    case Instruction::ExtractValue:
    case Instruction::ExtractElement: {
      // TODO: Support ExtractValue that returns a vector type
      assert(false && "ExtractElement and ExtractValue instructions should not appear in bundles to be vectorized, as they produce scalars");
      return 0;
    }
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::SIToFP:
    case Instruction::UIToFP:
    case Instruction::Trunc:
    case Instruction::FPTrunc:
    case Instruction::BitCast: {
      Type *SrcTy = VL0->getOperand(0)->getType();
      // TODO: Update reuse shuffle
      if (NeedToShuffleReuses) {
        ReuseShuffleCost -=
            (ReuseShuffleNumbers - VL.size()) *
            TTI->getCastInstrCost(S.Opcode, ElementTy, SrcTy, VL0);
      }

      // Calculate the cost of this instruction.
      int NarrowVecCost = VL.size() *
          TTI->getCastInstrCost(VL0->getOpcode(), VL0->getType(), SrcTy, VL0);

      // The revectorizer pass does not demote the bitwidth of value elements,
      // always add this cost
      VectorType *FusedSrcVecTy = getVectorType(SrcTy, VL.size());
      int FusedVecCost = TTI->getCastInstrCost(VL0->getOpcode(), VecTy, FusedSrcVecTy, VL0);

      return ReuseShuffleCost + FusedVecCost - NarrowVecCost;
    }
    /* Evaluate the cost of a fusion similar to:
     * <n x i1> %a = icmp eq <n x ty> %A, %B
     * <n x i1> %b = icmp eq <n x ty> %C, %D
     *   ==>
     * <2n x i1> %ab = icmp eq <2n x ty> %AC, %BD
     * where VecTy is <2n x ty> and ElementTy is <n x ty>
     */
    case Instruction::FCmp:
    case Instruction::ICmp:
    case Instruction::Select: {
      Type *narrowMaskTy = (ShuffleOrOp == Instruction::Select)
        // Select instructions have a condition vector as an operand
        ? VL0->getOperand(0)->getType()
        // Create a mask for FCmp and ICmp instructions, as there is no condition vector
        : VectorType::get(Builder.getInt1Ty(), VL0->getType()->getVectorNumElements());

      int NarrowVecCost = VL.size() *
          TTI->getCmpSelInstrCost(S.Opcode, ElementTy, narrowMaskTy, VL0);

      if (NeedToShuffleReuses) {
        // TODO: Is ReuseShuffleNumbers always greater than VL.size()?
        //       Both are unsigned values, being subtracted!
        ReuseShuffleCost -= (ReuseShuffleNumbers - VL.size()) *
                            TTI->getCmpSelInstrCost(S.Opcode, ElementTy, narrowMaskTy, VL0);
      }

      // Get the type of the condition vector for the fused instruction
      VectorType *wideMaskTy = VectorType::get(Builder.getInt1Ty(), getFusedSize(VL));
      int FusedVecCost = TTI->getCmpSelInstrCost(S.Opcode, VecTy, wideMaskTy, VL0);

      LLVM_DEBUG(dbgs() << "Revec: FCmp/ICmp/Select cost " << ReuseShuffleCost + FusedVecCost - NarrowVecCost
            << " (" << ReuseShuffleCost << " + " << FusedVecCost  << " - " << NarrowVecCost << "). Bundle:\n");
#ifndef NDEBUG
      for (Value *val : VL)
          LLVM_DEBUG(dbgs() << "Revec:    " << *val << "\n");
#endif

      return ReuseShuffleCost + FusedVecCost - NarrowVecCost;
    }
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor: {
      // Certain instructions can be cheaper to vectorize if they have a
      // constant second vector operand.
      const TargetTransformInfo::OperandValueKind Op1VK =
          TargetTransformInfo::OK_AnyValue;
      TargetTransformInfo::OperandValueKind Op2VK =
          TargetTransformInfo::OK_UniformConstantValue;
      const TargetTransformInfo::OperandValueProperties Op1VP =
          TargetTransformInfo::OP_None;
      const TargetTransformInfo::OperandValueProperties Op2VP =
          TargetTransformInfo::OP_None;

      // If all parallel operands are exactly the same ConstantVector then set the
      // operand kind to OK_UniformConstantValue.
      // If instead not all operands are constants, then set the operand kind
      // to OK_AnyValue. If all operands are constants but not the same,
      // then set the operand kind to OK_NonUniformConstantValue.
      ConstantVector *CVec = nullptr;
      for (unsigned i = 0; i < VL.size(); ++i) {
        const Instruction *I = cast<Instruction>(VL[i]);
        if (!isa<ConstantVector>(I->getOperand(1))) {
          Op2VK = TargetTransformInfo::OK_AnyValue;
          break;
        }
        if (i == 0) {
          CVec = cast<ConstantVector>(I->getOperand(1));
          continue;
        }
        if (Op2VK == TargetTransformInfo::OK_UniformConstantValue &&
            CVec != cast<ConstantVector>(I->getOperand(1)))
          Op2VK = TargetTransformInfo::OK_NonUniformConstantValue;
      }

      SmallVector<const Value *, 4> Operands(VL0->operand_values());
      if (NeedToShuffleReuses) {
        ReuseShuffleCost -=
            (ReuseShuffleNumbers - VL.size()) *
            TTI->getArithmeticInstrCost(S.Opcode, ElementTy, Op1VK, Op2VK, Op1VP,
                                        Op2VP, Operands);
      }

      int NarrowVecCost = VL.size() *
          TTI->getArithmeticInstrCost(S.Opcode, ElementTy, Op1VK, Op2VK, Op1VP,
                                      Op2VP, Operands);
      int FusedVecCost = TTI->getArithmeticInstrCost(S.Opcode, VecTy, Op1VK, Op2VK,
                                                Op1VP, Op2VP, Operands);
      return ReuseShuffleCost + FusedVecCost - NarrowVecCost;
    }
    case Instruction::GetElementPtr: {
      const TargetTransformInfo::OperandValueKind Op1VK =
          TargetTransformInfo::OK_AnyValue;
      const TargetTransformInfo::OperandValueKind Op2VK =
          TargetTransformInfo::OK_UniformConstantValue;

      if (NeedToShuffleReuses) {
        ReuseShuffleCost -= (ReuseShuffleNumbers - VL.size()) *
                            TTI->getArithmeticInstrCost(Instruction::Add,
                                                        ElementTy, Op1VK, Op2VK);
      }

      int NarrowVecCost = VL.size() *
          TTI->getArithmeticInstrCost(Instruction::Add, ElementTy, Op1VK, Op2VK);
      int FusedVecCost =
          TTI->getArithmeticInstrCost(Instruction::Add, VecTy, Op1VK, Op2VK);
      return ReuseShuffleCost + FusedVecCost - NarrowVecCost;
    }
    case Instruction::Load: {
      // Cost of wide load - cost of scalar loads.
      unsigned alignment = dyn_cast<LoadInst>(VL0)->getAlignment();
      if (NeedToShuffleReuses) {
        ReuseShuffleCost -= (ReuseShuffleNumbers - VL.size()) *
                            TTI->getMemoryOpCost(Instruction::Load, ElementTy,
                                                 alignment, 0, VL0);
      }

      int NarrowVecLdCost = VL.size() *
          TTI->getMemoryOpCost(Instruction::Load, ElementTy, alignment, 0, VL0);
      int FusedVecLdCost =
          TTI->getMemoryOpCost(Instruction::Load, VecTy, alignment, 0, VL0);
      if (!isConsecutiveAccess(VL[0], VL[1], *DL, *SE)) {
        FusedVecLdCost += TTI->getShuffleCost(
            TargetTransformInfo::SK_PermuteSingleSrc, VecTy);
      }
      return ReuseShuffleCost + FusedVecLdCost - NarrowVecLdCost;
    }
    case Instruction::Store: {
      // We know that we can merge the stores. Calculate the cost.
      unsigned alignment = dyn_cast<StoreInst>(VL0)->getAlignment();
      if (NeedToShuffleReuses) {
        ReuseShuffleCost -= (ReuseShuffleNumbers - VL.size()) *
                            TTI->getMemoryOpCost(Instruction::Store, ElementTy,
                                                 alignment, 0, VL0);
      }

      int NarrowVecStCost = VL.size() *
          TTI->getMemoryOpCost(Instruction::Store, ElementTy, alignment, 0, VL0);
      int FusedVecStCost = TTI->getMemoryOpCost(Instruction::Store,
                                                VecTy, alignment, 0, VL0);
      return ReuseShuffleCost + FusedVecStCost - NarrowVecStCost;
    }
    case Instruction::Call: {
      // Calculate the cost of the scalar and vector calls.
      CallInst *CI = cast<CallInst>(VL0);

      // Find the ID of this intrinsic
      Intrinsic::ID IID = getIntrinsicByCall(CI);
      // TODO: Check that buildTree_rec does not create bundles with non-intrinsics
      assert(IID != Intrinsic::not_intrinsic);

      LLVM_DEBUG(dbgs() << "Revec: Getting cost of call bundle" << "\n");
      LLVM_DEBUG(dbgs() << "Revec:     Starts with: " << *CI << "\n");
      LLVM_DEBUG(dbgs() << "Revec:     ElementTy: " << *ElementTy << "\n");
      LLVM_DEBUG(dbgs() << "Revec:     VecTy: " << *VecTy << "\n");

      SmallVector<Type*, 4> NarrowArgTys;
      for (unsigned op = 0, opc = CI->getNumArgOperands(); op != opc; ++op)
        NarrowArgTys.push_back(CI->getArgOperand(op)->getType());

      FastMathFlags FMF;
      if (auto *FPMO = dyn_cast<FPMathOperator>(CI))
        FMF = FPMO->getFastMathFlags();

      int NarrowVecCallCost = VL.size() *
          TTI->getIntrinsicInstrCost(IID, ElementTy, NarrowArgTys, FMF);

      LLVM_DEBUG(dbgs() << "Revec: Cost of " << VL.size() << " narrow calls like " << *CI << ", with intrinsic "
												<< (llvm::Intrinsic::isOverloaded(IID) ? std::to_string(IID) : Intrinsic::getName(IID).str())
												<< ", type " << *ElementTy << " = " << NarrowVecCallCost << "\n");

      if (NeedToShuffleReuses) {
        ReuseShuffleCost -=
            (ReuseShuffleNumbers - VL.size()) *
            TTI->getIntrinsicInstrCost(IID, ElementTy, NarrowArgTys, FMF);
      }

      // Find widened intrinsic ID
      const auto& target = getWidenedIntrinsic(IID, E->Scalars.size());
      int VF = target.first;
      Intrinsic::ID alt = target.second;
      assert(alt != Intrinsic::not_intrinsic && VF > 0 && "Attempting to compute cost of intrinsic call entry, but no wide equivalence found.");
      assert(VF == static_cast<long>(VL.size()) && "Cannot bundle intrinsic calls, where known widening factor does not match bundle size.");

      Optional<unsigned> preservedOperand = getPreservedOperand(IID);

      // Find widened vector return type
      Type *wideReturnTy = getVectorType(CI->getType(), VL.size());

      // Find widened vector argument types
      SmallVector<Type *, 4> WideArgTys;
      for (unsigned op = 0, opc = CI->getNumArgOperands(); op != opc; ++op) {
        Type *narrowArgTy = CI->getArgOperand(op)->getType();

        if (preservedOperand.hasValue() && op == preservedOperand.getValue()) {
          WideArgTys.push_back(narrowArgTy);
        } else {
          // Assume vertical concatenation of arguments in this bundle
          Type *wideArgTy = getVectorType(narrowArgTy, VL.size());
          WideArgTys.push_back(wideArgTy);
        }
      }

      int FusedVecCallCost =
          TTI->getIntrinsicInstrCost(alt, wideReturnTy, WideArgTys, FMF);
      if (FusedVecCallCost == NarrowVecCallCost) {
        // Discount fused cost by 0.0625 * VL.size() * narrow inst cost
        FusedVecCallCost = std::max(0, FusedVecCallCost - NarrowVecCallCost / 16);
      }

      LLVM_DEBUG(dbgs() << "Revec: Cost of widened call with intrinsic "
												<< (llvm::Intrinsic::isOverloaded(alt) ? std::to_string(alt) : Intrinsic::getName(alt).str())
												<< ", type " << *wideReturnTy << " = " << FusedVecCallCost << "\n");

      LLVM_DEBUG(dbgs() << "Revec: Calculated call cost " << ReuseShuffleCost << " + " << FusedVecCallCost << " - " << NarrowVecCallCost << " for calls:\n");
      for (Value *val : VL) {
          LLVM_DEBUG(dbgs() << "Revec:   " << *val << "\n");
      }
      return ReuseShuffleCost - 1;
    }
    case Instruction::ShuffleVector: {
      int ReuseShuffleCost = 0;
      if (NeedToShuffleReuses) {
        for (unsigned Idx : E->ReuseShuffleIndices) {
          Instruction *I = cast<Instruction>(VL[Idx]);
          ReuseShuffleCost -= getInstructionThroughput(I, 1);
        }
        for (Value *V : VL) {
          Instruction *I = cast<Instruction>(V);
          ReuseShuffleCost += getInstructionThroughput(I, 1);
        }
      }

      if (S.IsAltShuffle) {
        LLVM_DEBUG(dbgs() << "Revec: Getting cost of alt shuffle of binary operations");
        assert(Instruction::isBinaryOp(S.Opcode) &&
               Instruction::isBinaryOp(getAltOpcode(S.Opcode)) &&
               "Invalid Shuffle Vector Operand");

        int NarrowVecCost = 0;
        for (Value *i : VL) {
          Instruction *I = cast<Instruction>(i);
          assert(sameOpcodeOrAlt(S.Opcode, getAltOpcode(S.Opcode),
                                 I->getOpcode()) &&
                 "Unexpected main/alternate opcode");
          NarrowVecCost += getInstructionThroughput(I, 1);
        }

        // VecCost is equal to sum of the cost of creating 2 vectors
        // and the cost of creating shuffle.
        int VecCost = TTI->getArithmeticInstrCost(S.Opcode, VecTy) +
                      TTI->getArithmeticInstrCost(S.Opcode, VecTy) +
                      TTI->getShuffleCost(TargetTransformInfo::SK_Select, VecTy, 0);
        return ReuseShuffleCost + VecCost - NarrowVecCost;
      }

      LLVM_DEBUG(dbgs() << "Revec: Getting cost of shuffle vector bundle:\n");
      for (Value *val : VL) {
        LLVM_DEBUG(dbgs() << "Revec:   " << *val << "\n");
      }
      // assert(getBundleDecision(VL).hasValue() && "Attempting to get cost of non-vectorized shuffle bundle");

      int NarrowVecCost = 0;
      SmallVector<Constant *, 4> Masks;
      for (Value *i : VL) {
        ShuffleVectorInst *I = cast<ShuffleVectorInst>(i);
        Masks.push_back(I->getMask());

        // Get the cost of this shuffle
        NarrowVecCost += getInstructionThroughput(I, 1);
      }

      // TODO: Speculatively lower shuffle IR nodes to properly determine cost. There are many low-cost shuffle targets.

      LLVM_DEBUG(dbgs() << "Revec: getEntryCost: Prior to getShuffleBundle, ShuffleCache contents:\n");
      for (unsigned i = 0; i < ShuffleCache.size(); ++i) {
        ShuffleBundleDecision d = ShuffleCache[i];
        LLVM_DEBUG(dbgs() << "Revec:   MM: " << d.MergeMode << ", VL0: " << d.VL[0] << " = " << *d.VL[0] << "\n");
      }

      const auto& decision = getShuffleBundleDecision(VL);

      LLVM_DEBUG(dbgs() << "Revec: getEntryCost: Found decision with merge mode " << decision.MergeMode << " and bundle: \n");
      for (Value *val : decision.VL) {
        LLVM_DEBUG(dbgs() << "Revec:   " << val << " = " << *val << "\n");
      }

      int FusedVecCost = 0;
      switch (decision.MergeMode) {
        case ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask: {
					assert(decision.Mask != nullptr && "Need mask to estimate cost");
          VectorType *WideOp0Ty = getVectorType(VL0->getOperand(0)->getType(), VL.size());
          VectorType *WideOp1Ty = getVectorType(VL0->getOperand(1)->getType(), VL.size());
          if (isSplat(Masks))
            // Catch-all special case special shuffles like psrdq
            FusedVecCost = Log2_32(WideOp0Ty->getPrimitiveSizeInBits()) - 4;
          else
            FusedVecCost = getShuffleCost(WideOp0Ty, WideOp1Ty, decision.Mask);
          break;
        }
        case ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask:
        case ShuffleBundleDecision::FirstOp0_MergeOp1_ConcatenateMask:
        case ShuffleBundleDecision::Diagonal_VF2: {
					assert(decision.Mask != nullptr && "Need mask to estimate cost");
          VectorType *NarrowOp0Ty = cast<VectorType>(VL0->getOperand(0)->getType());
          VectorType *NarrowOp1Ty = cast<VectorType>(VL0->getOperand(0)->getType());
          FusedVecCost = getShuffleCost(NarrowOp0Ty, NarrowOp1Ty, decision.Mask);
          break;
        }
        case ShuffleBundleDecision::FirstOp0:
          FusedVecCost = 0;
          break;
        case ShuffleBundleDecision::Gather:
          FusedVecCost = NarrowVecCost + getGatherCost(VL);
          break;
        default:
          llvm_unreachable("Unrecognized decision type");
      }

      LLVM_DEBUG(dbgs() << "Revec: Calculated cost " << ReuseShuffleCost << " + " << FusedVecCost << " - " << NarrowVecCost << " for shuffle merge mode " << decision.MergeMode << "\n");
      for (Value *val : VL) {
          LLVM_DEBUG(dbgs() << "Revec:   " << *val << "\n");
      }

      return ReuseShuffleCost + FusedVecCost - NarrowVecCost;
    }
    default:
      llvm_unreachable("Unknown instruction");
  }
}

bool BoUpSLP::isFullyVectorizableTinyTree() {
  LLVM_DEBUG(dbgs() << "Revec: Check whether the tree with height " <<
        VectorizableTree.size() << " is fully vectorizable .\n");

  // We only handle trees of heights 1 and 2.
  if (VectorizableTree.size() == 1 && !VectorizableTree[0].NeedToGather)
    return true;

  if (VectorizableTree.size() != 2)
    return false;

  // Handle splat and all-constants stores.
  if (!VectorizableTree[0].NeedToGather &&
      (allConstant(VectorizableTree[1].Scalars) ||
       isSplat(VectorizableTree[1].Scalars)))
    return true;

  // Gathering cost would be too much for tiny trees.
  if (VectorizableTree[0].NeedToGather || VectorizableTree[1].NeedToGather)
    return false;

  return true;
}

bool BoUpSLP::isTreeTinyAndNotFullyVectorizable() {
  // We can vectorize the tree if its size is greater than or equal to the
  // minimum size specified by the MinTreeSize command line option.
  if (VectorizableTree.size() >= MinTreeSize)
    return false;

  // If we have a tiny tree (a tree whose size is less than MinTreeSize), we
  // can vectorize it if we can prove it fully vectorizable.
  if (isFullyVectorizableTinyTree())
    return false;

  assert(VectorizableTree.empty()
             ? ExternalUses.empty()
             : true && "We shouldn't have any external users for empty vectorizable tree");

  // Otherwise, we can't vectorize the tree. It is both tiny and not fully
  // vectorizable.
  return true;
}

int BoUpSLP::getSpillCost() {
  // Walk from the bottom of the tree to the top, tracking which values are
  // live. When we see a call instruction that is not part of our tree,
  // query TTI to see if there is a cost to keeping values live over it
  // (for example, if spills and fills are required).
  unsigned BundleWidth = VectorizableTree.front().Scalars.size();
  int Cost = 0;

  SmallPtrSet<Instruction*, 4> LiveValues;
  Instruction *PrevInst = nullptr;

  for (const auto &N : VectorizableTree) {
    Instruction *Inst = dyn_cast<Instruction>(N.Scalars[0]);
    if (!Inst)
      continue;

    if (!PrevInst) {
      PrevInst = Inst;
      continue;
    }

    // Update LiveValues.
    LiveValues.erase(PrevInst);
    for (auto &J : PrevInst->operands()) {
      if (isa<Instruction>(&*J) && getTreeEntry(&*J))
        LiveValues.insert(cast<Instruction>(&*J));
    }

    LLVM_DEBUG(
      dbgs() << "Revec: #LV: " << LiveValues.size();
      for (auto *X : LiveValues)
        dbgs() << " " << X->getName();
      dbgs() << ", Looking at ";
      Inst->dump();
      );

    // Now find the sequence of instructions between PrevInst and Inst.
    BasicBlock::reverse_iterator InstIt = ++Inst->getIterator().getReverse(),
                                 PrevInstIt =
                                     PrevInst->getIterator().getReverse();
    while (InstIt != PrevInstIt) {
      if (PrevInstIt == PrevInst->getParent()->rend()) {
        PrevInstIt = Inst->getParent()->rbegin();
        continue;
      }

      if (isa<CallInst>(&*PrevInstIt) && &*PrevInstIt != PrevInst) {
        SmallVector<Type*, 4> V;
        for (auto *II : LiveValues) {
          // Widen II's type by factor BundleWidth
          auto *narrowVectorTy = II->getType();
          assert(narrowVectorTy->isVectorTy() && "Trying to fuse scalars");
          V.push_back(getVectorType(narrowVectorTy, BundleWidth));
        }
        Cost += TTI->getCostOfKeepingLiveOverCall(V);
      }

      ++PrevInstIt;
    }

    PrevInst = Inst;
  }

  return Cost;
}

int BoUpSLP::getTreeCost() {
  LLVM_DEBUG(dbgs() << "Revec: Calculating cost for tree of size " <<
        VectorizableTree.size() << ".\n");

  unsigned BundleWidth = VectorizableTree[0].Scalars.size();

  int EntryCosts = 0;
  for (unsigned I = 0, E = VectorizableTree.size(); I < E; ++I) {
    TreeEntry &TE = VectorizableTree[I];

    // We create duplicate tree entries for gather sequences that have multiple
    // uses. However, we should not compute the cost of duplicate sequences.
    // For example, if we have a build vector (i.e., insertelement sequence)
    // that is used by more than one vector instruction, we only need to
    // compute the cost of the insertelement instructions once. The redundent
    // instructions will be eliminated by CSE.
    //
    // We should consider not creating duplicate tree entries for gather
    // sequences, and instead add additional edges to the tree representing
    // their uses. Since such an approach results in fewer total entries,
    // existing heuristics based on tree size may yield different results.
    //
    if (TE.NeedToGather &&
        std::any_of(std::next(VectorizableTree.begin(), I + 1),
                    VectorizableTree.end(), [TE](TreeEntry &Entry) {
                      return Entry.NeedToGather && Entry.isSame(TE.Scalars);
                    }))
      continue;

    int C = getEntryCost(&TE);
    TE.CostCached = C;
    LLVM_DEBUG(dbgs() << "Revec: Adding cost " << C << " for bundle that starts with "
                 << *TE.Scalars[0] << ".\n");
    EntryCosts += C;
  }

  SmallSet<Value *, 16> ExtractCostCalculated;
  int ExtractCost = 0;
  for (ExternalUser &EU : ExternalUses) {
    // We only add extract cost once for the same scalar.
    if (!ExtractCostCalculated.insert(EU.Scalar).second)
      continue;

    // Uses by ephemeral values are free (because the ephemeral value will be
    // removed prior to code generation, and so the extraction will be
    // removed as well).
    if (EphValues.count(EU.User))
      continue;

    auto *narrowVecTy = EU.Scalar->getType();
    auto *wideVecTy = getVectorType(narrowVecTy, BundleWidth);
    ExtractCost += TTI->getShuffleCost(
        TargetTransformInfo::ShuffleKind::SK_ExtractSubvector, wideVecTy,
        EU.Lane * narrowVecTy->getVectorNumElements(), narrowVecTy);
  }

  int SpillCost = getSpillCost();

  int Cost = EntryCosts + SpillCost + ExtractCost;

  std::string Str;
  {
    raw_string_ostream OS(Str);
    OS << "Revec costs for " << F->getName()
       << ": Entries = " << EntryCosts << ", "
       << "Spill = " << SpillCost << ", "
       << "Extract = " << ExtractCost << ", "
       << "Total = " << Cost << ".\n";
  }
  LLVM_DEBUG(dbgs() << Str);
  
  if (WriteRevecTree)
    WriteGraph(this, "Revec" + F->getName(), false, Str);

  return Cost;
}

int BoUpSLP::getGatherCost(ArrayRef<Value *> VL) {
  unsigned size = VL.size();
  assert((isPowerOf2_32(size) && size > 1) && "Cannot estimate gather cost of VL not of size 2^i, i >= 1");

  int Cost = 0;
#if 1
  Type *narrowVecTy = VL[0]->getType();
  unsigned narrowNumElements = narrowVecTy->getVectorNumElements();
  VectorType *wideVecTy = getVectorType(narrowVecTy, size);
  for (unsigned i = 0; i < size; ++i) {
    assert((VL[i]->getType() == narrowVecTy) && "Mismatched bundle types to be gathered!");
    unsigned Idx = i * narrowNumElements;
    Cost += TTI->getShuffleCost(TargetTransformInfo::SK_InsertSubvector,
      wideVecTy, Idx, narrowVecTy);
  }
#else
  unsigned CurrentVF = 1;
  Type *Ty = VL[0]->getType();
  while (CurrentVF < VL.size()) {
    Ty = getVectorType(Ty, 2);
    CurrentVF *= 2;
    Cost += (size / CurrentVF) *
      TTI->getShuffleCost(TargetTransformInfo::SK_PermuteTwoSrc, Ty);
  }
#endif

  return Cost;
}

// Reorder commutative operations in alternate shuffle if the resulting vectors
// are consecutive loads. This would allow us to vectorize the tree.
// If we have something like-
// load a[0] - load b[0]
// load b[1] + load a[1]
// load a[2] - load b[2]
// load a[3] + load b[3]
// Reordering the second load b[1]  load a[1] would allow us to vectorize this
// code.
void BoUpSLP::reorderAltShuffleOperands(unsigned Opcode, ArrayRef<Value *> VL,
                                        SmallVectorImpl<Value *> &Left,
                                        SmallVectorImpl<Value *> &Right) {
  // Push left and right operands of binary operation into Left and Right
  unsigned AltOpcode = getAltOpcode(Opcode);
  (void)AltOpcode;
  for (Value *V : VL) {
    auto *I = cast<Instruction>(V);
    assert(sameOpcodeOrAlt(Opcode, AltOpcode, I->getOpcode()) &&
           "Incorrect instruction in vector");
    Left.push_back(I->getOperand(0));
    Right.push_back(I->getOperand(1));
  }

  // Reorder if we have a commutative operation and consecutive access
  // are on either side of the alternate instructions.
  for (unsigned j = 0; j < VL.size() - 1; ++j) {
    if (LoadInst *L = dyn_cast<LoadInst>(Left[j])) {
      if (LoadInst *L1 = dyn_cast<LoadInst>(Right[j + 1])) {
        Instruction *VL1 = cast<Instruction>(VL[j]);
        Instruction *VL2 = cast<Instruction>(VL[j + 1]);
        if (VL1->isCommutative() && isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j], Right[j]);
          continue;
        } else if (VL2->isCommutative() &&
                   isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j + 1], Right[j + 1]);
          continue;
        }
        // else unchanged
      }
    }
    if (LoadInst *L = dyn_cast<LoadInst>(Right[j])) {
      if (LoadInst *L1 = dyn_cast<LoadInst>(Left[j + 1])) {
        Instruction *VL1 = cast<Instruction>(VL[j]);
        Instruction *VL2 = cast<Instruction>(VL[j + 1]);
        if (VL1->isCommutative() && isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j], Right[j]);
          continue;
        } else if (VL2->isCommutative() &&
                   isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j + 1], Right[j + 1]);
          continue;
        }
        // else unchanged
      }
    }
  }
}

// Return true if I should be commuted before adding it's left and right
// operands to the arrays Left and Right.
//
// The vectorizer is trying to either have all elements one side being
// instruction with the same opcode to enable further vectorization, or having
// a splat to lower the vectorizing cost.
static bool shouldReorderOperands(
    int i, unsigned Opcode, Instruction &I, ArrayRef<Value *> Left,
    ArrayRef<Value *> Right, bool AllSameOpcodeLeft, bool AllSameOpcodeRight,
    bool SplatLeft, bool SplatRight, Value *&VLeft, Value *&VRight) {
  VLeft = I.getOperand(0);
  VRight = I.getOperand(1);
  // If we have "SplatRight", try to see if commuting is needed to preserve it.
  if (SplatRight) {
    if (VRight == Right[i - 1])
      // Preserve SplatRight
      return false;
    if (VLeft == Right[i - 1]) {
      // Commuting would preserve SplatRight, but we don't want to break
      // SplatLeft either, i.e. preserve the original order if possible.
      // (FIXME: why do we care?)
      if (SplatLeft && VLeft == Left[i - 1])
        return false;
      return true;
    }
  }
  // Symmetrically handle Right side.
  if (SplatLeft) {
    if (VLeft == Left[i - 1])
      // Preserve SplatLeft
      return false;
    if (VRight == Left[i - 1])
      return true;
  }

  Instruction *ILeft = dyn_cast<Instruction>(VLeft);
  Instruction *IRight = dyn_cast<Instruction>(VRight);

  // If we have "AllSameOpcodeRight", try to see if the left operands preserves
  // it and not the right, in this case we want to commute.
  if (AllSameOpcodeRight) {
    unsigned RightPrevOpcode = cast<Instruction>(Right[i - 1])->getOpcode();
    if (IRight && RightPrevOpcode == IRight->getOpcode())
      // Do not commute, a match on the right preserves AllSameOpcodeRight
      return false;
    if (ILeft && RightPrevOpcode == ILeft->getOpcode()) {
      // We have a match and may want to commute, but first check if there is
      // not also a match on the existing operands on the Left to preserve
      // AllSameOpcodeLeft, i.e. preserve the original order if possible.
      // (FIXME: why do we care?)
      if (AllSameOpcodeLeft && ILeft &&
          cast<Instruction>(Left[i - 1])->getOpcode() == ILeft->getOpcode())
        return false;
      return true;
    }
  }
  // Symmetrically handle Left side.
  if (AllSameOpcodeLeft) {
    unsigned LeftPrevOpcode = cast<Instruction>(Left[i - 1])->getOpcode();
    if (ILeft && LeftPrevOpcode == ILeft->getOpcode())
      return false;
    if (IRight && LeftPrevOpcode == IRight->getOpcode())
      return true;
  }
  return false;
}

void BoUpSLP::reorderInputsAccordingToOpcode(unsigned Opcode,
                                             ArrayRef<Value *> VL,
                                             SmallVectorImpl<Value *> &Left,
                                             SmallVectorImpl<Value *> &Right) {
  if (!VL.empty()) {
    // Peel the first iteration out of the loop since there's nothing
    // interesting to do anyway and it simplifies the checks in the loop.
    auto *I = cast<Instruction>(VL[0]);
    Value *VLeft = I->getOperand(0);
    Value *VRight = I->getOperand(1);
    if (!isa<Instruction>(VRight) && isa<Instruction>(VLeft))
      // Favor having instruction to the right. FIXME: why?
      std::swap(VLeft, VRight);
    Left.push_back(VLeft);
    Right.push_back(VRight);
  }

  // Keep track if we have instructions with all the same opcode on one side.
  bool AllSameOpcodeLeft = isa<Instruction>(Left[0]);
  bool AllSameOpcodeRight = isa<Instruction>(Right[0]);
  // Keep track if we have one side with all the same value (broadcast).
  bool SplatLeft = true;
  bool SplatRight = true;

  for (unsigned i = 1, e = VL.size(); i != e; ++i) {
    Instruction *I = cast<Instruction>(VL[i]);
    assert(((I->getOpcode() == Opcode && I->isCommutative()) ||
            (I->getOpcode() != Opcode && Instruction::isCommutative(Opcode))) &&
           "Can only process commutative instruction");
    // Commute to favor either a splat or maximizing having the same opcodes on
    // one side.
    Value *VLeft;
    Value *VRight;
    if (shouldReorderOperands(i, Opcode, *I, Left, Right, AllSameOpcodeLeft,
                              AllSameOpcodeRight, SplatLeft, SplatRight, VLeft,
                              VRight)) {
      Left.push_back(VRight);
      Right.push_back(VLeft);
    } else {
      Left.push_back(VLeft);
      Right.push_back(VRight);
    }
    // Update Splat* and AllSameOpcode* after the insertion.
    SplatRight = SplatRight && (Right[i - 1] == Right[i]);
    SplatLeft = SplatLeft && (Left[i - 1] == Left[i]);
    AllSameOpcodeLeft = AllSameOpcodeLeft && isa<Instruction>(Left[i]) &&
                        (cast<Instruction>(Left[i - 1])->getOpcode() ==
                         cast<Instruction>(Left[i])->getOpcode());
    AllSameOpcodeRight = AllSameOpcodeRight && isa<Instruction>(Right[i]) &&
                         (cast<Instruction>(Right[i - 1])->getOpcode() ==
                          cast<Instruction>(Right[i])->getOpcode());
  }

  // If one operand end up being broadcast, return this operand order.
  if (SplatRight || SplatLeft)
    return;

  // Finally check if we can get longer vectorizable chain by reordering
  // without breaking the good operand order detected above.
  // E.g. If we have something like-
  // load a[0]  load b[0]
  // load b[1]  load a[1]
  // load a[2]  load b[2]
  // load a[3]  load b[3]
  // Reordering the second load b[1]  load a[1] would allow us to vectorize
  // this code and we still retain AllSameOpcode property.
  // FIXME: This load reordering might break AllSameOpcode in some rare cases
  // such as-
  // add a[0],c[0]  load b[0]
  // add a[1],c[2]  load b[1]
  // b[2]           load b[2]
  // add a[3],c[3]  load b[3]
  for (unsigned j = 0; j < VL.size() - 1; ++j) {
    if (LoadInst *L = dyn_cast<LoadInst>(Left[j])) {
      if (LoadInst *L1 = dyn_cast<LoadInst>(Right[j + 1])) {
        if (isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j + 1], Right[j + 1]);
          continue;
        }
      }
    }
    if (LoadInst *L = dyn_cast<LoadInst>(Right[j])) {
      if (LoadInst *L1 = dyn_cast<LoadInst>(Left[j + 1])) {
        if (isConsecutiveAccess(L, L1, *DL, *SE)) {
          std::swap(Left[j + 1], Right[j + 1]);
          continue;
        }
      }
    }
    // else unchanged
  }
}

void BoUpSLP::setInsertPointAfterBundle(ArrayRef<Value *> VL, Value *OpValue) {
  // Get the basic block this bundle is in. All instructions in the bundle
  // should be in this block.
  auto *Front = cast<Instruction>(OpValue);
  auto *BB = Front->getParent();
  const unsigned Opcode = cast<Instruction>(OpValue)->getOpcode();
  const unsigned AltOpcode = getAltOpcode(Opcode);
  assert(llvm::all_of(make_range(VL.begin(), VL.end()), [=](Value *V) -> bool {
    return !sameOpcodeOrAlt(Opcode, AltOpcode,
                            cast<Instruction>(V)->getOpcode()) ||
           cast<Instruction>(V)->getParent() == BB;
  }));

  // The last instruction in the bundle in program order.
  Instruction *LastInst = nullptr;

  // Find the last instruction. The common case should be that BB has been
  // scheduled, and the last instruction is VL.back(). So we start with
  // VL.back() and iterate over schedule data until we reach the end of the
  // bundle. The end of the bundle is marked by null ScheduleData.
  if (BlocksSchedules.count(BB)) {
    auto *Bundle =
        BlocksSchedules[BB]->getScheduleData(isOneOf(OpValue, VL.back()));
    if (Bundle && Bundle->isPartOfBundle())
      for (; Bundle; Bundle = Bundle->NextInBundle)
        if (Bundle->OpValue == Bundle->Inst)
          LastInst = Bundle->Inst;
  }

  // LastInst can still be null at this point if there's either not an entry
  // for BB in BlocksSchedules or there's no ScheduleData available for
  // VL.back(). This can be the case if buildTree_rec aborts for various
  // reasons (e.g., the maximum recursion depth is reached, the maximum region
  // size is reached, etc.). ScheduleData is initialized in the scheduling
  // "dry-run".
  //
  // If this happens, we can still find the last instruction by brute force. We
  // iterate forwards from Front (inclusive) until we either see all
  // instructions in the bundle or reach the end of the block. If Front is the
  // last instruction in program order, LastInst will be set to Front, and we
  // will visit all the remaining instructions in the block.
  //
  // One of the reasons we exit early from buildTree_rec is to place an upper
  // bound on compile-time. Thus, taking an additional compile-time hit here is
  // not ideal. However, this should be exceedingly rare since it requires that
  // we both exit early from buildTree_rec and that the bundle be out-of-order
  // (causing us to iterate all the way to the end of the block).
  if (!LastInst) {
    SmallPtrSet<Value *, 16> Bundle(VL.begin(), VL.end());
    for (auto &I : make_range(BasicBlock::iterator(Front), BB->end())) {
      if (Bundle.erase(&I) && sameOpcodeOrAlt(Opcode, AltOpcode, I.getOpcode()))
        LastInst = &I;
      if (Bundle.empty())
        break;
    }
  }

  // Set the insertion point after the last instruction in the bundle. Set the
  // debug location to Front.
  Builder.SetInsertPoint(BB, ++LastInst->getIterator());
  Builder.SetCurrentDebugLocation(Front->getDebugLoc());
}

#if 0
void BoUpSLP::RecordExternalUse(Value *ElementVector, llvm::User *User) {
  TreeEntry *E = getTreeEntry(ElementVector);

  // Add to our 'need-to-extract' list.
  if (E) {
    // Find which lane we need to extract.
    int FoundLane = -1;
    for (unsigned Lane = 0, LE = E->Scalars.size(); Lane != LE; ++Lane) {
      // Is this the lane containing the vector that we are looking for?
      if (E->Scalars[Lane] == ElementVector) {
        FoundLane = Lane;
        break;
      }
    }
    assert(FoundLane >= 0 && "Could not find the correct lane");
    if (!E->ReuseShuffleIndices.empty()) {
      FoundLane =
              std::distance(E->ReuseShuffleIndices.begin(),
                            llvm::find(E->ReuseShuffleIndices, FoundLane));
    }
    ExternalUses.push_back(ExternalUser(ElementVector, User, FoundLane));
  }
}
#endif

Value *BoUpSLP::Gather(ArrayRef<Value *> VL, VectorType *Ty) {
  unsigned size = VL.size();

#ifndef NDEBUG
  LLVM_DEBUG(dbgs() << "Revec: Gathering value list of size " << size << ":\n");
  for (Value *val : VL) {
    LLVM_DEBUG(dbgs() << "Revec:    " << *val << "\n");
  }
#endif

  Value *gathered;

  if (allConstant(VL)) {
    SmallVector<Constant *, 32> constants;

    // Extract scalar constants from each value
    for (Value *val : VL) {
      LLVM_DEBUG(dbgs() << "Revec: Extracting scalar constants from value: " << *val << " of type: " << *val->getType() << "\n");
      // assert(val->getType()->isAggregateType() && "Cannot get scalar constants from non-aggregate type.");
      Constant *vec = cast<Constant>(val);

      unsigned i = 0;
      Constant *scalar = nullptr;
      while ((scalar = vec->getAggregateElement(i))) {
        LLVM_DEBUG(dbgs() << "Revec:     " << *scalar << "\n");
        constants.push_back(scalar);
        i++;
      }
    }

    // NOTE: as this is a constant, it does not need to be inserted with IRBuilder
    gathered = ConstantVector::get(constants);
  } else if (ExtractInsertGather) {
    gathered = Gather_extract_insert(VL, Ty);
  } else {
    assert(isPowerOf2_32(size) && size > 1 && "Gathering value list that is not of a power of two length greater than 1");
#if 0
    gathered = concatenateVectors(Builder, VL);
#else
    gathered = Gather_rec(VL, Ty, 0, size);
#endif
    assert(gathered->getType() == Ty && "Gathered value has a different type than expected");
  }

  // TODO: Pad vector with undefs if the type does not match?
  assert(gathered->getType()->getTypeID() == Ty->getTypeID() &&
          "Gather generated a value of the incorrect type");
  assert(gathered->getType()->getVectorNumElements() == Ty->getVectorNumElements() &&
          "Gather generated a value with the incorrect number of elements");

  LLVM_DEBUG(dbgs() << "Revec: Gathered:\n");
  LLVM_DEBUG(dbgs() << "Revec:    " << *gathered << "\n");

  return gathered;
}

Value *BoUpSLP::Gather_extract_insert(ArrayRef<Value *> VL, VectorType *Ty) {
  // Generate a pair of 'ExtractElement' and 'InsertElement' instructions for each scalar in each VL vector

  // Create undefined vector to populate
  Value *V = UndefValue::get(Ty);
  unsigned numExtracted = 0;
  for (Value *narrowVec : VL) {
    // TODO: Could support inserting scalars as well
    Type *narrowVecTy = narrowVec->getType();
    assert(narrowVecTy->isVectorTy() && "attempted to extend a vector with a non-vector type");

#ifndef NDEBUG
    // FIXME: See Gather_two explaination
    assert(!getTreeEntry(narrowVec) && "Gathered an already vectorized value!");
#endif

    for (unsigned j = 0, e = narrowVecTy->getVectorNumElements(); j < e; ++j) {
      Value *scalar = Builder.CreateExtractElement(narrowVec, Builder.getInt32(j));
      V = Builder.CreateInsertElement(V, scalar, Builder.getInt32(numExtracted));
      ++numExtracted;

      if (Instruction *I = dyn_cast<Instruction>(V)) {
        // Record this insert for later optimization
        GatherSeq.insert(I);
        CSEBlocks.insert(I->getParent());
      }
    }
  }

  LLVM_DEBUG(dbgs() << "Revec: During gather, extracted and inserted" << numExtracted << " scalars to place in vector "
               << *V << " (fits " << Ty->getNumElements() << " elements)\n");

  return V;
}

Value *BoUpSLP::Gather_two(Value *L, Value *R) {
  assert((L->getType() == R->getType()) && "Gathering values of different types");

  unsigned leftElems = L->getType()->getVectorNumElements();
  unsigned rightElems = R->getType()->getVectorNumElements();

  assert((leftElems > 0 && leftElems == rightElems) && "Bad sizes for gathered operands");

  SmallVector<uint32_t, 32> mask;
  for (unsigned i = 0; i < leftElems + rightElems; ++i)
    mask.emplace_back(static_cast<uint32_t>(i));

  Value *V = Builder.CreateShuffleVector(L, R, mask);

  if (Instruction *I = dyn_cast<Instruction>(V)) {
    // Record this shuffle for later optimization
    GatherSeq.insert(I);
    CSEBlocks.insert(I->getParent());

		// Add to our 'need-to-extract' list.
		if (TreeEntry *E = getTreeEntry(L)) {
			int FoundLane = 0;
			if (!E->ReuseShuffleIndices.empty()) {
				// FIXME: Is this needed?
				FoundLane = std::distance(E->ReuseShuffleIndices.begin(),
																	llvm::find(E->ReuseShuffleIndices, FoundLane));
			}
			ExternalUses.push_back(ExternalUser(L, I, FoundLane));
		}

		if (TreeEntry *E = getTreeEntry(R)) {
			int FoundLane = 1;
			if (!E->ReuseShuffleIndices.empty()) {
				// FIXME: Is this needed?
				FoundLane = std::distance(E->ReuseShuffleIndices.begin(),
																	llvm::find(E->ReuseShuffleIndices, FoundLane));
			}
			ExternalUses.push_back(ExternalUser(R, I, FoundLane));
		}

#if 0
    // FIXME: It may in fact be possible for a value to be vectorized
    // (i.e. in a tree entry - non-vectorized values are not in the ScalartoTreeEntry map),
    // but gathered with some other values. This would require an extract, which is currently not
    // implemented. If these assertions fail on some benchmark, then L and R should be added
    // to ExternalUses, where the use is V, if a tree entry exists.
    assert(!getTreeEntry(L) && "Gathered an already vectorized value!");
    assert(!getTreeEntry(R) && "Gathered an already vectorized value!");
#endif
  }

  return V;
}

Value *BoUpSLP::Gather_rec(ArrayRef<Value *> VL, VectorType *Ty, int start, int end) {
  assert((end >= start) && "Bad bounds passed to Gather_rec");
  int size = end - start;
  assert(isPowerOf2_32(static_cast<uint32_t>(size)) && size > 1 && "Recursively gathering value list that is not of a power of two length, or is length 1");

  if (size == 2)
    return Gather_two(VL[start], VL[start+1]);

  int sizeLeft = size / 2;
  Value *L = Gather_rec(VL, Ty, start, start + sizeLeft);
  Value *R = Gather_rec(VL, Ty, start + sizeLeft, end);
  return Gather_two(L, R);
}

Value *BoUpSLP::vectorizeTree(ArrayRef<Value *> VL) {
  InstructionsState S = getSameOpcode(VL);
  if (S.Opcode) {
    if (TreeEntry *E = getTreeEntry(S.OpValue)) {
      if (E->isSame(VL)) {
        Value *V = vectorizeTree(E);
        if (VL.size() == E->Scalars.size() && !E->ReuseShuffleIndices.empty()) {
          // We need to get the vectorized value but without shuffle.
          if (auto *SV = dyn_cast<ShuffleVectorInst>(V)) {
            V = SV->getOperand(0);
          } else {
            // Reshuffle to get only unique values.
            SmallVector<unsigned, 4> UniqueIdxs;
            SmallSet<unsigned, 4> UsedIdxs;
            for(unsigned Idx : E->ReuseShuffleIndices)
              if (UsedIdxs.insert(Idx).second)
                UniqueIdxs.emplace_back(Idx);
            SmallVector<unsigned, 16> UniqueMask;
            expandLanes(UniqueIdxs, E->Scalars[0]->getType()->getVectorNumElements(), UniqueMask);
            V = Builder.CreateShuffleVector(V, UndefValue::get(V->getType()),
                                            UniqueMask);
          }
        }
        return V;
      }
    }
  }

  Type *OpValueTy = S.OpValue->getType();
  if (StoreInst *SI = dyn_cast<StoreInst>(S.OpValue))
    OpValueTy = SI->getValueOperand()->getType();

  // Check that every instruction appears once in this bundle.
  SmallVector<unsigned, 4> ReuseShuffleIndices;
  SmallVector<Value *, 4> UniqueValues;
  if (VL.size() > 2) {
    DenseMap<Value *, unsigned> UniquePositions;
    for (Value *V : VL) {
      auto Res = UniquePositions.try_emplace(V, UniqueValues.size());
      ReuseShuffleIndices.emplace_back(Res.first->second);
      if (Res.second || isa<Constant>(V))
        UniqueValues.emplace_back(V);
    }
    // Do not shuffle single element or if number of unique values is not power
    // of 2.
    if (UniqueValues.size() == VL.size() || UniqueValues.size() <= 1 ||
        !llvm::isPowerOf2_32(UniqueValues.size()))
      ReuseShuffleIndices.clear();
    else
      VL = UniqueValues;
  }

  assert(OpValueTy->isVectorTy() && "Trying to widen non-vector type");
  VectorType *VecTy = getVectorType(OpValueTy, VL.size());

  Value *V = Gather(VL, VecTy);
  if (!ReuseShuffleIndices.empty()) {
    SmallVector<unsigned, 32> ReuseShuffleMask;
    expandLanes(ReuseShuffleIndices, VL[0]->getType()->getVectorNumElements(), ReuseShuffleMask);
    V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                    ReuseShuffleMask, "shuffle");
    if (auto *I = dyn_cast<Instruction>(V)) {
      GatherSeq.insert(I);
      CSEBlocks.insert(I->getParent());
    }
  }
  return V;
}

Value *BoUpSLP::vectorizeTree(TreeEntry *E) {
  IRBuilder<>::InsertPointGuard Guard(Builder);

  if (E->VectorizedValue) {
    LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *E->Scalars[0] << ".\n");
    return E->VectorizedValue;
  }

  InstructionsState S = getSameOpcode(E->Scalars);
  Instruction *VL0 = cast<Instruction>(E->Scalars[0]);
  Type *ElementTy = VL0->getType();
  if (StoreInst *SI = dyn_cast<StoreInst>(VL0))
    ElementTy = SI->getValueOperand()->getType();

  VectorType *VecTy = getVectorType(ElementTy, E->Scalars.size());

  LLVM_DEBUG(dbgs() << "Revec: vectorizing bundle : \n");
  LLVM_DEBUG(
  	for (unsigned i = 0; i< E->Scalars.size(); i++) {
  	  dbgs() << "  ";
  	  E->Scalars[i]->print(dbgs());
  	  dbgs() << "\n";
  	}

    dbgs() << "Revec:   E->ReuseShuffleIndices, size = " << E->ReuseShuffleIndices.size() << ": \n";
    for (unsigned i = 0; i < E->ReuseShuffleIndices.size(); i++) {
      dbgs() << " " << E->ReuseShuffleIndices[i] << " ";
    }
    dbgs() << "\n";

    dbgs() << "Revec:   E->ReuseShuffleMask, size = " << E->ReuseShuffleMask.size() << ": \n";
    for (unsigned i = 0; i < E->ReuseShuffleMask.size(); i++) {
      dbgs() << " " << E->ReuseShuffleMask[i] << " ";
    }
    dbgs() << "\n";
  );
  LLVM_DEBUG(dbgs() << "  Revec: Need to gather: " << (E->NeedToGather ? "yes" : "no") << "\n");

  bool NeedToShuffleReuses = !E->ReuseShuffleIndices.empty();
  assert(((NeedToShuffleReuses && !E->ReuseShuffleMask.empty()) ||
          (!NeedToShuffleReuses && E->ReuseShuffleMask.empty())) && "Empty ReuseShuffleIndices and ReuseShuffleMask must be both true or false");

  if (E->NeedToGather) {
    setInsertPointAfterBundle(E->Scalars, VL0);
    auto *V = Gather(E->Scalars, VecTy);
    if (NeedToShuffleReuses) {
      V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                      E->ReuseShuffleMask, "shuffle");
      if (auto *I = dyn_cast<Instruction>(V)) {
        GatherSeq.insert(I);
        CSEBlocks.insert(I->getParent());
      }
    }
    E->VectorizedValue = V;
    LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
    return V;
  }

  unsigned ShuffleOrOp = S.IsAltShuffle ?
           (unsigned) Instruction::ShuffleVector : S.Opcode;
  switch (ShuffleOrOp) {
    case Instruction::PHI: {
      PHINode *PH = dyn_cast<PHINode>(VL0);
      Builder.SetInsertPoint(PH->getParent()->getFirstNonPHI());
      Builder.SetCurrentDebugLocation(PH->getDebugLoc());
      PHINode *NewPhi = Builder.CreatePHI(VecTy, PH->getNumIncomingValues());
      Value *V = NewPhi;
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;

      // PHINodes may have multiple entries from the same block. We want to
      // visit every block once.
      SmallSet<BasicBlock*, 4> VisitedBBs;

      for (unsigned i = 0, e = PH->getNumIncomingValues(); i < e; ++i) {
        ValueList Operands;
        BasicBlock *IBB = PH->getIncomingBlock(i);

        if (!VisitedBBs.insert(IBB).second) {
          NewPhi->addIncoming(NewPhi->getIncomingValueForBlock(IBB), IBB);
          continue;
        }

        // Prepare the operand vector.
        for (Value *V : E->Scalars)
          Operands.push_back(cast<PHINode>(V)->getIncomingValueForBlock(IBB));

        Builder.SetInsertPoint(IBB->getTerminator());
        Builder.SetCurrentDebugLocation(PH->getDebugLoc());
        Value *Vec = vectorizeTree(Operands);
        NewPhi->addIncoming(Vec, IBB);
      }

      assert(NewPhi->getNumIncomingValues() == PH->getNumIncomingValues() &&
             "Invalid number of incoming values");
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::ExtractValue:
    case Instruction::ExtractElement: {
      // TODO: Could ExtractValue returns be bundled?
      assert(false && "ExtractElement and ExtractValue instructions should not appear in bundles to be vectorized, as they produce scalars");
      return 0;
    }
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::SIToFP:
    case Instruction::UIToFP:
    case Instruction::Trunc:
    case Instruction::FPTrunc:
    case Instruction::BitCast: {
      LLVM_DEBUG(dbgs() << "Revec: Vectorizing cast bundle\n");
      ValueList SrcVL;
      for (Value *V : E->Scalars) {
        LLVM_DEBUG(dbgs() << "Revec:   " << *V << "\n");
        SrcVL.push_back(cast<Instruction>(V)->getOperand(0));
      }

      setInsertPointAfterBundle(E->Scalars, VL0);

      Value *SrcVec = vectorizeTree(SrcVL);
      LLVM_DEBUG(dbgs() << "Revec: Vectorized source operands for cast bundle starting with " << *VL0 << "\n");
      LLVM_DEBUG(dbgs() << "Revec:   Widened source: " << *SrcVec << "\n");
      LLVM_DEBUG(dbgs() << "Revec:   Destination type: " << *VecTy << "\n");

      if (E->VectorizedValue) {
        LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
        return E->VectorizedValue;
      }

#if 0
#ifndef NDEBUG
      Type *SrcVecTy = SrcVec->getType();
      if (!llvm::isPowerOf2_32(SrcVecTy->getVectorNumElements())) {
        LLVM_DEBUG(outs() << "Bad vectorized src type " << *SrcVecTy << "\n");
        User *SrcVecAsUser = dyn_cast<User>(SrcVec);
        BinaryOperator *SrcVecAsBinOp = dyn_cast<BinaryOperator>(SrcVec);
        assert(false);
      }
#endif
#endif

      CastInst *CI = dyn_cast<CastInst>(VL0);
      Value *V = Builder.CreateCast(CI->getOpcode(), SrcVec, VecTy);
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::FCmp:
    case Instruction::ICmp: {
      ValueList LHSV, RHSV;
      for (Value *V : E->Scalars) {
        LHSV.push_back(cast<Instruction>(V)->getOperand(0));
        RHSV.push_back(cast<Instruction>(V)->getOperand(1));
      }

      setInsertPointAfterBundle(E->Scalars, VL0);

      Value *L = vectorizeTree(LHSV);
      Value *R = vectorizeTree(RHSV);

      if (E->VectorizedValue) {
        LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
        return E->VectorizedValue;
      }

      CmpInst::Predicate P0 = cast<CmpInst>(VL0)->getPredicate();
      Value *V;
      if (S.Opcode == Instruction::FCmp)
        V = Builder.CreateFCmp(P0, L, R);
      else
        V = Builder.CreateICmp(P0, L, R);

      propagateIRFlags(V, E->Scalars, VL0);
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::Select: {
      ValueList TrueVec, FalseVec, CondVec;
      for (Value *V : E->Scalars) {
        CondVec.push_back(cast<Instruction>(V)->getOperand(0));
        TrueVec.push_back(cast<Instruction>(V)->getOperand(1));
        FalseVec.push_back(cast<Instruction>(V)->getOperand(2));
      }

      setInsertPointAfterBundle(E->Scalars, VL0);

      Value *Cond = vectorizeTree(CondVec);
      Value *True = vectorizeTree(TrueVec);
      Value *False = vectorizeTree(FalseVec);

      if (E->VectorizedValue) {
        LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
        return E->VectorizedValue;
      }

      Value *V = Builder.CreateSelect(Cond, True, False);
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor: {
      ValueList LHSVL, RHSVL;
      if (isa<BinaryOperator>(VL0) && VL0->isCommutative())
        reorderInputsAccordingToOpcode(S.Opcode, E->Scalars, LHSVL,
                                       RHSVL);
      else
        for (Value *V : E->Scalars) {
          auto *I = cast<Instruction>(V);
          LHSVL.push_back(I->getOperand(0));
          RHSVL.push_back(I->getOperand(1));
        }

      setInsertPointAfterBundle(E->Scalars, VL0);

      Value *LHS = vectorizeTree(LHSVL);
      Value *RHS = vectorizeTree(RHSVL);

      if (E->VectorizedValue) {
        LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
        return E->VectorizedValue;
      }

      Value *V = Builder.CreateBinOp(
          static_cast<Instruction::BinaryOps>(S.Opcode), LHS, RHS);
      propagateIRFlags(V, E->Scalars, VL0);
      if (auto *I = dyn_cast<Instruction>(V))
        V = propagateMetadata(I, E->Scalars);

      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;

      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::Load: {
      // Loads are inserted at the head of the tree because we don't want to
      // sink them all the way down past store instructions.
      bool IsReversed =
          !isConsecutiveAccess(E->Scalars[0], E->Scalars[1], *DL, *SE);
      if (IsReversed)
        VL0 = cast<Instruction>(E->Scalars.back());
      setInsertPointAfterBundle(E->Scalars, VL0);

      LoadInst *LI = cast<LoadInst>(VL0);
      Type *ScalarLoadTy = LI->getType();
      unsigned AS = LI->getPointerAddressSpace();

      Value *VecPtr = Builder.CreateBitCast(LI->getPointerOperand(),
                                            VecTy->getPointerTo(AS));

      // The pointer operand uses an in-tree scalar so we add the new BitCast to
      // ExternalUses list to make sure that an extract will be generated in the
      // future.
      Value *PO = LI->getPointerOperand();
      if (getTreeEntry(PO))
        ExternalUses.push_back(ExternalUser(PO, cast<User>(VecPtr), 0));

      unsigned Alignment = LI->getAlignment();
      LI = Builder.CreateLoad(VecPtr);
      if (!Alignment) {
        Alignment = DL->getABITypeAlignment(ScalarLoadTy);
      }
      LI->setAlignment(Alignment);
      Value *V = propagateMetadata(LI, E->Scalars);
      if (IsReversed) {
        SmallVector<uint32_t, 4> Mask(E->Scalars.size());
        std::iota(Mask.rbegin(), Mask.rend(), 0);
        V = Builder.CreateShuffleVector(V, UndefValue::get(V->getType()), Mask);
      }
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }

      LLVM_DEBUG(dbgs() << "Revec:   Revectorized Load bundle: " << *V << "\n");

      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::Store: {
      StoreInst *SI = cast<StoreInst>(VL0);
      unsigned Alignment = SI->getAlignment();
      unsigned AS = SI->getPointerAddressSpace();

      ValueList ScalarStoreValues;
      for (Value *V : E->Scalars)
        ScalarStoreValues.push_back(cast<StoreInst>(V)->getValueOperand());

      setInsertPointAfterBundle(E->Scalars, VL0);

      Value *VecValue = vectorizeTree(ScalarStoreValues);
      Value *ScalarPtr = SI->getPointerOperand();
      Value *VecPtr = Builder.CreateBitCast(ScalarPtr, VecTy->getPointerTo(AS));
      StoreInst *S = Builder.CreateStore(VecValue, VecPtr);

      // The pointer operand uses an in-tree scalar, so add the new BitCast to
      // ExternalUses to make sure that an extract will be generated in the
      // future.
      if (getTreeEntry(ScalarPtr))
        ExternalUses.push_back(ExternalUser(ScalarPtr, cast<User>(VecPtr), 0));

      if (!Alignment)
        Alignment = DL->getABITypeAlignment(SI->getValueOperand()->getType());

      S->setAlignment(Alignment);
      Value *V = propagateMetadata(S, E->Scalars);
      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::GetElementPtr: {
      setInsertPointAfterBundle(E->Scalars, VL0);

      ValueList Op0VL;
      for (Value *V : E->Scalars)
        Op0VL.push_back(cast<GetElementPtrInst>(V)->getOperand(0));

      Value *Op0 = vectorizeTree(Op0VL);

      std::vector<Value *> OpVecs;
      for (int j = 1, e = cast<GetElementPtrInst>(VL0)->getNumOperands(); j < e;
           ++j) {
        ValueList OpVL;
        for (Value *V : E->Scalars)
          OpVL.push_back(cast<GetElementPtrInst>(V)->getOperand(j));

        Value *OpVec = vectorizeTree(OpVL);
        OpVecs.push_back(OpVec);
      }

      Value *V = Builder.CreateGEP(
          cast<GetElementPtrInst>(VL0)->getSourceElementType(), Op0, OpVecs);
      if (Instruction *I = dyn_cast<Instruction>(V))
        V = propagateMetadata(I, E->Scalars);

      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }
      E->VectorizedValue = V;
      ++NumVectorInstructions;

      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::Call: {
      CallInst *CI = cast<CallInst>(VL0);
      setInsertPointAfterBundle(E->Scalars, VL0);
      Function *FI;
      Intrinsic::ID IID  = Intrinsic::not_intrinsic;
      if (CI && (FI = CI->getCalledFunction())) {
        IID = FI->getIntrinsicID();
      }

      assert(IID != Intrinsic::not_intrinsic);
      LLVM_DEBUG(dbgs() << "Revec: Vectorizing intrinsic function "
												<< (llvm::Intrinsic::isOverloaded(IID) ? std::to_string(IID) : llvm::Intrinsic::getName(IID).str())
												<< "\n");

      const auto& target = getWidenedIntrinsic(IID, E->Scalars.size());
      int VF = target.first;
      Intrinsic::ID alt = target.second;
      assert(alt != Intrinsic::not_intrinsic && VF > 0 && "Attempting to vectorize intrinsic call, but no equivalence found.");

      // TODO: Support splitting larger bundles followed by a gather if an intrinsic
      // for the full bundle is not available
      assert((VF == static_cast<long>(E->Scalars.size())) && "Cannot bundle intrinsic calls, where known widening factor does not match bundle size.");

      // Find intrinsic conversion and merge factor
      LLVM_DEBUG(dbgs() << "Revec:   translation: "
												<< (llvm::Intrinsic::isOverloaded(alt) ? std::to_string(alt) : llvm::Intrinsic::getName(alt).str())
												<< " widening factor: " << VF << ".\n");

      Optional<unsigned> preservedOperand = getPreservedOperand(IID);

      ValueList args;
      for (unsigned i = 0, e = CI->getNumArgOperands(); i != e; ++i) {
        Value *arg;

        if (preservedOperand.hasValue() && preservedOperand.getValue() == i) {
          arg = CI->getArgOperand(i);
        } else {
          // Prepare a vertical operand vector.
          ValueList Operands;
          for (Value *val : E->Scalars) {
            CallInst *CI2 = dyn_cast<CallInst>(val);
            Operands.push_back(CI2->getArgOperand(i));
          }
          arg = vectorizeTree(Operands);
        }

        LLVM_DEBUG(dbgs() << "Revec: Call wide arg " << i << " = " << *arg << "\n");
        args.push_back(arg);
      }

      Module *M = F->getParent();
      Function *CF = Intrinsic::getDeclaration(M, alt);
      SmallVector<OperandBundleDef, 1> OpBundles;
      CI->getOperandBundlesAsDefs(OpBundles);
      Value *V = Builder.CreateCall(CF, args, OpBundles);
      LLVM_DEBUG(dbgs() << "Revec: Wide call vector value : " << *V << "\n");

      propagateIRFlags(V, E->Scalars, VL0);

      if (NeedToShuffleReuses) {
        V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                        E->ReuseShuffleMask, "shuffle");
      }

      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    case Instruction::ShuffleVector: {
      if (S.IsAltShuffle) {
        // Vectorize alternating binary operations, and emit a shufflevector to select results
        ValueList LHSVL, RHSVL;
        assert(Instruction::isBinaryOp(S.Opcode) &&
              "Invalid Shuffle Vector Operand");
        reorderAltShuffleOperands(S.Opcode, E->Scalars, LHSVL, RHSVL);
        setInsertPointAfterBundle(E->Scalars, VL0);

        Value *LHS = vectorizeTree(LHSVL);
        Value *RHS = vectorizeTree(RHSVL);

        if (E->VectorizedValue) {
          LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
          return E->VectorizedValue;
        }

        // Create a vector of LHS op1 RHS
        Value *V0 = Builder.CreateBinOp(
            static_cast<Instruction::BinaryOps>(S.Opcode), LHS, RHS);

        unsigned AltOpcode = getAltOpcode(S.Opcode);
        // Create a vector of LHS op2 RHS
        Value *V1 = Builder.CreateBinOp(
            static_cast<Instruction::BinaryOps>(AltOpcode), LHS, RHS);

        // Create shuffle to take alternate operations from the vector.
        // Also, gather up odd and even scalar ops to propagate IR flags to
        // each vector operation.
        ValueList OddScalars, EvenScalars;
        unsigned e = E->Scalars.size();
        SmallVector<Constant *, 8> Mask(e);
        for (unsigned i = 0; i < e; ++i) {
          if (isOdd(i)) {
            Mask[i] = Builder.getInt32(e + i);
            OddScalars.push_back(E->Scalars[i]);
          } else {
            Mask[i] = Builder.getInt32(i);
            EvenScalars.push_back(E->Scalars[i]);
          }
        }

        Value *ShuffleMask = ConstantVector::get(Mask);
        propagateIRFlags(V0, EvenScalars);
        propagateIRFlags(V1, OddScalars);

        Value *V = Builder.CreateShuffleVector(V0, V1, ShuffleMask);
        if (Instruction *I = dyn_cast<Instruction>(V))
          V = propagateMetadata(I, E->Scalars);
        if (NeedToShuffleReuses) {
          V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                          E->ReuseShuffleMask, "shuffle");
        }
        E->VectorizedValue = V;
        ++NumVectorInstructions;

        LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
        return V;
      }

      // Collect operand bundles based on cached decision
      Value *V = nullptr;
      const ShuffleBundleDecision &decision = getShuffleBundleDecision(E->Scalars);

      LLVM_DEBUG(dbgs() << "Revec: vectorizeTree: Found decision with merge mode " << decision.MergeMode << " and bundle: \n");
      for (Value *val : decision.VL) {
        LLVM_DEBUG(dbgs() << "Revec:   " << val << " = " << *val << "\n");
      }

      switch (decision.MergeMode) {
        case ShuffleBundleDecision::FirstOp0: {
          ShuffleVectorInst *SV0 = cast<ShuffleVectorInst>(E->Scalars[0]);
          V = SV0->getOperand(0);
          break;
        }
        case ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask: {
          assert((decision.Mask != nullptr) && "No mask available for IndexOp0_IndexOp1_WidenMask decision");

          ValueList LeftBundle, RightBundle;
          for (unsigned i = 0, Max = E->Scalars.size(); i < Max; ++i) {
            ShuffleVectorInst *Inst = cast<ShuffleVectorInst>(E->Scalars[i]);
            LeftBundle.push_back(
                Inst->getOperand(decision.Op0Indices[i] ? 1 : 0));
            RightBundle.push_back(
                Inst->getOperand(decision.Op0Indices[i] ? 0 : 1));
          }

          setInsertPointAfterBundle(E->Scalars, VL0);
          Value *Op0 = vectorizeTree(LeftBundle);
          Value *Op1 = vectorizeTree(RightBundle);

          V = Builder.CreateShuffleVector(Op0, Op1, decision.Mask);
          break;
        }
        case ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask:
        case ShuffleBundleDecision::FirstOp0_MergeOp1_ConcatenateMask: {
          assert((decision.Mask != nullptr) && "No mask available");

          // Get the first operand
          ShuffleVectorInst *FirstInst = cast<ShuffleVectorInst>(E->Scalars[0]);
          Value *Op0 = FirstInst->getOperand(0);
          Value *Op1 = (decision.MergeMode == ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask)
                           ? FirstInst->getOperand(1) // Simply use the first shuffle's operand 1
                           : decision.Op1Value; // The merged operand 1 was precomputed in buildTree_rec

          setInsertPointAfterBundle(E->Scalars, VL0);
          V = Builder.CreateShuffleVector(Op0, Op1, decision.Mask);
          break;
        }
        case ShuffleBundleDecision::Diagonal_VF2: {
          assert((decision.Mask != nullptr) && "No mask available");

          // Use Op0 and Op1 from VL0, and decision.Mask
          ShuffleVectorInst *FirstInst = cast<ShuffleVectorInst>(E->Scalars[0]);
          Value *Op0 = FirstInst->getOperand(0);
          Value *Op1 = FirstInst->getOperand(1);

          setInsertPointAfterBundle(E->Scalars, VL0);
          V = Builder.CreateShuffleVector(Op0, Op1, decision.Mask);
          break;
        }
        case ShuffleBundleDecision::Gather: {
#if 1
          assert(E->NeedToGather && "Should not be gathering shuffle bundle if E->NeedToGather is false");
#else
#ifndef NDEBUG
          printf("Revec: ERROR: Unexpectedly gathering shuffle bundle. E->NeedToGather = %d\n", E->NeedToGather);
          LLVM_DEBUG(dbgs() << "Revec: ERROR: Unexpectedly gathering shuffle bundle. E->NeedToGather = " << E->NeedToGather << "\n");
          for (Value *val : E->Scalars)
            LLVM_DEBUG(dbgs() << "Revec:   " << *val << "\n");
#endif

          setInsertPointAfterBundle(E->Scalars, VL0);
          V = Gather(E->Scalars, VecTy);
          if (NeedToShuffleReuses) {
            V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                            E->ReuseShuffleIndices, "shufflegather");
            if (auto *I = dyn_cast<Instruction>(V)) {
              GatherSeq.insert(I);
              CSEBlocks.insert(I->getParent());
            }
          }
#endif
          break;
        }
        default:
          llvm_unreachable("Unrecognized decision type");
      }

#if 0
      if (decision.MergeMode == ShuffleBundleDecision::FirstOp0) {
        ShuffleVectorInst *SV0 = cast<ShuffleVectorInst>(E->Scalars[0]);
        V = SV0->getOperand(0);
      } else if (decision.MergeMode == ShuffleBundleDecision::IndexOp0_IndexOp1_WidenMask) {
        ValueList LeftBundle, RightBundle;
        for (unsigned i = 0, Max = E->Scalars.size(); i < Max; ++i) {
          ShuffleVectorInst *Inst = cast<ShuffleVectorInst>(E->Scalars[i]);
          LeftBundle.push_back(
              Inst->getOperand(decision.Op0Indices[i] ? 1 : 0));
          RightBundle.push_back(
              Inst->getOperand(decision.Op0Indices[i] ? 0 : 1));
        }

        setInsertPointAfterBundle(E->Scalars, VL0);
        Value *Op0 = vectorizeTree(LeftBundle);
        Value *Op1 = vectorizeTree(RightBundle);

        assert((decision.Mask != nullptr) && "No mask available for IndexOp0_IndexOp1_WidenMask decision");

        V = Builder.CreateShuffleVector(Op0, Op1, decision.Mask);
      } else if (decision.MergeMode == ShuffleBundleDecision::Gather) {
#ifndef NDEBUG
        printf("Revec: ERROR: Unexpectedly gathering shuffle bundle. E->NeedToGather = %d\n", E->NeedToGather);
        LLVM_DEBUG(dbgs() << "Revec: ERROR: Unexpectedly gathering shuffle bundle. E->NeedToGather = " << E->NeedToGather << "\n");
        for (Value *val : E->Scalars)
          LLVM_DEBUG(dbgs() << "Revec:   " << *val << "\n");
#endif

        setInsertPointAfterBundle(E->Scalars, VL0);
        V = Gather(E->Scalars, VecTy);
        if (NeedToShuffleReuses) {
          V = Builder.CreateShuffleVector(V, UndefValue::get(VecTy),
                                          E->ReuseShuffleIndices, "shufflegather");
          if (auto *I = dyn_cast<Instruction>(V)) {
            GatherSeq.insert(I);
            CSEBlocks.insert(I->getParent());
          }
        }
      } else {
        assert((decision.MergeMode ==
                    ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask ||
                decision.MergeMode ==
                    ShuffleBundleDecision::FirstOp0_MergeOp1_ConcatenateMask) &&
               "Unknown shuffle bundle merge mode");

        assert((decision.Mask != nullptr) && "No mask available");

        // Get the first operand
        ShuffleVectorInst *FirstInst = cast<ShuffleVectorInst>(E->Scalars[0]);
        Value *Op0 = FirstInst->getOperand(0);
        Value *Op1 = (decision.MergeMode == ShuffleBundleDecision::FirstOp0_FirstOp1_ConcatenateMask)
                         ? FirstInst->getOperand(1) // Simply use the first shuffle's operand 1
                         : decision.Op1Value; // The merged operand 1 was precomputed in buildTree_rec

        V = Builder.CreateShuffleVector(Op0, Op1, decision.Mask);
      }
#endif

      if (E->VectorizedValue) {
        // TODO: Can this happen? Does it need to happen after dispatching calls to vectorizeTree?
        LLVM_DEBUG(dbgs() << "Revec: Diamond merged for " << *VL0 << ".\n");
        return E->VectorizedValue;
      }

      LLVM_DEBUG(dbgs() << "Revec: Emitting revectorized shuffle: " << *V << "\n");
      assert(V && "Unable to merge shuffles");
      E->VectorizedValue = V;
      ++NumVectorInstructions;
      LLVM_DEBUG(dbgs() << "  Revec: vectorized value: " << *V << "\n");
      return V;
    }
    default:
    llvm_unreachable("unknown inst");
  }
  return nullptr;
}

Value *BoUpSLP::vectorizeTree() {
  ExtraValueToDebugLocsMap ExternallyUsedValues;
  return vectorizeTree(ExternallyUsedValues);
}

Value *
BoUpSLP::vectorizeTree(ExtraValueToDebugLocsMap &ExternallyUsedValues) {
  // All blocks must be scheduled before any instructions are inserted.
  for (auto &BSIter : BlocksSchedules) {
    scheduleBlock(BSIter.second.get());
  }

  Builder.SetInsertPoint(&F->getEntryBlock().front());
  vectorizeTree(&VectorizableTree[0]);

  LLVM_DEBUG(dbgs() << "Revec: Extracting " << ExternalUses.size() << " values .\n");

  // Extract all of the elements with the external uses.
  for (const auto &ExternalUse : ExternalUses) {
    Value *narrowVec = ExternalUse.Scalar;
    VectorType *narrowVecTy = cast<VectorType>(narrowVec->getType());
    llvm::User *User = ExternalUse.User;

    // Skip users that we already RAUW. This happens when one instruction
    // has multiple uses of the same value.
    if (User && !is_contained(narrowVec->users(), User))
      continue;
    TreeEntry *E = getTreeEntry(narrowVec);
    assert(E && "Invalid narrow vector to be extracted");
    assert(!E->NeedToGather && "Extracting from a gather list");

    Value *wideVec = E->VectorizedValue;
    VectorType *wideVecTy = cast<VectorType>(wideVec->getType());
    assert(wideVec && "Can't find vectorizable value");

    // If User == nullptr, the narrowVec is used as extra arg. Generate
    // ExtractElement instruction and update the record for this narrow vector in
    // ExternallyUsedValues.
    if (!User) {
      assert(ExternallyUsedValues.count(narrowVec) &&
             "Narrow vector with nullptr as an external user must be registered in "
             "ExternallyUsedValues map");
      if (auto *wideVecI = dyn_cast<Instruction>(wideVec)) {
        Builder.SetInsertPoint(wideVecI->getParent(),
                               std::next(wideVecI->getIterator()));
      } else {
        Builder.SetInsertPoint(&F->getEntryBlock().front());
      }

      // FIXME: Can the element type of the vectorized value differ from that of the narrow value?
      //        If so, bitwidths could be used to still handle the extraction.
      assert((narrowVecTy->getElementType() == wideVecTy->getElementType())
             && "Narrow vector to be extracted and vectorized value have differing element types");

      // Extract narrow vector with shuffle
      SmallVector<uint32_t, 16> Mask;
      for (unsigned i = 0, NumToExtract = narrowVecTy->getNumElements(); i < NumToExtract; ++i) {
        Mask.push_back(ExternalUse.Lane * NumToExtract + i);
      }
      Value *Ex = Builder.CreateShuffleVector(wideVec, UndefValue::get(wideVecTy), Mask);

      CSEBlocks.insert(cast<Instruction>(narrowVec)->getParent());
      auto &Locs = ExternallyUsedValues[narrowVec];
      ExternallyUsedValues.insert({Ex, Locs});
      ExternallyUsedValues.erase(narrowVec);
      continue;
    }

    // Generate extracts for out-of-tree users.
    // Find the insertion point for the shufflevector.
    if (auto *wideVecI = dyn_cast<Instruction>(wideVec)) {
      if (PHINode *PH = dyn_cast<PHINode>(User)) {
        for (int i = 0, e = PH->getNumIncomingValues(); i != e; ++i) {
          if (PH->getIncomingValue(i) == narrowVec) {
            TerminatorInst *IncomingTerminator =
                PH->getIncomingBlock(i)->getTerminator();
            if (isa<CatchSwitchInst>(IncomingTerminator)) {
              Builder.SetInsertPoint(wideVecI->getParent(),
                                     std::next(wideVecI->getIterator()));
            } else {
              // FIXME: Can't this just be `Builder.SetInsertPoint(IncomingTerminator)`?
              Builder.SetInsertPoint(PH->getIncomingBlock(i)->getTerminator());
            }

            // Extract narrow vector with shuffle
            SmallVector<uint32_t, 16> Mask;
            for (unsigned i = 0, NumToExtract = narrowVecTy->getNumElements(); i < NumToExtract; ++i)
              Mask.push_back(ExternalUse.Lane * NumToExtract + i);
            Value *Ex = Builder.CreateShuffleVector(wideVec, UndefValue::get(wideVecTy), Mask);

            CSEBlocks.insert(PH->getIncomingBlock(i));
            PH->setOperand(i, Ex);
          }
        }
      } else {
        Builder.SetInsertPoint(cast<Instruction>(User));

        // Extract narrow vector with shuffle
        SmallVector<uint32_t, 16> Mask;
        for (unsigned i = 0, NumToExtract = narrowVecTy->getNumElements(); i < NumToExtract; ++i)
          Mask.push_back(ExternalUse.Lane * NumToExtract + i);
        Value *Ex = Builder.CreateShuffleVector(wideVec, UndefValue::get(wideVecTy), Mask);

        CSEBlocks.insert(cast<Instruction>(User)->getParent());
        User->replaceUsesOfWith(narrowVec, Ex);
      }
    } else {
      Builder.SetInsertPoint(&F->getEntryBlock().front());

      // Extract narrow vector with shuffle
      SmallVector<uint32_t, 16> Mask;
      for (unsigned i = 0, NumToExtract = narrowVecTy->getNumElements(); i < NumToExtract; ++i)
        Mask.push_back(ExternalUse.Lane * NumToExtract + i);
      Value *Ex = Builder.CreateShuffleVector(wideVec, UndefValue::get(wideVecTy), Mask);

      CSEBlocks.insert(&F->getEntryBlock());
      User->replaceUsesOfWith(narrowVec, Ex);
    }

    LLVM_DEBUG(dbgs() << "Revec: Replaced:" << *User << ".\n");
  }

  // For each vectorized value:
  for (TreeEntry &EIdx : VectorizableTree) {
    TreeEntry *Entry = &EIdx;

    // No need to handle users of gathered values.
    if (Entry->NeedToGather)
      continue;

    assert(Entry->VectorizedValue && "Can't find vectorizable value");

    // For each lane:
    for (int Lane = 0, LE = Entry->Scalars.size(); Lane != LE; ++Lane) {
      Value *narrowVec = Entry->Scalars[Lane];

      Type *narrowVecTy = narrowVec->getType();
      if (!narrowVecTy->isVoidTy()) {
#ifndef NDEBUG
        for (User *U : narrowVec->users()) {
          LLVM_DEBUG(dbgs() << "Revec: \tvalidating user:" << *U << ".\n");

          // It is legal to replace users in the ignorelist by undef.
          assert((getTreeEntry(U) || is_contained(UserIgnoreList, U)) &&
                 "Replacing out-of-tree value with undef");
        }
#endif
        Value *Undef = UndefValue::get(narrowVecTy);
        narrowVec->replaceAllUsesWith(Undef);
      }
      LLVM_DEBUG(dbgs() << "Revec: \tErasing narrow vector:" << *narrowVec << ".\n");
      eraseInstruction(cast<Instruction>(narrowVec));
    }
  }

  Builder.ClearInsertionPoint();

  return VectorizableTree[0].VectorizedValue;
}

void BoUpSLP::optimizeGatherSequence() {
  LLVM_DEBUG(dbgs() << "Revec: Optimizing " << GatherSeq.size()
        << " gather sequences instructions.\n");
  // LICM InsertElementInst or ShuffleVectorInst sequences.
  for (Instruction *I : GatherSeq) {
    if (!isa<InsertElementInst>(I) && !isa<ShuffleVectorInst>(I))
      continue;

    // Check if this block is inside a loop.
    Loop *L = LI->getLoopFor(I->getParent());
    if (!L)
      continue;

    // Check if it has a preheader.
    BasicBlock *PreHeader = L->getLoopPreheader();
    if (!PreHeader)
      continue;

    // If the vector or the element that we insert into it are
    // instructions that are defined in this basic block then we can't
    // hoist this instruction.
    auto *Op0 = dyn_cast<Instruction>(I->getOperand(0));
    auto *Op1 = dyn_cast<Instruction>(I->getOperand(1));
    if (Op0 && L->contains(Op0))
      continue;
    if (Op1 && L->contains(Op1))
      continue;

    // We can hoist this instruction. Move it to the pre-header.
    I->moveBefore(PreHeader->getTerminator());
  }

  // Make a list of all reachable blocks in our CSE queue.
  SmallVector<const DomTreeNode *, 8> CSEWorkList;
  CSEWorkList.reserve(CSEBlocks.size());
  for (BasicBlock *BB : CSEBlocks)
    if (DomTreeNode *N = DT->getNode(BB)) {
      assert(DT->isReachableFromEntry(N));
      CSEWorkList.push_back(N);
    }

  // Sort blocks by domination. This ensures we visit a block after all blocks
  // dominating it are visited.
  std::stable_sort(CSEWorkList.begin(), CSEWorkList.end(),
                   [this](const DomTreeNode *A, const DomTreeNode *B) {
    return DT->properlyDominates(A, B);
  });

  // Perform O(N^2) search over the gather sequences and merge identical
  // instructions. TODO: We can further optimize this scan if we split the
  // instructions into different buckets based on the insert lane.
  SmallVector<Instruction *, 16> Visited;
  for (auto I = CSEWorkList.begin(), E = CSEWorkList.end(); I != E; ++I) {
    assert((I == CSEWorkList.begin() || !DT->dominates(*I, *std::prev(I))) &&
           "Worklist not sorted properly!");
    BasicBlock *BB = (*I)->getBlock();
    // For all instructions in blocks containing gather sequences:
    for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e;) {
      Instruction *In = &*it++;
      // TODO: Can InsertElementInst or ExtractElementInst appear?
      if (!isa<InsertElementInst>(In) && !isa<ExtractElementInst>(In) &&!isa<ShuffleVectorInst>(In))
        continue;

      // Check if we can replace this instruction with any of the
      // visited instructions.
      for (Instruction *v : Visited) {
        if (In->isIdenticalTo(v) &&
            DT->dominates(v->getParent(), In->getParent())) {
          In->replaceAllUsesWith(v);
          eraseInstruction(In);
          In = nullptr;
          break;
        }
      }
      if (In) {
        assert(!is_contained(Visited, In));
        Visited.push_back(In);
      }
    }
  }
  CSEBlocks.clear();
  GatherSeq.clear();
}

// Groups the instructions to a bundle (which is then a single scheduling entity)
// and schedules instructions until the bundle gets ready.
bool BoUpSLP::BlockScheduling::tryScheduleBundle(ArrayRef<Value *> VL,
                                                 BoUpSLP *SLP, Value *OpValue) {
  if (isa<PHINode>(OpValue))
    return true;

  // Initialize the instruction bundle.
  Instruction *OldScheduleEnd = ScheduleEnd;
  ScheduleData *PrevInBundle = nullptr;
  ScheduleData *Bundle = nullptr;
  bool ReSchedule = false;
  LLVM_DEBUG(dbgs() << "Revec:  bundle: " << *OpValue << "\n");

  // Make sure that the scheduling region contains all
  // instructions of the bundle.
  for (Value *V : VL) {
    if (!extendSchedulingRegion(V, OpValue))
      return false;
  }

  for (Value *V : VL) {
    ScheduleData *BundleMember = getScheduleData(V);
    assert(BundleMember &&
           "no ScheduleData for bundle member (maybe not in same basic block)");
    if (BundleMember->IsScheduled) {
      // A bundle member was scheduled as single instruction before and now
      // needs to be scheduled as part of the bundle. We just get rid of the
      // existing schedule.
      LLVM_DEBUG(dbgs() << "Revec:  reset schedule because " << *BundleMember
                   << " was already scheduled\n");
      ReSchedule = true;
    }
    assert(BundleMember->isSchedulingEntity() &&
           "bundle member already part of other bundle");
    if (PrevInBundle) {
      PrevInBundle->NextInBundle = BundleMember;
    } else {
      Bundle = BundleMember;
    }
    BundleMember->UnscheduledDepsInBundle = 0;
    Bundle->UnscheduledDepsInBundle += BundleMember->UnscheduledDeps;

    // Group the instructions to a bundle.
    BundleMember->FirstInBundle = Bundle;
    PrevInBundle = BundleMember;
  }
  if (ScheduleEnd != OldScheduleEnd) {
    // The scheduling region got new instructions at the lower end (or it is a
    // new region for the first bundle). This makes it necessary to
    // recalculate all dependencies.
    // It is seldom that this needs to be done a second time after adding the
    // initial bundle to the region.
    for (auto *I = ScheduleStart; I != ScheduleEnd; I = I->getNextNode()) {
      doForAllOpcodes(I, [](ScheduleData *SD) {
        SD->clearDependencies();
      });
    }
    ReSchedule = true;
  }
  if (ReSchedule) {
    resetSchedule();
    initialFillReadyList(ReadyInsts);
  }

  LLVM_DEBUG(dbgs() << "Revec: ReadyList ReadyInsts size: " << ReadyInsts.size() << "\n");

  LLVM_DEBUG(dbgs() << "Revec: try schedule bundle " << *Bundle << " in block "
               << BB->getName() << "\n");

  calculateDependencies(Bundle, true, SLP);

  // Now try to schedule the new bundle. As soon as the bundle is "ready" it
  // means that there are no cyclic dependencies and we can schedule it.
  // Note that's important that we don't "schedule" the bundle yet (see
  // cancelScheduling).
  while (!Bundle->isReady() && !ReadyInsts.empty()) {

    ScheduleData *pickedSD = ReadyInsts.back();
    ReadyInsts.pop_back();

    // TODO: Need to put back in pickedSD->isReady(), but no ScheduleData bundle is ready on a test...
    if (pickedSD->isSchedulingEntity() && pickedSD->isReady()) {
    // if (pickedSD->isSchedulingEntity()) {
      schedule(pickedSD, ReadyInsts);
    }
  }
  if (!Bundle->isReady()) {
    cancelScheduling(VL, OpValue);
    return false;
  }
  return true;
}

void BoUpSLP::BlockScheduling::cancelScheduling(ArrayRef<Value *> VL,
                                                Value *OpValue) {
  if (isa<PHINode>(OpValue))
    return;

  ScheduleData *Bundle = getScheduleData(OpValue);
  LLVM_DEBUG(dbgs() << "Revec:  cancel scheduling of " << *Bundle << "\n");
  assert(!Bundle->IsScheduled &&
         "Can't cancel bundle which is already scheduled");
  assert(Bundle->isSchedulingEntity() && Bundle->isPartOfBundle() &&
         "tried to unbundle something which is not a bundle");

  // Un-bundle: make single instructions out of the bundle.
  ScheduleData *BundleMember = Bundle;
  while (BundleMember) {
    assert(BundleMember->FirstInBundle == Bundle && "corrupt bundle links");
    BundleMember->FirstInBundle = BundleMember;
    ScheduleData *Next = BundleMember->NextInBundle;
    BundleMember->NextInBundle = nullptr;
    BundleMember->UnscheduledDepsInBundle = BundleMember->UnscheduledDeps;
    if (BundleMember->UnscheduledDepsInBundle == 0) {
      ReadyInsts.insert(BundleMember);
    }
    BundleMember = Next;
  }
}

BoUpSLP::ScheduleData *BoUpSLP::BlockScheduling::allocateScheduleDataChunks() {
  // Allocate a new ScheduleData for the instruction.
  if (ChunkPos >= ChunkSize) {
    ScheduleDataChunks.push_back(llvm::make_unique<ScheduleData[]>(ChunkSize));
    ChunkPos = 0;
  }
  return &(ScheduleDataChunks.back()[ChunkPos++]);
}

bool BoUpSLP::BlockScheduling::extendSchedulingRegion(Value *V,
                                                      Value *OpValue) {
  if (getScheduleData(V, isOneOf(OpValue, V)))
    return true;
  Instruction *I = dyn_cast<Instruction>(V);
  assert(I && "bundle member must be an instruction");
  assert(!isa<PHINode>(I) && "phi nodes don't need to be scheduled");
  auto &&CheckSheduleForI = [this, OpValue](Instruction *I) -> bool {
    ScheduleData *ISD = getScheduleData(I);
    if (!ISD)
      return false;
    assert(isInSchedulingRegion(ISD) &&
           "ScheduleData not in scheduling region");
    ScheduleData *SD = allocateScheduleDataChunks();
    SD->Inst = I;
    SD->init(SchedulingRegionID, OpValue);
    ExtraScheduleDataMap[I][OpValue] = SD;
    return true;
  };
  if (CheckSheduleForI(I))
    return true;
  if (!ScheduleStart) {
    // It's the first instruction in the new region.
    initScheduleData(I, I->getNextNode(), nullptr, nullptr);
    ScheduleStart = I;
    ScheduleEnd = I->getNextNode();
    if (isOneOf(OpValue, I) != I)
      CheckSheduleForI(I);
    assert(ScheduleEnd && "tried to vectorize a TerminatorInst?");
    LLVM_DEBUG(dbgs() << "Revec:  initialize schedule region to " << *I << "\n");
    return true;
  }
  // Search up and down at the same time, because we don't know if the new
  // instruction is above or below the existing scheduling region.
  BasicBlock::reverse_iterator UpIter =
      ++ScheduleStart->getIterator().getReverse();
  BasicBlock::reverse_iterator UpperEnd = BB->rend();
  BasicBlock::iterator DownIter = ScheduleEnd->getIterator();
  BasicBlock::iterator LowerEnd = BB->end();
  while (true) {
    if (++ScheduleRegionSize > ScheduleRegionSizeLimit) {
      LLVM_DEBUG(dbgs() << "Revec:  exceeded schedule region size limit\n");
      return false;
    }

    if (UpIter != UpperEnd) {
      if (&*UpIter == I) {
        initScheduleData(I, ScheduleStart, nullptr, FirstLoadStoreInRegion);
        ScheduleStart = I;
        if (isOneOf(OpValue, I) != I)
          CheckSheduleForI(I);
        LLVM_DEBUG(dbgs() << "Revec:  extend schedule region start to " << *I << "\n");
        return true;
      }
      UpIter++;
    }
    if (DownIter != LowerEnd) {
      if (&*DownIter == I) {
        initScheduleData(ScheduleEnd, I->getNextNode(), LastLoadStoreInRegion,
                         nullptr);
        ScheduleEnd = I->getNextNode();
        if (isOneOf(OpValue, I) != I)
          CheckSheduleForI(I);
        assert(ScheduleEnd && "tried to vectorize a TerminatorInst?");
        LLVM_DEBUG(dbgs() << "Revec:  extend schedule region end to " << *I << "\n");
        return true;
      }
      DownIter++;
    }
    assert((UpIter != UpperEnd || DownIter != LowerEnd) &&
           "instruction not found in block");
  }
  return true;
}

void BoUpSLP::BlockScheduling::initScheduleData(Instruction *FromI,
                                                Instruction *ToI,
                                                ScheduleData *PrevLoadStore,
                                                ScheduleData *NextLoadStore) {
  ScheduleData *CurrentLoadStore = PrevLoadStore;
  for (Instruction *I = FromI; I != ToI; I = I->getNextNode()) {
    ScheduleData *SD = ScheduleDataMap[I];
    if (!SD) {
      SD = allocateScheduleDataChunks();
      ScheduleDataMap[I] = SD;
      SD->Inst = I;
    }
    assert(!isInSchedulingRegion(SD) &&
           "new ScheduleData already in scheduling region");
    SD->init(SchedulingRegionID, I);

    if (I->mayReadOrWriteMemory() &&
        (!isa<IntrinsicInst>(I) ||
         cast<IntrinsicInst>(I)->getIntrinsicID() != Intrinsic::sideeffect)) {
      // Update the linked list of memory accessing instructions.
      if (CurrentLoadStore) {
        CurrentLoadStore->NextLoadStore = SD;
      } else {
        FirstLoadStoreInRegion = SD;
      }
      CurrentLoadStore = SD;
    }
  }
  if (NextLoadStore) {
    if (CurrentLoadStore)
      CurrentLoadStore->NextLoadStore = NextLoadStore;
  } else {
    LastLoadStoreInRegion = CurrentLoadStore;
  }
}

void BoUpSLP::BlockScheduling::calculateDependencies(ScheduleData *SD,
                                                     bool InsertInReadyList,
                                                     BoUpSLP *SLP) {
  assert(SD->isSchedulingEntity());

  SmallVector<ScheduleData *, 10> WorkList;
  WorkList.push_back(SD);

  while (!WorkList.empty()) {
    ScheduleData *SD = WorkList.back();
    WorkList.pop_back();

    ScheduleData *BundleMember = SD;
    while (BundleMember) {
      assert(isInSchedulingRegion(BundleMember));
      if (!BundleMember->hasValidDependencies()) {

        LLVM_DEBUG(dbgs() << "Revec:       update deps of " << *BundleMember << "\n");
        BundleMember->Dependencies = 0;
        BundleMember->resetUnscheduledDeps();

        // Handle def-use chain dependencies.
        if (BundleMember->OpValue != BundleMember->Inst) {
          ScheduleData *UseSD = getScheduleData(BundleMember->Inst);
          if (UseSD && isInSchedulingRegion(UseSD->FirstInBundle)) {
            BundleMember->Dependencies++;
            ScheduleData *DestBundle = UseSD->FirstInBundle;
            if (!DestBundle->IsScheduled)
              BundleMember->incrementUnscheduledDeps(1);
            if (!DestBundle->hasValidDependencies())
              WorkList.push_back(DestBundle);
          }
        } else {
          for (User *U : BundleMember->Inst->users()) {
            if (isa<Instruction>(U)) {
              ScheduleData *UseSD = getScheduleData(U);
              if (UseSD && isInSchedulingRegion(UseSD->FirstInBundle)) {
                BundleMember->Dependencies++;
                ScheduleData *DestBundle = UseSD->FirstInBundle;
                if (!DestBundle->IsScheduled)
                  BundleMember->incrementUnscheduledDeps(1);
                if (!DestBundle->hasValidDependencies())
                  WorkList.push_back(DestBundle);
              }
            } else {
              // I'm not sure if this can ever happen. But we need to be safe.
              // This lets the instruction/bundle never be scheduled and
              // eventually disable vectorization.

              // TODO: remove and log
              assert(false && "Unexpected user type - not an Instruction");
              BundleMember->Dependencies++;
              BundleMember->incrementUnscheduledDeps(1);
            }
          }
        }

        // Handle the memory dependencies.
        ScheduleData *DepDest = BundleMember->NextLoadStore;
        if (DepDest) {
          Instruction *SrcInst = BundleMember->Inst;
          MemoryLocation SrcLoc = getLocation(SrcInst, SLP->AA);
          bool SrcMayWrite = BundleMember->Inst->mayWriteToMemory();
          unsigned numAliased = 0;
          unsigned DistToSrc = 1;

          while (DepDest) {
            assert(isInSchedulingRegion(DepDest));

            // We have two limits to reduce the complexity:
            // 1) AliasedCheckLimit: It's a small limit to reduce calls to
            //    SLP->isAliased (which is the expensive part in this loop).
            // 2) MaxMemDepDistance: It's for very large blocks and it aborts
            //    the whole loop (even if the loop is fast, it's quadratic).
            //    It's important for the loop break condition (see below) to
            //    check this limit even between two read-only instructions.
            if (DistToSrc >= MaxMemDepDistance ||
                    ((SrcMayWrite || DepDest->Inst->mayWriteToMemory()) &&
                     (numAliased >= AliasedCheckLimit ||
                      SLP->isAliased(SrcLoc, SrcInst, DepDest->Inst)))) {

              LLVM_DEBUG(dbgs() << "Revec: aliased memory locations for instructions "
                           << *SrcInst << ", " << *DepDest->Inst << "\n");

              // We increment the counter only if the locations are aliased
              // (instead of counting all alias checks). This gives a better
              // balance between reduced runtime and accurate dependencies.
              numAliased++;

              DepDest->MemoryDependencies.push_back(BundleMember);
              BundleMember->Dependencies++;
              ScheduleData *DestBundle = DepDest->FirstInBundle;
              if (!DestBundle->IsScheduled) {
                BundleMember->incrementUnscheduledDeps(1);
              }
              if (!DestBundle->hasValidDependencies()) {
                WorkList.push_back(DestBundle);
              }
            }
            DepDest = DepDest->NextLoadStore;

            // Example, explaining the loop break condition: Let's assume our
            // starting instruction is i0 and MaxMemDepDistance = 3.
            //
            //                      +--------v--v--v
            //             i0,i1,i2,i3,i4,i5,i6,i7,i8
            //             +--------^--^--^
            //
            // MaxMemDepDistance let us stop alias-checking at i3 and we add
            // dependencies from i0 to i3,i4,.. (even if they are not aliased).
            // Previously we already added dependencies from i3 to i6,i7,i8
            // (because of MaxMemDepDistance). As we added a dependency from
            // i0 to i3, we have transitive dependencies from i0 to i6,i7,i8
            // and we can abort this loop at i6.
            if (DistToSrc >= 2 * MaxMemDepDistance)
              break;
            DistToSrc++;
          }
        }
      }
      BundleMember = BundleMember->NextInBundle;
    }
    if (InsertInReadyList && SD->isReady()) {
      ReadyInsts.push_back(SD);
      LLVM_DEBUG(dbgs() << "Revec:     gets ready on update: " << *SD->Inst << "\n");
    }
  }
}

void BoUpSLP::BlockScheduling::resetSchedule() {
  assert(ScheduleStart &&
         "tried to reset schedule on block which has not been scheduled");
  for (Instruction *I = ScheduleStart; I != ScheduleEnd; I = I->getNextNode()) {
    doForAllOpcodes(I, [&](ScheduleData *SD) {
      assert(isInSchedulingRegion(SD) &&
             "ScheduleData not in scheduling region");
      SD->IsScheduled = false;
      SD->resetUnscheduledDeps();
    });
  }
  ReadyInsts.clear();
}

void BoUpSLP::scheduleBlock(BlockScheduling *BS) {
  if (!BS->ScheduleStart)
    return;

  LLVM_DEBUG(dbgs() << "Revec: schedule block " << BS->BB->getName() << "\n");

  BS->resetSchedule();

  // For the real scheduling we use a more sophisticated ready-list: it is
  // sorted by the original instruction location. This lets the final schedule
  // be as  close as possible to the original instruction order.
  struct ScheduleDataCompare {
    bool operator()(ScheduleData *SD1, ScheduleData *SD2) const {
      return SD2->SchedulingPriority < SD1->SchedulingPriority;
    }
  };
  std::set<ScheduleData *, ScheduleDataCompare> ReadyInsts;

  // Ensure that all dependency data is updated and fill the ready-list with
  // initial instructions.
  int Idx = 0;
  int NumToSchedule = 0;
  for (auto *I = BS->ScheduleStart; I != BS->ScheduleEnd;
       I = I->getNextNode()) {
    BS->doForAllOpcodes(I, [this, &Idx, &NumToSchedule, BS](ScheduleData *SD) {
      assert(SD->isPartOfBundle() ==
                 (getTreeEntry(SD->Inst) != nullptr) &&
             "scheduler and vectorizer bundle mismatch");
      SD->FirstInBundle->SchedulingPriority = Idx++;
      if (SD->isSchedulingEntity()) {
        BS->calculateDependencies(SD, false, this);
        NumToSchedule++;
      }
    });
  }
  BS->initialFillReadyList(ReadyInsts);

  Instruction *LastScheduledInst = BS->ScheduleEnd;

  // Do the "real" scheduling.
  while (!ReadyInsts.empty()) {
    ScheduleData *picked = *ReadyInsts.begin();
    ReadyInsts.erase(ReadyInsts.begin());

    // Move the scheduled instruction(s) to their dedicated places, if not
    // there yet.
    ScheduleData *BundleMember = picked;
    while (BundleMember) {
      Instruction *pickedInst = BundleMember->Inst;
      if (LastScheduledInst->getNextNode() != pickedInst) {
        BS->BB->getInstList().remove(pickedInst);
        BS->BB->getInstList().insert(LastScheduledInst->getIterator(),
                                     pickedInst);
      }
      LastScheduledInst = pickedInst;
      BundleMember = BundleMember->NextInBundle;
    }

    BS->schedule(picked, ReadyInsts);
    NumToSchedule--;
  }
  assert(NumToSchedule == 0 && "could not schedule all instructions");

  // Avoid duplicate scheduling of the block.
  BS->ScheduleStart = nullptr;
}

unsigned BoUpSLP::getVectorSize(Value *V) {
  // If V is a store, just return the width of the stored value without
  // traversing the expression tree. This is the common case.
  if (auto *Store = dyn_cast<StoreInst>(V))
    return DL->getTypeSizeInBits(Store->getValueOperand()->getType());

  // TODO: In which cases is the value not a store?
  return 0u;
}

unsigned BoUpSLP::getVectorElementSize(Value *V) {
  // If V is a store, just return the width of the stored value without
  // traversing the expression tree. This is the common case.
  if (auto *Store = dyn_cast<StoreInst>(V))
    return DL->getTypeSizeInBits(Store->getValueOperand()->getType());

  // If V is not a store, we can traverse the expression tree to find loads
  // that feed it. The type of the loaded value may indicate a more suitable
  // width than V's type. We want to base the vector element size on the width
  // of memory operations where possible.
  SmallVector<Instruction *, 16> Worklist;
  SmallPtrSet<Instruction *, 16> Visited;
  if (auto *I = dyn_cast<Instruction>(V))
    Worklist.push_back(I);

  // Traverse the expression tree in bottom-up order looking for loads. If we
  // encounter an instruciton we don't yet handle, we give up.
  auto MaxWidth = 0u;
  auto FoundUnknownInst = false;
  while (!Worklist.empty() && !FoundUnknownInst) {
    auto *I = Worklist.pop_back_val();
    Visited.insert(I);

    // We should only be looking at scalar instructions here. If the current
    // instruction has a vector type, give up.
    auto *Ty = I->getType();
    if (isa<VectorType>(Ty))
      FoundUnknownInst = true;

    // If the current instruction is a load, update MaxWidth to reflect the
    // width of the loaded value.
    else if (isa<LoadInst>(I))
      MaxWidth = std::max<unsigned>(MaxWidth, DL->getTypeSizeInBits(Ty));

    // Otherwise, we need to visit the operands of the instruction. We only
    // handle the interesting cases from buildTree here. If an operand is an
    // instruction we haven't yet visited, we add it to the worklist.
    else if (isa<PHINode>(I) || isa<CastInst>(I) || isa<GetElementPtrInst>(I) ||
             isa<CmpInst>(I) || isa<SelectInst>(I) || isa<BinaryOperator>(I)) {
      for (Use &U : I->operands())
        if (auto *J = dyn_cast<Instruction>(U.get()))
          if (!Visited.count(J))
            Worklist.push_back(J);
    }

    // If we don't yet handle the instruction, give up.
    else
      FoundUnknownInst = true;
  }

  // If we didn't encounter a memory access in the expression tree, or if we
  // gave up for some reason, just return the width of V.
  if (!MaxWidth || FoundUnknownInst)
    return DL->getTypeSizeInBits(V->getType());

  // Otherwise, return the maximum width we found.
  return MaxWidth;
}

namespace {

/// The Revectorizer Pass.
struct Revectorizer : public FunctionPass {
  RevectorizerPass Impl;

  /// Pass identification, replacement for typeid
  static char ID;

  explicit Revectorizer() : FunctionPass(ID) {
    initializeRevectorizerPass(*PassRegistry::getPassRegistry());
  }

  bool doInitialization(Module &M) override {
    return false;
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto *TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
    auto *TLIP = getAnalysisIfAvailable<TargetLibraryInfoWrapperPass>();
    auto *TLI = TLIP ? &TLIP->getTLI() : nullptr;
    auto *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
    auto *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto *AC = &getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    auto *DB = &getAnalysis<DemandedBitsWrapperPass>().getDemandedBits();
    auto *ORE = &getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();

    return Impl.runImpl(F, SE, TTI, TLI, AA, LI, DT, AC, DB, ORE);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    FunctionPass::getAnalysisUsage(AU);
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<DemandedBitsWrapperPass>();
    AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<AAResultsWrapperPass>();
    AU.addPreserved<GlobalsAAWrapperPass>();
    AU.setPreservesCFG();
  }
};

} // end anonymous namespace

PreservedAnalyses RevectorizerPass::run(Function &F, FunctionAnalysisManager &AM) {
  auto *SE = &AM.getResult<ScalarEvolutionAnalysis>(F);
  auto *TTI = &AM.getResult<TargetIRAnalysis>(F);
  auto *TLI = AM.getCachedResult<TargetLibraryAnalysis>(F);
  auto *AA = &AM.getResult<AAManager>(F);
  auto *LI = &AM.getResult<LoopAnalysis>(F);
  auto *DT = &AM.getResult<DominatorTreeAnalysis>(F);
  auto *AC = &AM.getResult<AssumptionAnalysis>(F);
  auto *DB = &AM.getResult<DemandedBitsAnalysis>(F);
  auto *ORE = &AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  bool Changed = runImpl(F, SE, TTI, TLI, AA, LI, DT, AC, DB, ORE);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<AAManager>();
  PA.preserve<GlobalsAA>();
  return PA;
}

bool RevectorizerPass::runImpl(Function &F, ScalarEvolution *SE_,
                                TargetTransformInfo *TTI_,
                                TargetLibraryInfo *TLI_, AliasAnalysis *AA_,
                                LoopInfo *LI_, DominatorTree *DT_,
                                AssumptionCache *AC_, DemandedBits *DB_,
                                OptimizationRemarkEmitter *ORE_) {
  SE = SE_;
  TTI = TTI_;
  TLI = TLI_;
  AA = AA_;
  LI = LI_;
  DT = DT_;
  AC = AC_;
  DB = DB_;
  DL = &F.getParent()->getDataLayout();

  Stores.clear();

  // If the target claims to have no vector registers don't attempt
  // vectorization.
  if (!TTI->getNumberOfRegisters(true)) {
    LLVM_DEBUG(dbgs() << "Revec: Not revectorizing " << F.getName() << " as target has no vector registers\n");
    return false;
  }

  // Don't vectorize when the attribute NoImplicitFloat is used.
  if (F.hasFnAttribute(Attribute::NoImplicitFloat)) {
    LLVM_DEBUG(dbgs() << "Revec: Not revectorizing " << F.getName() << " as it has attribute NoImplicitFloat\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "\n================================\n");
  LLVM_DEBUG(dbgs() << "Revec: Analyzing blocks in " << F.getName() << ".\n");
  LLVM_DEBUG(dbgs() << "Revec: Function IR:\n");
  LLVM_DEBUG(dbgs() << F << "\n");

  // Use the bottom up slp vectorizer to construct chains that start with
  // store instructions.
  BoUpSLP R(&F, SE, TTI, TLI, AA, LI, DT, AC, DB, DL, ORE_);

  // A general note: the vectorizer must use BoUpSLP::eraseInstruction() to
  // delete instructions.

  bool Changed = false;

  // Scan the blocks in the function in post order.
  for (auto BB : post_order(&F.getEntryBlock())) {
    collectSeedInstructions(BB, R);

    // Vectorize trees that end at stores.
    if (!Stores.empty()) {
      LLVM_DEBUG(dbgs() << "Revec: Found stores for " << Stores.size()
                   << " underlying objects.\n");
      Changed |= vectorizeStoreChains(R);
    }

    // Vectorize trees that end at reductions.
    Changed |= vectorizeChainsInBlock(BB, R);

  }

  if (Changed) {
    R.optimizeGatherSequence();
    LLVM_DEBUG(dbgs() << "Revec: revectorized \"" << F.getName() << "\"\n");
    LLVM_DEBUG(dbgs() << "Revec: revectorized Function IR:\n");
    LLVM_DEBUG(dbgs() << F << "\n\n");
    LLVM_DEBUG(verifyFunction(F));
  }

  return Changed;
}

/// \brief Check that the Values in the slice in VL array are still existent in
/// the WeakTrackingVH array.
/// Vectorization of part of the VL array may cause later values in the VL array
/// to become invalid. We track when this has happened in the WeakTrackingVH
/// array.
static bool hasValueBeenRAUWed(ArrayRef<Value *> VL,
                               ArrayRef<WeakTrackingVH> VH, unsigned SliceBegin,
                               unsigned SliceSize) {
  VL = VL.slice(SliceBegin, SliceSize);
  VH = VH.slice(SliceBegin, SliceSize);
  return !std::equal(VL.begin(), VL.end(), VH.begin());
}

bool RevectorizerPass::vectorizeStoreChain(ArrayRef<Value *> Chain, BoUpSLP &R,
                                            unsigned VecRegSize) {
  const unsigned ChainLen = Chain.size();
  LLVM_DEBUG(dbgs() << "Revec: Analyzing a store chain of length " << ChainLen
        << "\n");
  for (auto *StoreValue : Chain) {
    LLVM_DEBUG(dbgs() << "Revec:   " << *StoreValue << "\n");
  }

  // TODO: getVectorElementSize checks that the value is not of a vector type.
  //       Switch to getVectorSize (equivalent for now)
  const unsigned Sz = R.getVectorElementSize(Chain[0]);
  const unsigned VF = VecRegSize / Sz;

  LLVM_DEBUG(dbgs() << "Revec: Chain[0] size: " << Sz << ", VF: " << VF << "\n");

  if (!isPowerOf2_32(Sz) || VF < 2) {
    LLVM_DEBUG(dbgs() << "Revec: Unable to vectorize store chain, as store 0 stores non-power-of-two size " << Sz
                      << ", or VF " << VF << " < 2\n");
    LLVM_DEBUG(dbgs() << "Revec:  Chain[0] = " << *Chain[0] << "\n");
    return false;
  }

  // Keep track of values that were deleted by vectorizing in the loop below.
  const SmallVector<WeakTrackingVH, 8> TrackValues(Chain.begin(), Chain.end());

  bool Changed = false;
#if 0
  // Look for profitable vectorizable trees at all offsets, starting at zero.
  for (unsigned i = 0, e = ChainLen; i + VF <= e; ++i) {
#else
  // Look for profitable vectorizable trees at all offsets, starting at at the end of the chain.
  if (ChainLen < VF) {
    LLVM_DEBUG(dbgs() << "Revec: Unable to vectorize store chain, as chain length is less than VF.\n");
    return false;
  }

  for (int i = ChainLen - VF; i >= 0; --i) {
#endif

    // Check that a previous iteration of this loop did not delete the Value.
    if (hasValueBeenRAUWed(Chain, TrackValues, i, VF)) {
      LLVM_DEBUG(dbgs() << "Revec: Store has been RAUWed, skipping offset " << i << "\n");
      continue;
    }

    LLVM_DEBUG(dbgs() << "Revec: Analyzing " << VF << " stores at offset " << i
          << "\n");
    ArrayRef<Value *> Operands = Chain.slice(i, VF);

    R.buildTree(Operands);
    if (R.isTreeTinyAndNotFullyVectorizable())
      continue;

    int Cost = R.getTreeCost();

    LLVM_DEBUG(dbgs() << "Revec: Found cost=" << Cost << " for VF=" << VF << "\n");
    if (Cost < -RevecCostThreshold || ForceRevec) {
      LLVM_DEBUG(dbgs() << "Revec: Decided to vectorize cost=" << Cost << "\n");

      using namespace ore;

      R.getORE()->emit(OptimizationRemark(SV_NAME, "StoresVectorized",
                                          cast<StoreInst>(Chain[i]))
                       << "Stores revectorized with cost " << NV("Cost", Cost)
                       << " and with tree size "
                       << NV("TreeSize", R.getTreeSize()));

      R.vectorizeTree();

      // Move to the next bundle.
      i -= VF - 1;
      Changed = true;
    }
  }

  return Changed;
}

bool RevectorizerPass::vectorizeStores(ArrayRef<StoreInst *> Stores,
                                        BoUpSLP &R) {
  SetVector<StoreInst *> Heads;
  SmallDenseSet<StoreInst *> Tails;
  SmallDenseMap<StoreInst *, StoreInst *> ConsecutiveChain;

  // We may run into multiple chains that merge into a single chain. We mark the
  // stores that we vectorized so that we don't visit the same store twice.
  BoUpSLP::ValueSet VectorizedStores;
  bool Changed = false;

  // Do a quadratic search on all of the given stores in reverse order and find
  // all of the pairs of stores that follow each other.
  LLVM_DEBUG(dbgs() << "Revec: Searching for adjacencies in store chain of length " << Stores.size()
                    << "\n");
  for (StoreInst *Store : Stores) {
      LLVM_DEBUG(dbgs() << "Revec:   " << *Store << "\n");
  }

  SmallVector<unsigned, 16> IndexQueue;
  unsigned E = Stores.size();
  IndexQueue.resize(E - 1);
  for (unsigned I = E; I > 0; --I) {
    unsigned Idx = I - 1;
    // If a store has multiple consecutive store candidates, search Stores
    // array according to the sequence: Idx-1, Idx+1, Idx-2, Idx+2, ...
    // This is because usually pairing with immediate succeeding or preceding
    // candidate create the best chance to find slp vectorization opportunity.
    unsigned Offset = 1;
    unsigned Cnt = 0;
    for (unsigned J = 0; J < E - 1; ++J, ++Offset) {
      if (Idx >= Offset) {
        IndexQueue[Cnt] = Idx - Offset;
        ++Cnt;
      }
      if (Idx + Offset < E) {
        IndexQueue[Cnt] = Idx + Offset;
        ++Cnt;
      }
    }

    for (auto K : IndexQueue) {
      if (isConsecutiveAccess(Stores[K], Stores[Idx], *DL, *SE)) {
        Tails.insert(Stores[Idx]);
        Heads.insert(Stores[K]);
        ConsecutiveChain[Stores[K]] = Stores[Idx];
        break;
      }
    }
  }

  LLVM_DEBUG(dbgs() << "Revec: Counted " << Heads.size() << " heads:\n");
  for (auto *SI : Heads) {
    LLVM_DEBUG(dbgs() << "Revec:   " << *SI << "\n");
  }

  // For stores that start but don't end a link in the chain:
  for (auto *SI : Heads) {
    if (Tails.count(SI))
      continue;

    // We found a store instr that starts a chain. Now follow the chain and try
    // to vectorize it.
    BoUpSLP::ValueList Operands;
    StoreInst *I = SI;
    // Collect the chain into a list.
    while ((Tails.count(I) || Heads.count(I)) && !VectorizedStores.count(I)) {
      Operands.push_back(I);
      // Move to the next value in the chain.
      I = ConsecutiveChain[I];
    }

    LLVM_DEBUG(dbgs() << "Revec: Grabbing packs from store chain of length " << Operands.size() << "\n");

    // FIXME: Is division-by-2 the correct step? Should we assert that the
    // register size is a power-of-2?
    for (unsigned Size = R.getMaxVecRegSize(); Size >= R.getMinVecRegSize();
         Size /= 2) {
      if (vectorizeStoreChain(Operands, R, Size)) {
        // Mark the vectorized stores so that we don't vectorize them again.
        VectorizedStores.insert(Operands.begin(), Operands.end());
        Changed = true;
        break;
      }
    }
  }

  return Changed;
}

void RevectorizerPass::collectSeedInstructions(BasicBlock *BB, BoUpSLP &R) {
  // Initialize the collections. We will make a single pass over the block.
  Stores.clear();

  // Visit the store and getelementptr instructions in BB and organize them in
  // Stores and GEPs according to the underlying objects of their pointer
  // operands.
  LLVM_DEBUG(dbgs() << "Revec: Collecting seed instructions from basic block:\n");
  for (Instruction &I : *BB) {
    LLVM_DEBUG(dbgs() << "Revec:   " << I << "\n");
    // Ignore store instructions that are volatile or have a pointer operand
    // that doesn't point to a vector type.
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!SI->isSimple())
        continue;
      Type *valueTy = SI->getValueOperand()->getType();
      if (!(isValidElementType(valueTy) &&
            R.isNarrowVectorType(valueTy))) {
        continue;
      }
      Stores[GetUnderlyingObject(SI->getPointerOperand(), *DL)].push_back(SI);
    }
  }
}

bool RevectorizerPass::vectorizeStoreChains(BoUpSLP &R) {
  bool Changed = false;
  // Attempt to sort and vectorize each of the store-groups.
#if 1
  //for (auto& StorePair : llvm::reverse(Stores)) {
  for (auto& StorePair : Stores) {
    if (StorePair.second.size() < 2)
      continue;

    LLVM_DEBUG(dbgs() << "Revec: Analyzing a store chain of length "
        << StorePair.second.size() << " in vectorizeStoreChains.\n");

    // Process the stores to this underlying object in chunks of 256 at most
    int CI = std::max<int>((int) StorePair.second.size() - MaxStoreChunkSize, 0);
    int CE = StorePair.second.size();
    while (CI >= 0) {
      unsigned Len = (unsigned) std::min<int>(CE - CI, MaxStoreChunkSize);
      Changed |= vectorizeStores(makeArrayRef(&StorePair.second[CI], Len), R);
      CI -= MaxStoreChunkSize;
    }
  }
#else
  for (StoreListMap::iterator it = Stores.begin(), e = Stores.end(); it != e;
       ++it) {
    if (it->second.size() < 2)
      continue;

    LLVM_DEBUG(dbgs() << "Revec: Analyzing a store chain of length "
          << it->second.size() << " in vectorizeStoreChains.\n");

    // Process the stores in chunks of 32.
    // TODO: The limit of 32 inhibits greater vectorization factors.
    //       For example, AVX512 supports v64i8. Increasing this limit, however,
    //       may cause a significant compile-time increase.
    for (unsigned CI = 0, CE = it->second.size(); CI < CE; CI+=32) {
      unsigned Len = std::min<unsigned>(CE - CI, 32);
      Changed |= vectorizeStores(makeArrayRef(&it->second[CI], Len), R);
    }
  }
#endif
  return Changed;
}


// Reduction Start

//some forward declarations

static Value *createRdxShuffleMask(unsigned VecLen, unsigned NumEltsToRdx,
                                   bool IsPairwise, bool IsLeft,
                                   IRBuilder<> &Builder);


 static void printBundle(ArrayRef<Value *> V){
   for(unsigned i = 0; i < V.size(); i++)
     REVEC_DEBUG(dbgs() << *V[i] << "\n");
 }


namespace {

/// Model horizontal reductions.
///
/// A horizontal reduction is a tree of reduction operations (currently add and
/// fadd) that has operations that can be put into a vector as its leaf.
/// For example, this tree:
///
/// mul mul mul mul
///  \  /    \  /
///   +       +
///    \     /
///       +
/// This tree has "mul" as its reduced values and "+" as its reduction
/// operations. A reduction might be feeding into a store or a binary operation
/// feeding a phi.
///    ...
///    \  /
///     +
///     |
///  phi +=
///
///  Or:
///    ...
///    \  /
///     +
///     |
///   *p =
///
class HorizontalReduction {
  using ReductionOpsType = SmallVector<Value *, 16>;
  using ReductionOpsListType = SmallVector<ReductionOpsType, 2>;
  ReductionOpsListType  ReductionOps;
  SmallVector<Value *, 32> ReducedVals;
  // Use map vector to make stable output.
  MapVector<Instruction *, Value *> ExtraArgs;

  /// Kind of the reduction data.
  enum ReductionKind {
    RK_None,       /// Not a reduction.
    RK_Arithmetic, /// Binary reduction data.
    RK_Min,        /// Minimum reduction data.
    RK_UMin,       /// Unsigned minimum reduction data.
    RK_Max,        /// Maximum reduction data.
    RK_UMax,       /// Unsigned maximum reduction data.
  };

  /// Contains info about operation, like its opcode, left and right operands.
  class OperationData {
    /// Opcode of the instruction.
    unsigned Opcode = 0;

    /// Left operand of the reduction operation.
    Value *LHS = nullptr;

    /// Right operand of the reduction operation.
    Value *RHS = nullptr;

    /// Kind of the reduction operation.
    ReductionKind Kind = RK_None;

    /// True if float point min/max reduction has no NaNs.
    bool NoNaN = false;

    /// Checks if the reduction operation can be vectorized.
    bool isVectorizable() const {
      return LHS && RHS &&
             // We currently only support adds && min/max reductions.
             ((Kind == RK_Arithmetic &&
               (Opcode == Instruction::Add || Opcode == Instruction::FAdd)) ||
              ((Opcode == Instruction::ICmp || Opcode == Instruction::FCmp) &&
               (Kind == RK_Min || Kind == RK_Max)) ||
              (Opcode == Instruction::ICmp &&
               (Kind == RK_UMin || Kind == RK_UMax)));
    }

    /// Creates reduction operation with the current opcode.
    Value *createOp(IRBuilder<> &Builder, const Twine &Name) const {
      assert(isVectorizable() &&
             "Expected add|fadd or min/max reduction operation.");
      Value *Cmp;
      switch (Kind) {
      case RK_Arithmetic:
        return Builder.CreateBinOp((Instruction::BinaryOps)Opcode, LHS, RHS,
                                   Name);
      case RK_Min:
        Cmp = Opcode == Instruction::ICmp ? Builder.CreateICmpSLT(LHS, RHS)
                                          : Builder.CreateFCmpOLT(LHS, RHS);
        break;
      case RK_Max:
        Cmp = Opcode == Instruction::ICmp ? Builder.CreateICmpSGT(LHS, RHS)
                                          : Builder.CreateFCmpOGT(LHS, RHS);
        break;
      case RK_UMin:
        assert(Opcode == Instruction::ICmp && "Expected integer types.");
        Cmp = Builder.CreateICmpULT(LHS, RHS);
        break;
      case RK_UMax:
        assert(Opcode == Instruction::ICmp && "Expected integer types.");
        Cmp = Builder.CreateICmpUGT(LHS, RHS);
        break;
      case RK_None:
        llvm_unreachable("Unknown reduction operation.");
      }
      return Builder.CreateSelect(Cmp, LHS, RHS, Name);
    }

  public:
    explicit OperationData() = default;

    /// Construction for reduced values. They are identified by opcode only and
    /// don't have associated LHS/RHS values.
    explicit OperationData(Value *V) {
      if (auto *I = dyn_cast<Instruction>(V))
        Opcode = I->getOpcode();
    }

    /// Constructor for reduction operations with opcode and its left and
    /// right operands.
    OperationData(unsigned Opcode, Value *LHS, Value *RHS, ReductionKind Kind,
                  bool NoNaN = false)
        : Opcode(Opcode), LHS(LHS), RHS(RHS), Kind(Kind), NoNaN(NoNaN) {
      assert(Kind != RK_None && "One of the reduction operations is expected.");
    }

    explicit operator bool() const { return Opcode; }

    /// Get the index of the first operand.
    unsigned getFirstOperandIndex() const {
      assert(!!*this && "The opcode is not set.");
      switch (Kind) {
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax:
        return 1;
      case RK_Arithmetic:
      case RK_None:
        break;
      }
      return 0;
    }

    /// Total number of operands in the reduction operation.
    unsigned getNumberOfOperands() const {
      assert(Kind != RK_None && !!*this && LHS && RHS &&
             "Expected reduction operation.");
      switch (Kind) {
      case RK_Arithmetic:
        return 2;
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax:
        return 3;
      case RK_None:
        break;
      }
      llvm_unreachable("Reduction kind is not set");
    }

    /// Checks if the operation has the same parent as \p P.
    bool hasSameParent(Instruction *I, Value *P, bool IsRedOp) const {
      assert(Kind != RK_None && !!*this && LHS && RHS &&
             "Expected reduction operation.");
      if (!IsRedOp)
        return I->getParent() == P;
      switch (Kind) {
      case RK_Arithmetic:
        // Arithmetic reduction operation must be used once only.
        return I->getParent() == P;
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax: {
        // SelectInst must be used twice while the condition op must have single
        // use only.
        auto *Cmp = cast<Instruction>(cast<SelectInst>(I)->getCondition());
        return I->getParent() == P && Cmp && Cmp->getParent() == P;
      }
      case RK_None:
        break;
      }
      llvm_unreachable("Reduction kind is not set");
    }
    /// Expected number of uses for reduction operations/reduced values.
    bool hasRequiredNumberOfUses(Instruction *I, bool IsReductionOp) const {
      assert(Kind != RK_None && !!*this && LHS && RHS &&
             "Expected reduction operation.");
      switch (Kind) {
      case RK_Arithmetic:
        return I->hasOneUse();
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax:
        return I->hasNUses(2) &&
               (!IsReductionOp ||
                cast<SelectInst>(I)->getCondition()->hasOneUse());
      case RK_None:
        break;
      }
      llvm_unreachable("Reduction kind is not set");
    }

    /// Initializes the list of reduction operations.
    void initReductionOps(ReductionOpsListType &ReductionOps) {
      assert(Kind != RK_None && !!*this && LHS && RHS &&
             "Expected reduction operation.");
      switch (Kind) {
      case RK_Arithmetic:
        ReductionOps.assign(1, ReductionOpsType());
        break;
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax:
        ReductionOps.assign(2, ReductionOpsType());
        break;
      case RK_None:
        llvm_unreachable("Reduction kind is not set");
      }
    }
    /// Add all reduction operations for the reduction instruction \p I.
    void addReductionOps(Instruction *I, ReductionOpsListType &ReductionOps) {
      assert(Kind != RK_None && !!*this && LHS && RHS &&
             "Expected reduction operation.");
      switch (Kind) {
      case RK_Arithmetic:
        ReductionOps[0].emplace_back(I);
        break;
      case RK_Min:
      case RK_UMin:
      case RK_Max:
      case RK_UMax:
        ReductionOps[0].emplace_back(cast<SelectInst>(I)->getCondition());
        ReductionOps[1].emplace_back(I);
        break;
      case RK_None:
        llvm_unreachable("Reduction kind is not set");
      }
    }

    /// Checks if instruction is associative and can be vectorized.
    bool isAssociative(Instruction *I) const {
      assert(Kind != RK_None && *this && LHS && RHS &&
             "Expected reduction operation.");
      switch (Kind) {
      case RK_Arithmetic:
        return I->isAssociative();
      case RK_Min:
      case RK_Max:
        return Opcode == Instruction::ICmp ||
               cast<Instruction>(I->getOperand(0))->isFast();
      case RK_UMin:
      case RK_UMax:
        assert(Opcode == Instruction::ICmp &&
               "Only integer compare operation is expected.");
        return true;
      case RK_None:
        break;
      }
      llvm_unreachable("Reduction kind is not set");
    }

    /// Checks if the reduction operation can be vectorized.
    bool isVectorizable(Instruction *I) const {
      return isVectorizable() && isAssociative(I);
    }

    /// Checks if two operation data are both a reduction op or both a reduced
    /// value.
    bool operator==(const OperationData &OD) {
      assert(((Kind != OD.Kind) || ((!LHS == !OD.LHS) && (!RHS == !OD.RHS))) &&
             "One of the comparing operations is incorrect.");
      return this == &OD || (Kind == OD.Kind && Opcode == OD.Opcode);
    }
    bool operator!=(const OperationData &OD) { return !(*this == OD); }
    void clear() {
      Opcode = 0;
      LHS = nullptr;
      RHS = nullptr;
      Kind = RK_None;
      NoNaN = false;
    }

    /// Get the opcode of the reduction operation.
    unsigned getOpcode() const {
      assert(isVectorizable() && "Expected vectorizable operation.");
      return Opcode;
    }

    /// Get kind of reduction data.
    ReductionKind getKind() const { return Kind; }
    Value *getLHS() const { return LHS; }
    Value *getRHS() const { return RHS; }
    Type *getConditionType() const {
      switch (Kind) {
      case RK_Arithmetic:
        return nullptr;
      case RK_Min:
      case RK_Max:
      case RK_UMin:
      case RK_UMax:
        return CmpInst::makeCmpResultType(LHS->getType());
      case RK_None:
        break;
      }
      llvm_unreachable("Reduction kind is not set");
    }

    /// Creates reduction operation with the current opcode with the IR flags
    /// from \p ReductionOps.
    Value *createOp(IRBuilder<> &Builder, const Twine &Name,
                    const ReductionOpsListType &ReductionOps) const {
      assert(isVectorizable() &&
             "Expected add|fadd or min/max reduction operation.");
      auto *Op = createOp(Builder, Name);
      switch (Kind) {
      case RK_Arithmetic:
        propagateIRFlags(Op, ReductionOps[0]);
        return Op;
      case RK_Min:
      case RK_Max:
      case RK_UMin:
      case RK_UMax:
        if (auto *SI = dyn_cast<SelectInst>(Op))
          propagateIRFlags(SI->getCondition(), ReductionOps[0]);
        propagateIRFlags(Op, ReductionOps[1]);
        return Op;
      case RK_None:
        break;
      }
      llvm_unreachable("Unknown reduction operation.");
    }
    /// Creates reduction operation with the current opcode with the IR flags
    /// from \p I.
    Value *createOp(IRBuilder<> &Builder, const Twine &Name,
                    Instruction *I) const {
      assert(isVectorizable() &&
             "Expected add|fadd or min/max reduction operation.");
      auto *Op = createOp(Builder, Name);
      switch (Kind) {
      case RK_Arithmetic:
        propagateIRFlags(Op, I);
        return Op;
      case RK_Min:
      case RK_Max:
      case RK_UMin:
      case RK_UMax:
        if (auto *SI = dyn_cast<SelectInst>(Op)) {
          propagateIRFlags(SI->getCondition(),
                           cast<SelectInst>(I)->getCondition());
        }
        propagateIRFlags(Op, I);
        return Op;
      case RK_None:
        break;
      }
      llvm_unreachable("Unknown reduction operation.");
    }

    TargetTransformInfo::ReductionFlags getFlags() const {
      TargetTransformInfo::ReductionFlags Flags;
      Flags.NoNaN = NoNaN;
      switch (Kind) {
      case RK_Arithmetic:
        break;
      case RK_Min:
        Flags.IsSigned = Opcode == Instruction::ICmp;
        Flags.IsMaxOp = false;
        break;
      case RK_Max:
        Flags.IsSigned = Opcode == Instruction::ICmp;
        Flags.IsMaxOp = true;
        break;
      case RK_UMin:
        Flags.IsSigned = false;
        Flags.IsMaxOp = false;
        break;
      case RK_UMax:
        Flags.IsSigned = false;
        Flags.IsMaxOp = true;
        break;
      case RK_None:
        llvm_unreachable("Reduction kind is not set");
      }
      return Flags;
    }


    void printOpData(){
      dbgs() << "opcode : " << Opcode << "\n";
      if(LHS)
	dbgs() << "lhs : " << *LHS << "\n";
      if(RHS)
	dbgs() << "rhs : " << *RHS << "\n";
      dbgs() << "kind : " << Kind << " " << RK_None << "\n";
      dbgs() << "isVector : " << isVectorizable() << "\n";
       
    }

  };

  Instruction *ReductionRoot = nullptr;

  /// The operation data of the reduction operation.
  OperationData ReductionData;

  /// The operation data of the values we perform a reduction on.
  OperationData ReducedValueData;

  /// Should we model this reduction as a pairwise reduction tree or a tree that
  /// splits the vector in halves and adds those halves.
  bool IsPairwiseReduction = false;

  /// Checks if the ParentStackElem.first should be marked as a reduction
  /// operation with an extra argument or as extra argument itself.
  void markExtraArg(std::pair<Instruction *, unsigned> &ParentStackElem,
                    Value *ExtraArg) {
    if (ExtraArgs.count(ParentStackElem.first)) {
      ExtraArgs[ParentStackElem.first] = nullptr;
      // We ran into something like:
      // ParentStackElem.first = ExtraArgs[ParentStackElem.first] + ExtraArg.
      // The whole ParentStackElem.first should be considered as an extra value
      // in this case.
      // Do not perform analysis of remaining operands of ParentStackElem.first
      // instruction, this whole instruction is an extra argument.
      ParentStackElem.second = ParentStackElem.first->getNumOperands();
    } else {
      // We ran into something like:
      // ParentStackElem.first += ... + ExtraArg + ...
      ExtraArgs[ParentStackElem.first] = ExtraArg;
    }
  }

  static OperationData getOperationData(Value *V) {
    if (!V)
      return OperationData();

    Value *LHS;
    Value *RHS;
    if (m_BinOp(m_Value(LHS), m_Value(RHS)).match(V)) {
      return OperationData(cast<BinaryOperator>(V)->getOpcode(), LHS, RHS,
                           RK_Arithmetic);
    }
    if (auto *Select = dyn_cast<SelectInst>(V)) {
      // Look for a min/max pattern.
      if (m_UMin(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(Instruction::ICmp, LHS, RHS, RK_UMin);
      } else if (m_SMin(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(Instruction::ICmp, LHS, RHS, RK_Min);
      } else if (m_OrdFMin(m_Value(LHS), m_Value(RHS)).match(Select) ||
                 m_UnordFMin(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(
            Instruction::FCmp, LHS, RHS, RK_Min,
            cast<Instruction>(Select->getCondition())->hasNoNaNs());
      } else if (m_UMax(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(Instruction::ICmp, LHS, RHS, RK_UMax);
      } else if (m_SMax(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(Instruction::ICmp, LHS, RHS, RK_Max);
      } else if (m_OrdFMax(m_Value(LHS), m_Value(RHS)).match(Select) ||
                 m_UnordFMax(m_Value(LHS), m_Value(RHS)).match(Select)) {
        return OperationData(
            Instruction::FCmp, LHS, RHS, RK_Max,
            cast<Instruction>(Select->getCondition())->hasNoNaNs());
      } else {
        // Try harder: look for min/max pattern based on instructions producing
        // same values such as: select ((cmp Inst1, Inst2), Inst1, Inst2).
        // During the intermediate stages of SLP, it's very common to have
        // pattern like this (since optimizeGatherSequence is run only once
        // at the end):
        // %1 = extractelement <2 x i32> %a, i32 0
        // %2 = extractelement <2 x i32> %a, i32 1
        // %cond = icmp sgt i32 %1, %2
        // %3 = extractelement <2 x i32> %a, i32 0
        // %4 = extractelement <2 x i32> %a, i32 1
        // %select = select i1 %cond, i32 %3, i32 %4
        CmpInst::Predicate Pred;
        Instruction *L1;
        Instruction *L2;

        LHS = Select->getTrueValue();
        RHS = Select->getFalseValue();
        Value *Cond = Select->getCondition();

        // TODO: Support inverse predicates.
        if (match(Cond, m_Cmp(Pred, m_Specific(LHS), m_Instruction(L2)))) {
          if (!isa<ExtractElementInst>(RHS) ||
              !L2->isIdenticalTo(cast<Instruction>(RHS)))
            return OperationData(V);
        } else if (match(Cond, m_Cmp(Pred, m_Instruction(L1), m_Specific(RHS)))) {
          if (!isa<ExtractElementInst>(LHS) ||
              !L1->isIdenticalTo(cast<Instruction>(LHS)))
            return OperationData(V);
        } else {
          if (!isa<ExtractElementInst>(LHS) || !isa<ExtractElementInst>(RHS))
            return OperationData(V);
          if (!match(Cond, m_Cmp(Pred, m_Instruction(L1), m_Instruction(L2))) ||
              !L1->isIdenticalTo(cast<Instruction>(LHS)) ||
              !L2->isIdenticalTo(cast<Instruction>(RHS)))
            return OperationData(V);
        }
        switch (Pred) {
        default:
          return OperationData(V);

        case CmpInst::ICMP_ULT:
        case CmpInst::ICMP_ULE:
          return OperationData(Instruction::ICmp, LHS, RHS, RK_UMin);

        case CmpInst::ICMP_SLT:
        case CmpInst::ICMP_SLE:
          return OperationData(Instruction::ICmp, LHS, RHS, RK_Min);

        case CmpInst::FCMP_OLT:
        case CmpInst::FCMP_OLE:
        case CmpInst::FCMP_ULT:
        case CmpInst::FCMP_ULE:
          return OperationData(Instruction::FCmp, LHS, RHS, RK_Min,
                               cast<Instruction>(Cond)->hasNoNaNs());

        case CmpInst::ICMP_UGT:
        case CmpInst::ICMP_UGE:
          return OperationData(Instruction::ICmp, LHS, RHS, RK_UMax);

        case CmpInst::ICMP_SGT:
        case CmpInst::ICMP_SGE:
          return OperationData(Instruction::ICmp, LHS, RHS, RK_Max);

        case CmpInst::FCMP_OGT:
        case CmpInst::FCMP_OGE:
        case CmpInst::FCMP_UGT:
        case CmpInst::FCMP_UGE:
          return OperationData(Instruction::FCmp, LHS, RHS, RK_Max,
                               cast<Instruction>(Cond)->hasNoNaNs());
        }
      }
    }
    return OperationData(V);
  }

public:
  HorizontalReduction() = default;

  /// Try to find a reduction tree.
  bool matchAssociativeReduction(PHINode *Phi, Instruction *B) {
    assert((!Phi || is_contained(Phi->operands(), B)) &&
           "Thi phi needs to use the binary operator");

    ReductionData = getOperationData(B);

    // We could have a initial reductions that is not an add.
    //  r *= v1 + v2 + v3 + v4
    // In such a case start looking for a tree rooted in the first '+'.
    if (Phi) {
      if (ReductionData.getLHS() == Phi) {
        Phi = nullptr;
        B = dyn_cast<Instruction>(ReductionData.getRHS());
        ReductionData = getOperationData(B);
      } else if (ReductionData.getRHS() == Phi) {
        Phi = nullptr;
        B = dyn_cast<Instruction>(ReductionData.getLHS());
        ReductionData = getOperationData(B);
      }
    }

    if (!ReductionData.isVectorizable(B))
      return false;
  

    Type *Ty = B->getType();
    if (!isValidElementType(Ty))
      return false;
    if (!Ty->isIntOrIntVectorTy() && !Ty->isFPOrFPVectorTy())
      return false;

    ReducedValueData.clear();
    ReductionRoot = B;

    LLVM_DEBUG(dbgs() << "vectorizable reductions : " << *B << "\n";);
  

    // Post order traverse the reduction tree starting at B. We only handle true
    // trees containing only binary operators.
    SmallVector<std::pair<Instruction *, unsigned>, 32> Stack;
    Stack.push_back(std::make_pair(B, ReductionData.getFirstOperandIndex()));
    ReductionData.initReductionOps(ReductionOps);

    LLVM_DEBUG(dbgs() << "for ins first: " << *B << "\n");
    LLVM_DEBUG(ReductionData.printOpData());

    while (!Stack.empty()) {

      LLVM_DEBUG(dbgs() << "stack contents : [\n");
      for(unsigned i = 0; i < Stack.size(); i++){
	LLVM_DEBUG(dbgs() << *Stack[i].first << "\n");
	LLVM_DEBUG(dbgs() << Stack[i].second << "\n");
      }
      LLVM_DEBUG(dbgs() << "]\n");


      Instruction *TreeN = Stack.back().first;
      unsigned EdgeToVist = Stack.back().second++;
      OperationData OpData = getOperationData(TreeN);

      LLVM_DEBUG(dbgs() << "for ins in loop: " << *TreeN << "\n");
      LLVM_DEBUG(OpData.printOpData());

      bool IsReducedValue = OpData != ReductionData;

      // Postorder vist.
      if (IsReducedValue || EdgeToVist == OpData.getNumberOfOperands()) {
        if (IsReducedValue){
	  LLVM_DEBUG(dbgs() << "reducedVal = true\n");
          ReducedVals.push_back(TreeN);
	}
        else {
          auto I = ExtraArgs.find(TreeN);
          if (I != ExtraArgs.end() && !I->second) {
            // Check if TreeN is an extra argument of its parent operation.
            if (Stack.size() <= 1) {
              // TreeN can't be an extra argument as it is a root reduction
              // operation.
              return false;
            }
            // Yes, TreeN is an extra argument, do not add it to a list of
            // reduction operations.
            // Stack[Stack.size() - 2] always points to the parent operation.
            markExtraArg(Stack[Stack.size() - 2], TreeN);
            ExtraArgs.erase(TreeN);
          } else
            ReductionData.addReductionOps(TreeN, ReductionOps);
        }
        // Retract.
	LLVM_DEBUG(dbgs() << "poped\n");
        Stack.pop_back();
        continue;
      }

      // Visit left or right.
      Value *NextV = TreeN->getOperand(EdgeToVist);
      LLVM_DEBUG(dbgs() << "NextV" << *NextV << "\n";
      if(Phi)
	dbgs() << "Phi" << *Phi << "\n";);
      if (NextV != Phi) {
        auto *I = dyn_cast<Instruction>(NextV);
        OpData = getOperationData(I);
        // Continue analysis if the next operand is a reduction operation or
        // (possibly) a reduced value. If the reduced value opcode is not set,
        // the first met operation != reduction operation is considered as the
        // reduced value class.
        if (I && (!ReducedValueData || OpData == ReducedValueData ||
                  OpData == ReductionData)) {
          const bool IsReductionOperation = OpData == ReductionData;
          // Only handle trees in the current basic block.
          if (!ReductionData.hasSameParent(I, B->getParent(),
                                           IsReductionOperation)) {
            // I is an extra argument for TreeN (its parent operation).
            markExtraArg(Stack.back(), I);
            continue;
          }

          // Each tree node needs to have minimal number of users except for the
          // ultimate reduction.
          if (!ReductionData.hasRequiredNumberOfUses(I,
                                                     OpData == ReductionData) &&
              I != B) {
            // I is an extra argument for TreeN (its parent operation).
            markExtraArg(Stack.back(), I);
            continue;
          }

          if (IsReductionOperation) {
            // We need to be able to reassociate the reduction operations.
            if (!OpData.isAssociative(I)) {
              // I is an extra argument for TreeN (its parent operation).
              markExtraArg(Stack.back(), I);
              continue;
            }
          } else if (ReducedValueData &&
                     ReducedValueData != OpData) {
            // Make sure that the opcodes of the operations that we are going to
            // reduce match.
            // I is an extra argument for TreeN (its parent operation).
            markExtraArg(Stack.back(), I);
            continue;
          } else if (!ReducedValueData)
            ReducedValueData = OpData;

          Stack.push_back(std::make_pair(I, OpData.getFirstOperandIndex()));
          continue;
        }
      }
      // NextV is an extra argument for TreeN (its parent operation).
      markExtraArg(Stack.back(), NextV);
    }

    LLVM_DEBUG(dbgs() << "printing reduced values: \n";
    for(unsigned i = 0; i < ReducedVals.size(); i++){
      dbgs() << *ReducedVals[i] << "\n";
    });


    return true;
  }

  /// Attempt to vectorize the tree found by
  /// matchAssociativeReduction.
  bool tryToReduce(BoUpSLP &V, TargetTransformInfo *TTI) {
    if (ReducedVals.empty())
      return false;

    // If there is a sufficient number of reduction values, reduce
    // to a nearby power-of-2. Can safely generate oversized
    // vectors and rely on the backend to split them to legal sizes.
    unsigned NumReducedVals = ReducedVals.size();
    if (NumReducedVals < 4)
      return false;

    
    //unsigned ReduxWidth = PowerOf2Floor(NumReducedVals);
    unsigned ReduxWidth = PowerOf2Floor(V.getMaxPackingFactor(ReducedVals[0]));
    LLVM_DEBUG(dbgs() << "redux width : " << ReduxWidth << "\n");


    Value *VectorizedTree = nullptr;
    IRBuilder<> Builder(ReductionRoot);
    FastMathFlags Unsafe;
    Unsafe.setFast();
    Builder.setFastMathFlags(Unsafe);
    unsigned i = 0;

    BoUpSLP::ExtraValueToDebugLocsMap ExternallyUsedValues;
    // The same extra argument may be used several time, so log each attempt
    // to use it.
    for (auto &Pair : ExtraArgs)
      ExternallyUsedValues[Pair.second].push_back(Pair.first);
    SmallVector<Value *, 16> IgnoreList;
    for (auto &V : ReductionOps)
      IgnoreList.append(V.begin(), V.end());
    while (i < NumReducedVals - ReduxWidth + 1 && ReduxWidth >= 2) { //equal added by me
      auto VL = makeArrayRef(&ReducedVals[i], ReduxWidth);
     
      LLVM_DEBUG(dbgs() << "before buildTree\n";
		 printBundle(VL));

      V.EnableDebug = true;
      V.buildTree(VL, ExternallyUsedValues, IgnoreList);
      V.EnableDebug = false;

      Optional<ArrayRef<unsigned>> Order = V.bestOrder();
      // TODO: Handle orders of size less than number of elements in the vector.
      if (Order && Order->size() == VL.size()) {
        // TODO: reorder tree nodes without tree rebuilding.
        SmallVector<Value *, 4> ReorderedOps(VL.size());
        llvm::transform(*Order, ReorderedOps.begin(),
                        [VL](const unsigned Idx) { return VL[Idx]; });
        V.buildTree(ReorderedOps, ExternallyUsedValues, IgnoreList);
      }


      LLVM_DEBUG(dbgs() << "tree size: " << V.getTreeSize() << "\n");


      if (V.isTreeTinyAndNotFullyVectorizable())
        break;

      //COMMENT
      //V.computeMinimumValueSizes();

      // Estimate cost.
      int TreeCost = V.getTreeCost();

      i += ReduxWidth;
      ReduxWidth = PowerOf2Floor(NumReducedVals - i);
      continue;


      int ReductionCost = getReductionCost(TTI, ReducedVals[i], ReduxWidth);
      int Cost = TreeCost + ReductionCost;
      if (Cost >= -RevecCostThreshold) {
          V.getORE()->emit([&]() {
              return OptimizationRemarkMissed(
                         SV_NAME, "HorSLPNotBeneficial", cast<Instruction>(VL[0]))
                     << "Vectorizing horizontal reduction is possible"
                     << "but not beneficial with cost "
                     << ore::NV("Cost", Cost) << " and threshold "
                     << ore::NV("Threshold", -RevecCostThreshold);
          });
          break;
      }

      REVEC_DEBUG(dbgs() << "SLP: Vectorizing horizontal reduction at cost:"
                        << Cost << ". (HorRdx)\n");
      V.getORE()->emit([&]() {
          return OptimizationRemark(
                     SV_NAME, "VectorizedHorizontalReduction", cast<Instruction>(VL[0]))
          << "Vectorized horizontal reduction with cost "
          << ore::NV("Cost", Cost) << " and with tree size "
          << ore::NV("TreeSize", V.getTreeSize());
      });

      // Vectorize a tree.
      DebugLoc Loc = cast<Instruction>(ReducedVals[i])->getDebugLoc();
      Value *VectorizedRoot = V.vectorizeTree(ExternallyUsedValues);

      // Emit a reduction.
      Value *ReducedSubTree =
          emitReduction(VectorizedRoot, Builder, ReduxWidth, TTI);
      if (VectorizedTree) {
        Builder.SetCurrentDebugLocation(Loc);
        OperationData VectReductionData(ReductionData.getOpcode(),
                                        VectorizedTree, ReducedSubTree,
                                        ReductionData.getKind());
        VectorizedTree =
            VectReductionData.createOp(Builder, "op.rdx", ReductionOps);
      } else
        VectorizedTree = ReducedSubTree;
      i += ReduxWidth;
      ReduxWidth = PowerOf2Floor(NumReducedVals - i);
    }

    if (VectorizedTree) {
      // Finish the reduction.
      for (; i < NumReducedVals; ++i) {
        auto *I = cast<Instruction>(ReducedVals[i]);
        Builder.SetCurrentDebugLocation(I->getDebugLoc());
        OperationData VectReductionData(ReductionData.getOpcode(),
                                        VectorizedTree, I,
                                        ReductionData.getKind());
        VectorizedTree = VectReductionData.createOp(Builder, "", ReductionOps);
      }
      for (auto &Pair : ExternallyUsedValues) {
        assert(!Pair.second.empty() &&
               "At least one DebugLoc must be inserted");
        // Add each externally used value to the final reduction.
        for (auto *I : Pair.second) {
          Builder.SetCurrentDebugLocation(I->getDebugLoc());
          OperationData VectReductionData(ReductionData.getOpcode(),
                                          VectorizedTree, Pair.first,
                                          ReductionData.getKind());
          VectorizedTree = VectReductionData.createOp(Builder, "op.extra", I);
        }
      }
      // Update users.
      ReductionRoot->replaceAllUsesWith(VectorizedTree);
    }
    return VectorizedTree != nullptr;
  }

  unsigned numReductionValues() const {
    return ReducedVals.size();
  }

private:
  /// Calculate the cost of a reduction.
  int getReductionCost(TargetTransformInfo *TTI, Value *FirstReducedVal,
                       unsigned ReduxWidth) {
    Type *ScalarTy = FirstReducedVal->getType();
    Type *VecTy = VectorType::get(ScalarTy, ReduxWidth);

    int PairwiseRdxCost;
    int SplittingRdxCost;
    switch (ReductionData.getKind()) {
    case RK_Arithmetic:
      PairwiseRdxCost =
          TTI->getArithmeticReductionCost(ReductionData.getOpcode(), VecTy,
                                          /*IsPairwiseForm=*/true);
      SplittingRdxCost =
          TTI->getArithmeticReductionCost(ReductionData.getOpcode(), VecTy,
                                          /*IsPairwiseForm=*/false);
      break;
    case RK_Min:
    case RK_Max:
    case RK_UMin:
    case RK_UMax: {
      Type *VecCondTy = CmpInst::makeCmpResultType(VecTy);
      bool IsUnsigned = ReductionData.getKind() == RK_UMin ||
                        ReductionData.getKind() == RK_UMax;
      PairwiseRdxCost =
          TTI->getMinMaxReductionCost(VecTy, VecCondTy,
                                      /*IsPairwiseForm=*/true, IsUnsigned);
      SplittingRdxCost =
          TTI->getMinMaxReductionCost(VecTy, VecCondTy,
                                      /*IsPairwiseForm=*/false, IsUnsigned);
      break;
    }
    case RK_None:
      llvm_unreachable("Expected arithmetic or min/max reduction operation");
    }

    IsPairwiseReduction = PairwiseRdxCost < SplittingRdxCost;
    int VecReduxCost = IsPairwiseReduction ? PairwiseRdxCost : SplittingRdxCost;

    int ScalarReduxCost;
    switch (ReductionData.getKind()) {
    case RK_Arithmetic:
      ScalarReduxCost =
          TTI->getArithmeticInstrCost(ReductionData.getOpcode(), ScalarTy);
      break;
    case RK_Min:
    case RK_Max:
    case RK_UMin:
    case RK_UMax:
      ScalarReduxCost =
          TTI->getCmpSelInstrCost(ReductionData.getOpcode(), ScalarTy) +
          TTI->getCmpSelInstrCost(Instruction::Select, ScalarTy,
                                  CmpInst::makeCmpResultType(ScalarTy));
      break;
    case RK_None:
      llvm_unreachable("Expected arithmetic or min/max reduction operation");
    }
    ScalarReduxCost *= (ReduxWidth - 1);

    LLVM_DEBUG(dbgs() << "SLP: Adding cost " << VecReduxCost - ScalarReduxCost
                      << " for reduction that starts with " << *FirstReducedVal
                      << " (It is a "
                      << (IsPairwiseReduction ? "pairwise" : "splitting")
                      << " reduction)\n");

    return VecReduxCost - ScalarReduxCost;
  }

  /// Emit a horizontal reduction of the vectorized value.
  Value *emitReduction(Value *VectorizedValue, IRBuilder<> &Builder,
                       unsigned ReduxWidth, const TargetTransformInfo *TTI) {
    assert(VectorizedValue && "Need to have a vectorized tree node");
    assert(isPowerOf2_32(ReduxWidth) &&
           "We only handle power-of-two reductions for now");

    if (!IsPairwiseReduction)
      return createSimpleTargetReduction(
          Builder, TTI, ReductionData.getOpcode(), VectorizedValue,
          ReductionData.getFlags(), ReductionOps.back());

    Value *TmpVec = VectorizedValue;
    for (unsigned i = ReduxWidth / 2; i != 0; i >>= 1) {
      Value *LeftMask =
          createRdxShuffleMask(ReduxWidth, i, true, true, Builder);
      Value *RightMask =
          createRdxShuffleMask(ReduxWidth, i, true, false, Builder);

      Value *LeftShuf = Builder.CreateShuffleVector(
          TmpVec, UndefValue::get(TmpVec->getType()), LeftMask, "rdx.shuf.l");
      Value *RightShuf = Builder.CreateShuffleVector(
          TmpVec, UndefValue::get(TmpVec->getType()), (RightMask),
          "rdx.shuf.r");
      OperationData VectReductionData(ReductionData.getOpcode(), LeftShuf,
                                      RightShuf, ReductionData.getKind());
      TmpVec = VectReductionData.createOp(Builder, "op.rdx", ReductionOps);
    }

    // The result is in the first element of the vector.
    return Builder.CreateExtractElement(TmpVec, Builder.getInt32(0));
  }
};

} // end anonymous namespace


/// Recognize construction of vectors like
///  %ra = insertelement <4 x float> undef, float %s0, i32 0
///  %rb = insertelement <4 x float> %ra, float %s1, i32 1
///  %rc = insertelement <4 x float> %rb, float %s2, i32 2
///  %rd = insertelement <4 x float> %rc, float %s3, i32 3
///  starting from the last insertelement instruction.
///
/// Returns true if it matches
static bool findBuildVector(InsertElementInst *LastInsertElem,
                            TargetTransformInfo *TTI,
                            SmallVectorImpl<Value *> &BuildVectorOpds,
                            int &UserCost) {
  UserCost = 0;
  Value *V = nullptr;
  do {
    if (auto *CI = dyn_cast<ConstantInt>(LastInsertElem->getOperand(2))) {
      UserCost += TTI->getVectorInstrCost(Instruction::InsertElement,
                                          LastInsertElem->getType(),
                                          CI->getZExtValue());
    }
    BuildVectorOpds.push_back(LastInsertElem->getOperand(1));
    V = LastInsertElem->getOperand(0);
    if (isa<UndefValue>(V))
      break;
    LastInsertElem = dyn_cast<InsertElementInst>(V);
    if (!LastInsertElem || !LastInsertElem->hasOneUse())
      return false;
  } while (true);
  std::reverse(BuildVectorOpds.begin(), BuildVectorOpds.end());
  return true;
}

/// Generate a shuffle mask to be used in a reduction tree.
///
/// \param VecLen The length of the vector to be reduced.
/// \param NumEltsToRdx The number of elements that should be reduced in the
///        vector.
/// \param IsPairwise Whether the reduction is a pairwise or splitting
///        reduction. A pairwise reduction will generate a mask of
///        <0,2,...> or <1,3,..> while a splitting reduction will generate
///        <2,3, undef,undef> for a vector of 4 and NumElts = 2.
/// \param IsLeft True will generate a mask of even elements, odd otherwise.
static Value *createRdxShuffleMask(unsigned VecLen, unsigned NumEltsToRdx,
                                   bool IsPairwise, bool IsLeft,
                                   IRBuilder<> &Builder) {
  assert((IsPairwise || !IsLeft) && "Don't support a <0,1,undef,...> mask");

  SmallVector<Constant *, 32> ShuffleMask(
      VecLen, UndefValue::get(Builder.getInt32Ty()));

  if (IsPairwise)
    // Build a mask of 0, 2, ... (left) or 1, 3, ... (right).
    for (unsigned i = 0; i != NumEltsToRdx; ++i)
      ShuffleMask[i] = Builder.getInt32(2 * i + !IsLeft);
  else
    // Move the upper half of the vector to the lower half.
    for (unsigned i = 0; i != NumEltsToRdx; ++i)
      ShuffleMask[i] = Builder.getInt32(NumEltsToRdx + i);

  return ConstantVector::get(ShuffleMask);
}


/// Like findBuildVector, but looks for construction of aggregate.
///
/// \return true if it matches.
static bool findBuildAggregate(InsertValueInst *IV,
                               SmallVectorImpl<Value *> &BuildVectorOpds) {
  Value *V;
  do {
    BuildVectorOpds.push_back(IV->getInsertedValueOperand());
    V = IV->getAggregateOperand();
    if (isa<UndefValue>(V))
      break;
    IV = dyn_cast<InsertValueInst>(V);
    if (!IV || !IV->hasOneUse())
      return false;
  } while (true);
  std::reverse(BuildVectorOpds.begin(), BuildVectorOpds.end());
  return true;
}

/// Checks if the vector of instructions can be represented as a shuffle, like:
/// %x0 = extractelement <4 x i8> %x, i32 0
/// %x3 = extractelement <4 x i8> %x, i32 3
/// %y1 = extractelement <4 x i8> %y, i32 1
/// %y2 = extractelement <4 x i8> %y, i32 2
/// %x0x0 = mul i8 %x0, %x0
/// %x3x3 = mul i8 %x3, %x3
/// %y1y1 = mul i8 %y1, %y1
/// %y2y2 = mul i8 %y2, %y2
/// %ins1 = insertelement <4 x i8> undef, i8 %x0x0, i32 0
/// %ins2 = insertelement <4 x i8> %ins1, i8 %x3x3, i32 1
/// %ins3 = insertelement <4 x i8> %ins2, i8 %y1y1, i32 2
/// %ins4 = insertelement <4 x i8> %ins3, i8 %y2y2, i32 3
/// ret <4 x i8> %ins4
/// can be transformed into:
/// %1 = shufflevector <4 x i8> %x, <4 x i8> %y, <4 x i32> <i32 0, i32 3, i32 5,
///                                                         i32 6>
/// %2 = mul <4 x i8> %1, %1
/// ret <4 x i8> %2
/// We convert this initially to something like:
/// %x0 = extractelement <4 x i8> %x, i32 0
/// %x3 = extractelement <4 x i8> %x, i32 3
/// %y1 = extractelement <4 x i8> %y, i32 1
/// %y2 = extractelement <4 x i8> %y, i32 2
/// %1 = insertelement <4 x i8> undef, i8 %x0, i32 0
/// %2 = insertelement <4 x i8> %1, i8 %x3, i32 1
/// %3 = insertelement <4 x i8> %2, i8 %y1, i32 2
/// %4 = insertelement <4 x i8> %3, i8 %y2, i32 3
/// %5 = mul <4 x i8> %4, %4
/// %6 = extractelement <4 x i8> %5, i32 0
/// %ins1 = insertelement <4 x i8> undef, i8 %6, i32 0
/// %7 = extractelement <4 x i8> %5, i32 1
/// %ins2 = insertelement <4 x i8> %ins1, i8 %7, i32 1
/// %8 = extractelement <4 x i8> %5, i32 2
/// %ins3 = insertelement <4 x i8> %ins2, i8 %8, i32 2
/// %9 = extractelement <4 x i8> %5, i32 3
/// %ins4 = insertelement <4 x i8> %ins3, i8 %9, i32 3
/// ret <4 x i8> %ins4
/// InstCombiner transforms this into a shuffle and vector mul
/// TODO: Can we split off and reuse the shuffle mask detection from
/// TargetTransformInfo::getInstructionThroughput?
static Optional<TargetTransformInfo::ShuffleKind>
isShuffle(ArrayRef<Value *> VL) {
  auto *EI0 = cast<ExtractElementInst>(VL[0]);
  unsigned Size = EI0->getVectorOperandType()->getVectorNumElements();
  Value *Vec1 = nullptr;
  Value *Vec2 = nullptr;
  enum ShuffleMode { Unknown, Select, Permute };
  ShuffleMode CommonShuffleMode = Unknown;
  for (unsigned I = 0, E = VL.size(); I < E; ++I) {
    auto *EI = cast<ExtractElementInst>(VL[I]);
    auto *Vec = EI->getVectorOperand();
    // All vector operands must have the same number of vector elements.
    if (Vec->getType()->getVectorNumElements() != Size)
      return None;
    auto *Idx = dyn_cast<ConstantInt>(EI->getIndexOperand());
    if (!Idx)
      return None;
    // Undefined behavior if Idx is negative or >= Size.
    if (Idx->getValue().uge(Size))
      continue;
    unsigned IntIdx = Idx->getValue().getZExtValue();
    // We can extractelement from undef vector.
    if (isa<UndefValue>(Vec))
      continue;
    // For correct shuffling we have to have at most 2 different vector operands
    // in all extractelement instructions.
    if (!Vec1 || Vec1 == Vec)
      Vec1 = Vec;
    else if (!Vec2 || Vec2 == Vec)
      Vec2 = Vec;
    else
      return None;
    if (CommonShuffleMode == Permute)
      continue;
    // If the extract index is not the same as the operation number, it is a
    // permutation.
    if (IntIdx != I) {
      CommonShuffleMode = Permute;
      continue;
    }
    CommonShuffleMode = Select;
  }
  // If we're not crossing lanes in different vectors, consider it as blending.
  if (CommonShuffleMode == Select && Vec2)
    return TargetTransformInfo::SK_Select;
  // If Vec2 was never used, we have a permutation of a single vector, otherwise
  // we have permutation of 2 vectors.
  return Vec2 ? TargetTransformInfo::SK_PermuteTwoSrc
              : TargetTransformInfo::SK_PermuteSingleSrc;
}


static bool PhiTypeSorterFunc(Value *V, Value *V2) {

  PHINode * p1 = dyn_cast<PHINode>(V);
  PHINode * p2 = dyn_cast<PHINode>(V2);

  assert(p1 && p2);
  
  return p1->getType() < p2->getType();
}



/// Try and get a reduction value from a phi node.
///
/// Given a phi node \p P in a block \p ParentBB, consider possible reductions
/// if they come from either \p ParentBB or a containing loop latch.
///
/// \returns A candidate reduction value if possible, or \code nullptr \endcode
/// if not possible.
static Value *getReductionValue(const DominatorTree *DT, PHINode *P,
                                BasicBlock *ParentBB, LoopInfo *LI) {
  // There are situations where the reduction value is not dominated by the
  // reduction phi. Vectorizing such cases has been reported to cause
  // miscompiles. See PR25787.
  auto DominatedReduxValue = [&](Value *R) {
    return isa<Instruction>(R) &&
           DT->dominates(P->getParent(), cast<Instruction>(R)->getParent());
  };

  Value *Rdx = nullptr;

  // Return the incoming value if it comes from the same BB as the phi node.
  if (P->getIncomingBlock(0) == ParentBB) {
    Rdx = P->getIncomingValue(0);
  } else if (P->getIncomingBlock(1) == ParentBB) {
    Rdx = P->getIncomingValue(1);
  }

  if (Rdx && DominatedReduxValue(Rdx) && isValidElementType(Rdx->getType()))
    return Rdx;

  // Otherwise, check whether we have a loop latch to look at.
  Loop *BBL = LI->getLoopFor(ParentBB);
  if (!BBL)
    return nullptr;
  BasicBlock *BBLatch = BBL->getLoopLatch();
  if (!BBLatch)
    return nullptr;

  // There is a loop latch, return the incoming value if it comes from
  // that. This reduction pattern occasionally turns up.
  if (P->getIncomingBlock(0) == BBLatch) {
    Rdx = P->getIncomingValue(0);
  } else if (P->getIncomingBlock(1) == BBLatch) {
    Rdx = P->getIncomingValue(1);
  }

  if (Rdx && DominatedReduxValue(Rdx) && isValidElementType(Rdx->getType()))
    return Rdx;

  return nullptr;
}



bool RevectorizerPass::vectorizeInsertValueInst(InsertValueInst *IVI,
                                                 BasicBlock *BB, BoUpSLP &R) {
  const DataLayout &DL = BB->getModule()->getDataLayout();
  if (!R.canMapToVector(IVI->getType(), DL))
    return false;

  SmallVector<Value *, 16> BuildVectorOpds;
  if (!findBuildAggregate(IVI, BuildVectorOpds))
    return false;

  LLVM_DEBUG(dbgs() << "SLP: array mappable to vector: " << *IVI << "\n");
  // Aggregate value is unlikely to be processed in vector register, we need to
  // extract scalars into scalar registers, so NeedExtraction is set true.
  return tryToVectorizeList(BuildVectorOpds, R);
}

bool RevectorizerPass::vectorizeInsertElementInst(InsertElementInst *IEI,
                                                   BasicBlock *BB, BoUpSLP &R) {
  int UserCost;
  SmallVector<Value *, 16> BuildVectorOpds;
  if (!findBuildVector(IEI, TTI, BuildVectorOpds, UserCost) ||
      (llvm::all_of(BuildVectorOpds,
                    [](Value *V) { return isa<ExtractElementInst>(V); }) &&
       isShuffle(BuildVectorOpds)))
    return false;

  // Vectorize starting with the build vector operands ignoring the BuildVector
  // instructions for the purpose of scheduling and user extraction.
  return tryToVectorizeList(BuildVectorOpds, R, UserCost);
}

bool RevectorizerPass::vectorizeCmpInst(CmpInst *CI, BasicBlock *BB,
                                         BoUpSLP &R) {
  if (tryToVectorizePair(CI->getOperand(0), CI->getOperand(1), R))
    return true;

  bool OpsChanged = false;
  for (int Idx = 0; Idx < 2; ++Idx) {
    OpsChanged |=
        vectorizeRootInstruction(nullptr, CI->getOperand(Idx), BB, R, TTI);
  }
  return OpsChanged;
}


bool RevectorizerPass::vectorizeSimpleInstructions(
    SmallVectorImpl<WeakVH> &Instructions, BasicBlock *BB, BoUpSLP &R) {
  bool OpsChanged = false;
  for (auto &VH : reverse(Instructions)) {
    auto *I = dyn_cast_or_null<Instruction>(VH);
    if (!I)
      continue;

    auto *LastInsertValue = dyn_cast<InsertValueInst>(I);
    auto *LastInsertElem = dyn_cast<InsertElementInst>(I);
    assert(!LastInsertValue && !LastInsertElem);

    //if (auto *LastInsertValue = dyn_cast<InsertValueInst>(I))
    //  OpsChanged |= vectorizeInsertValueInst(LastInsertValue, BB, R);
    //else if (auto *LastInsertElem = dyn_cast<InsertElementInst>(I))
    //  OpsChanged |= vectorizeInsertElementInst(LastInsertElem, BB, R);
    if (auto *CI = dyn_cast<CmpInst>(I))
      OpsChanged |= vectorizeCmpInst(CI, BB, R);
  }
  Instructions.clear();
  return OpsChanged;
}



// reductions related functions
bool RevectorizerPass::vectorizeChainsInBlock(BasicBlock *BB, BoUpSLP &R) {

  bool Changed = false;
  SmallVector<Value *, 4> Incoming;
  SmallPtrSet<Value *, 16> VisitedInstrs;

  bool HaveVectorizedPhiNodes = true;

  while (HaveVectorizedPhiNodes) {
    HaveVectorizedPhiNodes = false;

    // Collect the incoming values from the PHIs.
    Incoming.clear();
    for (Instruction &I : *BB) {
      PHINode *P = dyn_cast<PHINode>(&I);
      if (!P)
        break;

      if (!VisitedInstrs.count(P))
        Incoming.push_back(P);
    }

    // Sort by type.
    std::stable_sort(Incoming.begin(), Incoming.end(), PhiTypeSorterFunc);

    // Try to vectorize elements base on their type.
    for (SmallVector<Value *, 4>::iterator IncIt = Incoming.begin(),
                                           E = Incoming.end();
         IncIt != E;) {

      // Look for the next elements with the same type.
      SmallVector<Value *, 4>::iterator SameTypeIt = IncIt;
      while (SameTypeIt != E &&
             (*SameTypeIt)->getType() == (*IncIt)->getType()) {

	PHINode * p = dyn_cast<PHINode>(*SameTypeIt);
	PHINode * pc = dyn_cast<PHINode>(*IncIt);

	if(p->getNumIncomingValues() != pc->getNumIncomingValues()) break;
	
        VisitedInstrs.insert(*SameTypeIt);
        ++SameTypeIt;
      }

      // Try to vectorize them.
      unsigned NumElts = (SameTypeIt - IncIt);
      LLVM_DEBUG(dbgs() << "Revec: Trying to vectorize starting at PHIs ("
                        << NumElts << ")\n");
      // The order in which the phi nodes appear in the program does not matter.
      // So allow tryToVectorizeList to reorder them if it is beneficial. This
      // is done when there are exactly two elements since tryToVectorizeList
      // asserts that there are only two values when AllowReorder is true.
      bool AllowReorder = NumElts == 2;

      //check for revec_reduce metadata

      bool isReduction = false;
      MDNode * md = (dyn_cast<PHINode>(*IncIt))->getMetadata("revec_reduce");

      if(md){
	MDString * md_string = dyn_cast<MDString>(md->getOperand(0));
	bool isSame = true;
	for(auto it = IncIt; it != SameTypeIt; it++){
	  MDNode * md_next = (dyn_cast<PHINode>(*it))->getMetadata("revec_reduce");
	  if(!md_next){ isSame = false; break;}
	  MDString * md_next_string = dyn_cast<MDString>(md_next->getOperand(0));
	  if(md_string->getString() != md_next_string->getString()){ isSame = false; break;}
	}
	isReduction = isSame;
      }

      REVEC_DEBUG(dbgs() << "isReduction : " << isReduction << "\n");
      REVEC_DEBUG(dbgs() << " values : \n");
      REVEC_DEBUG(printBundle(makeArrayRef(IncIt,NumElts)));
      

      if (NumElts > 1 && isReduction && tryToVectorizeList(makeArrayRef(IncIt, NumElts), R,
                                            /*UserCost=*/0, AllowReorder)) {
        // Success start over because instructions might have been changed.
        HaveVectorizedPhiNodes = true;
        Changed = true;
        break;
      }

      // Start over at the next instruction of a different type (or the end).
      IncIt = SameTypeIt;
    }
  }

  return Changed;
  
  //upto PHI-Nodes it's revectorizing

  VisitedInstrs.clear();

  SmallVector<WeakVH, 8> PostProcessInstructions;
  SmallDenseSet<Instruction *, 4> KeyNodes;
  for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e; it++) {
    
    //TODO: change vectorizeSimpleInstructions
    // We may go through BB multiple times so skip the one we have checked.
    //if (!VisitedInstrs.insert(&*it).second) {
    //  if (it->use_empty() && KeyNodes.count(&*it) > 0 &&
    //      vectorizeSimpleInstructions(PostProcessInstructions, BB, R)) {
        // We would like to start over since some instructions are deleted
        // and the iterator may become invalid value.
    //    Changed = true;
    //    it = BB->begin();
    //    e = BB->end();
    //  }
    //  continue;
    //}

    if (isa<DbgInfoIntrinsic>(it))
      continue;

    // Try to vectorize reductions that use PHINodes.
    if (PHINode *P = dyn_cast<PHINode>(it)) {
      // Check that the PHI is a reduction PHI.
      if (P->getNumIncomingValues() != 2)
        return Changed;


      //get the reduction value first
      Value * RedVal = getReductionValue(DT, P, BB, LI);

      LLVM_DEBUG(if(RedVal)
	 REVEC_DEBUG(dbgs() << "reduction : " << *P << "\n" << *RedVal << "\n"));

      // Try to match and vectorize a horizontal reduction.
      if (vectorizeRootInstruction(P, RedVal, BB, R,
                                   TTI)) {
        Changed = true;
        it = BB->begin();
        e = BB->end();
        continue;
      }
      continue;
    }

    // Ran into an instruction without users, like terminator, or function call
    // with ignored return value, store. Ignore unused instructions (basing on
    // instruction type, except for CallInst and InvokeInst).
    if (it->use_empty() && (it->getType()->isVoidTy() || isa<CallInst>(it) ||
                            isa<InvokeInst>(it))) {
      KeyNodes.insert(&*it);
      bool OpsChanged = false;
      if (ShouldStartVectorizeHorAtStore || !isa<StoreInst>(it)) {
        for (auto *V : it->operand_values()) {
          // Try to match and vectorize a horizontal reduction.
          OpsChanged |= vectorizeRootInstruction(nullptr, V, BB, R, TTI);
        }
      }
      // Start vectorization of post-process list of instructions from the
      // top-tree instructions to try to vectorize as many instructions as
      // possible.
      //OpsChanged |= vectorizeSimpleInstructions(PostProcessInstructions, BB, R);
      //if (OpsChanged) {
        // We would like to start over since some instructions are deleted
        // and the iterator may become invalid value.
      //  Changed = true;
      //  it = BB->begin();
      //  e = BB->end();
      //  continue;
      //}
    }

    //if (isa<InsertElementInst>(it) || isa<CmpInst>(it) ||
    //    isa<InsertValueInst>(it))
    //  PostProcessInstructions.push_back(&*it);
  }

  return Changed;
}


//vectorizing non-store seeded instructions

//TODO: add to the header
bool RevectorizerPass::tryToVectorize(Instruction *I, BoUpSLP &R) {
  if (!I)
    return false;

  if (!isa<BinaryOperator>(I) && !isa<CmpInst>(I))
    return false;

  Value *P = I->getParent();

  // Vectorize in current basic block only.
  auto *Op0 = dyn_cast<Instruction>(I->getOperand(0));
  auto *Op1 = dyn_cast<Instruction>(I->getOperand(1));
  if (!Op0 || !Op1 || Op0->getParent() != P || Op1->getParent() != P)
    return false;

  // Try to vectorize V.
  if (tryToVectorizePair(Op0, Op1, R))
    return true;

  auto *A = dyn_cast<BinaryOperator>(Op0);
  auto *B = dyn_cast<BinaryOperator>(Op1);
  // Try to skip B.
  if (B && B->hasOneUse()) {
    auto *B0 = dyn_cast<BinaryOperator>(B->getOperand(0));
    auto *B1 = dyn_cast<BinaryOperator>(B->getOperand(1));
    if (B0 && B0->getParent() == P && tryToVectorizePair(A, B0, R))
      return true;
    if (B1 && B1->getParent() == P && tryToVectorizePair(A, B1, R))
      return true;
  }

  // Try to skip A.
  if (A && A->hasOneUse()) {
    auto *A0 = dyn_cast<BinaryOperator>(A->getOperand(0));
    auto *A1 = dyn_cast<BinaryOperator>(A->getOperand(1));
    if (A0 && A0->getParent() == P && tryToVectorizePair(A0, B, R))
      return true;
    if (A1 && A1->getParent() == P && tryToVectorizePair(A1, B, R))
      return true;
  }
  return false;
}

//TODO: add to the header
bool RevectorizerPass::tryToVectorizePair(Value *A, Value *B, BoUpSLP &R) {
  if (!A || !B)
    return false;
  Value *VL[] = { A, B };
  return tryToVectorizeList(VL, R, /*UserCost=*/0, true);
}


//TODO: add to the header
bool RevectorizerPass::tryToVectorizeList(ArrayRef<Value *> VL, BoUpSLP &R,
                                           int UserCost, bool AllowReorder) {
  if (VL.size() < 2)
    return false;

  LLVM_DEBUG(dbgs() << "Revec: Trying to vectorize a list of length = "
                    << VL.size() << ".\n");

  // Check that all of the parts are scalar instructions of the same type,
  // we permit an alternate opcode via InstructionsState.
  InstructionsState S = getSameOpcode(VL);
  if (!S.Opcode)
    return false;

  Instruction *I0 = cast<Instruction>(S.OpValue);
  unsigned Sz = R.getVectorElementSize(I0);
  unsigned MinVF = std::max(2U, R.getMinVecRegSize() / Sz);
  unsigned MaxVF = std::max<unsigned>(PowerOf2Floor(VL.size()), MinVF);
  if (MaxVF < 2) {
    R.getORE()->emit([&]() {
      return OptimizationRemarkMissed(SV_NAME, "SmallVF", I0)
             << "Cannot SLP vectorize list: vectorization factor "
             << "less than 2 is not supported";
    });
    return false;
  }

  for (Value *V : VL) {
    Type *Ty = V->getType();
    if (!isValidElementType(Ty)) {
      // NOTE: the following will give user internal llvm type name, which may
      // not be useful.
      R.getORE()->emit([&]() {
        std::string type_str;
        llvm::raw_string_ostream rso(type_str);
        Ty->print(rso);
        return OptimizationRemarkMissed(SV_NAME, "UnsupportedType", I0)
               << "Cannot SLP vectorize list: type "
               << rso.str() + " is unsupported by vectorizer";
      });
      return false;
    }
  }

  bool debugP = false;
  PHINode * P = dyn_cast<PHINode>(VL[0]);
  REVEC_DEBUG(if(P){
    dbgs() << "[\n";
    for(unsigned i = 0; i < VL.size(); i++){
      dbgs() << *VL[i] << "\n";
    }
    dbgs() << "]\n";
    dbgs() << MaxVF << "\n";
    debugP = true;
    });

  //recalculate the vectorization factor
  unsigned VF = R.getMaxVecRegSize() / Sz;
  REVEC_DEBUG(if(P){
    dbgs() << R.getMinVecRegSize() << " " << R.getMaxVecRegSize() << " " << Sz << "\n";
    dbgs() << "vec fac : " << VF << "\n";

    BasicBlock * curBB = P->getParent();
    BasicBlock * outside = nullptr;
    
    for(unsigned i = 0; i < P->getNumIncomingValues(); i++){
      BasicBlock * IBB = P->getIncomingBlock(i);
      if(IBB != curBB){
	outside = IBB;
      }
    }

    if(outside){
      dbgs() << *outside << "\n";
    }
    
    });



  bool Changed = false;
  bool CandidateFound = false;
  int MinCost = RevecCostThreshold;

  // Keep track of values that were deleted by vectorizing in the loop below.
  SmallVector<WeakTrackingVH, 8> TrackValues(VL.begin(), VL.end());

  unsigned NextInst = 0, MaxInst = VL.size();
  for(unsigned NextInst = 0; NextInst < MaxInst; NextInst += VF){
    
    auto *VecTy = getVectorType(VL[0]->getType(), VF);
    if (TTI->getNumberOfParts(VecTy) == VF)
      continue;
    unsigned OpsWidth = 0;

    if (NextInst + VF > MaxInst)
      OpsWidth = MaxInst - NextInst;
    else
      OpsWidth = VF;

    if (!isPowerOf2_32(OpsWidth) || OpsWidth < 2)
      break;

    // Check that a previous iteration of this loop did not delete the Value.
    if (hasValueBeenRAUWed(VL, TrackValues, NextInst, OpsWidth))
      continue;

    if(debugP)
      REVEC_DEBUG(dbgs() << "Revec: Analyzing " << OpsWidth << " operations "
		  << "\n");
    ArrayRef<Value *> Ops = VL.slice(NextInst, OpsWidth);

    R.buildTree(Ops);
    Optional<ArrayRef<unsigned>> Order = R.bestOrder();
    // TODO: check if we can allow reordering for more cases.
    if (AllowReorder && Order) {
      // TODO: reorder tree nodes without tree rebuilding.
      // Conceptually, there is nothing actually preventing us from trying to
      // reorder a larger list. In fact, we do exactly this when vectorizing
      // reductions. However, at this point, we only expect to get here when
      // there are exactly two operations.
      assert(Ops.size() == 2);
      Value *ReorderedOps[] = {Ops[1], Ops[0]};
      R.buildTree(ReorderedOps, None);
    }
    if (R.isTreeTinyAndNotFullyVectorizable())
      continue;
    
    //COMMENT
    //R.computeMinimumValueSizes();
    
    int Cost = R.getTreeCost() - UserCost;
    CandidateFound = true;
    MinCost = std::min(MinCost, Cost);

    RevecCostThreshold = -10;
    if (Cost < -RevecCostThreshold) {
      if(debugP)
	REVEC_DEBUG(dbgs() << "Revec: Vectorizing list at cost:" << Cost << ".\n");
      R.getORE()->emit(OptimizationRemark(SV_NAME, "VectorizedList",
					  cast<Instruction>(Ops[0]))
		       << "SLP vectorized with cost " << ore::NV("Cost", Cost)
		       << " and with tree size "
		       << ore::NV("TreeSize", R.getTreeSize()));
      
      R.vectorizeTree();
      // Move to the next bundle.
      Changed = true;
    }
  }


  if (!Changed && CandidateFound) {
    R.getORE()->emit([&]() {
      return OptimizationRemarkMissed(SV_NAME, "NotBeneficial", I0)
             << "List vectorization was possible but not beneficial with cost "
             << ore::NV("Cost", MinCost) << " >= "
             << ore::NV("Treshold", -RevecCostThreshold);
    });
  } else if (!Changed) {
    R.getORE()->emit([&]() {
      return OptimizationRemarkMissed(SV_NAME, "NotPossible", I0)
             << "Cannot SLP vectorize list: vectorization was impossible"
             << " with available vectorization factors";
    });
  }
  return Changed;
}



//TODO: seems like a static function, may not need to add it to the header file
/// Attempt to reduce a horizontal reduction.
/// If it is legal to match a horizontal reduction feeding the phi node \a P
/// with reduction operators \a Root (or one of its operands) in a basic block
/// \a BB, then check if it can be done. If horizontal reduction is not found
/// and root instruction is a binary operation, vectorization of the operands is
/// attempted.
/// \returns true if a horizontal reduction was matched and reduced or operands
/// of one of the binary instruction were vectorized.
/// \returns false if a horizontal reduction was not matched (or not possible)
/// or no vectorization of any binary operation feeding \a Root instruction was
/// performed.
static bool tryToVectorizeHorReductionOrInstOperands(
    PHINode *P, Instruction *Root, BasicBlock *BB, BoUpSLP &R,
    TargetTransformInfo *TTI,
    const function_ref<bool(Instruction *, BoUpSLP &)> Vectorize) {
  if (!ShouldVectorizeHor)
    return false;

  if (!Root)
    return false;

  if (Root->getParent() != BB || isa<PHINode>(Root))
    return false;
  // Start analysis starting from Root instruction. If horizontal reduction is
  // found, try to vectorize it. If it is not a horizontal reduction or
  // vectorization is not possible or not effective, and currently analyzed
  // instruction is a binary operation, try to vectorize the operands, using
  // pre-order DFS traversal order. If the operands were not vectorized, repeat
  // the same procedure considering each operand as a possible root of the
  // horizontal reduction.
  // Interrupt the process if the Root instruction itself was vectorized or all
  // sub-trees not higher that RecursionMaxDepth were analyzed/vectorized.
  SmallVector<std::pair<WeakTrackingVH, unsigned>, 8> Stack(1, {Root, 0});
  SmallPtrSet<Value *, 8> VisitedInstrs;
  bool Res = false;
  while (!Stack.empty()) {
    Value *V;
    unsigned Level;
    std::tie(V, Level) = Stack.pop_back_val();
    if (!V)
      continue;
    auto *Inst = dyn_cast<Instruction>(V);
    if (!Inst)
      continue;
    auto *BI = dyn_cast<BinaryOperator>(Inst);
    auto *SI = dyn_cast<SelectInst>(Inst);
    if (BI || SI) {
      HorizontalReduction HorRdx;
      if (HorRdx.matchAssociativeReduction(P, Inst)) {
        if (HorRdx.tryToReduce(R, TTI)) {
          Res = true;
          // Set P to nullptr to avoid re-analysis of phi node in
          // matchAssociativeReduction function unless this is the root node.
          P = nullptr;
          continue;
        }
      }
      if (P && BI) {
        Inst = dyn_cast<Instruction>(BI->getOperand(0));
        if (Inst == P)
          Inst = dyn_cast<Instruction>(BI->getOperand(1));
        if (!Inst) {
          // Set P to nullptr to avoid re-analysis of phi node in
          // matchAssociativeReduction function unless this is the root node.
          P = nullptr;
          continue;
        }
      }
    }
    // Set P to nullptr to avoid re-analysis of phi node in
    // matchAssociativeReduction function unless this is the root node.
    P = nullptr;
    if (Vectorize(Inst, R)) {
      Res = true;
      continue;
    }

    // Try to vectorize operands.
    // Continue analysis for the instruction from the same basic block only to
    // save compile time.
    if (++Level < RecursionMaxDepth)
      for (auto *Op : Inst->operand_values())
        if (VisitedInstrs.insert(Op).second)
          if (auto *I = dyn_cast<Instruction>(Op))
            if (!isa<PHINode>(I) && I->getParent() == BB)
              Stack.emplace_back(Op, Level);
  }
  return Res;
}



bool RevectorizerPass::vectorizeRootInstruction(PHINode *P, Value *V,
                                                 BasicBlock *BB, BoUpSLP &R,
                                                 TargetTransformInfo *TTI) {
  if (!V)
    return false;
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  if (!isa<BinaryOperator>(I))
    P = nullptr;
  // Try to match and vectorize a horizontal reduction.
  auto &&ExtraVectorization = [this](Instruction *I, BoUpSLP &R) -> bool {
    return tryToVectorize(I, R);
  };
  return tryToVectorizeHorReductionOrInstOperands(P, I, BB, R, TTI,
                                                  ExtraVectorization);
}





char Revectorizer::ID = 0;

static const char lv_name[] = "Revectorizer";

INITIALIZE_PASS_BEGIN(Revectorizer, SV_NAME, lv_name, false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(DemandedBitsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
// INITIALIZE_PASS_DEPENDENCY(SLPVectorizer)
INITIALIZE_PASS_END(Revectorizer, SV_NAME, lv_name, false, false)

Pass *llvm::createRevectorizerPass() { return new Revectorizer(); }
