//===-- AArch64TargetObjectFile.cpp - AArch64 Object Info -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AArch64TargetObjectFile.h"
#include "AArch64TargetMachine.h"
#include "MCTargetDesc/AArch64MCExpr.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCValue.h"
using namespace llvm;
using namespace dwarf;

void AArch64_ELFTargetObjectFile::Initialize(MCContext &Ctx,
                                             const TargetMachine &TM) {
  TargetLoweringObjectFileELF::Initialize(Ctx, TM);
  // AARCH64 ELF ABI does not define static relocation type for TLS offset
  // within a module.  Do not generate AT_location for TLS variables.
  SupportDebugThreadLocalLocation = false;
}

const MCExpr *AArch64_ELFTargetObjectFile::getIndirectSymViaGOTPCRel(
    const GlobalValue *GV, const MCSymbol *Sym, const MCValue &MV,
    int64_t Offset, MachineModuleInfo *MMI, MCStreamer &Streamer) const {
  int64_t FinalOffset = Offset + MV.getConstant();
  const MCExpr *Res =
      MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_GOTPCREL, getContext());
  const MCExpr *Off = MCConstantExpr::create(FinalOffset, getContext());
  return MCBinaryExpr::createAdd(Res, Off, getContext());
}

AArch64_MachoTargetObjectFile::AArch64_MachoTargetObjectFile() {
  SupportGOTPCRelWithOffset = false;
}

const MCExpr *AArch64_MachoTargetObjectFile::getTTypeGlobalReference(
    const GlobalValue *GV, unsigned Encoding, const TargetMachine &TM,
    MachineModuleInfo *MMI, MCStreamer &Streamer) const {
  // On Darwin, we can reference dwarf symbols with foo@GOT-., which
  // is an indirect pc-relative reference. The default implementation
  // won't reference using the GOT, so we need this target-specific
  // version.
  if (Encoding & (DW_EH_PE_indirect | DW_EH_PE_pcrel)) {
    const MCSymbol *Sym = TM.getSymbol(GV);
    const MCExpr *Res =
        MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_GOT, getContext());
    MCSymbol *PCSym = getContext().createTempSymbol();
    Streamer.emitLabel(PCSym);
    const MCExpr *PC = MCSymbolRefExpr::create(PCSym, getContext());
    return MCBinaryExpr::createSub(Res, PC, getContext());
  }

  return TargetLoweringObjectFileMachO::getTTypeGlobalReference(
      GV, Encoding, TM, MMI, Streamer);
}

MCSymbol *AArch64_MachoTargetObjectFile::getCFIPersonalitySymbol(
    const GlobalValue *GV, const TargetMachine &TM,
    MachineModuleInfo *MMI) const {
  return TM.getSymbol(GV);
}

const MCExpr *AArch64_MachoTargetObjectFile::getIndirectSymViaGOTPCRel(
    const GlobalValue *GV, const MCSymbol *Sym, const MCValue &MV,
    int64_t Offset, MachineModuleInfo *MMI, MCStreamer &Streamer) const {
  assert((Offset+MV.getConstant() == 0) &&
         "Arch64 does not support GOT PC rel with extra offset");
  // On ARM64 Darwin, we can reference symbols with foo@GOT-., which
  // is an indirect pc-relative reference.
  const MCExpr *Res =
      MCSymbolRefExpr::create(Sym, MCSymbolRefExpr::VK_GOT, getContext());
  MCSymbol *PCSym = getContext().createTempSymbol();
  Streamer.emitLabel(PCSym);
  const MCExpr *PC = MCSymbolRefExpr::create(PCSym, getContext());
  return MCBinaryExpr::createSub(Res, PC, getContext());
}

void AArch64_MachoTargetObjectFile::getNameWithPrefix(
    SmallVectorImpl<char> &OutName, const GlobalValue *GV,
    const TargetMachine &TM) const {
  // AArch64 does not use section-relative relocations so any global symbol must
  // be accessed via at least a linker-private symbol.
  getMangler().getNameWithPrefix(OutName, GV, /* CannotUsePrivateLabel */ true);
}

template <typename MachineModuleInfoTarget>
static MCSymbol *getAuthPtrSlotSymbolHelper(
    MCContext &Ctx, const TargetMachine &TM, MachineModuleInfo *MMI,
    MachineModuleInfoTarget &TargetMMI, const MCSymbol *RawSym,
    AArch64PACKey::ID Key, uint16_t Discriminator,
    int64_t RawSymOffset = 0, bool HasAddressDiversity = false) {
  const DataLayout &DL = MMI->getModule()->getDataLayout();

  // Mangle the offset into the stub name.  Avoid '-' in symbols and extra logic
  // by using the uint64_t representation for negative numbers.
  uint64_t OffsetV = RawSymOffset;
  std::string Suffix = "$";
  if (OffsetV)
    Suffix += utostr(OffsetV) + "$";
  Suffix += (Twine("auth_ptr$") + AArch64PACKeyIDToString(Key) + "$" +
             utostr(Discriminator))
                .str();

  if (HasAddressDiversity)
    Suffix += "$addr";

  MCSymbol *StubSym = Ctx.getOrCreateSymbol(DL.getLinkerPrivateGlobalPrefix() +
                                            RawSym->getName() + Suffix);

  const MCExpr *&StubAuthPtrRef = TargetMMI.getAuthPtrStubEntry(StubSym);

  if (StubAuthPtrRef)
    return StubSym;

  const MCExpr *Sym = MCSymbolRefExpr::create(RawSym, Ctx);

  // If there is an addend, turn that into the appropriate MCExpr.
  if (RawSymOffset > 0)
    Sym = MCBinaryExpr::createAdd(
        Sym, MCConstantExpr::create(RawSymOffset, Ctx), Ctx);
  else if (RawSymOffset < 0)
    Sym = MCBinaryExpr::createSub(
        Sym, MCConstantExpr::create(-RawSymOffset, Ctx), Ctx);

  StubAuthPtrRef = AArch64AuthMCExpr::create(Sym, Discriminator, Key,
                                             HasAddressDiversity, Ctx);
  return StubSym;
}

MCSymbol *AArch64_ELFTargetObjectFile::getAuthPtrSlotSymbol(
    const TargetMachine &TM, MachineModuleInfo *MMI, const MCSymbol *RawSym,
    AArch64PACKey::ID Key, uint16_t Discriminator) const {
  auto &ELFMMI = MMI->getObjFileInfo<MachineModuleInfoELF>();
  return getAuthPtrSlotSymbolHelper(getContext(), TM, MMI, ELFMMI, RawSym, Key,
                                    Discriminator);
}

MCSymbol *AArch64_MachoTargetObjectFile::getAuthPtrSlotSymbol(
    const TargetMachine &TM, MachineModuleInfo *MMI, const MCSymbol *RawSym,
    AArch64PACKey::ID Key, uint16_t Discriminator,
    int64_t RawSymOffset, bool HasAddressDiversity) const {
  auto &MachOMMI = MMI->getObjFileInfo<MachineModuleInfoMachO>();
  return getAuthPtrSlotSymbolHelper(getContext(), TM, MMI, MachOMMI, RawSym,
                                    Key, Discriminator, RawSymOffset,
                                    HasAddressDiversity);
}
