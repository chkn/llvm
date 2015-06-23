//===-- MSILBackend.h - TargetMachine for the MSIL --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the MSILWriter that is used by the MSIL.
//
//===----------------------------------------------------------------------===//
#ifndef MSILBACKEND_H
#define MSILBACKEND_H

#include "llvm/Pass.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/CodeGen/IntrinsicLowering.h"

namespace llvm {
  extern Target TheMSILTarget;

  void initializeMSILModulePass(PassRegistry&);

  class MSILWriter;

  class MSILModule : public ModulePass {
    Module *ModulePtr;

  public:
    MSILWriter* Writer;

    static char ID;
    MSILModule() : ModulePass(ID) {
      initializeMSILModulePass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<FindUsedTypes>();
      AU.addRequired<DataLayoutPass>();
    }

    virtual const char *getPassName() const {
      return "MSIL backend definitions";
    }

    virtual bool doInitialization(Module &M) override;

    virtual bool runOnModule(Module &M) override;

    bool lowerIntrinsics(Module &M);
    bool runOnBasicBlock(BasicBlock &BB, IntrinsicLowering &IL);
  };

  void initializeMSILWriterPass(PassRegistry&);

  class MSILWriter : public FunctionPass {
    struct StaticInitializer {
      const Constant* constant;
      uint64_t offset;
      
      StaticInitializer()
        : constant(0), offset(0) {}

      StaticInitializer(const Constant* _constant, uint64_t _offset)
        : constant(_constant), offset(_offset) {} 
    };

    uint64_t UniqID;

    uint64_t getUniqID() {
      return ++UniqID;
    }

  public:
    formatted_raw_ostream *Out;
    Module *ModulePtr;
    const DataLayout *TD;
    LoopInfo *LInfo;
    SetVector<Type *> UsedTypes;
    std::vector<StaticInitializer>* InitListPtr;
    std::map<const GlobalVariable*,std::vector<StaticInitializer> >
      StaticInitList;
    static char ID;
    DenseMap<const Value*, unsigned> AnonValueNumbers;
    unsigned NextAnonValueNumber;

    MSILWriter() : FunctionPass(ID), NextAnonValueNumber(0) {
      initializeMSILWriterPass(*PassRegistry::getPassRegistry());
      UniqID = 0;
    }

    enum ValueType {
      UndefVT,
      GlobalVT,
      InternalVT,
      ArgumentVT,
      LocalVT,
      ConstVT,
      ConstExprVT
    };

    bool isVariable(ValueType V) {
      return V==GlobalVT || V==InternalVT || V==ArgumentVT || V==LocalVT;
    }

    bool isConstValue(ValueType V) {
      return V==ConstVT || V==ConstExprVT;
    }

    virtual const char *getPassName() const { return "MSIL backend"; }

    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<MSILModule>();
      AU.addRequired<LoopInfo>();
      AU.setPreservesAll();
    }

    bool runOnFunction(Function &F);

    virtual bool doInitialization(Module &M);

    virtual bool doFinalization(Module &M);

    void printStartup();
    void printModuleStartup();

    bool isZeroValue(const Value* V);

    std::string getValueName(const Value* V, bool WrapInSpaces = true);

    std::string getLabelName(const Value* V);

    std::string getLabelName(const std::string& Name);

    std::string getConvModopt(CallingConv::ID CallingConvID);

    std::string getArrayTypeName(Type::TypeID TyID, const Type* Ty);

    std::string getPrimitiveTypeName(const Type* Ty, bool isSigned);

    std::string getFunctionTypeName(const Type* Ty);

    std::string getPointerTypeName(const Type* Ty);

    std::string getTypeToken(const Type* Ty);

    std::string getILClassTypeToken(const Type* Ty);

    std::string getILGenericTypes(const Type* Ty);

    std::string getTypeName(const Type* Ty, bool isSigned = false,
                            bool isNested = false, bool isToken = false);

    std::string getFunctionRetTypeName(const FunctionType *FTy);

    std::string getFunctionArgTypeName(const FunctionType *FTy,
                                       Function::const_arg_iterator Arg);

    ValueType getValueLocation(const Value* V);

    std::string getTypePostfix(const Type* Ty, bool Expand,
                               bool isSigned = false);

    void printConvToPtr();

    void printPtrLoad(uint64_t N);

    void printValuePtrLoad(const Value* V);

    void printConstLoad(const Constant* C);

    void printValueLoad(const Value* V, bool LoadValueAddress = false);

    void printValueSave(const Value* V);

    void printBinaryInstruction(const char* Name, const Value* Left,
                                const Value* Right);

    void printSimpleInstruction(const char* Inst, const char* Operand = NULL);

    void printPHICopy(const BasicBlock* Src, const BasicBlock* Dst);

    void printBranchToBlock(const BasicBlock* CurrBB,
                            const BasicBlock* TrueBB,
                            const BasicBlock* FalseBB);

    void printBranchInstruction(const BranchInst* Inst);

    void printSelectInstruction(const Value* Cond, const Value* VTrue,
                                const Value* VFalse);

    void printIndirectLoad(const Value* V);

    void printLoadInstruction(const Value* Val);

    void printIndirectSave(const Value* Ptr, const Value* Val);

    void printIndirectSave(const Type* Ty);

    void printStoreInstruction(const Value* Ptr, const Value* Val);

    void printCastInstruction(unsigned int Op, const Value* V,
                              const Type* Ty, const Type* SrcTy=0);

    void printGepInstruction(const Value* V, gep_type_iterator I,
                             gep_type_iterator E);

    enum CLICallType {
      CLI_Native,
      CLI_Ctor,
      CLI_Static,
      CLI_Instance
    };

    std::string getCallSignature(const FunctionType* Ty,
                                 const Instruction* Inst,
                                 std::string Name, CLICallType = CLI_Native);

    void printFunctionCall(const Value* FnVal, const Instruction* Inst);

    typedef SmallVector<Value *, 6> FnArgs;
    void printFunctionCallArgs(CallingConv::ID CC, FnArgs &Args);

    void printManagedStaticCall(const Function* Fn, const Instruction* Inst);

    void printManagedInstanceCall(const Function* Fn, const Instruction* Inst);

    void printNewObjCall(const Function* Fn, const Instruction* Inst);

    void printIntrinsicCall(const CallInst* Inst);

    void printCallInstruction(const Instruction* Inst);

    void printICmpInstruction(unsigned Predicate, const Value* Left,
                              const Value* Right);

    void printFCmpInstruction(unsigned Predicate, const Value* Left,
                              const Value* Right);

    void printInvokeInstruction(const InvokeInst* Inst);

    void printSwitchInstruction(const SwitchInst* Inst);

    void printVAArgInstruction(const VAArgInst* Inst);

    void printAllocaInstruction(const AllocaInst* Inst, bool &NeedsValueSave);

    void printInstruction(const Instruction* Inst, bool &NeedsValueSave);

    void printLoop(const Loop* L);

    void printBasicBlock(const BasicBlock* BB);

    void printLocalVariables(const Function& F);

    void printFunctionBody(const Function& F);

    void printConstantExpr(const ConstantExpr* CE);

    void printStaticInitializerList();

    void printFunction(const Function& F);

    void printDeclarations(const ValueSymbolTable& ST);

    unsigned int getBitWidth(const Type* Ty);

    void printStaticConstant(const Constant* C, uint64_t& Offset);

    void printStaticInitializer(const Constant* C, const std::string& Name);

    void printGlobalConstructors(const GlobalVariable* G);

    void printVariableDefinition(const GlobalVariable* G);

    void printGlobalVariables();

    const char* getLibraryName(const Function* F);

    const char* getLibraryName(const GlobalVariable* GV); 
    
    const char* getLibraryForSymbol(StringRef Name, bool isFunction,
                                    CallingConv::ID CallingConv);

    void printExternals();

    // Managed type helpers

    bool isManagedType(const Type* Ty);
    bool isValueClassType(const Type* Ty);
    Type* getLocalType(Type* Ty);
    bool getManagedName(const Type* Ty, std::string& Assembly,
                        std::string& Type);
    bool hasRecursiveManagedType(const Type* Ty, Type*& RecTy);
  };

}

#endif

