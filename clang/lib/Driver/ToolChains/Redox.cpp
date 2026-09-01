//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Redox.h"
#include "Arch/RISCV.h"
#include "clang/Config/config.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

using tools::addPathIfExists;

std::string Redox::getMultiarchTriple(const Driver &D,
                                         const llvm::Triple &TargetTriple,
                                         StringRef SysRoot) const {
  switch (TargetTriple.getArch()) {
  default:
    return TargetTriple.str();
  case llvm::Triple::x86:
    return "i586-unknown-redox";
  case llvm::Triple::x86_64:
    return "x86_64-unknown-redox";
  case llvm::Triple::aarch64:
    return "aarch64-unknown-redox";
  case llvm::Triple::riscv64:
    return "riscv64-unknown-redox";
  }
}

static StringRef getOSLibDir(const llvm::Triple &Triple, const ArgList &Args) {
  return "lib";
}

Redox::Redox(const Driver &D, const llvm::Triple &Triple,
                   const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  GCCInstallation.init(Triple, Args);
  Multilibs = GCCInstallation.getMultilibs();
  SelectedMultilibs.assign({GCCInstallation.getMultilib()});
  std::string SysRoot = computeSysRoot();

  ToolChain::path_list &PPaths = getProgramPaths();

  Generic_GCC::PushPPaths(PPaths);

#ifdef ENABLE_LINKER_BUILD_ID
  ExtraOpts.push_back("--build-id");
#endif

  // The selection of paths to try here is designed to match the patterns which
  // the GCC driver itself uses, as this is part of the GCC-compatible driver.
  // This was determined by running GCC in a fake filesystem, creating all
  // possible permutations of these directories, and seeing which ones it added
  // to the link paths.
  path_list &Paths = getFilePaths();

  const std::string OSLibDir = std::string(getOSLibDir(Triple, Args));
  const std::string MultiarchTriple = getMultiarchTriple(D, Triple, SysRoot);

  Generic_GCC::AddMultilibPaths(D, SysRoot, OSLibDir, MultiarchTriple, Paths);

  addPathIfExists(D, concat(SysRoot, "/lib", MultiarchTriple), Paths);
  addPathIfExists(D, concat(SysRoot, "/lib/..", OSLibDir), Paths);
  addPathIfExists(D, concat(SysRoot, "/usr/lib", MultiarchTriple), Paths);
  addPathIfExists(D, concat(SysRoot, "/usr", OSLibDir), Paths);

  Generic_GCC::AddMultiarchPaths(D, SysRoot, OSLibDir, Paths);

  addPathIfExists(D, concat(SysRoot, "/lib"), Paths);
  addPathIfExists(D, concat(SysRoot, "/usr/lib"), Paths);
}

bool Redox::HasNativeLLVMSupport() const { return true; }

const char *Redox::getDefaultLinker() const { return "ld.lld"; }

ToolChain::CXXStdlibType Redox::GetDefaultCXXStdlibType() const {
  return ToolChain::CST_Libcxx;
}

ToolChain::RuntimeLibType Redox::GetDefaultRuntimeLibType() const {
  return ToolChain::RLT_CompilerRT;
}

Tool *Redox::buildLinker() const {
  return new tools::gnutools::Linker(*this);
}

Tool *Redox::buildStaticLibTool() const {
  return new tools::gnutools::StaticLibTool(*this);
}

Tool *Redox::buildAssembler() const {
  return new tools::gnutools::Assembler(*this);
}

std::string Redox::computeSysRoot() const {
  if (!getDriver().SysRoot.empty())
    return getDriver().SysRoot;
  return std::string();
}

std::string Redox::getDynamicLinker(const ArgList &Args) const {
  switch (getTriple().getArch()) {
  case llvm::Triple::x86:
    return "/lib/ld.so.1";
  case llvm::Triple::x86_64:
  case llvm::Triple::aarch64:
  case llvm::Triple::riscv64:
    return "/lib/ld64.so.1";
  default:
    llvm_unreachable("unsupported architecture");
  }
}

void Redox::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                         ArgStringList &CC1Args) const {
  const Driver &D = getDriver();
  std::string SysRoot = computeSysRoot();

  if (DriverArgs.hasArg(clang::driver::options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nostdlibinc))
    addSystemInclude(DriverArgs, CC1Args, SysRoot + "/usr/include");

  // Add 'include' in the resource directory, which is similar to
  // GCC_INCLUDE_DIR (private headers) in GCC.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> ResourceDirInclude(D.ResourceDir);
    llvm::sys::path::append(ResourceDirInclude, "include");
    addSystemInclude(DriverArgs, CC1Args, ResourceDirInclude);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // TOOL_INCLUDE_DIR
  AddMultilibIncludeArgs(DriverArgs, CC1Args);

  // Check for configure-time C include directories.
  StringRef CIncludeDirs(C_INCLUDE_DIRS);
  if (CIncludeDirs != "") {
    SmallVector<StringRef, 5> dirs;
    CIncludeDirs.split(dirs, ":");
    for (StringRef dir : dirs) {
      StringRef Prefix =
          llvm::sys::path::is_absolute(dir) ? StringRef(SysRoot) : "";
      addExternCSystemInclude(DriverArgs, CC1Args, Prefix + dir);
    }
    return;
  }

  // On systems using multiarch, add /usr/include/$triple before
  // /usr/include.
  std::string MultiarchIncludeDir = getMultiarchTriple(D, getTriple(), SysRoot);
  if (!MultiarchIncludeDir.empty())
    addExternCSystemInclude(
        DriverArgs, CC1Args,
        concat(SysRoot, "/usr/include", MultiarchIncludeDir));

  // Add an include of '/include' directly. This isn't provided by default by
  // system GCCs, but is often used with cross-compiling GCCs, and harmless to
  // add even when Clang is acting as-if it were a system compiler.
  addExternCSystemInclude(DriverArgs, CC1Args, concat(SysRoot, "/include"));

  addExternCSystemInclude(DriverArgs, CC1Args, concat(SysRoot, "/usr/include"));
}

void Redox::addLibStdCxxIncludePaths(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args) const {
  // We need a detected GCC installation on Redox to provide libstdc++'s
  // headers.
  if (!GCCInstallation.isValid())
    return;

  StringRef TripleStr = GCCInstallation.getTriple().str();

  // Try generic GCC detection.
  Generic_GCC::addGCCLibStdCxxIncludePaths(DriverArgs, CC1Args, TripleStr);
}

SanitizerMask Redox::getSupportedSanitizers() const {
  const bool IsX86_64 = getTriple().getArch() == llvm::Triple::x86_64;
  SanitizerMask Res = ToolChain::getSupportedSanitizers();
  Res |= SanitizerKind::PointerCompare;
  Res |= SanitizerKind::PointerSubtract;
  Res |= SanitizerKind::KernelAddress;
  Res |= SanitizerKind::Vptr;
  if (IsX86_64)
    Res |= SanitizerKind::KernelMemory;
  return Res;
}

void Redox::addExtraOpts(llvm::opt::ArgStringList &CmdArgs) const {
  for (const auto &Opt : ExtraOpts)
    CmdArgs.push_back(Opt.c_str());
}
