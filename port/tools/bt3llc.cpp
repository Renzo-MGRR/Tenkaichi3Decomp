// A minimal llc for the PS3 port that does NOT replace the module's data layout.
//
// llc's IR parser always overwrites the module's data layout with the triple's default
// (llvm/tools/llc/lib/llcdriver.cpp, ParserCallbacks). The ppc64 default has no
// 32-bit address space for the game's `__ptr32` pointers, so llc reads and writes
// 8 bytes for every game pointer even though the IR types are 4 bytes wide.
// This driver parses the IR with the same callback but returns the data layout we
// want, then runs the normal target-machine codegen.
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include <memory>
#include <optional>
#include <string>

using namespace llvm;

static std::string sDataLayout =
    "E-m:e-Fi64-i64:64-i128:128-n32:64-S128-v256:256:256-v512:512:512-p271:32:32";

int main(int argc, char **argv) {
  std::string input, output, triple = "powerpc64-unknown-linux-gnu";
  std::string cpu, features = "-hard-float";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--mattr=", 0) == 0)
      features = a.substr(8);
    else if (a.rfind("--triple=", 0) == 0)
      triple = a.substr(9);
    else if (a.rfind("--dl=", 0) == 0)
      sDataLayout = a.substr(5);
    else if (input.empty())
      input = a;
    else
      output = a;
  }
  if (input.empty() || output.empty()) {
    errs() << "usage: bt3llc <in.ll> <out.o> [--mattr=...] [--triple=...] [--dl=...]\n";
    return 2;
  }

  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  LLVMContext context;
  SMDiagnostic err;
  auto module = parseIRFile(input, err, context,
                            ParserCallbacks([](StringRef, StringRef) {
                              return std::optional<std::string>(sDataLayout);
                            }));
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }
  module->setTargetTriple(Triple(triple));

  std::string error;
  const Target *target = TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) {
    errs() << "bt3llc: " << error << "\n";
    return 1;
  }
  TargetOptions options;
  std::unique_ptr<TargetMachine> tm(target->createTargetMachine(
      Triple(triple), cpu, features, options, Reloc::Static, std::nullopt,
      CodeGenOptLevel::Default));
  if (tm == nullptr) {
    errs() << "bt3llc: cannot create the target machine\n";
    return 1;
  }

  std::error_code ec;
  raw_fd_ostream os(output, ec, sys::fs::OF_None);
  if (ec) {
    errs() << "bt3llc: " << ec.message() << "\n";
    return 1;
  }
  legacy::PassManager passes;
  if (tm->addPassesToEmitFile(passes, os, nullptr, CodeGenFileType::ObjectFile)) {
    errs() << "bt3llc: the target cannot write an object file\n";
    return 1;
  }
  passes.run(*module);
  os.close();
  return 0;
}
