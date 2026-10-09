//===- GotoCounterPass.cpp - Count executed branch instructions -----------===//
//
// An LLVM module pass that instruments every branch-like terminator so that,
// when the program runs, we know how many "goto" instructions it executed.
//
// In LLVM IR there is no `goto` keyword. Every control transfer between basic
// blocks is a terminator instruction. We classify them as:
//
//   kind 0  goto        `br label %dest`              (unconditional branch)
//   kind 1  cond br     `br i1 %c, label %a, label %b`
//   kind 2  switch      `switch i32 %v, label %default [...]`
//   kind 3  indirectbr  `indirectbr ptr %addr, [...]` (GNU computed goto)
//
// "Goto instructions" in the task = kind 0, the IR counterpart of Soot's
// GotoStmt. The other kinds are counted too, for context.
//
// How it works (static part, at compile time):
//   1. Walk every defined function and find each branch site.
//   2. Create one 64-bit counter per site in a global array `__gc_counters`,
//      plus constant tables describing each site (kind, function, line).
//   3. Insert `atomicrmw add __gc_counters[i], 1` right before site i.
//   4. Add a module constructor that registers the tables with the runtime
//      library (runtime/runtime.c), which prints a report when the program
//      exits.
//
// The dynamic part happens when the instrumented program runs: each executed
// branch bumps its counter.
//
//===----------------------------------------------------------------------===//

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <array>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<bool> StaticReport(
    "goto-counter-static",
    cl::desc("Print static (compile-time) branch counts per function to stderr"),
    cl::init(false));

namespace
{

enum BranchKind : uint32_t
{
    KindGoto = 0,
    KindCondBr = 1,
    KindSwitch = 2,
    KindIndirectBr = 3,
    NumKinds = 4
};

struct BranchSite
{
    Instruction *Term;
    BranchKind Kind;
    uint32_t FuncIdx;
    uint32_t Line;
};

// Returns true and sets Kind if T is a branch we count.
bool classify(const Instruction *T, BranchKind &Kind)
{
    if (const BranchInst *Br = dyn_cast<BranchInst>(T))
    {
        if (Br->isUnconditional())
        {
            Kind = KindGoto;
        }
        else
        {
            Kind = KindCondBr;
        }
        return true;
    }
    if (isa<SwitchInst>(T))
    {
        Kind = KindSwitch;
        return true;
    }
    if (isa<IndirectBrInst>(T))
    {
        Kind = KindIndirectBr;
        return true;
    }
    return false;
}

// Creates a private constant C string and returns it as a global.
GlobalVariable *makeString(Module &M, StringRef Str, const Twine &Name)
{
    Constant *Init = ConstantDataArray::getString(M.getContext(), Str);
    GlobalVariable *GV = new GlobalVariable(
        M,
        Init->getType(),
        /*isConstant=*/true,
        GlobalValue::PrivateLinkage,
        Init,
        Name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return GV;
}

GlobalVariable *makeConstArray(Module &M, Type *ElemTy, ArrayRef<Constant *> Elems, const Twine &Name)
{
    ArrayType *Ty = ArrayType::get(ElemTy, Elems.size());
    return new GlobalVariable(
        M,
        Ty,
        /*isConstant=*/true,
        GlobalValue::PrivateLinkage,
        ConstantArray::get(Ty, Elems),
        Name);
}

struct GotoCounterPass : PassInfoMixin<GotoCounterPass>
{
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
    {
        std::vector<BranchSite> Sites;
        std::vector<std::string> FuncNames;

        // ---- Step 1: find every branch site (pure static analysis). ----
        for (Function &F : M)
        {
            if (F.isDeclaration())
            {
                continue;
            }
            uint32_t FuncIdx = FuncNames.size();
            FuncNames.push_back(demangle(F.getName().str()));

            std::array<unsigned, NumKinds> Static{};
            for (BasicBlock &BB : F)
            {
                Instruction *T = BB.getTerminator();
                BranchKind Kind;
                if (!T || !classify(T, Kind))
                {
                    continue;
                }
                uint32_t Line = 0;
                if (const DebugLoc &DL = T->getDebugLoc())
                {
                    Line = DL.getLine();
                }
                Sites.push_back({T, Kind, FuncIdx, Line});
                ++Static[Kind];
            }

            if (StaticReport)
            {
                errs() << "[GotoCounter static] " << FuncNames.back()
                       << ": goto=" << Static[KindGoto]
                       << " cond_br=" << Static[KindCondBr]
                       << " switch=" << Static[KindSwitch]
                       << " indirectbr=" << Static[KindIndirectBr] << "\n";
            }
        }

        if (Sites.empty())
        {
            return PreservedAnalyses::all();
        }

        LLVMContext &Ctx = M.getContext();
        Type *I32 = Type::getInt32Ty(Ctx);
        Type *I64 = Type::getInt64Ty(Ctx);
        PointerType *Ptr = PointerType::getUnqual(Ctx);
        const uint64_t N = Sites.size();

        // ---- Step 2: per-site counters and description tables. ----
        ArrayType *CounterTy = ArrayType::get(I64, N);
        GlobalVariable *Counters = new GlobalVariable(
            M,
            CounterTy,
            /*isConstant=*/false,
            GlobalValue::PrivateLinkage,
            Constant::getNullValue(CounterTy),
            "__gc_counters");

        std::vector<Constant *> Kinds, FuncIdxs, Lines;
        for (const BranchSite &S : Sites)
        {
            Kinds.push_back(ConstantInt::get(I32, S.Kind));
            FuncIdxs.push_back(ConstantInt::get(I32, S.FuncIdx));
            Lines.push_back(ConstantInt::get(I32, S.Line));
        }
        std::vector<Constant *> Names;
        for (const std::string &Name : FuncNames)
        {
            Names.push_back(makeString(M, Name, "__gc_fname"));
        }

        GlobalVariable *KindsGV = makeConstArray(M, I32, Kinds, "__gc_kinds");
        GlobalVariable *FuncIdxGV = makeConstArray(M, I32, FuncIdxs, "__gc_func_idx");
        GlobalVariable *LinesGV = makeConstArray(M, I32, Lines, "__gc_lines");
        GlobalVariable *NamesGV = makeConstArray(M, Ptr, Names, "__gc_fnames");
        GlobalVariable *ModNameGV = makeString(M, M.getSourceFileName(), "__gc_module_name");

        // ---- Step 3: insert `counter[i] += 1` before each branch. ----
        for (uint64_t I = 0; I < N; ++I)
        {
            IRBuilder<> B(Sites[I].Term);
            Value *Slot = B.CreateConstInBoundsGEP2_64(CounterTy, Counters, 0, I);
            B.CreateAtomicRMW( AtomicRMWInst::Add, Slot, ConstantInt::get(I64, 1), MaybeAlign(8), AtomicOrdering::Monotonic);
        }

        // ---- Step 4: module constructor that registers with the runtime. ----
        // void __gc_register(uint64_t *counters, const uint32_t *kinds,
        //                    const uint32_t *func_idx, const uint32_t *lines,
        //                    uint32_t n_sites, const char *const *func_names,
        //                    uint32_t n_funcs, const char *module_name);
        FunctionCallee Register = M.getOrInsertFunction( "__gc_register", Type::getVoidTy(Ctx), // return type
            Ptr,                  // counters
            Ptr,                  // kinds
            Ptr,                  // func_idx
            Ptr,                  // lines
            I32,                  // n_sites
            Ptr,                  // func_names
            I32,                  // n_funcs
            Ptr);                 // module_name
        
        Function *Ctor = Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false), GlobalValue::InternalLinkage, "__gc_module_ctor", M);
        IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Ctor));
        B.CreateCall(
            Register,
            {
                Counters,
                KindsGV,
                FuncIdxGV,
                LinesGV,
                ConstantInt::get(I32, N),
                NamesGV,
                ConstantInt::get(I32, FuncNames.size()),
                ModNameGV,
            });
        B.CreateRetVoid();
        appendToGlobalCtors(M, Ctor, /*Priority=*/0);

        return PreservedAnalyses::none();
    }

    // Run even on functions marked `optnone` (clang -O0 adds that attribute).
    static bool isRequired()
    {
        return true;
    }
};

// Called by opt for each name in `-passes=...`; we handle "goto-counter".
bool parsePipelineName(StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>)
{
    if (Name != "goto-counter")
    {
        return false;
    }
    MPM.addPass(GotoCounterPass());
    return true;
}

// Called by clang (-fpass-plugin): run at the end of the optimization pipeline.
void addAtOptimizerEnd(ModulePassManager &MPM, OptimizationLevel)
{
    MPM.addPass(GotoCounterPass());
}

void registerCallbacks(PassBuilder &PB)
{
    PB.registerPipelineParsingCallback(parsePipelineName);
    PB.registerOptimizerLastEPCallback(addAtOptimizerEnd);
}

} // namespace

// Plugin entry point. Makes the pass available as:
//   opt   -load-pass-plugin=libGotoCounter.so -passes=goto-counter in.bc
//   clang -fpass-plugin=libGotoCounter.so ...   (runs at the end of the
//                                                 optimization pipeline)
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return {LLVM_PLUGIN_API_VERSION, "GotoCounter", "1.0", registerCallbacks};
}
