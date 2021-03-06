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

#include "PromoteResourceToDirectAS.h"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include "common/LLVMWarningsPop.hpp"
#include "common/IGCIRBuilder.h"
#include "common/igc_regkeys.hpp"

#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CodeGenPublicEnums.h"

#include "common/igc_regkeys.hpp"

using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;

// Register pass to igc-opt
#define PASS_FLAG "igc-promote-resources-to-direct-addrspace"
#define PASS_DESCRIPTION "Pass promotes indirect addrspace resource access to direct addrspace"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(PromoteResourceToDirectAS, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(PromoteResourceToDirectAS, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char PromoteResourceToDirectAS::ID = 0;

PromoteResourceToDirectAS::PromoteResourceToDirectAS()
    : FunctionPass(ID)
{
    initializePromoteResourceToDirectASPass(*PassRegistry::getPassRegistry());
}

bool PromoteResourceToDirectAS::runOnFunction(Function &F)
{
    if(IGC_IS_FLAG_ENABLED(DisablePromoteToDirectAS))
    {
        return false;
    }

    m_pCodeGenContext = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    m_pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    visit(F);
    return true;
}

// Get the buffer pointer operand for supported buffer access instructions
Value* GetBufferOperand(Instruction* inst)
{
    Value* pBuffer = nullptr;
    if (LoadInst* load = dyn_cast<LoadInst>(inst))
    {
        pBuffer = load->getPointerOperand();
    }
    else if (StoreInst* store = dyn_cast<StoreInst>(inst))
    {
        pBuffer = store->getPointerOperand();
    }
    else if (GenIntrinsicInst* intr = dyn_cast<GenIntrinsicInst>(inst))
    {
        switch (intr->getIntrinsicID())
        {
            case GenISAIntrinsic::GenISA_storerawvector_indexed:
            case GenISAIntrinsic::GenISA_ldrawvector_indexed:
            case GenISAIntrinsic::GenISA_storeraw_indexed:
            case GenISAIntrinsic::GenISA_ldraw_indexed:
                pBuffer = intr->getOperand(0);
                break;
            case GenISAIntrinsic::GenISA_intatomicraw:
            case GenISAIntrinsic::GenISA_floatatomicraw:
            case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
            case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
            case GenISAIntrinsic::GenISA_intatomicrawA64:
            case GenISAIntrinsic::GenISA_floatatomicrawA64:
            case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
            case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
                pBuffer = intr->getOperand(1);
                break;
            default:
                break;
        }
    }
    return pBuffer;
}

// Determine the new buffer type
Type* GetBufferAccessType(Instruction *inst)
{
    if (LoadInst* load = dyn_cast<LoadInst>(inst))
    {
        return load->getType();
    }
    else if (StoreInst* store = dyn_cast<StoreInst>(inst))
    {
        return store->getOperand(0)->getType();
    }
    else if (GenIntrinsicInst* pIntr = dyn_cast<GenIntrinsicInst>(inst))
    {
        switch(pIntr->getIntrinsicID())
        {
            case GenISAIntrinsic::GenISA_storeraw_indexed:
            case GenISAIntrinsic::GenISA_storerawvector_indexed:
                return pIntr->getOperand(2)->getType();
            case GenISAIntrinsic::GenISA_ldrawvector_indexed:
            case GenISAIntrinsic::GenISA_ldraw_indexed:
            case GenISAIntrinsic::GenISA_intatomicraw:
            case GenISAIntrinsic::GenISA_floatatomicraw:
            case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
            case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
            case GenISAIntrinsic::GenISA_intatomicrawA64:
            case GenISAIntrinsic::GenISA_floatatomicrawA64:
            case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
            case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
                return pIntr->getType();
            default:
                break;
        }
    }

    assert(0 && "Unsupported buffer access intrinsic");
    return inst->getType();
}

void PromoteResourceToDirectAS::PromoteSamplerTextureToDirectAS(GenIntrinsicInst *&pIntr, Value* resourcePtr)
{
    IGCIRBuilder<> builder(pIntr);

    unsigned addrSpace = resourcePtr->getType()->getPointerAddressSpace();

    if (addrSpace != 1 && addrSpace != 2 && IGC::IsDirectIdx(addrSpace))
    {
        // Already direct addrspace, no need to promote
        // Only try to promote bindless pointers ( as(1) or as(2) ), or indirect buffer access
        return;
    }
    unsigned bufID;
    BufferType bufTy;
    BufferAccessType accTy;
    bool canPromote = false;

    Value* srcPtr = IGC::TracePointerSource(resourcePtr);
    if (srcPtr)
    {
        // Trace the resource pointer.
        // If we can find it, we can promote the indirect access to direct access
        // by encoding the BTI as a direct addrspace
        if (srcPtr->getType()->isPointerTy() &&
            IGC::GetResourcePointerInfo(srcPtr, bufID, bufTy, accTy))
        {
            canPromote = true;
        }
        else if (Argument* argPtr = dyn_cast<Argument>(srcPtr))
        {
            // Source comes from kernel arguments
            // We only promote if the argument comes from the entry function.
            // Default to bindless if sampler called from subroutine.
            Function* function = argPtr->getParent();
            if (isEntryFunc(m_pMdUtils, function))
            {
                assert(m_pCodeGenContext->type == ShaderType::OPENCL_SHADER);

                IGCMD::ResourceAllocMetaDataHandle resAllocMD = m_pMdUtils->getFunctionsInfoItem(function)->getResourceAlloc();
                IGCMD::ArgAllocMetaDataHandle argInfo = resAllocMD->getArgAllocsItem(argPtr->getArgNo());

                if (argInfo->getType() == IGCMD::ResourceTypeEnum::BindlessUAVResourceType)
                {
                    bufID = (unsigned) argInfo->getIndex();
                    bufTy = BufferType::UAV;
                    canPromote = true;
                }
                else if (argInfo->getType() == IGCMD::ResourceTypeEnum::BindlessSamplerResourceType)
                {
                    bufID = (unsigned) argInfo->getIndex();
                    bufTy = BufferType::SAMPLER;
                    canPromote = true;
                }
            }
        }
    }

    if (canPromote)
    {
        addrSpace = IGC::EncodeAS4GFXResource(*builder.getInt32(bufID), bufTy, 0);
        PointerType* newptrType = PointerType::get(resourcePtr->getType()->getPointerElementType(), addrSpace);
        Constant* mutePtr = ConstantPointerNull::get(newptrType);
        IGC::ChangePtrTypeInIntrinsic(pIntr, resourcePtr, mutePtr);
    }
}

bool PatchGetElementPtr(const std::vector<Value*> &instList, Type* dstTy, unsigned directAS, Value* patchedSourcePtr, Value* &dstPtr)
{
    unsigned numInstructions = instList.size();
    Value* patchedInst = patchedSourcePtr;
    dstPtr = nullptr;

    // Find all the instructions we need to patch, starting from the top.
    // If there is more than one GEP instruction, we need to patch all of them, as well
    // as any pointer casts. All other instructions are not supported.
    // %0 = getelementptr int, int addrspace(1)* %ptr, i32 3
    // %1 = bitcast int addrspace(1)* %0 to float addrspace(1)*
    // %2 = getelementptr float, float addrspace(1)* %1, i32 8
    // PROMOTED TO:
    // %0 = getelementptr int, int addrspace(131072)* null, i32 3
    // %1 = bitcast int addrspace(131072)* %0 to float addrspace(131072)*
    // %2 = getelementptr float, float addrspace(131072)* %1, i32 8
    std::vector<Value*> patchInstructions;
    for(int i = numInstructions - 1; i >= 0; i--)
    {
        Value* inst = instList[i];
        if(isa<GetElementPtrInst>(inst))
        {
            patchInstructions.push_back(inst);
        }
        else if(BitCastInst* cast = dyn_cast<BitCastInst>(inst))
        {
            // Bitcast from pointer to pointer
            if(cast->getType()->isPointerTy() && cast->getOperand(0)->getType()->isPointerTy())
                patchInstructions.push_back(inst);
        }
    }

    if (!patchedInst)
    {
        Type* patchTy = nullptr;
        if (patchInstructions.size() > 0)
        {
            // Get the original pointer type before any GEPs or bitcasts modifies it
            patchTy = cast<Instruction>(patchInstructions[0])->getOperand(0)->getType()->getPointerElementType();
        }
        else
        {
            // If there is nothing to patch, set the pointer type to the same type as the buffer access type
            patchTy = dstTy;
        }
        PointerType* newptrType = PointerType::get(patchTy, directAS);
        patchedInst = ConstantPointerNull::get(newptrType);
    }

    for (unsigned i = 0; i < (unsigned) patchInstructions.size(); i++)
    {
        Value* inst = patchInstructions[i];
        if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(inst))
        {
            llvm::SmallVector<llvm::Value*, 4> gepArgs(gepInst->idx_begin(), gepInst->idx_end());
            // Create the new GEP instruction
            if (gepInst->isInBounds())
                patchedInst = GetElementPtrInst::CreateInBounds(nullptr, patchedInst, gepArgs, "", gepInst);
            else
                patchedInst = GetElementPtrInst::Create(nullptr, patchedInst, gepArgs, "", gepInst);
        }
        else if (BitCastInst* cast = dyn_cast<BitCastInst>(inst))
        {
            PointerType* newptrType = PointerType::get(cast->getType()->getPointerElementType(), directAS);
            patchedInst = BitCastInst::Create(Instruction::BitCast, patchedInst, newptrType, "", cast);
        }
        else
        {
            assert(0 && "Can not patch unsupported instruction!");
            return false;
        }
    }

    dstPtr = patchedInst;

    // Final types must match
    return (dstPtr->getType()->getPointerElementType() == dstTy);
}

bool PatchInstructionAddressSpace(const std::vector<Value*> &instList, Type* dstTy, unsigned directAS, Value* &dstPtr)
{
    unsigned numInstructions = instList.size();
    dstPtr = nullptr;
    bool success = false;

    // Find the first PHI node or select we need to patch, if any.
    // In the most simple case, we assume only one branching instruction. If there are multiple selects, phis, or any
    // combination of the two, we won't be able to handle it.

    // First, we find the phi/select instruction. We patch all the GEP and ptrcast instructions for each branch, then
    // finally any GEP and ptrcast instructions that comes after the phi/select, but before the load
    PHINode* phiNode = nullptr;
    SelectInst* selectInst = nullptr;
    std::vector<Value*> remainingInst;
    for(unsigned i = 0; i < numInstructions; i++)
    {
        Value* inst = instList[i];
        if (PHINode* phi = dyn_cast<PHINode>(inst))
        {
            phiNode = phi;
            break;
        }
        else if (SelectInst *select = dyn_cast<SelectInst>(inst))
        {
            selectInst = select;
            break;
        }
        else
        {
            remainingInst.push_back(inst);
        }
    }

    if (selectInst)
    {
        Value* newSelectInst = nullptr;
        Value* bufferPtr0 = nullptr;
        Value* bufferPtr1 = nullptr;
        std::vector<Value*> tempList0, tempList1;
        // Call trace again to get the instructions list for each branch of the select
        if(IGC::TracePointerSource(selectInst->getOperand(1), true, true, tempList0) &&
           IGC::TracePointerSource(selectInst->getOperand(2), true, true, tempList1))
        {
            assert(selectInst->getOperand(1)->getType()->isPointerTy() && selectInst->getOperand(2)->getType()->isPointerTy());
            Type* srcType0 = selectInst->getOperand(1)->getType()->getPointerElementType();
            Type* srcType1 = selectInst->getOperand(1)->getType()->getPointerElementType();

            // Patch both branches, then patch the select instruction
            if (PatchGetElementPtr(tempList0, srcType0, directAS, nullptr, bufferPtr0) &&
                PatchGetElementPtr(tempList1, srcType1, directAS, nullptr, bufferPtr1))
            {
                newSelectInst = SelectInst::Create(selectInst->getOperand(0), bufferPtr0, bufferPtr1, "", selectInst);
            }
            // If there are any remaining GEP/bitcast instructions after the select, patch them as well
            if (newSelectInst)
            {
                success = PatchGetElementPtr(remainingInst, dstTy, directAS, newSelectInst, dstPtr);
            }
        }
    }
    else if (phiNode)
    {
        PointerType* newPhiTy = PointerType::get(phiNode->getType()->getPointerElementType(), directAS);
        PHINode* pNewPhi = PHINode::Create(newPhiTy, phiNode->getNumIncomingValues(), "", phiNode);
        for(unsigned int i = 0; i < phiNode->getNumIncomingValues(); ++i)
        {
            Value* incomingVal = phiNode->getIncomingValue(i);
            assert(incomingVal->getType()->isPointerTy());

            std::vector<Value*> tempList;
            Value* srcPtr = IGC::TracePointerSource(incomingVal, true, true, tempList);

            // We know srcPtr is trace-able, since it's been traced already, we just need to get the
            // list of instructions we need to patch
            assert(srcPtr);

            // Patch the GEPs for each phi node path
            Value* bufferPtr = nullptr;
            Type* incomingTy = incomingVal->getType()->getPointerElementType();
            if (!PatchGetElementPtr(tempList, incomingTy, directAS, nullptr, bufferPtr))
            {
                // Patching must succeed for all paths
                pNewPhi->eraseFromParent();
                return false;
            }
            pNewPhi->addIncoming(bufferPtr, phiNode->getIncomingBlock(i));
        }

        // If there are any remaining GEP/bitcast instructions after the PHI node, patch them as well
        success = PatchGetElementPtr(remainingInst, dstTy, directAS, pNewPhi, dstPtr);
    }
    else
    {
        // If there are no PHI nodes or selects, we can just patch the GEPs
        success = PatchGetElementPtr(instList, dstTy, directAS, nullptr, dstPtr);
    }

    if(!dstPtr || !dstPtr->getType()->isPointerTy())
        return false;
    if(dstPtr->getType()->getPointerElementType() != dstTy)
        return false;

    return success;
}

void PromoteResourceToDirectAS::PromoteBufferToDirectAS(Instruction* inst, Value* resourcePtr)
{
    IGCIRBuilder<> builder(inst);

    unsigned addrSpace = resourcePtr->getType()->getPointerAddressSpace();

    if (addrSpace != 1 && addrSpace != 2 && IGC::IsDirectIdx(addrSpace))
    {
        // Already direct addrspace, no need to promote
        // Only try to promote stateless buffer pointers ( as(1) or as(2) ), or indirect buffer access
        return;
    }

    // Vulkan encodes address space differently, with the reserve bits set.
    // TODO: Investigate how addrspace is encoded in Vulkan,
    // for now skip promoting if it's an address space we dont recognize.
    if ((addrSpace & 0xFFE00000) != 0x0)
    {
        return;
    }

    std::vector<Value*> instructionList;
    Value* srcPtr = IGC::TracePointerSource(resourcePtr, false, true, instructionList);

    if (!srcPtr)
    {
        // Cannot trace the resource pointer back to it's source, cannot promote
        return;
    }

    unsigned bufferID;
    BufferType bufType;
    BufferAccessType accType;
    if (!IGC::GetResourcePointerInfo(srcPtr, bufferID, bufType, accType))
    {
        // Can't promote if we don't know the explicit buffer ID and type
        return;
    }

    // Get the new direct address space
    unsigned directAS = IGC::EncodeAS4GFXResource(*builder.getInt32(bufferID), bufType, 0);

    Value* pBuffer = nullptr;
    Type*  pBufferType = GetBufferAccessType(inst);

    if(!PatchInstructionAddressSpace(instructionList, pBufferType, directAS, pBuffer))
    {
        // Patching failed
        return;
    }

    if (LoadInst* load = dyn_cast<LoadInst>(inst))
    {
        LoadInst* newload = IGC::cloneLoad(load, pBuffer);
        load->replaceAllUsesWith(newload);
        load->eraseFromParent();
    }
    else if (StoreInst* store = dyn_cast<StoreInst>(inst))
    {
        StoreInst* newstore = IGC::cloneStore(store, store->getOperand(0), pBuffer);
        store->replaceAllUsesWith(newstore);
        store->eraseFromParent();
    }
    else if (GenIntrinsicInst* pIntr = dyn_cast<GenIntrinsicInst>(inst))
    {
        Value* pNewBufferAccessInst = nullptr;

        switch(pIntr->getIntrinsicID())
        {
            // TODO: ldraw and storeraw currently does not support non-aligned memory, if promote fails
            // then default alignment is 4. Need to implement support for ldraw and storeraw to support
            // non-aligned memory access, to preserve the alignment of the original load/store.

            // WA:
            // %522 = load <4 x i8> addrspace(131073)* %521
            // For this example instruction, InstructionCombining pass generates align4 if alignment
            // is not set. Forcing alignment to 1 generates the correct alignment value align2.
            // TODO: Why is no alignment and align1 treated differently by InstructionCombining?
            case GenISAIntrinsic::GenISA_ldraw_indexed:
            case GenISAIntrinsic::GenISA_ldrawvector_indexed:
            {
                Value* offsetVal = pIntr->getOperand(1);
                PointerType *ptrType = PointerType::get(pBufferType, directAS);
                pBuffer = builder.CreateIntToPtr(offsetVal, ptrType);

                unsigned alignment = cast<LdRawIntrinsic>(pIntr)->getAlignment();

                // Promote ldraw back to load
                pNewBufferAccessInst = builder.CreateAlignedLoad(pBuffer, alignment);
                break;
            }
            case GenISAIntrinsic::GenISA_storeraw_indexed:
            case GenISAIntrinsic::GenISA_storerawvector_indexed:
            {
                Value* offsetVal = pIntr->getOperand(1);
                PointerType *ptrType = PointerType::get(pBufferType, directAS);
                pBuffer = builder.CreateIntToPtr(offsetVal, ptrType);

                unsigned alignment = pBufferType->getScalarSizeInBits() / 8;

                // Promote storeraw back to store
                Value* storeVal = pIntr->getOperand(2);
                pNewBufferAccessInst = builder.CreateAlignedStore(storeVal, pBuffer, alignment);
                break;
            }
            default:
            {
                bool is64BitPtr = true;
                switch (pIntr->getIntrinsicID())
                {
                    case GenISAIntrinsic::GenISA_intatomicraw:
                    case GenISAIntrinsic::GenISA_floatatomicraw:
                    case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
                    case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
                        is64BitPtr = false;
                        break;
                    case GenISAIntrinsic::GenISA_intatomicrawA64:
                    case GenISAIntrinsic::GenISA_floatatomicrawA64:
                    case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
                    case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
                    default:
                        is64BitPtr = true;
                        break;
                }

                // clone atomicraw instructions
                llvm::SmallVector<llvm::Value*, 8> args;
                llvm::SmallVector<Type*, 3> types;

                PointerType* newptrType = PointerType::get(pBufferType, directAS);
                Value* sourcePointer = ConstantPointerNull::get(newptrType);
                Value* bufferAddress = nullptr;

                types.push_back(pIntr->getType());
                types.push_back(sourcePointer->getType());

                if (is64BitPtr)
                {
                    if (!isa<ConstantPointerNull>(pBuffer))
                    {
                        bufferAddress = pBuffer;
                    }
                    else
                    {
                        bufferAddress = sourcePointer;
                    }
                    types.push_back(bufferAddress->getType());
                }
                else
                {
                    if (!isa<ConstantPointerNull>(pBuffer))
                    {
                        bufferAddress = builder.CreatePtrToInt(pBuffer, builder.getInt32Ty());
                    }
                    else
                    {
                        bufferAddress = builder.getInt32(0);
                    }
                }

                args.push_back(sourcePointer);
                args.push_back(bufferAddress);
                for (unsigned i = 2; i < pIntr->getNumArgOperands(); i++)
                {
                    args.push_back(pIntr->getArgOperand(i));
                }

                Module* module = pIntr->getParent()->getParent()->getParent();
                Function* pFunc = GenISAIntrinsic::getDeclaration(module, pIntr->getIntrinsicID(), types);
                pNewBufferAccessInst = builder.CreateCall(pFunc, args);
                break;
            }
        }

        if (pNewBufferAccessInst)
        {
            Instruction* oldInst = inst;
            Instruction* newInst = cast<Instruction>(pNewBufferAccessInst);

            // Clone metadata
            llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> MDs;
            oldInst->getAllMetadata(MDs);
            for (llvm::SmallVectorImpl<std::pair<unsigned, llvm::MDNode *> >::iterator MI = MDs.begin(), ME = MDs.end(); MI != ME; ++MI)
            {
                newInst->setMetadata(MI->first, MI->second);
            }
            oldInst->replaceAllUsesWith(newInst);
            oldInst->eraseFromParent();
        }
    }
}

void PromoteResourceToDirectAS::PromoteStatelessToBindlessBuffers(Instruction* inst, Value* resourcePtr)
{
	IGCIRBuilder<> builder(inst);

	unsigned addrSpace = resourcePtr->getType()->getPointerAddressSpace();

	if (addrSpace != 1 && addrSpace != 2)
	{
		// Only try to promote stateless buffer pointers ( as(1) or as(2) )
		return;
	}

	if (!isa<LoadInst>(inst) && !isa<StoreInst>(inst))
	{
		// Do we need to support other instructions besides load/store?
		return;
	}

	std::vector<Value*> instructionList;
	Value* srcPtr = IGC::TracePointerSource(resourcePtr, false, true, instructionList);

	if (!srcPtr ||
		!srcPtr->getType()->isPointerTy() ||
		!isa<Argument>(srcPtr))
	{
		// Cannot trace the resource pointer back to it's source, cannot promote
		return;
	}

	// Calculate the buffer offset value, and fix the instructions in the trace path if needed
	Value* bufferOffset = builder.getInt32(0);
	if (Argument* argPtr = dyn_cast<Argument>(srcPtr))
	{
		Constant* nullPtr = ConstantPointerNull::get(cast<PointerType>(srcPtr->getType()));

		// The last instruction should be the argument
		assert(instructionList.back() == srcPtr);
		instructionList.pop_back();

		int numInstsInPath = (int) instructionList.size();

		// Check the list of instructions in the trace path. If there are any with multiple uses, we
		// want to clone all the instructions in the trace path so we don't interfere with the other users.
		bool needClonePath = false;
		for (int instIndex = numInstsInPath - 1; instIndex >= 0; instIndex--)
		{
			Instruction* nextInst = cast<Instruction>(instructionList[instIndex]);
			if (nextInst->getNumUses() > 1)
			{
				needClonePath = true;
				break;
			}
		}

		if (needClonePath)
		{
			// Clone each instruction in the trace path
			Value* replacementValue = srcPtr;
			Value* newValue = nullPtr;
			for (int instIndex = numInstsInPath - 1; instIndex >= 0; instIndex--)
			{
				Instruction* nextInst = cast<Instruction>(instructionList[instIndex]);
				Instruction* cloneInst = nextInst->clone();
				cloneInst->insertAfter(nextInst);
				cloneInst->replaceUsesOfWith(replacementValue, newValue);
				replacementValue = nextInst;
				newValue = cloneInst;
			}
			bufferOffset = builder.CreatePtrToInt(newValue, builder.getInt32Ty());
		}
		else
		{
			// If there is only one user, we just directly replace the pointer with a null
			if (numInstsInPath > 0)
			{
				cast<Instruction>(instructionList[numInstsInPath - 1])->replaceUsesOfWith(srcPtr, nullPtr);
				bufferOffset = builder.CreatePtrToInt(resourcePtr, builder.getInt32Ty());
			}
		}

		IGCMD::ResourceAllocMetaDataHandle resAllocMD = m_pMdUtils->getFunctionsInfoItem(inst->getParent()->getParent())->getResourceAlloc();
		IGCMD::ArgAllocMetaDataHandle argInfo = resAllocMD->getArgAllocsItem(argPtr->getArgNo());
		if (argInfo->getType() == IGCMD::ResourceTypeEnum::UAVResourceType)
		{
			// Update metadata to show bindless resource type
			argInfo->setType(IGCMD::ResourceTypeEnum::BindlessUAVResourceType);
		}
	}

	// Get the base bindless pointer
	unsigned bindlessAS = IGC::EncodeAS4GFXResource(*UndefValue::get(builder.getInt32Ty()), IGC::BINDLESS, 0);
	PointerType* basePointerType = PointerType::get(resourcePtr->getType()->getPointerElementType(), bindlessAS);
	Value* basePointer = builder.CreatePointerCast(srcPtr, basePointerType);

	if (LoadInst* load = dyn_cast<LoadInst>(inst))
	{
		Value* ldraw = IGC::CreateLoadRawIntrinsic(load, cast<Instruction>(basePointer), bufferOffset);
		load->replaceAllUsesWith(ldraw);
		load->eraseFromParent();
	}
	else if (StoreInst* store = dyn_cast<StoreInst>(inst))
	{
		IGC::CreateStoreRawIntrinsic(store, cast<Instruction>(basePointer), bufferOffset);
		store->eraseFromParent();
	}
}

void PromoteResourceToDirectAS::visitInstruction(Instruction &I)
{
    bool resourceAccessed = false;
    if (llvm::GenIntrinsicInst *pIntr = llvm::dyn_cast<llvm::GenIntrinsicInst>(&I))
    {
        // Figure out the intrinsic operands for texture & sampler
        llvm::Value *pTextureValue = nullptr, *pSamplerValue = nullptr;
        IGC::getTextureAndSamplerOperands(pIntr, pTextureValue, pSamplerValue);

        if (pTextureValue && pTextureValue->getType()->isPointerTy())
        {
            PromoteSamplerTextureToDirectAS(pIntr, pTextureValue);
            resourceAccessed = true;
        }
        if (pSamplerValue && pSamplerValue->getType()->isPointerTy())
        {
            PromoteSamplerTextureToDirectAS(pIntr, pSamplerValue);
            resourceAccessed = true;
        }
    }

    // Handle buffer access call instructions
    if (!resourceAccessed)
    {
        Value* bufptr = GetBufferOperand(&I);

        if (bufptr && bufptr->getType()->isPointerTy())
        {
			if (m_pCodeGenContext->type == ShaderType::OPENCL_SHADER)
			{
				if (static_cast<OpenCLProgramContext*>(m_pCodeGenContext)->m_InternalOptions.PromoteStatelessToBindless)
				{
					PromoteStatelessToBindlessBuffers(&I, bufptr);
				}
			}
			else
			{
				PromoteBufferToDirectAS(&I, bufptr);
			}
            resourceAccessed = true;
        }
    }
}

