//===-- MSILBackend.cpp - Library for converting LLVM code to MSIL --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This library converts LLVM code to MSIL code.
//
//===----------------------------------------------------------------------===//

#include "MSILBackend.h"
#include "MSILTargetMachine.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/Passes.h"
using namespace llvm;

extern "C" void LLVMInitializeMSILTarget() {
  // Register the target.
  RegisterTargetMachine<MSILTargetMachine> X(TheMSILTarget);
}

bool MSILModule::doInitialization(Module &M) {
  ModulePtr = &M;
  return false;
}

bool MSILModule::runOnModule(Module &M) {
  ModulePtr = &M;
  Writer->TD = &getAnalysis<DataLayoutPass>().getDataLayout();
  Writer->printStartup();

  bool Changed = false;
  // Find named types.
  ValueSymbolTable& Table = M.getValueSymbolTable();
  const SetVector<Type *>& Types = getAnalysis<FindUsedTypes>().getTypes();
  Writer->UsedTypes = Types;

#if 0
  for (ValueSymbolTable::iterator I = Table.begin(), E = Table.end(); I!=E; ) {
    if (!I->second->getType()->isStructTy() /*&& !I->second->getType()->isOpaqueTy()*/)
      Table.remove(I++);
    else {
      std::set<const Type *>::iterator T = Types.find(I->second);
      if (T==Types.end())
        Table.remove(I++);
      else {
        Types.erase(T);
        ++I;
      }
    }
  }

  // Find unnamed types.
  unsigned RenameCounter = 0;
  for (std::set<const Type *>::const_iterator I = Types.begin(),
       E = Types.end(); I!=E; ++I)
    if (const StructType *STy = dyn_cast<StructType>(*I)) {
      while (ModulePtr->addTypeName("unnamed$"+utostr(RenameCounter), STy))
        ++RenameCounter;
      Changed = true;
    }
  // Pointer for FunctionPass.
  UsedTypes = &getAnalysis<FindUsedTypes>().getTypes();
#endif

  Changed |= lowerIntrinsics(M);

  return Changed;
}

bool MSILModule::lowerIntrinsics(Module &M) {
  bool dirty = false;

  IntrinsicLowering IL(*Writer->TD);
  IL.AddPrototypes(M);

  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    for (Function::iterator FI = I->begin(), FE = I->end(); FI != FE; ++FI)
      dirty |= runOnBasicBlock(*FI, IL);
  }

  return dirty;
}

static bool isManagedIntrinsic(Intrinsic::ID Id) {
  switch(Id) {
  default: return false;
  case Intrinsic::cil_ldstr:
  case Intrinsic::cil_ldnull:
  case Intrinsic::cil_newobj:
  case Intrinsic::cil_newvalue:
  case Intrinsic::cil_copyvalue:
  case Intrinsic::cil_newarr:
  case Intrinsic::cil_ldarr:
  case Intrinsic::cil_starr:
  case Intrinsic::cil_box:
    return true;
  }
}

bool MSILModule::runOnBasicBlock(BasicBlock &BB, IntrinsicLowering &IL) {
  bool dirty = false;
  for (BasicBlock::iterator BI = BB.begin(), BE = BB.end(); BI != BE;) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(&*BI);
    ++BI;

    if (!II) continue;
    Intrinsic::ID Id = II->getIntrinsicID();

    if (isManagedIntrinsic(Id))
      continue;
    
    switch (Id) {
    default:
      IL.LowerIntrinsicCall(II);
      dirty = true;
      break;
    case Intrinsic::vastart:
    case Intrinsic::vaend:
    case Intrinsic::vacopy:
      continue;
    }
  }

  return dirty;
}

char MSILModule::ID = 0;
INITIALIZE_PASS_BEGIN(MSILModule, "msil-module",
                "MSIL module pass", false, false)
INITIALIZE_PASS_DEPENDENCY(DataLayoutPass)
INITIALIZE_PASS_DEPENDENCY(FindUsedTypes)
INITIALIZE_PASS_END(MSILModule, "msil-module",
                "MSIL module pass", false, false)

char MSILWriter::ID = 0;
INITIALIZE_PASS_BEGIN(MSILWriter, "msil-writer",
                "MSIL writer pass", false, false)
INITIALIZE_PASS_DEPENDENCY(MSILModule)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_END(MSILWriter, "msil-writer",
                "MSIL writer pass", false, false)

bool MSILWriter::runOnFunction(Function &F) {
  if (F.isDeclaration()) return false;

  // Do not codegen any 'available_externally' functions at all, they have
  // definitions *Outside the translation unit.
  if (F.hasAvailableExternallyLinkage())
    return false;

  LInfo = &getAnalysis<LoopInfo>();
  printFunction(F);
  return false;
}

bool MSILWriter::doInitialization(Module &M) {
  ModulePtr = &M;
  return false;
}

bool MSILWriter::doFinalization(Module &M) {
  return false;
}

void MSILWriter::printStartup() {
  *Out << ".assembly extern mscorlib {}\n";
  *Out << ".assembly MSIL {}\n\n";
  *Out << "// External\n";
  printExternals();
  *Out << "// Declarations\n";
  printDeclarations(ModulePtr->getValueSymbolTable());
  *Out << "// Definitions\n";
  printGlobalVariables();
  *Out << "// Startup code\n";
  printModuleStartup();
}

void MSILWriter::printModuleStartup() {
  *Out <<
  ".method static public int32 $MSIL_Startup() {\n"
  "\t.entrypoint\n"
  "\t.locals (native int i)\n"
  "\t.locals (native int argc)\n"
  "\t.locals (native int ptr)\n"
  "\t.locals (void* argv)\n"
  "\t.locals (string[] args)\n"
  "\tcall\tstring[] [mscorlib]System.Environment::GetCommandLineArgs()\n"
  "\tdup\n"
  "\tstloc\targs\n"
  "\tldlen\n"
  "\tconv.i4\n"
  "\tdup\n"
  "\tstloc\targc\n";
  printPtrLoad(TD->getPointerSize());
  *Out <<
  "\tmul\n"
  "\tlocalloc\n"
  "\tstloc\targv\n"
  "\tldc.i4.0\n"
  "\tstloc\ti\n"
  "L_01:\n"
  "\tldloc\ti\n"
  "\tldloc\targc\n"
  "\tceq\n"
  "\tbrtrue\tL_02\n"
  "\tldloc\targs\n"
  "\tldloc\ti\n"
  "\tldelem.ref\n"
  "\tcall\tnative int [mscorlib]System.Runtime.InteropServices.Marshal::"
           "StringToHGlobalAnsi(string)\n"
  "\tstloc\tptr\n"
  "\tldloc\targv\n"
  "\tldloc\ti\n";
  printPtrLoad(TD->getPointerSize());
  *Out << 
  "\tmul\n"
  "\tadd\n"
  "\tldloc\tptr\n"
  "\tstind.i\n"
  "\tldloc\ti\n"
  "\tldc.i4.1\n"
  "\tadd\n"
  "\tstloc\ti\n"
  "\tbr\tL_01\n"
  "L_02:\n"
  "\tcall void $MSIL_Init()\n";

  // Call user 'main' function.
  const Function* F = ModulePtr->getFunction("main");
  if (!F || F->isDeclaration()) {
    *Out << "\tldc.i4.0\n\tret\n}\n";
    return;
  }
  bool BadSig = true;
  std::string Args("");
  Function::const_arg_iterator Arg1,Arg2;

  switch (F->arg_size()) {
  case 0:
    BadSig = false;
    break;
  case 1:
    Arg1 = F->arg_begin();
    if (Arg1->getType()->isIntegerTy()) {
      *Out << "\tldloc\targc\n";
      Args = getFunctionArgTypeName(F->getFunctionType(), Arg1);
      BadSig = false;
    }
    break;
  case 2:
    Arg1 = Arg2 = F->arg_begin(); ++Arg2;
    if (Arg1->getType()->isIntegerTy() && 
        Arg2->getType()->getTypeID() == Type::PointerTyID) {
      *Out << "\tldloc\targc\n\tldloc\targv\n";
      Args = getFunctionArgTypeName(F->getFunctionType(), Arg1)+","+
        getFunctionArgTypeName(F->getFunctionType(), Arg2);
      BadSig = false;
    }
    break;
  default:
    BadSig = true;
  }

  bool RetVoid = (F->getReturnType()->getTypeID() == Type::VoidTyID);
  if (BadSig || (!F->getReturnType()->isIntegerTy() && !RetVoid)) {
    *Out << "\tldc.i4.0\n";
  } else {
    *Out << "\tcall\t" << getFunctionRetTypeName(F->getFunctionType()) <<
      getConvModopt(F->getCallingConv()) << "main(" << Args << ")\n";
    if (RetVoid)
      *Out << "\tldc.i4.0\n";
    else
      *Out << "\tconv.i4\n";
  }
  *Out << "\tret\n}\n";
}

bool MSILWriter::isZeroValue(const Value* V) {
  if (const Constant *C = dyn_cast<Constant>(V))
    return C->isNullValue();
  return false;
}

static std::string DemangleName(StringRef Name) {
  if (!Name.endswith("]"))
    return Name.str();
  return Name.substr(0, Name.find_last_of("["));
}

static std::string GetFunctionRecordName(StringRef Name) {
  StringRef::size_type I = Name.find_last_of("::");
  if (I == StringRef::npos)
    return StringRef();
  return Name.substr(0, --I);
}

static std::string GetFunctionMethodName(StringRef Name) {
  StringRef::size_type I = Name.find_last_of("::");
  if (I == StringRef::npos)
    return Name;
  return Name.substr(--I);
}

std::string MSILWriter::getValueName(const Value* V, bool WrapInQuotes) {
  std::string Name;
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(V))
    Name = GV->getName();
  else if (!V->getName().empty()) {
    Name = V->getName();
  } else {
    unsigned No;
    if (AnonValueNumbers.count(V))
      No = AnonValueNumbers[V];
    else 
      No = AnonValueNumbers[V] = NextAnonValueNumber++;
    Name = "." + utostr(No);
  }

  if (const Function* F = dyn_cast<Function>(V)) {
    std::string RecordName = GetFunctionRecordName(Name);
    std::string MethodName = GetFunctionMethodName(Name);

    if (StructType *STy = ModulePtr->getTypeByName(RecordName)) {
      Name = DemangleName(getTypeName(STy)) + MethodName;
    } else {
      Name = DemangleName(RecordName) + MethodName;
    }
  }

  Name = DemangleName(Name);
  
  if (!WrapInQuotes)
    return Name;

  // Name into the quotes allow control and space characters.
  return "'"+Name+"'";
}


std::string MSILWriter::getLabelName(const std::string& Name) {
  if (Name.find('.')!=std::string::npos) {
    std::string Tmp(Name);
    // Replace unaccepable characters in the label name.
    for (std::string::iterator I = Tmp.begin(), E = Tmp.end(); I!=E; ++I)
      if (*I=='.') *I = '@';
    return Tmp;
  }
  return Name;
}


std::string MSILWriter::getLabelName(const Value* V) {
  std::string Name;
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(V))
    Name = GV->getName();
  else {
    unsigned &No = AnonValueNumbers[V];
    if (No == 0) No = ++NextAnonValueNumber;
    Name = "tmp" + utostr(No);
  }
  
  return getLabelName(Name);
}


std::string MSILWriter::getConvModopt(CallingConv::ID CallingConvID) {
  switch (CallingConvID) {
  case CallingConv::C:
  case CallingConv::Cold:
  case CallingConv::Fast:
    return "modopt([mscorlib]System.Runtime.CompilerServices.CallConvCdecl) ";
  case CallingConv::X86_FastCall:
    return "modopt([mscorlib]System.Runtime.CompilerServices.CallConvFastcall) ";
  case CallingConv::X86_StdCall:
    return "modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) ";
  case CallingConv::X86_ThisCall:
    return "modopt([mscorlib]System.Runtime.CompilerServices.CallConvThiscall) ";
  case CallingConv::CIL_Static:
  case CallingConv::CIL_Instance:
  case CallingConv::CIL_NewObj:
    return "";
  default:
    errs() << "CallingConvID = " << CallingConvID << '\n';
    llvm_unreachable("Unsupported calling convention");
  }
  return ""; // Not reached
}


std::string MSILWriter::getArrayTypeName(Type::TypeID TyID, const Type* Ty) {
  std::string Tmp = "";
  const Type* ElemTy = Ty;
  assert(Ty->getTypeID()==TyID && "Invalid type passed");
  // Walk trought array element types.
  for (;;) {
    // Multidimensional array.
    if (ElemTy->getTypeID()==TyID) {
      if (const ArrayType* ATy = dyn_cast<ArrayType>(ElemTy))
        Tmp += utostr(ATy->getNumElements());
      else if (const VectorType* VTy = dyn_cast<VectorType>(ElemTy))
        Tmp += utostr(VTy->getNumElements());
      ElemTy = cast<SequentialType>(ElemTy)->getElementType();
    }
    // Base element type found.
    if (ElemTy->getTypeID()!=TyID) break;
    Tmp += ",";
  }
  return getTypeName(ElemTy, false, true)+"["+Tmp+"]";
}


std::string MSILWriter::getPrimitiveTypeName(const Type* Ty, bool isSigned) {
  unsigned NumBits = 0;
  switch (Ty->getTypeID()) {
  case Type::VoidTyID:
    return "void ";
  case Type::IntegerTyID:
    NumBits = getBitWidth(Ty);
    if(NumBits==1)
      return "bool ";
    if (!isSigned)
      return "unsigned int"+utostr(NumBits)+" ";
    return "int"+utostr(NumBits)+" ";
  case Type::FloatTyID:
    return "float32 ";
  case Type::DoubleTyID:
    return "float64 "; 
  default:
    //errs() << "Type = " << *Ty->d << '\n';
    llvm_unreachable("Invalid primitive type");
  }
  return ""; // Not reached
}

std::string MSILWriter::getTypeToken(const Type* Ty) {
  return getTypeName(Ty, false, false, true);
}

std::string MSILWriter::getILClassTypeToken(const Type* Ty) {
  const StructType* Struct = cast<StructType>(Ty);
  if (!Struct->hasName())
    return StringRef();
  // FIXME: for now assume opaque structs are managed types.
  if (Struct->isOpaque()) {
    if (isValueClassType(Ty))
      return "valuetype ";
    else
      return "class ";
  }
}

std::string MSILWriter::getILGenericTypes(const Type* Ty) {
  MDNode *Node = Ty->getMetadata("cil.generic");
  if (!Node) return "";

  std::string Out = "<";
  for (unsigned I = 0, E = Node->getNumOperands(); I != E; ++I) {
    ConstantPointerNull *P = dyn_cast<ConstantPointerNull>(Node->getOperand(I));
    assert(P && "Expected a valid constant pointer");

    llvm::Type *Ty = P->getType()->getPointerElementType();
    Out += getTypeName(Ty);

    if (I + 1 < E)
      Out += ", ";
  }
  Out += ">";

  return Out;
}

static bool GetCLISignednessParameter(const FunctionType* Ty,
                                   llvm::SmallVector<unsigned, 4> &Signedness) {
  MDNode *Params = Ty->getMetadata("cil.signedness");
  if (!Params) return false;
  
  for (unsigned I = 0, E = Params->getNumOperands(); I != E; ++I) {
    ConstantInt *Op = dyn_cast<ConstantInt>(Params->getOperand(I));
    assert(Op && "Expected a constant integer metadata node");
    Signedness.push_back(Op->getLimitedValue());
  }

  assert(Signedness.size() == Ty->getNumParams() + 1);
  return true;
}

std::string MSILWriter::getFunctionRetTypeName(const FunctionType *FTy) {
  llvm::SmallVector<unsigned, 4> Signedness;
  bool HasSignedness = GetCLISignednessParameter(FTy, Signedness);

  return getTypeName(FTy->getReturnType(), HasSignedness && Signedness[0]);
}

std::string MSILWriter::getFunctionArgTypeName(const FunctionType *FTy,
                                    Function::const_arg_iterator Arg) {
  llvm::SmallVector<unsigned, 4> Signedness;
  bool HasSignedness = GetCLISignednessParameter(FTy, Signedness);

  return getTypeName(Arg->getType(), HasSignedness && Signedness[Arg->getArgNo()]);
}

std::string MSILWriter::getTypeName(const Type* Ty, bool isSigned,
                                    bool isNested, bool isToken) {
  if (Ty->isPrimitiveType() || Ty->isIntegerTy())
    return getPrimitiveTypeName(Ty,isSigned);
  // FIXME: "OpaqueType" support
  switch (Ty->getTypeID()) {
  case Type::PointerTyID: {
    Type* RecTy = 0;
    if (hasRecursiveManagedType(Ty, RecTy)) {
      std::string TypeName = getTypeName(Ty->getPointerElementType(), isSigned,
        isNested, isToken);
      if (!cast<PointerType>(Ty)->isManagedHandle() && !isToken)
        TypeName += "& ";
      return TypeName;
    }
    return "void* ";
  }
  case Type::StructTyID: {
    const StructType* Struct = cast<StructType>(Ty);
    if (!Struct->hasName())
      return StringRef();
    // FIXME: for now assume opaque structs are managed types.
    if (Struct->isOpaque()) {
      return getILClassTypeToken(Ty)+DemangleName(Ty->getStructName().str())
        + getILGenericTypes(Ty) + " ";
    }
    if (isNested)
      return Ty->getStructName();
    return "class " + Ty->getStructName().str()+" ";
  }
  case Type::ArrayTyID: {
    Type* ElemTy = Ty->getArrayElementType();
    Type* ElemRecTy = 0;
    if (hasRecursiveManagedType(ElemTy, ElemRecTy))
      return getTypeName(ElemRecTy, isSigned, isNested, isToken)+"[]";
    if (isNested)
      return getArrayTypeName(Ty->getTypeID(),Ty);
    return "valuetype '"+getArrayTypeName(Ty->getTypeID(),Ty)+"' ";
  }
  case Type::VectorTyID:
    if (isNested)
      return getArrayTypeName(Ty->getTypeID(),Ty);
    return "valuetype '"+getArrayTypeName(Ty->getTypeID(),Ty)+"' ";
  default:
    //errs() << "Type = " << *Ty << '\n';
    llvm_unreachable("Invalid type in getTypeName()");
  }
  return ""; // Not reached
}


MSILWriter::ValueType MSILWriter::getValueLocation(const Value* V) {
  // Function argument
  if (isa<Argument>(V))
    return ArgumentVT;
  // Function
  else if (const Function* F = dyn_cast<Function>(V))
    return F->hasLocalLinkage() ? InternalVT : GlobalVT;
  // Variable
  else if (const GlobalVariable* G = dyn_cast<GlobalVariable>(V))
    return G->hasLocalLinkage() ? InternalVT : GlobalVT;
  // Constant
  else if (isa<Constant>(V))
    return isa<ConstantExpr>(V) ? ConstExprVT : ConstVT;
  // Local variable
  return LocalVT;
}


std::string MSILWriter::getTypePostfix(const Type* Ty, bool Expand,
                                       bool isSigned) {
  unsigned NumBits = 0;
  switch (Ty->getTypeID()) {
  // Integer constant, expanding for stack operations.
  case Type::IntegerTyID:
    NumBits = getBitWidth(Ty);
    // Expand integer value to "int32" or "int64".
    if (Expand) return (NumBits<=32 ? "i4" : "i8");
    if (NumBits==1) return "i1";
    return (isSigned ? "i" : "u")+utostr(NumBits/8);
  // Float constant.
  case Type::FloatTyID:
    return "r4";
  case Type::DoubleTyID:
    return "r8";
  case Type::PointerTyID:
    return "i"+utostr(TD->getTypeAllocSize((llvm::Type*)Ty));
  default:
    errs() << "TypeID = " << Ty->getTypeID() << '\n';
    llvm_unreachable("Invalid type in TypeToPostfix()");
  }
  return ""; // Not reached
}


void MSILWriter::printConvToPtr() {
  switch (TD->getPointerSize()) {
  case 4:
    printSimpleInstruction("conv.u4");
    break;
  case 8:
    printSimpleInstruction("conv.u8");
    break;
  default:
    llvm_unreachable("Module use not supporting pointer size");
  }
}


void MSILWriter::printPtrLoad(uint64_t N) {
  switch (TD->getPointerSize()) {
  case 4:
    printSimpleInstruction("ldc.i4",utostr(N).c_str());
    // FIXME: Need overflow test?
    if (!isUInt<32>(N)) {
      errs() << "Value = " << utostr(N) << '\n';
      llvm_unreachable("32-bit pointer overflowed");
    }
    break;
  case 8:
    printSimpleInstruction("ldc.i8",utostr(N).c_str());
    break;
  default:
    llvm_unreachable("Module use not supporting pointer size");
  }
}


void MSILWriter::printValuePtrLoad(const Value* V) {
  printValueLoad(V);
  printConvToPtr();
}


void MSILWriter::printConstLoad(const Constant* C) {
  if (const ConstantInt* CInt = dyn_cast<ConstantInt>(C)) {
    // Integer constant
    *Out << "\tldc." << getTypePostfix(C->getType(),true) << '\t';
    if (CInt->isMinValue(true))
      *Out << CInt->getSExtValue();
    else
      *Out << CInt->getZExtValue();
  } else if (const ConstantFP* FP = dyn_cast<ConstantFP>(C)) {
    // Float constant
    uint64_t X;
    unsigned Size;
    const char *Type = 0;
    if (FP->getType()->getTypeID()==Type::FloatTyID) {
      X = (uint32_t)FP->getValueAPF().bitcastToAPInt().getZExtValue();
      Size = 4;
      Type = "float32";
    } else {
      X = FP->getValueAPF().bitcastToAPInt().getZExtValue();
      Size = 8;
      Type = "float64";
    }
    *Out << "\tldc.r" << Size << "\t" << Type << "(" << X << ")";
  } else if (isa<UndefValue>(C)) {
    // Undefined constant value = NULL.
    printPtrLoad(0);
  } else {
    errs() << "Constant = " << *C << '\n';
    llvm_unreachable("Invalid constant value");
  }
  *Out << '\n';
}


void MSILWriter::printValueLoad(const Value* V, bool LoadValueAddress) {
  MSILWriter::ValueType Location = getValueLocation(V);
  switch (Location) {
  // Global variable or function address.
  case GlobalVT:
  case InternalVT:
    if (const Function* F = dyn_cast<Function>(V)) {
      std::string Name = getConvModopt(F->getCallingConv())+getValueName(F);
      printSimpleInstruction("ldftn",
        getCallSignature(F->getFunctionType(),NULL,Name).c_str());
    } else {
      std::string Tmp;
      const Type* ElemTy = cast<PointerType>(V->getType())->getElementType();
      if (Location==GlobalVT && cast<GlobalVariable>(V)->hasDLLImportStorageClass()) {
        Tmp = "void* "+getValueName(V);
        printSimpleInstruction("ldsfld",Tmp.c_str());
      } else {
        Tmp = getTypeName(ElemTy)+getValueName(V);
        printSimpleInstruction("ldsflda",Tmp.c_str());
      }
    }
    break;
  // Function argument.
  case ArgumentVT:
    printSimpleInstruction("ldarg",getValueName(V).c_str());
    break;
  // Local function variable.
  case LocalVT:
    if (isValueClassType(V->getType()) && LoadValueAddress) {
      printSimpleInstruction("ldloca",getValueName(V).c_str());
      break;
    }
    printSimpleInstruction("ldloc",getValueName(V).c_str());
    break;
  // Constant value.
  case ConstVT:
    if (isa<ConstantPointerNull>(V))
      printPtrLoad(0);
    else
      printConstLoad(cast<Constant>(V));
    break;
  // Constant expression.
  case ConstExprVT:
    printConstantExpr(cast<ConstantExpr>(V));
    break;
  default:
    errs() << "Value = " << *V << '\n';
    llvm_unreachable("Invalid value location");
  }
}


void MSILWriter::printValueSave(const Value* V) {
  switch (getValueLocation(V)) {
  case ArgumentVT:
    printSimpleInstruction("starg",getValueName(V).c_str());
    break;
  case LocalVT:
    printSimpleInstruction("stloc",getValueName(V).c_str());
    break;
  default:
    errs() << "Value  = " << *V << '\n';
    llvm_unreachable("Invalid value location");
  }
}


void MSILWriter::printBinaryInstruction(const char* Name, const Value* Left,
                                        const Value* Right) {
  printValueLoad(Left);
  printValueLoad(Right);
  *Out << '\t' << Name << '\n';
}


void MSILWriter::printSimpleInstruction(const char* Inst, const char* Operand) {
  if(Operand) 
    *Out << '\t' << Inst << '\t' << Operand << '\n';
  else
    *Out << '\t' << Inst << '\n';
}


void MSILWriter::printPHICopy(const BasicBlock* Src, const BasicBlock* Dst) {
  for (BasicBlock::const_iterator I = Dst->begin(); isa<PHINode>(I); ++I) {
    const PHINode* Phi = cast<PHINode>(I);
    const Value* Val = Phi->getIncomingValueForBlock(Src);
    if (isa<UndefValue>(Val)) continue;
    printValueLoad(Val);
    printValueSave(Phi);
  }
}


void MSILWriter::printBranchToBlock(const BasicBlock* CurrBB,
                                    const BasicBlock* TrueBB,
                                    const BasicBlock* FalseBB) {
  if (TrueBB==FalseBB) {
    // "TrueBB" and "FalseBB" destination equals
    printPHICopy(CurrBB,TrueBB);
    printSimpleInstruction("pop");
    printSimpleInstruction("br",getLabelName(TrueBB).c_str());
  } else if (FalseBB==NULL) {
    // If "FalseBB" not used the jump have condition
    printPHICopy(CurrBB,TrueBB);
    printSimpleInstruction("brtrue",getLabelName(TrueBB).c_str());
  } else if (TrueBB==NULL) {
    // If "TrueBB" not used the jump is unconditional
    printPHICopy(CurrBB,FalseBB);
    printSimpleInstruction("br",getLabelName(FalseBB).c_str());
  } else {
    // Copy PHI instructions for each block
    std::string TmpLabel;
    // Print PHI instructions for "TrueBB"
    if (isa<PHINode>(TrueBB->begin())) {
      TmpLabel = getLabelName(TrueBB)+"$phi_"+utostr(getUniqID());
      printSimpleInstruction("brtrue",TmpLabel.c_str());
    } else {
      printSimpleInstruction("brtrue",getLabelName(TrueBB).c_str());
    }
    // Print PHI instructions for "FalseBB"
    if (isa<PHINode>(FalseBB->begin())) {
      printPHICopy(CurrBB,FalseBB);
      printSimpleInstruction("br",getLabelName(FalseBB).c_str());
    } else {
      printSimpleInstruction("br",getLabelName(FalseBB).c_str());
    }
    if (isa<PHINode>(TrueBB->begin())) {
      // Handle "TrueBB" PHI Copy
      *Out << TmpLabel << ":\n";
      printPHICopy(CurrBB,TrueBB);
      printSimpleInstruction("br",getLabelName(TrueBB).c_str());
    }
  }
}


void MSILWriter::printBranchInstruction(const BranchInst* Inst) {
  if (Inst->isUnconditional()) {
    printBranchToBlock(Inst->getParent(),NULL,Inst->getSuccessor(0));
  } else {
    printValueLoad(Inst->getCondition());
    printBranchToBlock(Inst->getParent(),Inst->getSuccessor(0),
                       Inst->getSuccessor(1));
  }
}


void MSILWriter::printSelectInstruction(const Value* Cond, const Value* VTrue,
                                        const Value* VFalse) {
  std::string TmpLabel = std::string("select$true_")+utostr(getUniqID());
  printValueLoad(VTrue);
  printValueLoad(Cond);
  printSimpleInstruction("brtrue",TmpLabel.c_str());
  printSimpleInstruction("pop");
  printValueLoad(VFalse);
  *Out << TmpLabel << ":\n";
}


void MSILWriter::printLoadInstruction(const Value* V) {
  Type *Ty;
  if (hasRecursiveManagedType(V->getType(), Ty)) {
    printValueLoad(V);
    return;
  }

  printIndirectLoad(V);
}


void MSILWriter::printIndirectLoad(const Value* V) {
  const Type* Ty = V->getType();
  printValueLoad(V);
  if (const PointerType* P = dyn_cast<PointerType>(Ty))
    Ty = P->getElementType();
  std::string Tmp = "ldind."+getTypePostfix(Ty, false);
  printSimpleInstruction(Tmp.c_str());
}


void MSILWriter::printStoreInstruction(const Value* Ptr, const Value* Val) {
  llvm::Type *Ty = Val->getType();
  if (PointerType *PTy = dyn_cast<PointerType>(Ty)) {
    if (PTy->isManagedHandle()) {
      printValueLoad(Val);
      printValueSave(Ptr);
      return;
    }
  }
  printIndirectSave(Ptr, Val);
}

void MSILWriter::printIndirectSave(const Value* Ptr, const Value* Val) {
  printValueLoad(Ptr);
  printValueLoad(Val);
  printIndirectSave(Val->getType());
}


void MSILWriter::printIndirectSave(const Type* Ty) {
  // Instruction need signed postfix for any type.
  std::string postfix = getTypePostfix(Ty, false);
  if (*postfix.begin()=='u') *postfix.begin() = 'i';
  postfix = "stind."+postfix;
  printSimpleInstruction(postfix.c_str());
}


void MSILWriter::printCastInstruction(unsigned int Op, const Value* V,
                                      const Type* Ty, const Type* SrcTy) {
  std::string Tmp("");
  printValueLoad(V);
  switch (Op) {
  // Signed
  case Instruction::SExt:
    // If sign extending int, convert first from unsigned to signed
    // with the same bit size - because otherwise we will loose the sign.
    if (SrcTy) {
      Tmp = "conv."+getTypePostfix(SrcTy,false,true);
      printSimpleInstruction(Tmp.c_str());
    }
    // FALLTHROUGH
  case Instruction::SIToFP:
  case Instruction::FPToSI:
    Tmp = "conv."+getTypePostfix(Ty,false,true);
    printSimpleInstruction(Tmp.c_str());
    break;
  // Unsigned
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::FPToUI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    Tmp = "conv."+getTypePostfix(Ty,false);
    printSimpleInstruction(Tmp.c_str());
    break;
  // Do nothing
  case Instruction::BitCast:
    // FIXME: meaning that ld*/st* instruction do not change data format.
    break;
  default:
    errs() << "Opcode = " << Op << '\n';
    llvm_unreachable("Invalid conversion instruction");
  }
}


void MSILWriter::printGepInstruction(const Value* V, gep_type_iterator I,
                                     gep_type_iterator E) {
  unsigned Size;
  // Load address
  printValuePtrLoad(V);
  // Calculate element offset.
  for (; I!=E; ++I){
    Size = 0;
    const Value* IndexValue = I.getOperand();
    if (const StructType* StrucTy = dyn_cast<StructType>(*I)) {
      uint64_t FieldIndex = cast<ConstantInt>(IndexValue)->getZExtValue();
      // Offset is the sum of all previous structure fields.
      for (uint64_t F = 0; F<FieldIndex; ++F)
        Size += TD->getTypeAllocSize(StrucTy->getContainedType((unsigned)F));
      printPtrLoad(Size);
      printSimpleInstruction("add");
      continue;
    } else if (const SequentialType* SeqTy = dyn_cast<SequentialType>(*I)) {
      Size = TD->getTypeAllocSize(SeqTy->getElementType());
    } else {
      Size = TD->getTypeAllocSize(*I);
    }
    // Add offset of current element to stack top.
    if (!isZeroValue(IndexValue)) {
      // Constant optimization.
      if (const ConstantInt* C = dyn_cast<ConstantInt>(IndexValue)) {
        if (C->getValue().isNegative()) {
          printPtrLoad(C->getValue().abs().getZExtValue()*Size);
          printSimpleInstruction("sub");
          continue;
        } else
          printPtrLoad(C->getZExtValue()*Size);
      } else {
        printPtrLoad(Size);
        printValuePtrLoad(IndexValue);
        printSimpleInstruction("mul");
      }
      printSimpleInstruction("add");
    }
  }
}

static bool GetCLIGenericParameter(const FunctionType* Ty, const Instruction* Inst,
                                   llvm::SmallVector<unsigned, 4> &GenericParams,
                                   MSILWriter::CLICallType CallType) {
  MDNode *Params = Inst->getMetadata("cil.params");
  if (!Params) return false;
  
  for (unsigned I = 0, E = Params->getNumOperands(); I != E; ++I) {
    ConstantInt *Op = dyn_cast<ConstantInt>(Params->getOperand(I));
    assert(Op && "Expected a constant integer metadata node");
    GenericParams.push_back(Op->getLimitedValue());

    if ((CallType == MSILWriter::CLI_Instance) && (I==0)) {
      // Add an extra first parameter for instance methods
       GenericParams.push_back(0);
    }
  }

  assert(GenericParams.size() == Ty->getNumParams() + 1);
  
  return true;
}

std::string MSILWriter::getCallSignature(const FunctionType* Ty,
                                         const Instruction* Inst,
                                         std::string Name, CLICallType CallType) {
  bool isManaged = CallType != CLI_Native;

  llvm::SmallVector<unsigned, 4> GenericParams;
  bool HasGenericParams = false;

  llvm::SmallVector<unsigned, 4> Signedness;
  bool HasSignedness = false;
  
  if (isManaged) {
    HasGenericParams = GetCLIGenericParameter(Ty, Inst, GenericParams, CallType);
    HasSignedness = GetCLISignednessParameter(Ty, Signedness);
  }

  std::string Tmp("");
  if (Ty->isVarArg()) Tmp += "vararg ";
  // Name and return type.
  bool RetSign = HasSignedness && Signedness[0];
  if (HasGenericParams && GenericParams[0])
    Tmp +=  "!" + std::to_string(GenericParams[0] - 1) + " ";
  else
    Tmp += getTypeName(Ty->getReturnType(), RetSign, false);
  Tmp += /*getILClassTypeToken(Ty)+*/ Name+"(";
  // Function argument type list.
  unsigned NumParams = Ty->getNumParams();
  bool PrintComma = false;
  for (unsigned I = 0; I!=NumParams; ++I) {
    if ((CallType == CLI_Instance) && (I==0))
      continue; // Skip the first parameter for instance methods
    if (PrintComma) Tmp += ",";
    bool ParamSign = HasSignedness && Signedness[I+1];
    if (HasGenericParams && GenericParams[I+1])
      Tmp +=  "!" + std::to_string(GenericParams[I+1] - 1);
    else
      Tmp += getTypeName(Ty->getParamType(I), ParamSign, false);
    PrintComma = true;
  }
  // CLR needs to know the exact amount of parameters received by vararg
  // function, because caller cleans the stack.
  if (Ty->isVarArg() && Inst) {
    // Origin to function arguments in "CallInst" or "InvokeInst".
    unsigned Org = isa<InvokeInst>(Inst) ? 3 : 1;
    // Print variable argument types.
    unsigned NumOperands = Inst->getNumOperands()-Org;
    if (NumParams<NumOperands) {
      if (NumParams!=0) Tmp += ", ";
      Tmp += "... , ";
      for (unsigned J = NumParams; J!=NumOperands; ++J) {
        if (J!=NumParams) Tmp += ", ";
        Tmp += getTypeName(Inst->getOperand(J+Org)->getType(), false, false);
      }
    }
  }
  return Tmp+")";
}

void MSILWriter::printManagedStaticCall(const Function* Fn,
                                  const Instruction* Inst) {
  std::string Name = getValueName(Fn, false /*WrapInQuotes*/);
  printSimpleInstruction("call",
    getCallSignature(Fn->getFunctionType(),Inst,Name,CLI_Static).c_str());
}

void MSILWriter::printManagedInstanceCall(const Function* Fn,
                                  const Instruction* Inst) {
  std::string Name = getValueName(Fn, false /*WrapInQuotes*/);
  printSimpleInstruction("callvirt instance",
    getCallSignature(Fn->getFunctionType(),Inst,Name,CLI_Instance).c_str());
}

void MSILWriter::printNewObjCall(const Function* Fn,
                                 const Instruction* Inst) {
  std::string Name = getValueName(Fn, false /*WrapInQuotes*/);
  printSimpleInstruction("newobj instance",
    getCallSignature(Fn->getFunctionType(),Inst,Name,CLI_Ctor).c_str());
}

void MSILWriter::printFunctionCallArgs(CallingConv::ID CC, FnArgs &Args) {
  // Load arguments to stack and call function.
  for (int I = 0, E = Args.size(); I!=E; ++I) {
    bool LoadValueAddress = CC == CallingConv::CIL_Instance;
    printValueLoad(Args[I], LoadValueAddress);
  }
}

void MSILWriter::printFunctionCall(const Value* FnVal,
                                   const Instruction* Inst) {
  CallingConv::ID CC = CallingConv::C;
  FnArgs Args;

  if (const CallInst* Call = dyn_cast<CallInst>(Inst)) {
    CC = Call->getCallingConv();
    for (int I = 0, E = Call->getNumArgOperands(); I!=E; ++I)
      Args.push_back(Call->getArgOperand(I));
  } else if (const InvokeInst* Invoke = dyn_cast<InvokeInst>(Inst)) {
    CC = Invoke->getCallingConv();
    for (int I = 0, E = Invoke->getNumArgOperands(); I!=E; ++I)
      Args.push_back(Invoke->getArgOperand(I));
  } else {
    errs() << "Instruction = " << Inst->getName() << '\n';
    llvm_unreachable("Need \"Invoke\" or \"Call\" instruction only");
  }

  printFunctionCallArgs(CC, Args);

  // Get function calling convention.
  std::string Name = getConvModopt(CC);

  if (const Function* F = dyn_cast<Function>(FnVal)) {
    if (CC == CallingConv::CIL_Static) {
      printManagedStaticCall(F, Inst);
    } else if (CC == CallingConv::CIL_Instance) {
      printManagedInstanceCall(F, Inst);
    } else if (CC == CallingConv::CIL_NewObj) {
      printNewObjCall(F, Inst);
    }else {
      // Direct call.
      Name += getValueName(F);
      printSimpleInstruction("call",
        getCallSignature(F->getFunctionType(),Inst,Name).c_str());
    }
  } else {
    // Indirect function call.
    const PointerType* PTy = cast<PointerType>(FnVal->getType());
    const FunctionType* FTy = cast<FunctionType>(PTy->getElementType());
    // Load function address.
    printValueLoad(FnVal);
    printSimpleInstruction("calli",getCallSignature(FTy,Inst,Name).c_str());
  }
}


void MSILWriter::printIntrinsicCall(const CallInst* Inst) {
  std::string Name;
  Intrinsic::ID Id = (Intrinsic::ID) Inst->getCalledFunction()->
    getIntrinsicID();
  switch (Id) {
  case Intrinsic::vastart:
    Name = getValueName(Inst->getArgOperand(0));
    Name.insert(Name.length()-1,"$valist");
    // Obtain the argument handle.
    printSimpleInstruction("ldloca",Name.c_str());
    printSimpleInstruction("arglist");
    printSimpleInstruction("call",
      "instance void [mscorlib]System.ArgIterator::.ctor"
      "(valuetype [mscorlib]System.RuntimeArgumentHandle)");
    // Save as pointer type "void*"
    printValueLoad(Inst->getArgOperand(0));
    printSimpleInstruction("ldloca",Name.c_str());
    printIndirectSave(PointerType::getUnqual(
          IntegerType::get(Inst->getContext(), 8)));
    break;
  case Intrinsic::vaend:
    // Close argument list handle.
    printIndirectLoad(Inst->getArgOperand(0));
    printSimpleInstruction("call",
      "instance void [mscorlib]System.ArgIterator::End()");
    break;
  case Intrinsic::vacopy:
    // Copy "ArgIterator" valuetype.
    printIndirectLoad(Inst->getArgOperand(0));
    printIndirectLoad(Inst->getArgOperand(1));
    printSimpleInstruction("cpobj","[mscorlib]System.ArgIterator");
    break;
  case Intrinsic::cil_ldstr: {
    MDNode *MD = Inst->getMetadata("cil.str");
    assert(MD->getNumOperands() == 1);
    
    MDString *MS = dyn_cast<MDString>(MD->getOperand(0));
    assert(MS && "Expected a valid metadata string");
    
    printSimpleInstruction("ldstr",
      (std::string("\"") + MS->getString().str() + "\"").c_str());
    break;
  }
  case Intrinsic::cil_ldnull: {
    printSimpleInstruction("ldnull");
    break;
  }
  case Intrinsic::cil_newobj: {
    break;
  }
  case Intrinsic::cil_newvalue: {
    unsigned NumElements = Inst->getNumArgOperands();
    assert(NumElements == 1 && "Expected one value class type value");
    llvm::Value *Value = Inst->getOperand(0);
    printValueLoad(Value, /*LoadValueAddress=*/true);
    printSimpleInstruction("initobj", getTypeToken(Value->getType()).c_str());
    break;
  }
  case Intrinsic::cil_copyvalue: {
    unsigned NumElements = Inst->getNumArgOperands();
    assert(NumElements == 2 && "Expected 2 value class type values");
    llvm::Value *Value = Inst->getOperand(0);
    printValueLoad(Value, /*LoadValueAddress=*/true);
    printValueLoad(Inst->getOperand(1), /*LoadValueAddress=*/true);
    printSimpleInstruction("cpobj", getTypeToken(Value->getType()).c_str());
    break;
  }
  case Intrinsic::cil_newarr: {
    MDNode *MD = Inst->getMetadata("cil.type");
    assert(MD->getNumOperands() == 1);

    MDString *MS = dyn_cast<MDString>(MD->getOperand(0));
    assert(MS && "Expected a valid metadata string");

    unsigned NumElements = Inst->getNumArgOperands();
    printSimpleInstruction("ldc.i4", utostr_32(NumElements-1).c_str());
    printSimpleInstruction("newarr",  DemangleName(MS->getString().str()).c_str());
    for (int I = 1, E = NumElements; I!=E; ++I) {
      printSimpleInstruction("dup");
    }

    // Load arguments to stack
    for (int I = 1, E = NumElements; I!=E; ++I) {
      printSimpleInstruction("ldc.i4", utostr_32(I-1).c_str());
      printValueLoad(Inst->getArgOperand(I));
      printSimpleInstruction("stelem.ref");
    }
    break;
  }
  case Intrinsic::cil_ldarr: {
    unsigned NumElements = Inst->getNumArgOperands();
    assert(NumElements == 2);
    printValueLoad(Inst->getArgOperand(0)); // Array
    printValueLoad(Inst->getArgOperand(1)); // Index
    printSimpleInstruction("ldelem.ref");
    break;
  }
  case Intrinsic::cil_starr: {
    unsigned NumElements = Inst->getNumArgOperands();
    assert(NumElements == 3);
    printValueLoad(Inst->getArgOperand(0)); // Array
    printValueLoad(Inst->getArgOperand(2)); // Index
    printValueLoad(Inst->getArgOperand(1)); // Value
    printSimpleInstruction("stelem.ref");
    break;
  }
  case Intrinsic::cil_box: {
    unsigned NumElements = Inst->getNumArgOperands();
    assert(NumElements == 2);
    printValueLoad(Inst->getArgOperand(0)); // Value

    Type *Ty = Inst->getArgOperand(1)->getType()->getPointerElementType();
    assert(Ty->isStructTy());

    printSimpleInstruction("box", getTypeToken(Ty).c_str());
    break;
  }
  default:
    errs() << "Intrinsic ID = " << Id << '\n';
    llvm_unreachable("Invalid intrinsic function");
  }
}


void MSILWriter::printCallInstruction(const Instruction* Inst) {
  if (isa<IntrinsicInst>(Inst)) {
    // Handle intrinsic function.
    printIntrinsicCall(cast<IntrinsicInst>(Inst));
    return;
  }
  const CallInst *CI = cast<CallInst>(Inst);
  printFunctionCall(CI->getCalledFunction(), Inst);
}


void MSILWriter::printICmpInstruction(unsigned Predicate, const Value* Left,
                                      const Value* Right) {
  switch (Predicate) {
  case ICmpInst::ICMP_EQ:
    printBinaryInstruction("ceq",Left,Right);
    break;
  case ICmpInst::ICMP_NE:
    // Emulate = not neg (Op1 eq Op2)
    printBinaryInstruction("ceq",Left,Right);
    printSimpleInstruction("neg");
    printSimpleInstruction("not");
    break;
  case ICmpInst::ICMP_ULE:
  case ICmpInst::ICMP_SLE:
    // Emulate = (Op1 eq Op2) or (Op1 lt Op2)
    printBinaryInstruction("ceq",Left,Right);
    if (Predicate==ICmpInst::ICMP_ULE)
      printBinaryInstruction("clt.un",Left,Right);
    else
      printBinaryInstruction("clt",Left,Right);
    printSimpleInstruction("or");
    break;
  case ICmpInst::ICMP_UGE:
  case ICmpInst::ICMP_SGE:
    // Emulate = (Op1 eq Op2) or (Op1 gt Op2)
    printBinaryInstruction("ceq",Left,Right);
    if (Predicate==ICmpInst::ICMP_UGE)
      printBinaryInstruction("cgt.un",Left,Right);
    else
      printBinaryInstruction("cgt",Left,Right);
    printSimpleInstruction("or");
    break;
  case ICmpInst::ICMP_ULT:
    printBinaryInstruction("clt.un",Left,Right);
    break;
  case ICmpInst::ICMP_SLT:
    printBinaryInstruction("clt",Left,Right);
    break;
  case ICmpInst::ICMP_UGT:
    printBinaryInstruction("cgt.un",Left,Right);
    break;
  case ICmpInst::ICMP_SGT:
    printBinaryInstruction("cgt",Left,Right);
    break;
  default:
    errs() << "Predicate = " << Predicate << '\n';
    llvm_unreachable("Invalid icmp predicate");
  }
}


void MSILWriter::printFCmpInstruction(unsigned Predicate, const Value* Left,
                                      const Value* Right) {
  // FIXME: Correct comparison
  std::string NanFunc = "bool [mscorlib]System.Double::IsNaN(float64)";
  switch (Predicate) {
  case FCmpInst::FCMP_UGT:
    // X >  Y || llvm_fcmp_uno(X, Y)
    printBinaryInstruction("cgt",Left,Right);
    printFCmpInstruction(FCmpInst::FCMP_UNO,Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_OGT:
    // X >  Y
    printBinaryInstruction("cgt",Left,Right);
    break;
  case FCmpInst::FCMP_UGE:
    // X >= Y || llvm_fcmp_uno(X, Y)
    printBinaryInstruction("ceq",Left,Right);
    printBinaryInstruction("cgt",Left,Right);
    printSimpleInstruction("or");
    printFCmpInstruction(FCmpInst::FCMP_UNO,Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_OGE:
    // X >= Y
    printBinaryInstruction("ceq",Left,Right);
    printBinaryInstruction("cgt",Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_ULT:
    // X <  Y || llvm_fcmp_uno(X, Y)
    printBinaryInstruction("clt",Left,Right);
    printFCmpInstruction(FCmpInst::FCMP_UNO,Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_OLT:
    // X <  Y
    printBinaryInstruction("clt",Left,Right);
    break;
  case FCmpInst::FCMP_ULE:
    // X <= Y || llvm_fcmp_uno(X, Y)
    printBinaryInstruction("ceq",Left,Right);
    printBinaryInstruction("clt",Left,Right);
    printSimpleInstruction("or");
    printFCmpInstruction(FCmpInst::FCMP_UNO,Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_OLE:
    // X <= Y
    printBinaryInstruction("ceq",Left,Right);
    printBinaryInstruction("clt",Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_UEQ:
    // X == Y || llvm_fcmp_uno(X, Y)
    printBinaryInstruction("ceq",Left,Right);
    printFCmpInstruction(FCmpInst::FCMP_UNO,Left,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_OEQ:
    // X == Y
    printBinaryInstruction("ceq",Left,Right);
    break;
  case FCmpInst::FCMP_UNE:
    // X != Y
    printBinaryInstruction("ceq",Left,Right);
    printSimpleInstruction("neg");
    printSimpleInstruction("not");
    break;
  case FCmpInst::FCMP_ONE:
    // X != Y && llvm_fcmp_ord(X, Y)
    printBinaryInstruction("ceq",Left,Right);
    printSimpleInstruction("not");
    break;
  case FCmpInst::FCMP_ORD:
    // return X == X && Y == Y
    printBinaryInstruction("ceq",Left,Left);
    printBinaryInstruction("ceq",Right,Right);
    printSimpleInstruction("or");
    break;
  case FCmpInst::FCMP_UNO:
    // X != X || Y != Y
    printBinaryInstruction("ceq",Left,Left);
    printSimpleInstruction("not");
    printBinaryInstruction("ceq",Right,Right);
    printSimpleInstruction("not");
    printSimpleInstruction("or");
    break;
  default:
    llvm_unreachable("Illegal FCmp predicate");
  }
}


void MSILWriter::printInvokeInstruction(const InvokeInst* Inst) {
  std::string Label = "leave$normal_"+utostr(getUniqID());
  *Out << ".try {\n";
  //// Load arguments
  //for (int I = 0, E = Inst->getNumArgOperands(); I!=E; ++I)
  //  printValueLoad(Inst->getArgOperand(I));
  // Print call instruction
  printFunctionCall(Inst->getOperand(0),Inst);
  // Save function result and leave "try" block
  printValueSave(Inst);
  printSimpleInstruction("leave",Label.c_str());
  *Out << "}\n";
  *Out << "catch [mscorlib]System.Exception {\n";
  // Redirect to unwind block
  printSimpleInstruction("pop");
  printBranchToBlock(Inst->getParent(),NULL,Inst->getUnwindDest());
  *Out << "}\n" << Label << ":\n";
  // Redirect to continue block
  printBranchToBlock(Inst->getParent(),NULL,Inst->getNormalDest());
}


void MSILWriter::printSwitchInstruction(const SwitchInst* Inst) {
  // FIXME: Emulate with IL "switch" instruction
  // Emulate = if () else if () else if () else ...
  for (SwitchInst::ConstCaseIt I = Inst->case_begin();
       I != Inst->case_end(); ++I ) {
    printValueLoad(Inst->getCondition());
    printValueLoad(I.getCaseValue());
    printSimpleInstruction("ceq");
    // Condition jump to successor block
    printBranchToBlock(Inst->getParent(),I.getCaseSuccessor(),NULL);
  }
  // Jump to default block
  printBranchToBlock(Inst->getParent(),NULL,Inst->getDefaultDest());
}


void MSILWriter::printVAArgInstruction(const VAArgInst* Inst) {
  printIndirectLoad(Inst->getOperand(0));
  printSimpleInstruction("call",
    "instance typedref [mscorlib]System.ArgIterator::GetNextArg()");
  printSimpleInstruction("refanyval","void*");
  std::string Name = 
    "ldind."+getTypePostfix(PointerType::getUnqual(
            IntegerType::get(Inst->getContext(), 8)),false);
  printSimpleInstruction(Name.c_str());
}


void MSILWriter::printAllocaInstruction(const AllocaInst* Inst,
                                        bool &NeedsValueSave) {
  llvm::Type *AllocType = Inst->getAllocatedType();

  uint64_t Size = 0;
  if (AllocType->isSized())
    Size = TD->getTypeAllocSize(Inst->getAllocatedType());
  
  if (const PointerType *PTy = dyn_cast<PointerType>(AllocType)) {
    if (PTy->isManagedHandle()) {
      NeedsValueSave = false;
      return;
    }
  } else if (const StructType *STy = dyn_cast<StructType>(AllocType)) {
    if (isValueClassType(STy)) {
      NeedsValueSave = false;
      return;
    }
  }

  llvm::Value *Op = Inst->getOperand(0);
  // Constant optimization.
  if (const ConstantInt* CInt = dyn_cast<ConstantInt>(Op)) {
    printPtrLoad(CInt->getZExtValue()*Size);
  } else {
    printPtrLoad(Size);
    printValueLoad(Op);
    printSimpleInstruction("mul");
  }
  printSimpleInstruction("localloc");
}


void MSILWriter::printInstruction(const Instruction* Inst,
                                  bool &NeedsValueSave) {
  const Value *Left = 0, *Right = 0;
  if (Inst->getNumOperands()>=1) Left = Inst->getOperand(0);
  if (Inst->getNumOperands()>=2) Right = Inst->getOperand(1);
  // Print instruction
  // FIXME: "ShuffleVector","ExtractElement","InsertElement" support.
  switch (Inst->getOpcode()) {
  // Terminator
  case Instruction::Ret:
    if (Inst->getNumOperands()) {
      printValueLoad(Left);
      printSimpleInstruction("ret");
    } else
      printSimpleInstruction("ret");
    break;
  case Instruction::Br:
    printBranchInstruction(cast<BranchInst>(Inst));
    break;
  // Binary
  case Instruction::Add:
  case Instruction::FAdd:
    printBinaryInstruction("add",Left,Right);
    break;
  case Instruction::Sub:
  case Instruction::FSub:
    printBinaryInstruction("sub",Left,Right);
    break;
  case Instruction::Mul:
  case Instruction::FMul:
    printBinaryInstruction("mul",Left,Right);
    break;
  case Instruction::UDiv:
    printBinaryInstruction("div.un",Left,Right);
    break;
  case Instruction::SDiv:
  case Instruction::FDiv:
    printBinaryInstruction("div",Left,Right);
    break;
  case Instruction::URem:
    printBinaryInstruction("rem.un",Left,Right);
    break;
  case Instruction::SRem:
  case Instruction::FRem:
    printBinaryInstruction("rem",Left,Right);
    break;
  // Binary Condition
  case Instruction::ICmp:
    printICmpInstruction(cast<ICmpInst>(Inst)->getPredicate(),Left,Right);
    break;
  case Instruction::FCmp:
    printFCmpInstruction(cast<FCmpInst>(Inst)->getPredicate(),Left,Right);
    break;
  // Bitwise Binary
  case Instruction::And:
    printBinaryInstruction("and",Left,Right);
    break;
  case Instruction::Or:
    printBinaryInstruction("or",Left,Right);
    break;
  case Instruction::Xor:
    printBinaryInstruction("xor",Left,Right);
    break;
  case Instruction::Shl:
    printValueLoad(Left);
    printValueLoad(Right);
    printSimpleInstruction("conv.i4");
    printSimpleInstruction("shl");
    break;
  case Instruction::LShr:
    printValueLoad(Left);
    printValueLoad(Right);
    printSimpleInstruction("conv.i4");
    printSimpleInstruction("shr.un");
    break;
  case Instruction::AShr:
    printValueLoad(Left);
    printValueLoad(Right);
    printSimpleInstruction("conv.i4");
    printSimpleInstruction("shr");
    break;
  case Instruction::Select:
    printSelectInstruction(Inst->getOperand(0),Inst->getOperand(1),Inst->getOperand(2));
    break;
  case Instruction::Load:
    printLoadInstruction(Inst->getOperand(0));
    break;
  case Instruction::Store:
    printStoreInstruction(Inst->getOperand(1), Inst->getOperand(0));
    break;
  case Instruction::SExt:
    printCastInstruction(Inst->getOpcode(),Left,
                         cast<CastInst>(Inst)->getDestTy(),
                         cast<CastInst>(Inst)->getSrcTy());
    break;
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    printCastInstruction(Inst->getOpcode(),Left,
                         cast<CastInst>(Inst)->getDestTy());
    break;
  case Instruction::GetElementPtr:
    printGepInstruction(Inst->getOperand(0),gep_type_begin(Inst),
                        gep_type_end(Inst));
    break;
  case Instruction::Call:
    printCallInstruction(cast<CallInst>(Inst));
    break;
  case Instruction::Invoke:
    printInvokeInstruction(cast<InvokeInst>(Inst));
    break;
#if 0
  case Instruction::Unwind:
    printSimpleInstruction("newobj",
      "instance void [mscorlib]System.Exception::.ctor()");
    printSimpleInstruction("throw");
    break;
#endif
  case Instruction::Switch:
    printSwitchInstruction(cast<SwitchInst>(Inst));
    break;
  case Instruction::Alloca:
    printAllocaInstruction(cast<AllocaInst>(Inst), NeedsValueSave);
    break;
  case Instruction::Unreachable:
    printSimpleInstruction("ldstr", "\"Unreachable instruction\"");
    printSimpleInstruction("newobj",
      "instance void [mscorlib]System.Exception::.ctor(string)");
    printSimpleInstruction("throw");
    break;
  case Instruction::VAArg:
    printVAArgInstruction(cast<VAArgInst>(Inst));
    break;
  default:
    errs() << "Instruction = " << Inst->getName() << '\n';
    llvm_unreachable("Unsupported instruction");
  }
}


void MSILWriter::printLoop(const Loop* L) {
  *Out << getLabelName(L->getHeader()->getName()) << ":\n";
  const std::vector<BasicBlock*>& blocks = L->getBlocks();
  for (unsigned I = 0, E = blocks.size(); I!=E; I++) {
    BasicBlock* BB = blocks[I];
    Loop* BBLoop = LInfo->getLoopFor(BB);
    if (BBLoop == L)
      printBasicBlock(BB);
    else if (BB==BBLoop->getHeader() && BBLoop->getParentLoop()==L)
      printLoop(BBLoop);
  }
  printSimpleInstruction("br",getLabelName(L->getHeader()->getName()).c_str());
}


void MSILWriter::printBasicBlock(const BasicBlock* BB) {
  *Out << getLabelName(BB) << ":\n";
  for (BasicBlock::const_iterator I = BB->begin(), E = BB->end(); I!=E; ++I) {
    const Instruction* Inst = I;
    // Comment llvm original instruction
    // *Out << "\n//" << *Inst << "\n";
    // Do not handle PHI instruction in current block
    if (Inst->getOpcode()==Instruction::PHI) continue;
    // Print instruction
    bool NeedsValueSave = true;
    printInstruction(Inst, NeedsValueSave);
    // Save result
    if (Inst->getType()!=Type::getVoidTy(BB->getContext())) {
      // Do not save value after invoke, it done in "try" block
      if (Inst->getOpcode()==Instruction::Invoke) continue;
      if (NeedsValueSave)
        printValueSave(Inst);
    }
  }
}

bool MSILWriter::getManagedName(const Type* Ty, std::string& Assembly,
                                std::string& Type) {
  assert (Ty->isStructTy());

  if (!cast<StructType>(Ty)->hasName())
    return false;

  StringRef Name = Ty->getStructName();
  auto LBracket = Name.find_first_of("[");
  auto RBracket = Name.find_last_of("]");

  if (LBracket != StringRef::npos && RBracket != StringRef::npos) {
    Assembly = Name.substr(LBracket + 1, RBracket);
    Type = Name.substr(RBracket + 1);
    return true;
  }
  return false;
}

bool MSILWriter::isManagedType(const Type* Ty) {
  if (!Ty->isStructTy())
    return false;
  
  std::string Assembly;
  std::string Type;
  return getManagedName(Ty, Assembly, Type);
}

bool MSILWriter::isValueClassType(const Type* OrigTy) {
  Type *Ty;
  if (!hasRecursiveManagedType(OrigTy, Ty))
    return false;

  MDNode *MD = Ty->getMetadata("cil.type");
  if (!MD || !MD->getNumOperands())
    return false;

  MDString *MS = dyn_cast<MDString>(MD->getOperand(0));
  if (!MS)
    return false;

  return MS->getString().str() == "value";
}

bool MSILWriter::hasRecursiveManagedType(const Type* Ty, Type*& RecTy) {
  if (Ty->isPointerTy()) {
    RecTy = Ty->getPointerElementType();
    return hasRecursiveManagedType(RecTy, RecTy);
  } else if (Ty->isArrayTy()) {
    RecTy = Ty->getArrayElementType();
    return hasRecursiveManagedType(RecTy, RecTy);
  }

  RecTy = (Type*) Ty;
  return isManagedType(Ty);
}

Type* MSILWriter::getLocalType(Type* Ty) {
  Type* RecTy = 0;
  if (hasRecursiveManagedType(Ty, RecTy))
    return RecTy;
  return PointerType::getUnqual(Ty);
}

void MSILWriter::printLocalVariables(const Function& F) {
  std::string Name;
  const Type* Ty = NULL;
  std::set<const Value*> Printed;
  const Value* VaList = NULL;
  unsigned StackDepth = 8;

  // Find local variables
  for (const_inst_iterator I = inst_begin(&F), E = inst_end(&F); I!=E; ++I) {
    if (I->getOpcode() == Instruction::Call ||
        I->getOpcode() == Instruction::Invoke) {
      // Test stack depth.
      if (StackDepth < I->getNumOperands())
        StackDepth = I->getNumOperands();
    }

    const AllocaInst* AI = dyn_cast<AllocaInst>(&*I);
    if (AI && !isa<GlobalVariable>(AI)) {
      // Local variable allocation.
      Ty = getLocalType(AI->getAllocatedType());
      Name = getValueName(AI);
      *Out << "\t.locals (" << getTypeName(Ty) << Name << ")\n";
    } else if (I->getType()!=Type::getVoidTy(F.getContext())) {
      // Operation result.
      Ty = I->getType();
      Name = getValueName(&*I);
      *Out << "\t.locals (" << getTypeName(Ty) << Name << ")\n";
    }

    // Test on 'va_list' variable
    bool isVaList = false;
    if (const VAArgInst* VaInst = dyn_cast<VAArgInst>(&*I)) {
      // "va_list" as "va_arg" instruction operand.
      isVaList = true;
      VaList = VaInst->getOperand(0);
    } else if (const IntrinsicInst* Inst = dyn_cast<IntrinsicInst>(&*I)) {
      // "va_list" as intrinsic function operand. 
      switch (Inst->getIntrinsicID()) {
      case Intrinsic::vastart:
      case Intrinsic::vaend:
      case Intrinsic::vacopy:
        isVaList = true;
        VaList = Inst->getArgOperand(0);
        break;
      default:
        isVaList = false;
      }
    }

    // Print "va_list" variable.
    if (isVaList && Printed.insert(VaList).second) {
      Name = getValueName(VaList);
      Name.insert(Name.length()-1,"$valist");
      *Out << "\t.locals (valuetype [mscorlib]System.ArgIterator "
          << Name << ")\n";
    }
  }

  printSimpleInstruction(".maxstack",utostr(StackDepth*2).c_str());
}


void MSILWriter::printFunctionBody(const Function& F) {
  // Print body
  for (Function::const_iterator I = F.begin(), E = F.end(); I!=E; ++I) {
    if (Loop *L = LInfo->getLoopFor(I)) {
      if (L->getHeader()==I && L->getParentLoop()==0)
        printLoop(L);
    } else {
      printBasicBlock(I);
    }
  }
}


void MSILWriter::printConstantExpr(const ConstantExpr* CE) {
  const Value *left = 0, *right = 0;
  if (CE->getNumOperands()>=1) left = CE->getOperand(0);
  if (CE->getNumOperands()>=2) right = CE->getOperand(1);
  // Print instruction
  switch (CE->getOpcode()) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    printCastInstruction(CE->getOpcode(),left,CE->getType());
    break;
  case Instruction::GetElementPtr:
    printGepInstruction(CE->getOperand(0),gep_type_begin(CE),gep_type_end(CE));
    break;
  case Instruction::ICmp:
    printICmpInstruction(CE->getPredicate(),left,right);
    break;
  case Instruction::FCmp:
    printFCmpInstruction(CE->getPredicate(),left,right);
    break;
  case Instruction::Select:
    printSelectInstruction(CE->getOperand(0),CE->getOperand(1),CE->getOperand(2));
    break;
  case Instruction::Add:
  case Instruction::FAdd:
    printBinaryInstruction("add",left,right);
    break;
  case Instruction::Sub:
  case Instruction::FSub:
    printBinaryInstruction("sub",left,right);
    break;
  case Instruction::Mul:
  case Instruction::FMul:
    printBinaryInstruction("mul",left,right);
    break;
  case Instruction::UDiv:
    printBinaryInstruction("div.un",left,right);
    break;
  case Instruction::SDiv:
  case Instruction::FDiv:
    printBinaryInstruction("div",left,right);
    break;
  case Instruction::URem:
    printBinaryInstruction("rem.un",left,right);
    break;
  case Instruction::SRem:
  case Instruction::FRem:
    printBinaryInstruction("rem",left,right);
    break;
  case Instruction::And:
    printBinaryInstruction("and",left,right);
    break;
  case Instruction::Or:
    printBinaryInstruction("or",left,right);
    break;
  case Instruction::Xor:
    printBinaryInstruction("xor",left,right);
    break;
  case Instruction::Shl:
    printBinaryInstruction("shl",left,right);
    break;
  case Instruction::LShr:
    printBinaryInstruction("shr.un",left,right);
    break;
  case Instruction::AShr:
    printBinaryInstruction("shr",left,right);
    break;
  default:
    errs() << "Expression = " << *CE << "\n";
    llvm_unreachable("Invalid constant expression");
  }
}


void MSILWriter::printStaticInitializerList() {
  // List of global variables with uninitialized fields.
  for (std::map<const GlobalVariable*,std::vector<StaticInitializer> >::iterator
       VarI = StaticInitList.begin(), VarE = StaticInitList.end(); VarI!=VarE;
       ++VarI) {
    const std::vector<StaticInitializer>& InitList = VarI->second;
    if (InitList.empty()) continue;
    // For each uninitialized field.
    for (std::vector<StaticInitializer>::const_iterator I = InitList.begin(),
         E = InitList.end(); I!=E; ++I) {
      if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(I->constant)) {
        // *Out << "\n// Init " << getValueName(VarI->first) << ", offset " <<
        //  utostr(I->offset) << ", type "<< *I->constant->getType() << "\n\n";
        // Load variable address
        printValueLoad(VarI->first);
        // Add offset
        if (I->offset!=0) {
          printPtrLoad(I->offset);
          printSimpleInstruction("add");
        }
        // Load value
        printConstantExpr(CE);
        // Save result at offset
        std::string postfix = getTypePostfix(CE->getType(),true);
        if (*postfix.begin()=='u') *postfix.begin() = 'i';
        postfix = "stind."+postfix;
        printSimpleInstruction(postfix.c_str());
      } else {
        errs() << "Constant = " << *I->constant << '\n';
        llvm_unreachable("Invalid static initializer");
      }
    }
  }
}


void MSILWriter::printFunction(const Function& F) {
  // FIXME: Signedness for getTypeName
  bool isSigned = F.getAttributes().hasAttribute(0, Attribute::SExt);
  *Out << "\n.method static ";
  *Out << (F.hasLocalLinkage() ? "private " : "public ");
  if (F.isVarArg()) *Out << "vararg ";
  *Out << getFunctionRetTypeName(F.getFunctionType()) << 
    getConvModopt(F.getCallingConv()) << getValueName(&F) << '\n';
  // Arguments
  *Out << "\t(";
  unsigned ArgIdx = 1;
  for (Function::const_arg_iterator I = F.arg_begin(), E = F.arg_end(); I!=E;
       ++I, ++ArgIdx) {
    isSigned = F.getAttributes().hasAttribute(ArgIdx, Attribute::SExt);
    if (I!=F.arg_begin()) *Out << ", ";
    *Out << getFunctionArgTypeName(F.getFunctionType(), I) << getValueName(I);
  }
  *Out << ") cil managed\n";
  // Body
  *Out << "{\n";
  printLocalVariables(F);
  printFunctionBody(F);
  *Out << "}\n";
}


void MSILWriter::printDeclarations(const ValueSymbolTable& ST) {
  std::string Name;
  std::set<const Type*> Printed;
  
  for (SetVector<Type *>::iterator UI = UsedTypes.begin(), UE = UsedTypes.end();
       UI!=UE; ++UI) {
    const Type* Ty = *UI;
    if (Ty->isArrayTy() || Ty->isVectorTy() || Ty->isStructTy())
      Name = getTypeName(Ty, false, true);
    // Type with no need to declare.
    else continue;
    if (isManagedType(Ty)) continue;
    // Print not duplicated type
    if (Printed.insert(Ty).second) {
      *Out << ".class value explicit ansi sealed '" << Name << "'";
      *Out << " { .pack " << 1;
      if (Ty->isSized())
        *Out << " .size " << TD->getTypeAllocSize(const_cast<Type*>(Ty));
      *Out << " }\n\n";
    }
  }
}


unsigned int MSILWriter::getBitWidth(const Type* Ty) {
  unsigned int N = Ty->getPrimitiveSizeInBits();
  assert(N!=0 && "Invalid type in getBitWidth()");
  switch (N) {
  case 1:
  case 8:
  case 16:
  case 32:
  case 64:
    return N;
  default:
    errs() << "Bits = " << N << '\n';
    llvm_unreachable("Unsupported integer width");
  }
  return 0; // Not reached
}


void MSILWriter::printStaticConstant(const Constant* C, uint64_t& Offset) {
  uint64_t TySize = 0;
  const Type* Ty = C->getType();
  // Print zero initialized constant.
  if (isa<ConstantAggregateZero>(C) || C->isNullValue()) {
    TySize = TD->getTypeAllocSize(C->getType());
    Offset += TySize;
    *Out << "int8 (0) [" << TySize << "]";
    return;
  }
  // Print constant initializer
  switch (Ty->getTypeID()) {
  case Type::IntegerTyID: {
    TySize = TD->getTypeAllocSize((Type*)Ty);
    const ConstantInt* Int = cast<ConstantInt>(C);
    *Out << getPrimitiveTypeName(Ty,true) << "(" << Int->getSExtValue() << ")";
    break;
  }
  case Type::FloatTyID:
  case Type::DoubleTyID: {
    TySize = TD->getTypeAllocSize((Type*)Ty);
    const ConstantFP* FP = cast<ConstantFP>(C);
    if (Ty->getTypeID() == Type::FloatTyID)
      *Out << "int32 (" << 
        (uint32_t)FP->getValueAPF().bitcastToAPInt().getZExtValue() << ')';
    else
      *Out << "int64 (" << 
        FP->getValueAPF().bitcastToAPInt().getZExtValue() << ')';
    break;
  }
  case Type::ArrayTyID:
  case Type::VectorTyID:
  case Type::StructTyID:
    if (isa<ConstantDataSequential>(C)) {
      const ConstantDataSequential* Seq = cast<ConstantDataSequential>(C);
      for (unsigned I = 0, E = Seq->getNumElements(); I<E; I++) {
        if (I!=0) *Out << ",\n";
        printStaticConstant(Seq->getElementAsConstant(I), Offset);
      }
      break;
    }
    for (unsigned I = 0, E = C->getNumOperands(); I<E; I++) {
      if (I!=0) *Out << ",\n";
      printStaticConstant(cast<Constant>(C->getOperand(I)), Offset);
    }
    break;
  case Type::PointerTyID:
    TySize = TD->getTypeAllocSize(C->getType());
    // Initialize with global variable address
    if (const GlobalVariable *G = dyn_cast<GlobalVariable>(C)) {
      std::string name = getValueName(G);
      *Out << "&(" << name.insert(name.length()-1,"$data") << ")";
    } else {
      // Dynamic initialization
      if (!isa<ConstantPointerNull>(C) && !C->isNullValue())
        InitListPtr->push_back(StaticInitializer(C,Offset));
      // Null pointer initialization
      if (TySize==4) *Out << "int32 (0)";
      else if (TySize==8) *Out << "int64 (0)";
      else llvm_unreachable("Invalid pointer size");
    }
    break;
  default:
    errs() << "TypeID = " << Ty->getTypeID() << '\n';
    llvm_unreachable("Invalid type in printStaticConstant()");
  }
  // Increase offset.
  Offset += TySize;
}


void MSILWriter::printStaticInitializer(const Constant* C,
                                        const std::string& Name) {
  if (Name == "llvm.global_ctors") {
    // This is already handled in the initialization functions.
    return;
  }

  switch (C->getType()->getTypeID()) {
  case Type::IntegerTyID:
  case Type::FloatTyID:
  case Type::DoubleTyID: 
    *Out << getPrimitiveTypeName(C->getType(), false);
    break;
  case Type::ArrayTyID:
  case Type::VectorTyID:
  case Type::StructTyID:
  case Type::PointerTyID:
    *Out << getTypeName(C->getType());
    break;
  default:
    errs() << "Type = " << *C << "\n";
    llvm_unreachable("Invalid constant type");
  }
  // Print initializer
  std::string label = Name;
  label.insert(label.length()-1,"$data");
  *Out << Name << " at " << label << '\n';
  *Out << ".data " << label << " = {\n";
  uint64_t offset = 0;
  printStaticConstant(C,offset);
  *Out << "\n}\n\n";
}

void MSILWriter::printGlobalConstructors(const GlobalVariable* G) {
  const llvm::Constant *Init = G->getInitializer();
  const llvm::ConstantArray *Arr = cast<ConstantArray>(Init);
  if (!Arr) return;

  for(unsigned i = 0; i < Arr->getNumOperands(); ++i) {
    const ConstantStruct *Struct = cast<ConstantStruct>(
        Arr->getOperand(i));
    if (!Struct) continue;

    llvm::Function* Fn = cast<Function>(Struct->getOperand(1));
    if (!Fn) continue;

    CallInst *Call = CallInst::Create(Fn);
    printFunctionCall(Fn, Call);
    delete Call;
  }
}

void MSILWriter::printVariableDefinition(const GlobalVariable* G) {
  const Constant* C = G->getInitializer();
  if (C->isNullValue() || isa<ConstantAggregateZero>(C) || isa<UndefValue>(C))
    InitListPtr = 0;
  else
    InitListPtr = &StaticInitList[G];
  printStaticInitializer(C,getValueName(G));
}


void MSILWriter::printGlobalVariables() {
  if (ModulePtr->global_empty()) return;
  Module::global_iterator I,E;
  for (I = ModulePtr->global_begin(), E = ModulePtr->global_end(); I!=E; ++I) {
    // Variable definition
    *Out << ".field static " << (I->isDeclaration() ? "public " :
                                                     "private ");
    if (I->isDeclaration()) {
      *Out << getTypeName(I->getType()) << getValueName(&*I) << "\n\n";
    } else
      printVariableDefinition(&*I);
  }
}


const char* MSILWriter::getLibraryName(const Function* F) {
  return getLibraryForSymbol(F->getName(), true, F->getCallingConv());
}


const char* MSILWriter::getLibraryName(const GlobalVariable* GV) {
  return getLibraryForSymbol(GV->getName(), false, CallingConv::C);
}


const char* MSILWriter::getLibraryForSymbol(StringRef Name, bool isFunction,
                                            CallingConv::ID CallingConv) {
  // TODO: Read *.def file with function and libraries definitions.
  return "MSVCRT.DLL";  
}

static bool isManagedCallConv(CallingConv::ID CC) {
  switch(CC) {
  default: return false;
  case CallingConv::CIL_Static:
  case CallingConv::CIL_Instance:
  case CallingConv::CIL_NewObj:
    return true;
  }
}

void MSILWriter::printExternals() {
  Module::const_iterator I,E;
  // Functions.
  for (I=ModulePtr->begin(),E=ModulePtr->end(); I!=E; ++I) {
    // Skip intrisics
    if (I->isIntrinsic()) continue;
    if (I->isDeclaration()) {
      const Function* F = I; 
      if (isManagedIntrinsic((Intrinsic::ID)F->getIntrinsicID()))
        continue;

      CallingConv::ID CC = F->getCallingConv();
      // Managed functions don't need to be declared.
      if (isManagedCallConv(CC))
        continue;

      std::string Name = getConvModopt(CC)+getValueName(F);
      std::string Sig = 
        getCallSignature(cast<FunctionType>(F->getFunctionType()), NULL, Name);
      *Out << ".method static hidebysig pinvokeimpl(\""
          << getLibraryName(F) << "\")\n\t" << Sig << " preservesig {}\n\n";
    }
  }
  // External variables and static initialization.
  *Out <<
  ".method public hidebysig static pinvokeimpl(\"KERNEL32.DLL\" ansi winapi)"
  "  native int LoadLibrary(string) preservesig {}\n"
  ".method public hidebysig static pinvokeimpl(\"KERNEL32.DLL\" ansi winapi)"
  "  native int GetProcAddress(native int, string) preservesig {}\n";
  *Out <<
  ".method private static void* $MSIL_Import(string lib,string sym)\n"
  " managed cil\n{\n"
  "\tldarg\tlib\n"
  "\tcall\tnative int LoadLibrary(string)\n"
  "\tldarg\tsym\n"
  "\tcall\tnative int GetProcAddress(native int,string)\n"
  "\tdup\n"
  "\tbrtrue\tL_01\n"
  "\tldstr\t\"Can no import variable\"\n"
  "\tnewobj\tinstance void [mscorlib]System.Exception::.ctor(string)\n"
  "\tthrow\n"
  "L_01:\n"
  "\tret\n"
  "}\n\n"
  ".method static private void $MSIL_Init() managed cil\n{\n";
  printStaticInitializerList();
  // Foreach global variable.
  for (Module::global_iterator I = ModulePtr->global_begin(),
       E = ModulePtr->global_end(); I!=E; ++I) {
    if (!I->isDeclaration() || !I->hasDLLImportStorageClass()) continue;
    // Use "LoadLibrary"/"GetProcAddress" to recive variable address.
    std::string Tmp = getTypeName(I->getType())+getValueName(&*I);
    printSimpleInstruction("ldsflda",Tmp.c_str());
    *Out << "\tldstr\t\"" << getLibraryName(&*I) << "\"\n";
    *Out << "\tldstr\t\"" << I->getName() << "\"\n";
    printSimpleInstruction("call","void* $MSIL_Import(string,string)");
    printIndirectSave(I->getType());
  }
  
  llvm::GlobalVariable *GlobalCtors = ModulePtr->getGlobalVariable(
      "llvm.global_ctors");
  if (GlobalCtors)
    printGlobalConstructors(GlobalCtors);
  
  printSimpleInstruction("ret");
  *Out << "}\n\n";
}


//===----------------------------------------------------------------------===//
//                      External Interface declaration
//===----------------------------------------------------------------------===//

bool MSILTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                           formatted_raw_ostream &o,
                                           CodeGenFileType FileType,
                                           bool DisableVerify,
                                           AnalysisID StartAfter,
                                           AnalysisID StopAfter) {
  if (FileType != TargetMachine::CGFT_AssemblyFile) return true;

  MSILWriter* Writer = new MSILWriter();
  Writer->Out = &o;

  MSILModule* Module = new MSILModule();
  Module->Writer = Writer;

  PM.add(createGCLoweringPass());
  // FIXME: Handle switch through native IL instruction "switch"
  PM.add(createLowerSwitchPass());
  PM.add(createCFGSimplificationPass());
  PM.add(Module);
  PM.add(Writer);
  return false;
}

void MSILTargetMachine::addAnalysisPasses(PassManagerBase &PM) {
	
}
