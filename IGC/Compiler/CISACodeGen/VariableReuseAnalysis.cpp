/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "VariableReuseAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"

using namespace llvm;
using namespace IGC;

char VariableReuseAnalysis::ID = 0;

IGC_INITIALIZE_PASS_BEGIN(VariableReuseAnalysis, "VariableReuseAnalysis",
                          "VariableReuseAnalysis", false, true)
// IGC_INITIALIZE_PASS_DEPENDENCY(RegisterEstimator)
IGC_INITIALIZE_PASS_DEPENDENCY(WIAnalysis)
IGC_INITIALIZE_PASS_DEPENDENCY(LiveVarsAnalysis)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenPatternMatch)
IGC_INITIALIZE_PASS_DEPENDENCY(DeSSA)
IGC_INITIALIZE_PASS_DEPENDENCY(CoalescingEngine)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(VariableReuseAnalysis, "VariableReuseAnalysis",
                        "VariableReuseAnalysis", false, true)

llvm::FunctionPass *IGC::createVariableReuseAnalysisPass() {
  return new VariableReuseAnalysis;
}

VariableReuseAnalysis::VariableReuseAnalysis()
    : FunctionPass(ID),
      m_WIA(nullptr), m_LV(nullptr), m_DeSSA(nullptr),
      m_PatternMatch(nullptr), m_coalescingEngine(nullptr),
      m_pCtx(nullptr), m_RPE(nullptr), m_SimdSize(0),
      m_IsFunctionPressureLow(Status::Undef),
      m_IsBlockPressureLow(Status::Undef) {
  initializeVariableReuseAnalysisPass(*PassRegistry::getPassRegistry());
}

bool VariableReuseAnalysis::runOnFunction(Function &F)
{
  m_WIA = &(getAnalysis<WIAnalysis>());
  if (IGC_IS_FLAG_DISABLED(DisableDeSSA))
  {
	m_DeSSA = &getAnalysis<DeSSA>();
  }
  m_LV = &(getAnalysis<LiveVarsAnalysis>().getLiveVars());
  m_PatternMatch = &getAnalysis<CodeGenPatternMatch>();
  m_pCtx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
  m_coalescingEngine = &getAnalysis<CoalescingEngine>();

  // FIXME: enable RPE.
  // m_RPE = &getAnalysis<RegisterEstimator>();

  // Nothing but cleanup data from previous runs.
  reset();

  if (IGC_IS_FLAG_ENABLED(EnableVariableAlias) &&
      m_pCtx->platform.GetPlatformFamily() >= IGFX_GEN9_CORE)
  {
      visitLiveInstructions(&F);

      postProcessing();
  }

  return false;
}

static unsigned getMaxReuseDistance(uint16_t size) {
  return (size == 8) ? 10 : 5;
}

bool VariableReuseAnalysis::checkUseInst(Instruction *UseInst, LiveVars *LV) {
  BasicBlock *CurBB = UseInst->getParent();
  if (UseInst->isUsedOutsideOfBlock(CurBB))
    return false;

  // This situation can occur:
  //
  //     ,------.
  //     |      |
  //     |      v
  //     |   t2 = phi ... t1 ...
  //     |      |
  //     |      v
  //     |   t1 = ...
  //     |  ... = ... t1 ...
  //     |      |
  //     `------'
  //
  // Disallow reuse if t1 has a phi use.
  // Disallow reuse if t1 has a far away use when the pressure is not low.
  unsigned DefLoc = LV->getDistance(UseInst);
  unsigned FarUseLoc = 0;
  for (auto UI : UseInst->users()) {
    if (isa<PHINode>(UI))
      return false;

    auto Inst = dyn_cast<Instruction>(UI);
    if (!Inst)
      return false;
    unsigned UseLoc = LV->getDistance(Inst);
    FarUseLoc = std::max(FarUseLoc, UseLoc);
  }

  // When the whole function or block pressure is low, skip the distance check.
  if (isCurFunctionPressureLow() || isCurBlockPressureLow())
    return true;

  // Use distance to limit reuse.
  const unsigned FarUseDistance = getMaxReuseDistance(m_SimdSize);
  return FarUseLoc <= DefLoc + FarUseDistance;
}

bool VariableReuseAnalysis::checkDefInst(Instruction *DefInst,
                                         Instruction *UseInst, LiveVars *LV) {
  assert(DefInst && UseInst);
  if (isa<PHINode>(DefInst))
    return false;

  if (auto CI = dyn_cast<CallInst>(DefInst)) {
    Function *F = CI->getCalledFunction();
    // Do not reuse the return symbol of subroutine/stack calls.
    if (!F || !F->isDeclaration())
      return false;

    if (isa<GenIntrinsicInst>(DefInst)) {
      // Just skip all gen intrinsic calls. Some intrinsic calls may have
      // special meaning.
      return false;
    }
  }

  // This is a block level reuse.
  BasicBlock *CurBB = UseInst->getParent();
  if (DefInst->getParent() != CurBB || DefInst->isUsedOutsideOfBlock(CurBB))
    return false;

  // Check whether UseInst is the last use of DefInst. If not, this source
  // variable cannot be reused.
  Instruction *LastUse = LV->getLVInfo(DefInst).findKill(CurBB);
  if (LastUse != UseInst)
    return false;

  // When the whole function or block pressure is low, skip the distance check.
  if (isCurFunctionPressureLow() || isCurBlockPressureLow())
    return true;

  // Use distance to limit far reuses.
  unsigned DefLoc = LV->getDistance(DefInst);
  unsigned UseLoc = LV->getDistance(UseInst);
  const unsigned FarDefDistance = getMaxReuseDistance(m_SimdSize);
  return UseLoc <= DefLoc + FarDefDistance;
}

void VariableReuseAnalysis::visitLiveInstructions(Function* F)
{
    for (auto II = inst_begin(F), IE = inst_end(F); II != IE; ++II) {
        Instruction& I = *II;
        if (!m_PatternMatch->NeedInstruction(I))
            continue;
        visit(I);
    }
}

void VariableReuseAnalysis::postProcessing()
{
    //  m_ValueAliasMap's entry is a pair <aliaser, aliasee>, where
    //  aliaser is the key and aliasee is the value of the entry.
    // 
    // Normalizing alias map so that the following will be handled:
    //  1) alias chain relation
    //        a0 alias_to b0
    //        b0 alias_to b1
    //     Change to
    //        a0 alias_to b1
    //        b0 alias_to b1
    //    This make sure that any map value will not be a map key
    //  2) circular alias relation
    //     It might be possible to generate a circular alias relation like:
    //        a0 alias_to b0
    //        b0 alias_to b1
    //        b1 alias_to a0
    //     Change to the following by removing one of alias pair.
    //        a0 alias_to b1
    //        b0 alias_to b1
    //
    int sz = (int)m_ValueAliasMap.size();
    auto NI = m_ValueAliasMap.begin();
    auto IE = m_ValueAliasMap.end();
    for (auto II = NI; II != IE; II = NI)
    {
        ++NI;
        SSubVector& SV = II->second;
        Value* aliasee = SV.BaseVector;
        int off = SV.StartElementOffset;

        // Using k for checking circular alias relation.
        // With map's size = sz, it can do max looping of (sz -1)
        // trip count without revisiting an entry twice. If the loop
        // revisit an entry, it must have a circular alias relation.
        int k = 0;
        while (m_ValueAliasMap.count(aliasee) > 0 && k < sz)
        {
            ++k;
            SSubVector& tSV = m_ValueAliasMap[aliasee];
            off += tSV.StartElementOffset;
            aliasee = tSV.BaseVector;
        }
        if (k == sz)
        {
            // circular alias relation
            m_ValueAliasMap.erase(II);
            --sz;
            continue;
        }
        SV.BaseVector = aliasee;
        SV.StartElementOffset = off;
    }
}

void VariableReuseAnalysis::visitCallInst(CallInst& I)
{
    if (GenIntrinsicInst *CI = llvm::dyn_cast<GenIntrinsicInst>(&I))
    {
        switch (CI->getIntrinsicID()) {
        default:
            break;
        }
    }
}

void VariableReuseAnalysis::visitCastInst(CastInst& I)
{
    if (IGC_IS_FLAG_DISABLED(EnableVATemp))
        return;

    if (!canBeAlias(&I)) {
        return;
    }

    // Set alias of dst to CastInst's src
    // As CastInst is noop, its definition is dropped and
    // only its uses are merged to src's liveness info.
    Value* D = &I;
    Value* S = I.getOperand(0);
    auto II = m_ValueAliasMap.find(D);
    if (II == m_ValueAliasMap.end())
    {
        SSubVector SV;
        SV.BaseVector = S;
        SV.StartElementOffset = 0;
        m_ValueAliasMap.insert(std::make_pair(D, SV));

        // Extend S's liveness to contain D's
        m_LV->mergeUseFrom(S, D);
    }
    else {
        // Already alised, ignored (TODO: merge aliasee ?)
        return;
    }
}

bool VariableReuseAnalysis::isLocalValue(Value* V)
{
    Instruction* I = dyn_cast<Instruction>(V);
    if (!I)
        return false;
    BasicBlock* BB = I->getParent();
    return !m_LV->isLiveIn(I, *BB) && !m_LV->isLiveOut(I, *BB);
}

// Return true if V0 and V1's live ranges overlap, return false otherwise.
bool VariableReuseAnalysis::hasInterference(Value* V0, Value* V1)
{
    // Key assumption about dessa/liveness(LVInfo):
    //   1. Each Value's Liveness (LVInfo) has a single definition (it's true
    //      even we extend its liveness such as in patternmatch or elsewhere).
    //   2. If two values are combined, they generally should be included into
    //      the same congruent class, not by extending liveness to reflect both
    //      as doing so will violate the single definition for our liveness.
    //      For example:
    //          1:   v0 = 10
    //          2:      = v0 (last use)
    //          3:   v1 = 20
    //          4:      = v1 (last use)
    //        v0 and v1 will be in the same congruent class. We will not extend
    //        v0's liveness to "4", just {v0, v1} in the same congruent class.
    //        However, for the following:
    //          1:   v0 = 10
    //          2:      = v0 (last use)
    //          3:   v1 = bitcast v0
    //          4:      = v1 (last use) 
    //        then we can just extend v0's liveness to "4".
    //        
    SmallVector<Value*, 8> V0cc;  // V0's congruent class
    SmallVector<Value*, 8> V1cc;  // V1's congruent class
    if (m_DeSSA) {
        m_DeSSA->getAllValuesInCongruentClass(V0, V0cc);
        m_DeSSA->getAllValuesInCongruentClass(V1, V1cc);
    }
    else {
        V0cc.push_back(V0);
        V1cc.push_back(V1);
    }

    // Check every pair of values in two congruent classes
    for (int i = 0, sz0 = (int)V0cc.size(); i < sz0; ++i)
    {
        Value *val0 = V0cc[i];
        for (int j = 0, sz1 = (int)V1cc.size(); j < sz1; ++j)
        {
            Value* val1 = V1cc[j];
            if (m_LV->hasInterference(val0, val1))
                return true;
        }
    }

    return false;
}

// Check if From can be an alias to To. Return true if so. It is used
// for checking alias-possible instructions such as bitcast/inttoptr/ptrtoint
//     "From" can be an alias to "To" if and only if "To" will
//     not be changed during the entire live range of "From".
// This is trivial for LLVM IR, as LLVM IR is SSA. But after DeSSA,
// need to check other values in the congruent class of "To".
//
bool VariableReuseAnalysis::canBeAlias(CastInst* I)
{
    if (hasBeenPayloadCoalesced(I)) {
        return false;
    }
    if (!isNoOpInst(I, m_pCtx)) {
        return false;
    }

    // Set alias of dst to CastInst's src
    // As CastInst is noop, its definition is dropped and
    // only its uses are merged to src's liveness info.
    Value* D = I;
    Value* S = I->getOperand(0);
    if (isa<Constant>(S)) {
        return false;
    }

    if (hasBeenPayloadCoalesced(S)) {
        return false;
    }

    if (!m_DeSSA) {
        // No congruent class, so it can be alias!
        return true;
    }

    // If D is in a congruent class or both D and S have different
    // uniform property, give up.   
    if (m_DeSSA->getRootValue(D) ||
        (m_WIA && m_WIA->whichDepend(D) != m_WIA->whichDepend(S))) {
        return false;
    }

    SmallVector<Value*, 8> Scc;    // S's congruent class
    m_DeSSA->getAllValuesInCongruentClass(S, Scc);
    for (int i = 0, sz0 = (int)Scc.size(); i < sz0; ++i)
    {
        Value *v0 = Scc[i];
        if (v0 != S && m_LV->hasInterference(D, v0)) {
            return false;
        }
    }
    return true;        
}