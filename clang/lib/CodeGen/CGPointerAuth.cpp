//===--- CGPointerAuth.cpp - IR generation for pointer authentication -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains common routines relating to the emission of
// pointer authentication operations.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "CGCall.h"
#include "clang/AST/StableHash.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ConstantInitBuilder.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/SipHash.h"

using namespace clang;
using namespace CodeGen;

// FIXME: Temporarily allow both ConstantPtrAuth and llvm.ptrauth emission.
static llvm::cl::opt<bool> PtrauthEmitWrapperGlobals(
    "ptrauth-emit-wrapper-globals", llvm::cl::init(true), llvm::cl::Hidden,
    llvm::cl::desc("Emit llvm.ptrauth globals rather than ptrauth Constants"));

/// Given a pointer-authentication schema, return a concrete "other"
/// discriminator for it.
llvm::ConstantInt *
CodeGenModule::getPointerAuthOtherDiscriminator(const PointerAuthSchema &Schema,
                                                GlobalDecl Decl,
                                                QualType Type) {
  switch (Schema.getOtherDiscrimination()) {
  case PointerAuthSchema::Discrimination::None:
    return nullptr;

  case PointerAuthSchema::Discrimination::Type:
    assert(!Type.isNull() && "type not provided for type-discriminated schema");
    return llvm::ConstantInt::get(
        IntPtrTy, getContext().getPointerAuthTypeDiscriminator(Type));

  case PointerAuthSchema::Discrimination::Decl:
    assert(Decl.getDecl() &&
           "declaration not provided for decl-discriminated schema");
    return llvm::ConstantInt::get(IntPtrTy,
                                  getPointerAuthDeclDiscriminator(Decl));

  case PointerAuthSchema::Discrimination::Constant:
    return llvm::ConstantInt::get(IntPtrTy, Schema.getConstantDiscrimination());
  }
  llvm_unreachable("bad discrimination kind");
}

uint16_t CodeGen::getPointerAuthTypeDiscriminator(CodeGenModule &CGM,
                                                  QualType FunctionType) {
  return CGM.getContext().getPointerAuthTypeDiscriminator(FunctionType);
}

/// Compute an ABI-stable hash of the given string.
uint64_t CodeGen::computeStableStringHash(StringRef string) {
  return clang::getStableStringHash(string);
}

uint16_t CodeGen::getPointerAuthDeclDiscriminator(CodeGenModule &CGM,
                                                  GlobalDecl Declaration) {
  return CGM.getPointerAuthDeclDiscriminator(Declaration);
}

/// Return the "other" decl-specific discriminator for the given decl.
uint16_t
CodeGenModule::getPointerAuthDeclDiscriminator(GlobalDecl Declaration) {
  uint16_t &EntityHash = PtrAuthDiscriminatorHashes[Declaration];

  if (EntityHash == 0) {
    StringRef Name = getMangledName(Declaration);
    EntityHash = llvm::getPointerAuthStableSipHash(Name);
  }

  return EntityHash;
}

/// Return the abstract pointer authentication schema for a pointer to the given
/// function type.
CGPointerAuthInfo CodeGenModule::getFunctionPointerAuthInfo(QualType T) {
  const auto &Schema = getCodeGenOpts().PointerAuth.FunctionPointers;
  if (!Schema) return CGPointerAuthInfo();

  assert(!Schema.isAddressDiscriminated() &&
         "function pointers cannot use address-specific discrimination");

  llvm::Constant *Discriminator = nullptr;
  if (T->isFunctionPointerType() || T->isFunctionReferenceType())
    T = T->getPointeeType();
  if (T->isFunctionType())
    Discriminator = getPointerAuthOtherDiscriminator(Schema, GlobalDecl(), T);

  return CGPointerAuthInfo(Schema.getKey(), Schema.getAuthenticationMode(),
                           /*IsaPointer=*/false, /*AuthenticatesNull=*/false,
                           Discriminator);
}

CGPointerAuthInfo
CodeGenFunction::EmitPointerAuthInfo(PointerAuthQualifier qualifier,
                                     Address storageAddress) {
  assert(qualifier &&
         "don't call this if you don't know that the qualifier is present");

  llvm::Value *discriminator = nullptr;
  if (unsigned extra = qualifier.getExtraDiscriminator()) {
    discriminator = llvm::ConstantInt::get(IntPtrTy, extra);
  }

  if (qualifier.isAddressDiscriminated()) {
    assert(storageAddress.isValid() &&
           "address discrimination without address");
    auto storagePtr = storageAddress.emitRawPointer(*this);
    if (discriminator) {
      discriminator =
        EmitPointerAuthBlendDiscriminator(storagePtr, discriminator);
    } else {
      discriminator = Builder.CreatePtrToInt(storagePtr, IntPtrTy);
    }
  }

  return CGPointerAuthInfo(qualifier.getKey(),
                           qualifier.getAuthenticationMode(),
                           qualifier.isIsaPointer(),
                           qualifier.authenticatesNullValues(), discriminator);
}

/// Return the natural pointer authentication for values of the given
/// pointee type.
static CGPointerAuthInfo
getPointerAuthInfoForPointeeType(CodeGenModule &CGM, QualType PointeeType) {
  if (PointeeType.isNull())
    return CGPointerAuthInfo();

  // Function pointers use the function-pointer schema by default.
  if (PointeeType->isFunctionType())
    return CGM.getFunctionPointerAuthInfo(PointeeType);

  // Normal data pointers never use direct pointer authentication by default.
  return CGPointerAuthInfo();
}

CGPointerAuthInfo CodeGenModule::getPointerAuthInfoForPointeeType(QualType T) {
  return ::getPointerAuthInfoForPointeeType(*this, T);
}

/// Return the natural pointer authentication for values of the given
/// pointer type.
static CGPointerAuthInfo getPointerAuthInfoForType(CodeGenModule &CGM,
                                                   QualType PointerType) {
  assert(PointerType->isSignableType());

  // Block pointers are currently not signed.
  if (PointerType->isBlockPointerType())
    return CGPointerAuthInfo();

  auto PointeeType = PointerType->getPointeeType();

  if (PointeeType.isNull())
    return CGPointerAuthInfo();

  return ::getPointerAuthInfoForPointeeType(CGM, PointeeType);
}

CGPointerAuthInfo CodeGenModule::getPointerAuthInfoForType(QualType T) {
  return ::getPointerAuthInfoForType(*this, T);
}

static bool isZeroConstant(const llvm::Value *Value) {
  if (const auto *CI = dyn_cast<llvm::ConstantInt>(Value))
    return CI->isZero();
  return false;
}

static bool equalAuthPolicies(const CGPointerAuthInfo &Left,
                              const CGPointerAuthInfo &Right) {
  assert((Left.isSigned() || Right.isSigned()) &&
         "shouldn't be called if neither is signed");
  if (Left.isSigned() != Right.isSigned())
    return false;
  return Left.getKey() == Right.getKey() &&
         Left.getAuthenticationMode() == Right.getAuthenticationMode();
}

// Return the discriminator or return zero if the discriminator is null.
static llvm::Value *getDiscriminatorOrZero(const CGPointerAuthInfo &Info,
                                           CGBuilderTy &Builder) {
  llvm::Value *Discriminator = Info.getDiscriminator();
  return Discriminator ? Discriminator : Builder.getSize(0);
}

llvm::Value *
CodeGenFunction::emitPointerAuthResignCall(llvm::Value *Value,
                                           const CGPointerAuthInfo &CurAuth,
                                           const CGPointerAuthInfo &NewAuth) {
  assert(CurAuth && NewAuth);

  if (CurAuth.getAuthenticationMode() !=
          PointerAuthenticationMode::SignAndAuth ||
      NewAuth.getAuthenticationMode() !=
          PointerAuthenticationMode::SignAndAuth) {
    llvm::Value *AuthedValue = EmitPointerAuthAuth(CurAuth, Value);
    return EmitPointerAuthSign(NewAuth, AuthedValue);
  }
  // Convert the pointer to intptr_t before signing it.
  auto *OrigType = Value->getType();
  Value = Builder.CreatePtrToInt(Value, IntPtrTy);

  auto *CurKey = Builder.getInt32(CurAuth.getKey());
  auto *NewKey = Builder.getInt32(NewAuth.getKey());

  llvm::Value *CurDiscriminator = getDiscriminatorOrZero(CurAuth, Builder);
  llvm::Value *NewDiscriminator = getDiscriminatorOrZero(NewAuth, Builder);

  // call i64 @llvm.ptrauth.resign(i64 %pointer,
  //                               i32 %curKey, i64 %curDiscriminator,
  //                               i32 %newKey, i64 %newDiscriminator)
  auto *Intrinsic = CGM.getIntrinsic(llvm::Intrinsic::ptrauth_resign);
  Value = EmitRuntimeCall(
      Intrinsic, {Value, CurKey, CurDiscriminator, NewKey, NewDiscriminator});

  // Convert back to the original type.
  Value = Builder.CreateIntToPtr(Value, OrigType);
  return Value;
}

llvm::Value *CodeGenFunction::emitPointerAuthResign(
    llvm::Value *Value, QualType Type, const CGPointerAuthInfo &CurAuthInfo,
    const CGPointerAuthInfo &NewAuthInfo, bool IsKnownNonNull) {
  // Fast path: if neither schema wants a signature, we're done.
  if (!CurAuthInfo && !NewAuthInfo)
    return Value;

  llvm::Value *Null = nullptr;
  // If the value is obviously null, we're done.
  if (auto *PointerValue = dyn_cast<llvm::PointerType>(Value->getType())) {
    Null = CGM.getNullPointer(PointerValue, Type);
  } else {
    assert(Value->getType()->isIntegerTy());
    Null = llvm::ConstantInt::get(IntPtrTy, 0);
  }
  if (Value == Null)
    return Value;

  // If both schemas sign the same way, we're done.
  if (equalAuthPolicies(CurAuthInfo, NewAuthInfo)) {
    const llvm::Value *CurD = CurAuthInfo.getDiscriminator();
    const llvm::Value *NewD = NewAuthInfo.getDiscriminator();
    if (CurD == NewD)
      return Value;

    if ((CurD == nullptr && isZeroConstant(NewD)) ||
        (NewD == nullptr && isZeroConstant(CurD)))
      return Value;
  }

  llvm::BasicBlock *InitBB = Builder.GetInsertBlock();
  llvm::BasicBlock *ResignBB = nullptr, *ContBB = nullptr;

  // Null pointers have to be mapped to null, and the ptrauth_resign
  // intrinsic doesn't do that.
  if (!IsKnownNonNull && !llvm::isKnownNonZero(Value, CGM.getDataLayout())) {
    ContBB = createBasicBlock("resign.cont");
    ResignBB = createBasicBlock("resign.nonnull");

    auto *IsNonNull = Builder.CreateICmpNE(Value, Null);
    Builder.CreateCondBr(IsNonNull, ResignBB, ContBB);
    EmitBlock(ResignBB);
  }

  // Perform the auth/sign/resign operation.
  if (!NewAuthInfo)
    Value = EmitPointerAuthAuth(CurAuthInfo, Value);
  else if (!CurAuthInfo)
    Value = EmitPointerAuthSign(NewAuthInfo, Value);
  else
    Value = emitPointerAuthResignCall(Value, CurAuthInfo, NewAuthInfo);

  // Clean up with a phi if we branched before.
  if (ContBB) {
    EmitBlock(ContBB);
    auto *Phi = Builder.CreatePHI(Value->getType(), 2);
    Phi->addIncoming(Null, InitBB);
    Phi->addIncoming(Value, ResignBB);
    Value = Phi;
  }

  return Value;
}

static std::pair<llvm::Value *, CGPointerAuthInfo>
emitLoadOfOrigPointerRValue(CodeGenFunction &CGF, const LValue &lv,
                            SourceLocation loc) {
  auto value = CGF.EmitLoadOfScalar(lv, loc);
  CGPointerAuthInfo authInfo;
  if (auto ptrauth = lv.getQuals().getPointerAuth()) {
    authInfo = CGF.EmitPointerAuthInfo(ptrauth, lv.getAddress());
  } else {
    authInfo = getPointerAuthInfoForType(CGF.CGM, lv.getType());
  }
  return { value, authInfo };
}

std::pair<llvm::Value *, CGPointerAuthInfo>
CodeGenFunction::EmitOrigPointerRValue(const Expr *E) {
  assert(E->getType()->isPointerType());

  E = E->IgnoreParens();
  if (auto load = dyn_cast<ImplicitCastExpr>(E)) {
    if (load->getCastKind() == CK_LValueToRValue) {
      E = load->getSubExpr()->IgnoreParens();

      // We're semantically required to not emit loads of certain DREs naively.
      if (auto refExpr = dyn_cast<DeclRefExpr>(const_cast<Expr*>(E))) {
        if (auto result = tryEmitAsConstant(refExpr)) {
          // Fold away a use of an intermediate variable.
          if (!result.isReference())
            return { result.getValue(),
                      getPointerAuthInfoForType(CGM, refExpr->getType()) };

          // Fold away a use of an intermediate reference.
          auto lv = result.getReferenceLValue(*this, refExpr);
          return emitLoadOfOrigPointerRValue(*this, lv, refExpr->getLocation());
        }
      }

      // Otherwise, load and use the pointer
      auto lv = EmitCheckedLValue(E, CodeGenFunction::TCK_Load);
      return emitLoadOfOrigPointerRValue(*this, lv, E->getExprLoc());
    }
  }

  // Emit direct references to functions without authentication.
  if (auto DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
      return { CGM.getRawFunctionPointer(FD), CGPointerAuthInfo() };
    }
  } else if (auto ME = dyn_cast<MemberExpr>(E)) {
    if (auto FD = dyn_cast<FunctionDecl>(ME->getMemberDecl())) {
      EmitIgnoredExpr(ME->getBase());
      return { CGM.getRawFunctionPointer(FD), CGPointerAuthInfo() };
    }
  }

  // Fallback: just use the normal rules for the type.
  auto value = EmitScalarExpr(E);
  return { value, getPointerAuthInfoForType(CGM, E->getType()) };
}

llvm::Value *
CodeGenFunction::EmitPointerAuthQualify(PointerAuthQualifier destQualifier,
                                        const Expr *E,
                                        Address destStorageAddress) {
  assert(destQualifier);

  auto src = EmitOrigPointerRValue(E);
  auto value = src.first;
  auto curAuthInfo = src.second;

  auto destAuthInfo = EmitPointerAuthInfo(destQualifier, destStorageAddress);
  return emitPointerAuthResign(value, E->getType(), curAuthInfo, destAuthInfo,
                               isPointerKnownNonNull(E));
}

llvm::Value *
CodeGenFunction::EmitPointerAuthQualify(PointerAuthQualifier destQualifier,
                                        llvm::Value *value,
                                        QualType pointerType,
                                        Address destStorageAddress,
                                        bool isKnownNonNull) {
  assert(destQualifier);

  auto curAuthInfo = getPointerAuthInfoForType(CGM, pointerType);
  auto destAuthInfo = EmitPointerAuthInfo(destQualifier, destStorageAddress);
  return emitPointerAuthResign(value, pointerType, curAuthInfo, destAuthInfo,
                               isKnownNonNull);
}

llvm::Value *
CodeGenFunction::EmitPointerAuthUnqualify(PointerAuthQualifier curQualifier,
                                          llvm::Value *value,
                                          QualType pointerType,
                                          Address curStorageAddress,
                                          bool isKnownNonNull) {
  assert(curQualifier);

  auto curAuthInfo = EmitPointerAuthInfo(curQualifier, curStorageAddress);
  auto destAuthInfo = getPointerAuthInfoForType(CGM, pointerType);
  return emitPointerAuthResign(value, pointerType, curAuthInfo, destAuthInfo,
                               isKnownNonNull);
}

void CodeGenFunction::EmitPointerAuthCopy(PointerAuthQualifier qualifier,
                                          QualType type,
                                          Address destAddress,
                                          Address srcAddress) {
  assert(qualifier);

  llvm::Value *value = Builder.CreateLoad(srcAddress);

  // If we're using address-discrimination, we have to re-sign the value.
  if (qualifier.isAddressDiscriminated()) {
    auto srcPtrAuth = EmitPointerAuthInfo(qualifier, srcAddress);
    auto destPtrAuth = EmitPointerAuthInfo(qualifier, destAddress);
    value = emitPointerAuthResign(value, type, srcPtrAuth, destPtrAuth,
                                  /*is known nonnull*/ false);
  }

  Builder.CreateStore(value, destAddress);
}

/// We use an abstract, side-allocated cache for signed function pointers
/// because (1) most compiler invocations will not need this cache at all,
/// since they don't use signed function pointers, and (2) the
/// representation is pretty complicated (an llvm::ValueMap) and we don't
/// want to have to include that information in CodeGenModule.h.
template <class CacheTy>
static CacheTy &getOrCreateCache(void *&abstractStorage) {
  auto cache = static_cast<CacheTy*>(abstractStorage);
  if (cache) return *cache;

  abstractStorage = cache = new CacheTy();
  return *cache;
}

template <class CacheTy>
static void destroyCache(void *&abstractStorage) {
  delete static_cast<CacheTy*>(abstractStorage);
  abstractStorage = nullptr;
}

namespace {
struct PointerAuthConstantEntry {
  unsigned Key;
  llvm::Constant *OtherDiscriminator;
  llvm::GlobalVariable *Global;
};

using PointerAuthConstantEntries =
  std::vector<PointerAuthConstantEntry>;
using ByConstantCacheTy =
  llvm::ValueMap<llvm::Constant*, PointerAuthConstantEntries>;
using ByDeclCacheTy =
  llvm::DenseMap<const Decl *, llvm::Constant*>;
}

/// Build a global signed-pointer constant.
static llvm::GlobalVariable *
buildConstantSignedPointer(CodeGenModule &CGM,
                           llvm::Constant *pointer,
                           unsigned key,
                           llvm::Constant *storageAddress,
                           llvm::ConstantInt *otherDiscriminator) {
  ConstantInitBuilder builder(CGM);
  auto values = builder.beginStruct();
  values.add(pointer);
  values.addInt(CGM.Int32Ty, key);
  if (storageAddress) {
    if (isa<llvm::ConstantInt>(storageAddress)) {
      assert(!storageAddress->isNullValue() &&
             "expecting pointer or special address-discriminator indicator");
      values.add(storageAddress);
    } else {
      values.add(llvm::ConstantExpr::getPtrToInt(storageAddress, CGM.IntPtrTy));
    }
  } else {
    values.addInt(CGM.SizeTy, 0);
  }
  if (otherDiscriminator) {
    assert(otherDiscriminator->getType() == CGM.SizeTy);
    values.add(otherDiscriminator);
  } else {
    values.addInt(CGM.SizeTy, 0);
  }

  auto *stripped = pointer->stripPointerCasts();
  StringRef name;
  if (const auto *origGlobal = dyn_cast<llvm::GlobalValue>(stripped))
    name = origGlobal->getName();
  else if (const auto *ce = dyn_cast<llvm::ConstantExpr>(stripped))
    if (ce->getOpcode() == llvm::Instruction::GetElementPtr)
      name = cast<llvm::GEPOperator>(ce)->getPointerOperand()->getName();

  auto global = values.finishAndCreateGlobal(
      name + ".ptrauth",
      CGM.getPointerAlign(),
      /*constant*/ true,
      llvm::GlobalVariable::PrivateLinkage);
  global->setSection("llvm.ptrauth");

  return global;
}

llvm::Value *
CodeGenFunction::EmitPointerAuthBlendDiscriminator(llvm::Value *StorageAddress,
                                                   llvm::Value *Discriminator) {
  StorageAddress = Builder.CreatePtrToInt(StorageAddress, IntPtrTy);
  auto Intrinsic = CGM.getIntrinsic(llvm::Intrinsic::ptrauth_blend);
  return Builder.CreateCall(Intrinsic, {StorageAddress, Discriminator});
}

/// Emit the concrete pointer authentication informaton for the
/// given authentication schema.
CGPointerAuthInfo CodeGenFunction::EmitPointerAuthInfo(
    const PointerAuthSchema &Schema, llvm::Value *StorageAddress,
    GlobalDecl SchemaDecl, QualType SchemaType) {
  if (!Schema)
    return CGPointerAuthInfo();

  llvm::Value *Discriminator =
      CGM.getPointerAuthOtherDiscriminator(Schema, SchemaDecl, SchemaType);

  if (Schema.isAddressDiscriminated()) {
    assert(StorageAddress &&
           "address not provided for address-discriminated schema");

    if (Discriminator)
      Discriminator =
          EmitPointerAuthBlendDiscriminator(StorageAddress, Discriminator);
    else
      Discriminator = Builder.CreatePtrToInt(StorageAddress, IntPtrTy);
  }

  return CGPointerAuthInfo(Schema.getKey(), Schema.getAuthenticationMode(),
                           Schema.isIsaPointer(),
                           Schema.authenticatesNullValues(), Discriminator);
}

llvm::Constant *
CodeGenModule::getConstantSignedPointer(llvm::Constant *Pointer, unsigned Key,
                                        llvm::Constant *StorageAddress,
                                        llvm::ConstantInt *OtherDiscriminator) {
  if (PtrauthEmitWrapperGlobals) {
    // Unique based on the underlying value, not a signing of it.
    auto stripped = Pointer->stripPointerCasts();

    PointerAuthConstantEntries *entries = nullptr;

    // We can cache this for discriminators that aren't defined in terms
    // of globals.  Discriminators defined in terms of globals (1) would
    // require additional tracking to be safe and (2) only come up with
    // address-specific discrimination, where this entry is almost certainly
    // unique to the use-site anyway.
    if (!StorageAddress &&
        (!OtherDiscriminator ||
         isa<llvm::ConstantInt>(OtherDiscriminator))) {

      // Get or create the cache.
      auto &cache =
        getOrCreateCache<ByConstantCacheTy>(ConstantSignedPointersByConstant);

      // Check for an existing entry.
      entries = &cache[stripped];
      for (auto &entry : *entries) {
        if (entry.Key == Key && entry.OtherDiscriminator == OtherDiscriminator) {
          auto global = entry.Global;
          return llvm::ConstantExpr::getBitCast(global, Pointer->getType());
        }
      }
    }

    // Build the constant.
    auto global =
      buildConstantSignedPointer(*this, stripped, Key, StorageAddress,
                                 OtherDiscriminator);

    // Cache if applicable.
    if (entries) {
      entries->push_back({ Key, OtherDiscriminator, global });
    }

    // Cast to the original type.
    return llvm::ConstantExpr::getBitCast(global, Pointer->getType());
  }

  llvm::Constant *AddressDiscriminator;
  if (StorageAddress) {
    assert(StorageAddress->getType() == UnqualPtrTy);
    AddressDiscriminator = StorageAddress;
  } else {
    AddressDiscriminator = llvm::Constant::getNullValue(UnqualPtrTy);
  }

  llvm::ConstantInt *IntegerDiscriminator;
  if (OtherDiscriminator) {
    assert(OtherDiscriminator->getType() == Int64Ty);
    IntegerDiscriminator = OtherDiscriminator;
  } else {
    IntegerDiscriminator = llvm::ConstantInt::get(Int64Ty, 0);
  }

  return llvm::ConstantPtrAuth::get(Pointer,
                                    llvm::ConstantInt::get(Int32Ty, Key),
                                    IntegerDiscriminator, AddressDiscriminator);
}

// Does a given PointerAuthScheme require us to sign a value
bool CodeGenModule::shouldSignPointer(const PointerAuthSchema &Schema) {
  auto AuthenticationMode = Schema.getAuthenticationMode();
  return AuthenticationMode == PointerAuthenticationMode::SignAndStrip ||
         AuthenticationMode == PointerAuthenticationMode::SignAndAuth;
}

/// Sign a constant pointer using the given scheme, producing a constant
/// with the same IR type.
llvm::Constant *CodeGenModule::getConstantSignedPointer(
    llvm::Constant *Pointer, const PointerAuthSchema &Schema,
    llvm::Constant *StorageAddress, GlobalDecl SchemaDecl,
    QualType SchemaType) {
  assert(shouldSignPointer(Schema));
  llvm::ConstantInt *OtherDiscriminator =
      getPointerAuthOtherDiscriminator(Schema, SchemaDecl, SchemaType);

  return getConstantSignedPointer(Pointer, Schema.getKey(), StorageAddress,
                                  OtherDiscriminator);
}

llvm::Constant *
CodeGen::getConstantSignedPointer(CodeGenModule &CGM,
                                  llvm::Constant *pointer, unsigned key,
                                  llvm::Constant *storageAddress,
                                  llvm::ConstantInt *otherDiscriminator) {
  return CGM.getConstantSignedPointer(pointer, key, storageAddress,
                                      otherDiscriminator);
}

void CodeGenModule::destroyConstantSignedPointerCaches() {
  destroyCache<ByConstantCacheTy>(ConstantSignedPointersByConstant);
  destroyCache<ByDeclCacheTy>(ConstantSignedPointersByDecl);
  destroyCache<ByDeclCacheTy>(SignedThunkPointers);
}

/// If applicable, sign a given constant function pointer with the ABI rules for
/// functionType.
llvm::Constant *CodeGenModule::getFunctionPointer(llvm::Constant *Pointer,
                                                  QualType FunctionType,
                                                  GlobalDecl GD) {
  assert(FunctionType->isFunctionType() ||
         FunctionType->isFunctionReferenceType() ||
         FunctionType->isFunctionPointerType());

  if (auto pointerAuth = getFunctionPointerAuthInfo(FunctionType)) {
    // Check a cache that, for now, just has entries for functions signed
    // with the standard function-pointer scheme.
    // Cache function pointers based on their decl.  Anything without a decl is
    // going to be a one-off that doesn't need to be cached anyway.
    llvm::Constant **entry = nullptr;
    if (GD) {
      auto FD = cast<FunctionDecl>(GD.getDecl());
      auto &cache =
          getOrCreateCache<ByDeclCacheTy>(ConstantSignedPointersByDecl);
      entry = &cache[FD->getCanonicalDecl()];
      if (*entry)
        return llvm::ConstantExpr::getBitCast(*entry, Pointer->getType());
    }

    // If the cache misses, build a new constant.  It's not a *problem* to
    // have more than one of these for a particular function, but it's nice
    // to avoid it.
    Pointer = getConstantSignedPointer(
        Pointer, pointerAuth.getKey(), nullptr,
        cast_or_null<llvm::ConstantInt>(pointerAuth.getDiscriminator()));

    // Store the result back into the cache, if any.
    if (entry)
      *entry = Pointer;
  }

  return Pointer;
}

llvm::Constant *CodeGenModule::getFunctionPointer(GlobalDecl GD,
                                                  llvm::Type *Ty) {
  const auto *FD = cast<FunctionDecl>(GD.getDecl());
  QualType FuncType = FD->getType();

  // Annoyingly, K&R functions have prototypes in the clang AST, but
  // expressions referring to them are unprototyped.
  if (!FD->hasPrototype())
    if (const auto *Proto = FuncType->getAs<FunctionProtoType>())
      FuncType = Context.getFunctionNoProtoType(Proto->getReturnType(),
                                                Proto->getExtInfo());

  return getFunctionPointer(getRawFunctionPointer(GD, Ty), FuncType);
}

CGPointerAuthInfo CodeGenModule::getMemberFunctionPointerAuthInfo(QualType FT) {
  assert(FT->getAs<MemberPointerType>() && "MemberPointerType expected");
  const auto &Schema = getCodeGenOpts().PointerAuth.CXXMemberFunctionPointers;
  if (!Schema)
    return CGPointerAuthInfo();

  assert(!Schema.isAddressDiscriminated() &&
         "function pointers cannot use address-specific discrimination");

  llvm::ConstantInt *Discriminator =
      getPointerAuthOtherDiscriminator(Schema, GlobalDecl(), FT);
  return CGPointerAuthInfo(Schema.getKey(), Schema.getAuthenticationMode(),
                           /* IsIsaPointer */ false,
                           /* AuthenticatesNullValues */ false, Discriminator);
}

llvm::Constant *CodeGenModule::getMemberFunctionPointer(llvm::Constant *Pointer,
                                                        QualType FT,
                                                        const FunctionDecl *FD) {
  if (CGPointerAuthInfo PointerAuth = getMemberFunctionPointerAuthInfo(FT)) {
    llvm::Constant **Entry = nullptr;
    if (FD) {
      auto &Cache =
          getOrCreateCache<ByDeclCacheTy>(SignedThunkPointers);
      Entry = &Cache[FD->getCanonicalDecl()];
      if (*Entry)
        return llvm::ConstantExpr::getBitCast(*Entry, Pointer->getType());
    }

    Pointer = getConstantSignedPointer(
        Pointer, PointerAuth.getKey(), nullptr,
        cast_or_null<llvm::ConstantInt>(PointerAuth.getDiscriminator()));

    if (Entry)
      *Entry = Pointer;
  }

  return Pointer;
}

llvm::Constant *CodeGenModule::getMemberFunctionPointer(const FunctionDecl *FD,
                                                        llvm::Type *Ty) {
  QualType FT = FD->getType();
  FT = getContext().getMemberPointerType(
      FT, cast<CXXMethodDecl>(FD)->getParent()->getTypeForDecl());
  return getMemberFunctionPointer(getRawFunctionPointer(FD, Ty), FT, FD);
}

std::optional<PointerAuthQualifier>
CodeGenModule::computeVTPointerAuthentication(const CXXRecordDecl *ThisClass) {
  auto DefaultAuthentication = getCodeGenOpts().PointerAuth.CXXVTablePointers;
  if (!DefaultAuthentication)
    return std::nullopt;
  const CXXRecordDecl *PrimaryBase =
      Context.baseForVTableAuthentication(ThisClass);

  unsigned Key = DefaultAuthentication.getKey();
  bool AddressDiscriminated = DefaultAuthentication.isAddressDiscriminated();
  auto DefaultDiscrimination = DefaultAuthentication.getOtherDiscrimination();
  unsigned TypeBasedDiscriminator =
      Context.getPointerAuthVTablePointerDiscriminator(PrimaryBase);
  unsigned Discriminator;
  if (DefaultDiscrimination == PointerAuthSchema::Discrimination::Type) {
    Discriminator = TypeBasedDiscriminator;
  } else if (DefaultDiscrimination ==
             PointerAuthSchema::Discrimination::Constant) {
    Discriminator = DefaultAuthentication.getConstantDiscrimination();
  } else {
    assert(DefaultDiscrimination == PointerAuthSchema::Discrimination::None);
    Discriminator = 0;
  }
  if (auto ExplicitAuthentication =
          PrimaryBase->getAttr<VTablePointerAuthenticationAttr>()) {
    auto ExplicitAddressDiscrimination =
        ExplicitAuthentication->getAddressDiscrimination();
    auto ExplicitDiscriminator =
        ExplicitAuthentication->getExtraDiscrimination();

    unsigned ExplicitKey = ExplicitAuthentication->getKey();
    if (ExplicitKey == VTablePointerAuthenticationAttr::NoKey)
      return std::nullopt;

    if (ExplicitKey != VTablePointerAuthenticationAttr::DefaultKey) {
      if (ExplicitKey == VTablePointerAuthenticationAttr::ProcessIndependent)
        Key = (unsigned)PointerAuthSchema::ARM8_3Key::ASDA;
      else {
        assert(ExplicitKey ==
               VTablePointerAuthenticationAttr::ProcessDependent);
        Key = (unsigned)PointerAuthSchema::ARM8_3Key::ASDB;
      }
    }

    if (ExplicitAddressDiscrimination !=
        VTablePointerAuthenticationAttr::DefaultAddressDiscrimination)
      AddressDiscriminated =
          ExplicitAddressDiscrimination ==
          VTablePointerAuthenticationAttr::AddressDiscrimination;

    if (ExplicitDiscriminator ==
        VTablePointerAuthenticationAttr::TypeDiscrimination)
      Discriminator = TypeBasedDiscriminator;
    else if (ExplicitDiscriminator ==
             VTablePointerAuthenticationAttr::CustomDiscrimination)
      Discriminator = ExplicitAuthentication->getCustomDiscriminationValue();
    else if (ExplicitDiscriminator ==
             VTablePointerAuthenticationAttr::NoExtraDiscrimination)
      Discriminator = 0;
  }
  return PointerAuthQualifier::Create(Key, AddressDiscriminated, Discriminator,
                                      PointerAuthenticationMode::SignAndAuth,
                                      /* IsIsaPointer */ false,
                                      /* AuthenticatesNullValues */ false);
}

std::optional<PointerAuthQualifier>
CodeGenModule::getVTablePointerAuthentication(const CXXRecordDecl *Record) {
  if (!Record->getDefinition() || !Record->isPolymorphic())
    return std::nullopt;

  auto Existing = VTablePtrAuthInfos.find(Record);
  std::optional<PointerAuthQualifier> Authentication;
  if (Existing != VTablePtrAuthInfos.end()) {
    Authentication = Existing->getSecond();
  } else {
    Authentication = computeVTPointerAuthentication(Record);
    VTablePtrAuthInfos.insert(std::make_pair(Record, Authentication));
  }
  return Authentication;
}

std::optional<CGPointerAuthInfo>
CodeGenModule::getVTablePointerAuthInfo(CodeGenFunction *CGF,
                                        const CXXRecordDecl *Record,
                                        llvm::Value *StorageAddress) {
  auto Authentication = getVTablePointerAuthentication(Record);
  if (!Authentication)
    return std::nullopt;

  llvm::Value *Discriminator = nullptr;
  if (auto ExtraDiscriminator = Authentication->getExtraDiscriminator())
    Discriminator = llvm::ConstantInt::get(IntPtrTy, ExtraDiscriminator);

  if (Authentication->isAddressDiscriminated()) {
    assert(StorageAddress &&
           "address not provided for address-discriminated schema");
    if (Discriminator)
      Discriminator =
          CGF->EmitPointerAuthBlendDiscriminator(StorageAddress, Discriminator);
    else
      Discriminator = CGF->Builder.CreatePtrToInt(StorageAddress, IntPtrTy);
  }

  return CGPointerAuthInfo(Authentication->getKey(),
                           PointerAuthenticationMode::SignAndAuth,
                           /* IsIsaPointer */ false,
                           /* AuthenticatesNullValues */ false, Discriminator);
}

llvm::Value *CodeGenFunction::authPointerToPointerCast(llvm::Value *ResultPtr,
                                                       QualType SourceType,
                                                       QualType DestType) {
  CGPointerAuthInfo CurAuthInfo, NewAuthInfo;
  if (SourceType->isSignableType())
    CurAuthInfo = getPointerAuthInfoForType(CGM, SourceType);

  if (DestType->isSignableType())
    NewAuthInfo = getPointerAuthInfoForType(CGM, DestType);

  if (!CurAuthInfo && !NewAuthInfo)
    return ResultPtr;

  // If only one side of the cast is a function pointer, then we still need to
  // resign to handle casts to/from opaque pointers.
  if (!CurAuthInfo && DestType->isFunctionPointerType())
    CurAuthInfo = CGM.getFunctionPointerAuthInfo(SourceType);

  if (!NewAuthInfo && SourceType->isFunctionPointerType())
    NewAuthInfo = CGM.getFunctionPointerAuthInfo(DestType);

  return emitPointerAuthResign(ResultPtr, DestType, CurAuthInfo, NewAuthInfo,
                               /*IsKnownNonNull=*/false);
}

Address CodeGenFunction::authPointerToPointerCast(Address Ptr,
                                                  QualType SourceType,
                                                  QualType DestType) {
  CGPointerAuthInfo CurAuthInfo, NewAuthInfo;
  if (SourceType->isSignableType())
    CurAuthInfo = getPointerAuthInfoForType(CGM, SourceType);

  if (DestType->isSignableType())
    NewAuthInfo = getPointerAuthInfoForType(CGM, DestType);

  if (!CurAuthInfo && !NewAuthInfo)
    return Ptr;

  if (!CurAuthInfo && DestType->isFunctionPointerType()) {
    // When casting a non-signed pointer to a function pointer, just set the
    // auth info on Ptr to the assumed schema. The pointer will be resigned to
    // the effective type when used.
    Ptr.setPointerAuthInfo(CGM.getFunctionPointerAuthInfo(SourceType));
    return Ptr;
  }

  if (!NewAuthInfo && SourceType->isFunctionPointerType()) {
    NewAuthInfo = CGM.getFunctionPointerAuthInfo(DestType);
    Ptr = Ptr.getResignedAddress(NewAuthInfo, *this);
    Ptr.setPointerAuthInfo(CGPointerAuthInfo());
    return Ptr;
  }

  return Ptr;
}

Address CodeGenFunction::getAsNaturalAddressOf(Address Addr,
                                               QualType PointeeTy) {
  CGPointerAuthInfo Info =
      PointeeTy.isNull() ? CGPointerAuthInfo()
                         : CGM.getPointerAuthInfoForPointeeType(PointeeTy);
  return Addr.getResignedAddress(Info, *this);
}

Address Address::getResignedAddress(const CGPointerAuthInfo &NewInfo,
                                    CodeGenFunction &CGF) const {
  assert(isValid() && "pointer isn't valid");
  CGPointerAuthInfo CurInfo = getPointerAuthInfo();
  llvm::Value *Val;

  // Nothing to do if neither the current or the new ptrauth info needs signing.
  if (!CurInfo.isSigned() && !NewInfo.isSigned())
    return Address(getBasePointer(), getElementType(), getAlignment(),
                   isKnownNonNull());

  assert(ElementType && "Effective type has to be set");
  assert(!Offset && "unexpected non-null offset");

  // If the current and the new ptrauth infos are the same and the offset is
  // null, just cast the base pointer to the effective type.
  if (CurInfo == NewInfo && !hasOffset())
    Val = getBasePointer();
  else
    Val = CGF.emitPointerAuthResign(getBasePointer(), QualType(), CurInfo,
                                    NewInfo, isKnownNonNull());

  Val = CGF.Builder.CreateBitCast(Val, getType());
  return Address(Val, getElementType(), getAlignment(), NewInfo,
                 /*Offset=*/nullptr, isKnownNonNull());
}

llvm::Value *Address::emitRawPointerSlow(CodeGenFunction &CGF) const {
  return CGF.getAsNaturalPointerTo(*this, QualType());
}

llvm::Value *LValue::getPointer(CodeGenFunction &CGF) const {
  assert(isSimple());
  return emitResignedPointer(getType(), CGF);
}

llvm::Value *LValue::emitResignedPointer(QualType PointeeTy,
                                         CodeGenFunction &CGF) const {
  assert(isSimple());
  return CGF.getAsNaturalAddressOf(Addr, PointeeTy).getBasePointer();
}

llvm::Value *LValue::emitRawPointer(CodeGenFunction &CGF) const {
  assert(isSimple());
  return Addr.isValid() ? Addr.emitRawPointer(CGF) : nullptr;
}
