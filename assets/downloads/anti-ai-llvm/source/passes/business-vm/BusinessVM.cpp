// Experimental whole-function scalar VM. LLVM 21; opt-in, fail closed.
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <vector>
using namespace llvm;
namespace {
enum Op { MOVI,MOV,ADD,SUB,MUL,XOR,AND,OR,SHL,LSHR,ASHR,UDIV,SDIV,UREM,SREM,CMP,SEL,CAST,SEXT,JMP,BR,RET,NOPS };
constexpr unsigned W=9;
struct Ins { std::array<uint32_t,W> f{}; int nextLabel=-1,altLabel=-1; };
uint32_t mix(uint32_t x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
bool marked(Function &F){
 auto *A=F.getParent()->getNamedGlobal("llvm.global.annotations");if(!A||!A->hasInitializer())return false;
 auto *E=dyn_cast<ConstantArray>(A->getInitializer());if(!E)return false;
 for(auto &U:E->operands()){auto *S=dyn_cast<ConstantStruct>(U.get());if(!S||S->getNumOperands()<2)continue;
  if(S->getOperand(0)->stripPointerCasts()!=&F)continue;
  auto *G=dyn_cast<GlobalVariable>(S->getOperand(1)->stripPointerCasts());if(!G||!G->hasInitializer())continue;
  auto *D=dyn_cast<ConstantDataArray>(G->getInitializer());if(D&&D->isCString()&&D->getAsCString()=="omvll.business-vm")return true;
 }return false;
}
struct Lower {
 Function &F;Module &M;LLVMContext &C;std::mt19937 R;uint32_t seed;
 DenseMap<Value*,unsigned> ids;DenseMap<BasicBlock*,unsigned> labels;std::map<std::pair<BasicBlock*,BasicBlock*>,unsigned> edges;
 std::vector<Ins> code;std::map<unsigned,unsigned> addr;std::vector<ConstantInt*> constants;unsigned regs=0,nextLabel=0;std::array<uint32_t,NOPS> opcode;
 Lower(Function &f,uint32_t s):F(f),M(*f.getParent()),C(f.getContext()),R(s),seed(s){std::set<uint32_t>x;for(auto &v:opcode){do{v=R()|0x10000u;}while(!x.insert(v).second);}}
 unsigned reg(Value *V){auto it=ids.find(V);if(it!=ids.end())return it->second;auto *K=dyn_cast<ConstantInt>(V);if(!K)report_fatal_error("business-vm: unmapped operand");unsigned i=regs++;ids[V]=i;constants.push_back(K);return i;}
 unsigned edge(BasicBlock *a,BasicBlock*b){auto p=std::make_pair(a,b);auto it=edges.find(p);if(it!=edges.end())return it->second;return edges[p]=nextLabel++;}
 unsigned bits(Type*T){return cast<IntegerType>(T)->getBitWidth();}
 void emit(Op op,unsigned dst=0,unsigned a=0,unsigned b=0,unsigned c=0,uint32_t imm=0,unsigned width=32,int target=-1,int alt=-1){Ins i;i.f={opcode[op],dst,a,b,c,imm,width,uint32_t(code.size()+1),0};i.nextLabel=target;i.altLabel=alt;code.push_back(i);}
 bool validate(){
  if(F.isDeclaration()||F.arg_size()!=4||!F.getReturnType()->isIntegerTy(32)||F.isVarArg())return false;
  for(auto &a:F.args())if(!a.getType()->isIntegerTy(32))return false;
  if(F.size()>128)return false;
  for(auto &I:instructions(F)){
   if(!I.getType()->isVoidTy()&&(!I.getType()->isIntegerTy()||bits(I.getType())>32))return false;
   for(Value *V:I.operands())if(!isa<BasicBlock>(V)&&(!V->getType()->isIntegerTy()||bits(V->getType())>32||(!isa<Instruction,Argument,ConstantInt>(V))))return false;
   if(isa<PHINode,ICmpInst,SelectInst,BranchInst,ReturnInst>(I))continue;
   switch(I.getOpcode()){case Instruction::Add:case Instruction::Sub:case Instruction::Mul:case Instruction::Xor:case Instruction::And:case Instruction::Or:case Instruction::Shl:case Instruction::LShr:case Instruction::AShr:case Instruction::UDiv:case Instruction::SDiv:case Instruction::URem:case Instruction::SRem:case Instruction::Trunc:case Instruction::ZExt:case Instruction::SExt:break;default:return false;}
  }return true;
 }
 void compile(){
  for(auto &a:F.args())ids[&a]=regs++;
  for(auto &B:F){labels[&B]=nextLabel++;for(auto &I:B)if(!I.getType()->isVoidTy())ids[&I]=regs++;}
  for(auto &I:instructions(F))for(Value*V:I.operands())if(isa<ConstantInt>(V))reg(V);
  for(auto *K:constants)emit(MOVI,reg(K),0,0,0,uint32_t(K->getZExtValue()),bits(K->getType()));
  emit(JMP,0,0,0,0,0,32,labels[&F.getEntryBlock()]);
  std::vector<BasicBlock*> blocks;for(auto &B:F)blocks.push_back(&B);std::shuffle(blocks.begin(),blocks.end(),R);
  for(auto *B:blocks){addr[labels[B]]=code.size();for(auto &I:*B){if(isa<PHINode>(I))continue;
   if(auto *J=dyn_cast<BranchInst>(&I)){if(J->isConditional())emit(BR,0,reg(J->getCondition()),0,0,0,1,edge(B,J->getSuccessor(0)),edge(B,J->getSuccessor(1)));else emit(JMP,0,0,0,0,0,32,edge(B,J->getSuccessor(0)));continue;}
   if(auto *T=dyn_cast<ReturnInst>(&I)){emit(RET,0,reg(T->getReturnValue()));continue;}
   unsigned d=reg(&I),a=reg(I.getOperand(0));
   if(auto *S=dyn_cast<SelectInst>(&I)){emit(SEL,d,a,reg(S->getTrueValue()),reg(S->getFalseValue()),0,bits(I.getType()));continue;}
   if(auto *Q=dyn_cast<ICmpInst>(&I)){emit(CMP,d,a,reg(I.getOperand(1)),0,Q->getPredicate(),bits(I.getOperand(0)->getType()));continue;}
   if(I.isCast()){emit(I.getOpcode()==Instruction::SExt?SEXT:CAST,d,a,0,0,bits(I.getOperand(0)->getType()),bits(I.getType()));continue;}
   Op op;switch(I.getOpcode()){case Instruction::Add:op=ADD;break;case Instruction::Sub:op=SUB;break;case Instruction::Mul:op=MUL;break;case Instruction::Xor:op=XOR;break;case Instruction::And:op=AND;break;case Instruction::Or:op=OR;break;case Instruction::Shl:op=SHL;break;case Instruction::LShr:op=LSHR;break;case Instruction::AShr:op=ASHR;break;case Instruction::UDiv:op=UDIV;break;case Instruction::SDiv:op=SDIV;break;case Instruction::URem:op=UREM;break;case Instruction::SRem:op=SREM;break;default:llvm_unreachable("validated opcode");}
   emit(op,d,a,reg(I.getOperand(1)),0,0,bits(I.getType()));
  }}
  std::vector<std::pair<std::pair<BasicBlock*,BasicBlock*>,unsigned>> es(edges.begin(),edges.end());std::shuffle(es.begin(),es.end(),R);
  for(auto &E:es){auto *from=E.first.first,*to=E.first.second;addr[E.second]=code.size();std::vector<std::pair<PHINode*,unsigned>> copies;
   for(auto &I:*to){auto *P=dyn_cast<PHINode>(&I);if(!P)break;unsigned temp=regs++;emit(MOV,temp,reg(P->getIncomingValueForBlock(from)),0,0,0,bits(P->getType()));copies.emplace_back(P,temp);}
   for(auto [P,tmp]:copies)emit(MOV,reg(P),tmp,0,0,0,bits(P->getType()));emit(JMP,0,0,0,0,0,32,labels[to]);
  }
  for(auto &i:code){if(i.nextLabel>=0)i.f[7]=addr.at(i.nextLabel);if(i.altLabel>=0)i.f[8]=addr.at(i.altLabel);}
  // Randomize the complete logical register namespace, including PHI temporaries.
  std::vector<unsigned> perm(regs);for(unsigned i=0;i<regs;++i)perm[i]=i;std::shuffle(perm.begin(),perm.end(),R);
  for(auto &i:code)for(unsigned k=1;k<=4;++k)i.f[k]=perm.at(i.f[k]);
  for(auto &v:ids)v.second=perm.at(v.second);
 }
 Value *mixIR(IRBuilder<> &B,Value *x){x=B.CreateXor(x,B.CreateLShr(x,16));x=B.CreateMul(x,B.getInt32(0x7feb352d));x=B.CreateXor(x,B.CreateLShr(x,15));x=B.CreateMul(x,B.getInt32(0x846ca68b));return B.CreateXor(x,B.CreateLShr(x,16));}
 void materialize(){
  auto *I32=Type::getInt32Ty(C);std::vector<uint32_t> words;
  for(unsigned p=0;p<code.size();++p)for(unsigned k=0;k<W;++k)words.push_back(code[p].f[k]^mix(seed^(p*0x9e3779b9u)^(k*0x85ebca6bu)));
  auto *A=ConstantDataArray::get(C,words);auto *G=new GlobalVariable(M,A->getType(),true,GlobalValue::PrivateLinkage,A,"__bvm_code");G->setAlignment(Align(4));
  F.deleteBody();F.setAttributes(AttributeList());F.addFnAttr(Attribute::NoInline);F.addFnAttr(Attribute::OptimizeNone);F.addFnAttr(Attribute::NoUnwind);F.addFnAttr("omvll.business-vm.applied");
  auto *entry=BasicBlock::Create(C,"vm.entry",&F),*dispatch=BasicBlock::Create(C,"vm.dispatch",&F),*store=BasicBlock::Create(C,"vm.store",&F),*bad=BasicBlock::Create(C,"vm.invalid",&F);
  IRBuilder<> B(entry);auto *frameTy=ArrayType::get(I32,regs);auto *frame=B.CreateAlloca(frameTy);auto *pc=B.CreateAlloca(I32);auto *session=mixIR(B,B.CreateXor(F.getArg(0),B.CreateAdd(F.getArg(3),B.getInt32(seed^0xd1b54a35u))));
  auto ptr=[&](IRBuilder<> &Q,Value *id){return Q.CreateGEP(frameTy,frame,{Q.getInt32(0),id});};
  auto encode=[&](IRBuilder<> &Q,Value *val,Value *id){Value *mul=Q.CreateOr(mixIR(Q,Q.CreateXor(id,Q.getInt32(seed^0x7d3b29a1u))),Q.getInt32(1));Value *add=Q.CreateAdd(Q.CreateMul(id,Q.getInt32(0x85ebca6b)),session);Value *key=Q.CreateXor(Q.CreateMul(id,Q.getInt32(0xc2b2ae35)),session);return Q.CreateAdd(Q.CreateMul(Q.CreateXor(val,key),mul),add);};
  auto decode=[&](IRBuilder<> &Q,Value *id){auto *L=Q.CreateLoad(I32,ptr(Q,id));L->setVolatile(true);Value *mul=Q.CreateOr(mixIR(Q,Q.CreateXor(id,Q.getInt32(seed^0x7d3b29a1u))),Q.getInt32(1));Value *inv=mul;for(unsigned z=0;z<5;++z)inv=Q.CreateMul(inv,Q.CreateSub(Q.getInt32(2),Q.CreateMul(mul,inv)));Value *add=Q.CreateAdd(Q.CreateMul(id,Q.getInt32(0x85ebca6b)),session);Value *key=Q.CreateXor(Q.CreateMul(id,Q.getInt32(0xc2b2ae35)),session);return Q.CreateXor(Q.CreateMul(Q.CreateSub(L,add),inv),key);};
  for(unsigned i=0;i<regs;++i){auto *S=B.CreateStore(encode(B,B.getInt32(0),B.getInt32(i)),ptr(B,B.getInt32(i)));S->setVolatile(true);}
  for(auto &arg:F.args()){unsigned id=ids[&arg];auto *S=B.CreateStore(encode(B,&arg,B.getInt32(id)),ptr(B,B.getInt32(id)));S->setVolatile(true);}
  B.CreateStore(B.getInt32(0),pc)->setVolatile(true);B.CreateBr(dispatch);
  B.SetInsertPoint(dispatch);auto *P=B.CreateLoad(I32,pc);P->setVolatile(true);std::array<Value*,W> f;
  for(unsigned k=0;k<W;++k){Value *ix=B.CreateAdd(B.CreateMul(P,B.getInt32(W)),B.getInt32(k));auto *L=B.CreateLoad(I32,B.CreateGEP(A->getType(),G,{B.getInt32(0),ix}));L->setVolatile(true);Value *key=mixIR(B,B.CreateXor(B.CreateXor(B.getInt32(seed),B.CreateMul(P,B.getInt32(0x9e3779b9))),B.getInt32(k*0x85ebca6bu)));f[k]=B.CreateXor(L,key);}
  Value *a=decode(B,f[2]),*b=decode(B,f[3]),*c=decode(B,f[4]);Value *shift=B.CreateSub(B.getInt32(32),f[6]);Value *sa=B.CreateAShr(B.CreateShl(a,shift),shift),*sb=B.CreateAShr(B.CreateShl(b,shift),shift);
  auto *SW=B.CreateSwitch(f[0],bad,NOPS);IRBuilder<> SB(store);auto *result=SB.CreatePHI(I32,NOPS);Value *mask=SB.CreateLShr(SB.getInt32(0xffffffffu),SB.CreateSub(SB.getInt32(32),f[6]));Value *v=SB.CreateAnd(result,mask);SB.CreateStore(encode(SB,v,f[1]),ptr(SB,f[1]))->setVolatile(true);SB.CreateStore(f[7],pc)->setVolatile(true);SB.CreateBr(dispatch);
  for(unsigned op=0;op<NOPS;++op){auto *H=BasicBlock::Create(C,"vm.handler",&F);SW->addCase(B.getInt32(opcode[op]),H);IRBuilder<> Q(H);Value *v=nullptr;
   Value *nz=Q.CreateSelect(Q.CreateICmpEQ(b,Q.getInt32(0)),Q.getInt32(1),b);Value *sbad=Q.CreateOr(Q.CreateICmpEQ(sb,Q.getInt32(0)),Q.CreateAnd(Q.CreateICmpEQ(sa,Q.getInt32(0x80000000u)),Q.CreateICmpEQ(sb,Q.getInt32(0xffffffffu))));Value *snz=Q.CreateSelect(sbad,Q.getInt32(1),sb);Value *bs=Q.CreateAnd(b,Q.getInt32(31));
   switch(op){case MOVI:v=f[5];break;case MOV:v=a;break;case ADD:v=Q.CreateAdd(a,b);break;case SUB:v=Q.CreateSub(a,b);break;case MUL:v=Q.CreateMul(a,b);break;case XOR:v=Q.CreateXor(a,b);break;case AND:v=Q.CreateAnd(a,b);break;case OR:v=Q.CreateOr(a,b);break;case SHL:v=Q.CreateShl(a,bs);break;case LSHR:v=Q.CreateLShr(a,bs);break;case ASHR:v=Q.CreateAShr(sa,bs);break;case UDIV:v=Q.CreateUDiv(a,nz);break;case SDIV:v=Q.CreateSDiv(sa,snz);break;case UREM:v=Q.CreateURem(a,nz);break;case SREM:v=Q.CreateSRem(sa,snz);break;case SEL:v=Q.CreateSelect(Q.CreateICmpNE(a,Q.getInt32(0)),b,c);break;case CAST:v=a;break;
   case SEXT:{Value *s=Q.CreateSub(Q.getInt32(32),f[5]);v=Q.CreateAShr(Q.CreateShl(a,s),s);break;}
   case CMP:{v=Q.getInt32(0);for(unsigned pred=CmpInst::FIRST_ICMP_PREDICATE;pred<=CmpInst::LAST_ICMP_PREDICATE;++pred){bool sign=CmpInst::isSigned(CmpInst::Predicate(pred));Value *test=Q.CreateZExt(Q.CreateICmp(CmpInst::Predicate(pred),sign?sa:a,sign?sb:b),I32);v=Q.CreateSelect(Q.CreateICmpEQ(f[5],Q.getInt32(pred)),test,v);}break;}
   case JMP:Q.CreateStore(f[7],pc)->setVolatile(true);Q.CreateBr(dispatch);break;
   case BR:Q.CreateStore(Q.CreateSelect(Q.CreateICmpNE(a,Q.getInt32(0)),f[7],f[8]),pc)->setVolatile(true);Q.CreateBr(dispatch);break;
   case RET:Q.CreateRet(a);break;default:llvm_unreachable("op");}
   if(v){if(op==CMP)v=Q.CreateAnd(v,Q.getInt32(1));result->addIncoming(v,H);Q.CreateBr(store);}
  }
  B.SetInsertPoint(bad);B.CreateRet(B.getInt32(0x800000ffu));
  // Drop stale memory/return inference on existing caller sites too.
  for(Function &X:M){X.removeRetAttr(Attribute::Range);X.removeFnAttr(Attribute::Memory);for(Instruction &I:instructions(X))if(auto *CB=dyn_cast<CallBase>(&I)){CB->removeRetAttr(Attribute::Range);CB->removeFnAttr(Attribute::Memory);}}
  if(verifyFunction(F,&errs()))report_fatal_error("business-vm: generated invalid IR");
 }
};
struct BusinessVM:PassInfoMixin<BusinessVM>{PreservedAnalyses run(Module &M,ModuleAnalysisManager&){std::vector<Function*> fs;for(auto &F:M)if(marked(F)&&!F.hasFnAttribute("omvll.business-vm.applied"))fs.push_back(&F);if(fs.empty())return PreservedAnalyses::all();uint32_t seed=0x91c74badu;if(const char *s=std::getenv("OMVLL_VM_SEED"))seed=std::strtoul(s,nullptr,0);for(auto *F:fs){Lower Probe(*F,seed);if(!Probe.validate())report_fatal_error("business-vm: unsupported selected function; refusing plaintext fallback");}for(auto *F:fs){Lower L(*F,seed);L.compile();L.materialize();seed=mix(seed+1);}return PreservedAnalyses::none();}};
}
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo(){return {LLVM_PLUGIN_API_VERSION,"BusinessVM","0.1",[](PassBuilder &PB){PB.registerPipelineParsingCallback([](StringRef N,ModulePassManager &MPM,ArrayRef<PassBuilder::PipelineElement>){if(N!="v4-virtualize")return false;MPM.addPass(BusinessVM());return true;});}};}
