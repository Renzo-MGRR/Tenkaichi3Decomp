// The game's memory model on the PS3 (PSL1GHT / LV2, PPC64 big-endian), as an LLVM pass
// (opt -load-pass-plugin ps3win.so -passes=bt3-window).
//
// This is port/tools/macwin/macwin.cpp (the macOS/arm64 window model) ported to PS3LV2 ELF linking:
//
// The game's pointers are 4 bytes wide (port/tools/ptr32.py: `__ptr32 __uptr`, address space 271). Game code
// keeps every game address as an OFFSET into a 4 GB window; the window's base is a runtime variable,
// Port_Window (port/src/plat_mem.c), loaded at the start of each function.
//
// Differences from the macOS version:
//   - the program is linked at a FIXED effective address by an LV2 ELF64 SELF (no ASLR, no relocation), so the
//     constant anchor here is Port_Anchor (port/ps3/ps3win.c), a global the linker places at a known EA, instead
//     of macOS's _mh_execute_header;
//   - no LD passes ; the PS3 link uses GNU ld's --wrap / --defsym (done by the linker, not here), so +bt3-wrap
//     and +bt3-abs stay supported for parity with tests but are not needed;
//   - LLVM 21 instead of 22 in the pass's host build.
// All other semantics (constants, addrspacecast, ptrtoint narrowing, call signatures) are identical; see the
// header of macwin.cpp for the full description.
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <vector>

using namespace llvm;

namespace {

cl::list<std::string> Wrap("bt3-wrap", cl::desc("functions to wrap, as GNU ld --wrap"), cl::CommaSeparated);
cl::list<std::string> Abs("bt3-abs", cl::desc("NAME=ADDR: a symbol at a game address"), cl::CommaSeparated);
cl::opt<bool> Report("bt3-report", cl::desc("list host pointers made into integers outside the window arithmetic"), cl::init(false));
cl::opt<bool> PortModule("bt3-port", cl::desc("a port source, not the game's"), cl::init(false));
cl::opt<std::string> Defs("bt3-defs", cl::desc("write this module's function definitions to FILE, change nothing"), cl::init(""));
cl::opt<std::string> Sigs("bt3-sigs", cl::desc("the program's function definitions (FILE from -bt3-defs)"), cl::init(""));

constexpr unsigned GAME = 271; // __ptr32 __uptr: the game's 4-byte pointers
constexpr unsigned HOST64 = 272; // __ptr64: ordinary 64-bit pointers (ptr32.py uses them for variadic arguments)
constexpr int64_t ANCHOR_ADDR = 0x0000000010000000LL; // PL_PS3_ANCHOR: Port_Anchor's linked EA (port/src/plat_mem.c)
constexpr int64_t PROGRAM_OFFSET = 0x20000000; // where the program would start in a 4 GB window

struct Window {
    Module &M;
    LLVMContext &Ctx;
    Type *I64, *I32, *I8;
    PointerType *Host, *Game;
    Constant *W = nullptr;
    std::map<Function *, Value *> WIn; // per function: the base loaded from Port_Window
    std::map<Constant *, Constant *> Done;
    std::map<Constant *, Constant *> HostOf; // a 4-byte pointer constant made from a host address -> that address
    bool Failed = false;

    explicit Window(Module &M)
        : M(M), Ctx(M.getContext()), I64(Type::getInt64Ty(Ctx)), I32(Type::getInt32Ty(Ctx)), I8(Type::getInt8Ty(Ctx)),
          Host(PointerType::get(Ctx, 0)), Game(PointerType::get(Ctx, GAME)) {}

    Constant *window() {
        if (!W) {
            GlobalVariable *anchor = M.getGlobalVariable("Port_Anchor");
            while (anchor && anchor->hasInitializer() && dyn_cast<GlobalAlias>(anchor)) {}
            if (!anchor) {
                anchor = new GlobalVariable(M, I8, true, GlobalValue::ExternalLinkage, nullptr, "Port_Anchor");
                anchor->setVisibility(GlobalValue::HiddenVisibility);
                anchor->setDSOLocal(true);
            }
            // Port_Anchor is linked at ANCHOR_ADDR; the window begins ANCHOR_ADDRESS below the program's
            // canonical place in the window (0x20000000, as on Linux and macOS).
            W = ConstantExpr::getGetElementPtr(I8, anchor, ConstantInt::get(I64, PROGRAM_OFFSET - ANCHOR_ADDR));
        }
        return W;
    }

    bool isGame(Type *T) { return T->isPointerTy() && T->getPointerAddressSpace() == GAME; }
    bool isHostLike(Type *T) {
        return T->isPointerTy() && (T->getPointerAddressSpace() == 0 || T->getPointerAddressSpace() == HOST64);
    }

    // ---- constants (data and constant operands)
    Constant *hostC(Constant *off, PointerType *To) { // a 4-byte pointer constant as a host address
        if (off->isNullValue()) {
            return ConstantPointerNull::get(To);
        }
        auto known = HostOf.find(off);
        if (known != HostOf.end()) { // W + (p - W): p itself
            return To->getAddressSpace() == 0 ? known->second : ConstantExpr::getAddrSpaceCast(known->second, To);
        }
        Constant *p = ConstantExpr::getGetElementPtr(I8, window(), ConstantExpr::getPtrToInt(off, I64));
        return To->getAddressSpace() == 0 ? p : ConstantExpr::getAddrSpaceCast(p, To);
    }
    Constant *gameC(Constant *p) { // a host address constant as a 4-byte pointer
        if (p->isNullValue()) {
            return ConstantPointerNull::get(Game);
        }
        if (p->getType()->getPointerAddressSpace() != 0) {
            p = ConstantExpr::getAddrSpaceCast(p, Host);
        }
        Constant *d = ConstantExpr::getSub(ConstantExpr::getPtrToInt(p, I64), ConstantExpr::getPtrToInt(window(), I64));
        Constant *off = ConstantExpr::getIntToPtr(d, Game);
        HostOf[off] = p;
        return off;
    }
    Constant *fixC(Constant *C) {
        if (isa<GlobalValue>(C) || isa<ConstantData>(C)) {
            return C;
        }
        auto it = Done.find(C);
        if (it != Done.end()) {
            return it->second;
        }
        std::vector<Constant *> ops;
        bool changed = false;
        for (Use &U : C->operands()) {
            Constant *o = fixC(cast<Constant>(U.get()));
            changed |= o != U.get();
            ops.push_back(o);
        }
        Constant *R = C;
        if (auto *CE = dyn_cast<ConstantExpr>(C)) {
            Type *from = CE->getOperand(0)->getType(), *to = CE->getType();
            if (CE->getOpcode() == Instruction::AddrSpaceCast && isGame(from) && isHostLike(to)) {
                R = hostC(ops[0], cast<PointerType>(to));
            } else if (CE->getOpcode() == Instruction::AddrSpaceCast && isHostLike(from) && isGame(to)) {
                R = gameC(ops[0]);
            } else if (!PortModule && CE->getOpcode() == Instruction::PtrToInt && from->getPointerAddressSpace() == 0 &&
                       to->getIntegerBitWidth() <= 32) {
                R = ConstantExpr::getPtrToInt(gameC(ops[0]), to);
            } else if (!PortModule && CE->getOpcode() == Instruction::Trunc && to->getIntegerBitWidth() <= 32 &&
                       isa<ConstantExpr>(ops[0]) && cast<ConstantExpr>(ops[0])->getOpcode() == Instruction::PtrToInt &&
                       cast<ConstantExpr>(ops[0])->getOperand(0)->getType()->getPointerAddressSpace() == 0) {
                R = ConstantExpr::getPtrToInt(gameC(cast<ConstantExpr>(ops[0])->getOperand(0)), to);
            } else if (changed) {
                R = CE->getWithOperands(ops);
            }
        } else if (changed) {
            if (auto *S = dyn_cast<ConstantStruct>(C)) {
                R = ConstantStruct::get(S->getType(), ops);
            } else if (auto *A = dyn_cast<ConstantArray>(C)) {
                R = ConstantArray::get(A->getType(), ops);
            } else if (auto *V = dyn_cast<ConstantVector>(C)) {
                R = ConstantVector::get(ops);
            }
        }
        Done[C] = R;
        return R;
    }

    // ---- values in code
    static bool knownNonNull(Value *V) {
        V = V->stripInBoundsOffsets();
        return isa<AllocaInst>(V) || isa<GlobalValue>(V);
    }
    Value *windowIn(IRBuilder<> &B) {
        Function *F = B.GetInsertBlock()->getParent();
        auto it = WIn.find(F);
        if (it != WIn.end()) {
            return it->second;
        }
        GlobalVariable *var = M.getGlobalVariable("Port_Window");
        if (!var) {
            var = new GlobalVariable(M, Host, false, GlobalValue::ExternalLinkage, nullptr, "Port_Window");
        }
        IRBuilder<> EB(&*F->getEntryBlock().getFirstInsertionPt());
        LoadInst *w = EB.CreateLoad(Host, var, "w");
        //  W points at the program's 4 GB window; Port_Anchor sits ... (see window()'s constant form)
        w->setWeightHint?();
        w->setMustAccess();
        return WIn[F] = w;
    }
};

} // namespace
