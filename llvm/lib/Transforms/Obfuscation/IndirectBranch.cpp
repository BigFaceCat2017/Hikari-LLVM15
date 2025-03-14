// For open-source license, please refer to [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Obfuscation/Obfuscation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
using namespace llvm;
using namespace std;
namespace llvm {
struct IndirectBranch : public FunctionPass {
  static char ID;
  bool flag;
  bool initialized;
  map<BasicBlock *, unsigned long long> indexmap;
  IndirectBranch() : FunctionPass(ID) {
    this->flag = true;
    this->initialized = false;
  }
  IndirectBranch(bool flag) : FunctionPass(ID) {
    this->flag = flag;
    this->initialized = false;
  }
  StringRef getPassName() const override { return StringRef("IndirectBranch"); }
  bool initialize(Module &M) {
    vector<Constant *> BBs;
    unsigned long long i = 0;
    for (auto F = M.begin(); F != M.end(); F++) {
      for (auto BB = F->begin(); BB != F->end(); BB++) {
        BasicBlock *BBPtr = &*BB;
        if (BBPtr != &(BBPtr->getParent()->getEntryBlock())) {
          indexmap[BBPtr] = i++;
          BBs.push_back(BlockAddress::get(BBPtr));
        }
      }
    }
    ArrayType *AT =
        ArrayType::get(Type::getInt8PtrTy(M.getContext()), BBs.size());
    Constant *BlockAddressArray =
        ConstantArray::get(AT, ArrayRef<Constant *>(BBs));
    GlobalVariable *Table = new GlobalVariable(
        M, AT, false, GlobalValue::LinkageTypes::InternalLinkage,
        BlockAddressArray, "IndirectBranchingGlobalTable");
    appendToCompilerUsed(M, {Table});
    return true;
  }
  bool runOnFunction(Function &Func) override {
    if (!toObfuscate(flag, &Func, "indibr")) {
      return false;
    }
    if (this->initialized == false) {
      initialize(*Func.getParent());
      this->initialized = true;
    }
    errs() << "Running IndirectBranch On " << Func.getName() << "\n";
    vector<BranchInst *> BIs;
    for (inst_iterator I = inst_begin(Func); I != inst_end(Func); I++) {
      Instruction *Inst = &(*I);
      if (BranchInst *BI = dyn_cast<BranchInst>(Inst)) {
        BIs.push_back(BI);
      }
    } // Finish collecting branching conditions
    Value *zero =
        ConstantInt::get(Type::getInt32Ty(Func.getParent()->getContext()), 0);
    for (BranchInst *BI : BIs) {
      IRBuilder<> IRB(BI);
      vector<BasicBlock *> BBs;
      // We use the condition's evaluation result to generate the GEP
      // instruction  False evaluates to 0 while true evaluates to 1.  So here
      // we insert the false block first
      if (BI->isConditional()) {
        BBs.push_back(BI->getSuccessor(1));
      }
      BBs.push_back(BI->getSuccessor(0));
      ArrayType *AT = ArrayType::get(
          Type::getInt8PtrTy(Func.getParent()->getContext()), BBs.size());
      vector<Constant *> BlockAddresses;
      for (unsigned i = 0; i < BBs.size(); i++) {
        BlockAddresses.push_back(BlockAddress::get(BBs[i]));
      }
      GlobalVariable *LoadFrom = NULL;

      if (BI->isConditional() ||
          indexmap.find(BI->getSuccessor(0)) == indexmap.end()) {
        // Create a new GV
        Constant *BlockAddressArray =
            ConstantArray::get(AT, ArrayRef<Constant *>(BlockAddresses));
        LoadFrom = new GlobalVariable(
            *Func.getParent(), AT, false,
            GlobalValue::LinkageTypes::PrivateLinkage, BlockAddressArray,
            "HikariConditionalLocalIndirectBranchingTable");
        appendToCompilerUsed(*Func.getParent(), {LoadFrom});
      } else {
        LoadFrom = Func.getParent()->getGlobalVariable(
            "IndirectBranchingGlobalTable", true);
      }
      Value *index = NULL;
      if (BI->isConditional()) {
        Value *condition = BI->getCondition();
        index = IRB.CreateZExt(
            condition, Type::getInt32Ty(Func.getParent()->getContext()));
      } else {
        index =
            ConstantInt::get(Type::getInt32Ty(Func.getParent()->getContext()),
                             indexmap[BI->getSuccessor(0)]);
      }
#if LLVM_VERSION_MAJOR >= 15
      Value *GEP = IRB.CreateGEP(LoadFrom->getType()->getScalarType()->getNonOpaquePointerElementType(), LoadFrom, {zero, index});
      LoadInst *LI = IRB.CreateLoad(GEP->getType()->getNonOpaquePointerElementType(), GEP, "IndirectBranchingTargetAddress");
#elif LLVM_VERSION_MAJOR >= 14
      Value *GEP = IRB.CreateGEP(LoadFrom->getType()->getScalarType()->getPointerElementType(), LoadFrom, {zero, index});
      LoadInst *LI = IRB.CreateLoad(GEP->getType()->getPointerElementType(), GEP, "IndirectBranchingTargetAddress");
#else
      Value *GEP = IRB.CreateGEP(LoadFrom, {zero, index});
      LoadInst *LI = IRB.CreateLoad(GEP, "IndirectBranchingTargetAddress");
#endif
      IndirectBrInst *indirBr = IndirectBrInst::Create(LI, BBs.size());
      for (BasicBlock *BB : BBs) {
        indirBr->addDestination(BB);
      }
      ReplaceInstWithInst(BI, indirBr);
    }
    return true;
  }
  virtual bool doFinalization(Module &M) override {
    indexmap.clear();
    initialized = false;
    return false;
  }
};
} // namespace llvm
FunctionPass *llvm::createIndirectBranchPass() { return new IndirectBranch(); }
FunctionPass *llvm::createIndirectBranchPass(bool flag) {
  return new IndirectBranch(flag);
}
char IndirectBranch::ID = 0;
INITIALIZE_PASS(IndirectBranch, "indibran", "IndirectBranching", true, true)
