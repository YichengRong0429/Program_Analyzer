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
// Quick notes:
//   Module     = one source file in IR form
//   BasicBlock = a chunk of code with no jumps inside, ends with a terminator
//   Terminator = last instruction of a block (br, switch, ret, ...)
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

// Adds the flag -goto-counter-static to opt (with clang: -mllvm -goto-counter-static).
static cl::opt<bool> StaticReport(
    "goto-counter-static",
    cl::desc("Print static (compile-time) branch counts per function to stderr"),
    cl::init(false));

namespace
{

// Must match the enum in runtime/runtime.c.
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
    uint32_t FuncIdx;   // index into FuncNames
    uint32_t Line;      // 0 if no debug info
};

// Returns true and sets Kind if T is a branch we count.
// dyn_cast / isa are LLVM's version of dynamic_cast (LLVM doesn't use RTTI).
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
    // getString adds the '\0' at the end for us.
    Constant *Init = ConstantDataArray::getString(M.getContext(), Str);
    // new GlobalVariable(M, ...) adds it to the module, M owns it.
    // PrivateLinkage = only this file sees it, so no name clash when linking.
    GlobalVariable *GV = new GlobalVariable(
        M,
        Init->getType(),
        /*isConstant=*/true,
        GlobalValue::PrivateLinkage,
        Init,
        Name);
    // Address doesn't matter, so the linker can merge same strings.
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return GV;
}

// Same idea, but a constant array: static const T Name[] = {...};
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

// PassInfoMixin fills in the boilerplate for the new pass manager, we just write run().
struct GotoCounterPass : PassInfoMixin<GotoCounterPass>
{
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &)
    {
        std::vector<BranchSite> Sites;
        std::vector<std::string> FuncNames;

        // ---- Step 1: find every branch site (pure static analysis). ----
        // Only collect here. We need N before making the counter array.
        for (Function &F : M)
        {
            // No body here (like printf), skip.
            if (F.isDeclaration())
            {
                continue;
            }
            uint32_t FuncIdx = FuncNames.size();
            // demangle: _Z3fooi -> foo(int). C names stay the same.
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
                // Only has a line if compiled with -g.
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
                // errs() = LLVM's stderr
                errs() << "[GotoCounter static] " << FuncNames.back()
                       << ": goto=" << Static[KindGoto]
                       << " cond_br=" << Static[KindCondBr]
                       << " switch=" << Static[KindSwitch]
                       << " indirectbr=" << Static[KindIndirectBr] << "\n";
            }
        }

        // No function bodies here, so nothing to count. Nothing changed, so
        // all() = old analysis results are still fine.
        // A module with functions but no branch sites still registers (with
        // N = 0), so a branch-free program prints a report of zeros.
        if (FuncNames.empty())
        {
            return PreservedAnalyses::all();
        }

        LLVMContext &Ctx = M.getContext();
        Type *I32 = Type::getInt32Ty(Ctx);
        Type *I64 = Type::getInt64Ty(Ctx);
        // Just `ptr`. Since LLVM 15 all pointers are the same type.
        PointerType *Ptr = PointerType::getUnqual(Ctx);
        const uint64_t N = Sites.size();

        // ---- Step 2: per-site counters and description tables. ----
        // uint64_t __gc_counters[N] = {0};
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
            // Builder puts new instructions right before the branch.
            IRBuilder<> B(Sites[I].Term);
            // GEP = &__gc_counters[I], it only computes the address.
            Value *Slot = B.CreateConstInBoundsGEP2_64(CounterTy, Counters, 0, I);
            // Atomic +1 so threads don't mess up the count.
            // Monotonic = relaxed, the cheapest one.
            B.CreateAtomicRMW( AtomicRMWInst::Add, Slot, ConstantInt::get(I64, 1), MaybeAlign(8), AtomicOrdering::Monotonic);
        }

        // ---- Step 4: module constructor that registers with the runtime. ----
        // void __gc_register(uint64_t *counters, const uint32_t *kinds,
        //                    const uint32_t *func_idx, const uint32_t *lines,
        //                    uint32_t n_sites, const char *const *func_names,
        //                    uint32_t n_funcs, const char *module_name);
        // Only declares it here. The body is in runtime.c and gets linked later.
        FunctionCallee Register = M.getOrInsertFunction( "__gc_register", Type::getVoidTy(Ctx), // return type
            Ptr,                  // counters
            Ptr,                  // kinds
            Ptr,                  // func_idx
            Ptr,                  // lines
            I32,                  // n_sites
            Ptr,                  // func_names
            I32,                  // n_funcs
            Ptr);                 // module_name

        // static void __gc_module_ctor(void) { __gc_register(...); }
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
        // Runs Ctor before main(), like __attribute__((constructor)).
        appendToGlobalCtors(M, Ctor, /*Priority=*/0);

        // We changed the IR, so old analysis results are no good now.
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

// Hook the two functions above into LLVM.
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
// opt/clang look for this exact name, so extern "C" (no name mangling).
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return {LLVM_PLUGIN_API_VERSION, "GotoCounter", "1.0", registerCallbacks};
}
