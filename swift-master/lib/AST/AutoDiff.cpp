//===--- AutoDiff.cpp - Swift automatic differentiation utilities ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/AutoDiff.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Module.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"

using namespace swift;

AutoDiffDerivativeFunctionKind::AutoDiffDerivativeFunctionKind(
    StringRef string) {
  Optional<innerty> result = llvm::StringSwitch<Optional<innerty>>(string)
                                 .Case("jvp", JVP)
                                 .Case("vjp", VJP);
  assert(result && "Invalid string");
  rawValue = *result;
}

NormalDifferentiableFunctionTypeComponent::
    NormalDifferentiableFunctionTypeComponent(
        AutoDiffDerivativeFunctionKind kind) {
  switch (kind) {
  case AutoDiffDerivativeFunctionKind::JVP:
    rawValue = JVP;
    return;
  case AutoDiffDerivativeFunctionKind::VJP:
    rawValue = VJP;
    return;
  }
}

NormalDifferentiableFunctionTypeComponent::
    NormalDifferentiableFunctionTypeComponent(StringRef string) {
  Optional<innerty> result = llvm::StringSwitch<Optional<innerty>>(string)
                                 .Case("original", Original)
                                 .Case("jvp", JVP)
                                 .Case("vjp", VJP);
  assert(result && "Invalid string");
  rawValue = *result;
}

Optional<AutoDiffDerivativeFunctionKind>
NormalDifferentiableFunctionTypeComponent::getAsDerivativeFunctionKind() const {
  switch (rawValue) {
  case Original:
    return None;
  case JVP:
    return {AutoDiffDerivativeFunctionKind::JVP};
  case VJP:
    return {AutoDiffDerivativeFunctionKind::VJP};
  }
  llvm_unreachable("invalid derivative kind");
}

LinearDifferentiableFunctionTypeComponent::
    LinearDifferentiableFunctionTypeComponent(StringRef string) {
  Optional<innerty> result = llvm::StringSwitch<Optional<innerty>>(string)
                                 .Case("original", Original)
                                 .Case("transpose", Transpose);
  assert(result && "Invalid string");
  rawValue = *result;
}

DifferentiabilityWitnessFunctionKind::DifferentiabilityWitnessFunctionKind(
    StringRef string) {
  Optional<innerty> result = llvm::StringSwitch<Optional<innerty>>(string)
                                 .Case("jvp", JVP)
                                 .Case("vjp", VJP)
                                 .Case("transpose", Transpose);
  assert(result && "Invalid string");
  rawValue = *result;
}

Optional<AutoDiffDerivativeFunctionKind>
DifferentiabilityWitnessFunctionKind::getAsDerivativeFunctionKind() const {
  switch (rawValue) {
  case JVP:
    return {AutoDiffDerivativeFunctionKind::JVP};
  case VJP:
    return {AutoDiffDerivativeFunctionKind::VJP};
  case Transpose:
    return None;
  }
  llvm_unreachable("invalid derivative kind");
}

void SILAutoDiffIndices::print(llvm::raw_ostream &s) const {
  s << "(parameters=(";
  interleave(
      parameters->getIndices(), [&s](unsigned p) { s << p; },
      [&s] { s << ' '; });
  s << ") results=(";
  interleave(
      results->getIndices(), [&s](unsigned p) { s << p; }, [&s] { s << ' '; });
  s << "))";
}

void SILAutoDiffIndices::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

SILAutoDiffIndices AutoDiffConfig::getSILAutoDiffIndices() const {
  return SILAutoDiffIndices(parameterIndices, resultIndices);
}

void AutoDiffConfig::print(llvm::raw_ostream &s) const {
  s << "(parameters=";
  parameterIndices->print(s);
  s << " results=";
  resultIndices->print(s);
  if (derivativeGenericSignature) {
    s << " where=";
    derivativeGenericSignature->print(s);
  }
  s << ')';
}

bool swift::isDifferentiableProgrammingEnabled(SourceFile &SF) {
  auto &ctx = SF.getASTContext();
  // Return true if differentiable programming is explicitly enabled.
  if (ctx.LangOpts.EnableExperimentalDifferentiableProgramming)
    return true;
  // Otherwise, return true iff the `_Differentiation` module is imported in
  // the given source file.
  bool importsDifferentiationModule = false;
  for (auto import : namelookup::getAllImports(&SF)) {
    if (import.importedModule->getName() == ctx.Id_Differentiation) {
      importsDifferentiationModule = true;
      break;
    }
  }
  return importsDifferentiationModule;
}

// TODO(TF-874): This helper is inefficient and should be removed. Unwrapping at
// most once (for curried method types) is sufficient.
static void unwrapCurryLevels(AnyFunctionType *fnTy,
                              SmallVectorImpl<AnyFunctionType *> &results) {
  while (fnTy != nullptr) {
    results.push_back(fnTy);
    fnTy = fnTy->getResult()->getAs<AnyFunctionType>();
  }
}

static unsigned countNumFlattenedElementTypes(Type type) {
  if (auto *tupleTy = type->getCanonicalType()->getAs<TupleType>())
    return accumulate(tupleTy->getElementTypes(), 0,
                      [&](unsigned num, Type type) {
                        return num + countNumFlattenedElementTypes(type);
                      });
  return 1;
}

// TODO(TF-874): Simplify this helper and remove the `reverseCurryLevels` flag.
void AnyFunctionType::getSubsetParameters(
    IndexSubset *parameterIndices,
    SmallVectorImpl<AnyFunctionType::Param> &results, bool reverseCurryLevels) {
  SmallVector<AnyFunctionType *, 2> curryLevels;
  unwrapCurryLevels(this, curryLevels);

  SmallVector<unsigned, 2> curryLevelParameterIndexOffsets(curryLevels.size());
  unsigned currentOffset = 0;
  for (unsigned curryLevelIndex : llvm::reverse(indices(curryLevels))) {
    curryLevelParameterIndexOffsets[curryLevelIndex] = currentOffset;
    currentOffset += curryLevels[curryLevelIndex]->getNumParams();
  }

  // If `reverseCurryLevels` is true, reverse the curry levels and offsets.
  if (reverseCurryLevels) {
    std::reverse(curryLevels.begin(), curryLevels.end());
    std::reverse(curryLevelParameterIndexOffsets.begin(),
                 curryLevelParameterIndexOffsets.end());
  }

  for (unsigned curryLevelIndex : indices(curryLevels)) {
    auto *curryLevel = curryLevels[curryLevelIndex];
    unsigned parameterIndexOffset =
        curryLevelParameterIndexOffsets[curryLevelIndex];
    for (unsigned paramIndex : range(curryLevel->getNumParams()))
      if (parameterIndices->contains(parameterIndexOffset + paramIndex))
        results.push_back(curryLevel->getParams()[paramIndex]);
  }
}

void autodiff::getFunctionSemanticResultTypes(
    AnyFunctionType *functionType,
    SmallVectorImpl<AutoDiffSemanticFunctionResultType> &result,
    GenericEnvironment *genericEnv) {
  auto &ctx = functionType->getASTContext();

  // Remap type in `genericEnv`, if specified.
  auto remap = [&](Type type) {
    if (!genericEnv)
      return type;
    return genericEnv->mapTypeIntoContext(type);
  };

  // Collect formal result type as a semantic result, unless it is
  // `Void`.
  auto formalResultType = functionType->getResult();
  if (auto *resultFunctionType =
          functionType->getResult()->getAs<AnyFunctionType>()) {
    formalResultType = resultFunctionType->getResult();
  }
  if (!formalResultType->isEqual(ctx.TheEmptyTupleType))
    result.push_back({remap(formalResultType), /*isInout*/ false});

  // Collect `inout` parameters as semantic results.
  for (auto param : functionType->getParams())
    if (param.isInOut())
      result.push_back({remap(param.getPlainType()), /*isInout*/ true});
  if (auto *resultFunctionType =
          functionType->getResult()->getAs<AnyFunctionType>()) {
    for (auto param : resultFunctionType->getParams())
      if (param.isInOut())
        result.push_back({remap(param.getPlainType()), /*isInout*/ true});
  }
}

// TODO(TF-874): Simplify this helper. See TF-874 for WIP.
IndexSubset *
autodiff::getLoweredParameterIndices(IndexSubset *parameterIndices,
                                     AnyFunctionType *functionType) {
  SmallVector<AnyFunctionType *, 2> curryLevels;
  unwrapCurryLevels(functionType, curryLevels);

  // Compute the lowered sizes of all AST parameter types.
  SmallVector<unsigned, 8> paramLoweredSizes;
  unsigned totalLoweredSize = 0;
  auto addLoweredParamInfo = [&](Type type) {
    unsigned paramLoweredSize = countNumFlattenedElementTypes(type);
    paramLoweredSizes.push_back(paramLoweredSize);
    totalLoweredSize += paramLoweredSize;
  };
  for (auto *curryLevel : llvm::reverse(curryLevels))
    for (auto &param : curryLevel->getParams())
      addLoweredParamInfo(param.getPlainType());

  // Build lowered SIL parameter indices by setting the range of bits that
  // corresponds to each "set" AST parameter.
  llvm::SmallVector<unsigned, 8> loweredSILIndices;
  unsigned currentBitIndex = 0;
  for (unsigned i : range(parameterIndices->getCapacity())) {
    auto paramLoweredSize = paramLoweredSizes[i];
    if (parameterIndices->contains(i)) {
      auto indices = range(currentBitIndex, currentBitIndex + paramLoweredSize);
      loweredSILIndices.append(indices.begin(), indices.end());
    }
    currentBitIndex += paramLoweredSize;
  }

  return IndexSubset::get(functionType->getASTContext(), totalLoweredSize,
                          loweredSILIndices);
}

GenericSignature autodiff::getConstrainedDerivativeGenericSignature(
    SILFunctionType *originalFnTy, IndexSubset *diffParamIndices,
    GenericSignature derivativeGenSig, LookupConformanceFn lookupConformance,
    bool isTranspose) {
  if (!derivativeGenSig)
    derivativeGenSig = originalFnTy->getInvocationGenericSignature();
  if (!derivativeGenSig)
    return nullptr;
  auto &ctx = originalFnTy->getASTContext();
  auto *diffableProto = ctx.getProtocol(KnownProtocolKind::Differentiable);
  SmallVector<Requirement, 4> requirements;
  for (unsigned paramIdx : diffParamIndices->getIndices()) {
    // Require differentiability parameters to conform to `Differentiable`.
    auto paramType = originalFnTy->getParameters()[paramIdx].getInterfaceType();
    Requirement req(RequirementKind::Conformance, paramType,
                    diffableProto->getDeclaredType());
    requirements.push_back(req);
    if (isTranspose) {
      // Require linearity parameters to additionally satisfy
      // `Self == Self.TangentVector`.
      auto tanSpace = paramType->getAutoDiffTangentSpace(lookupConformance);
      auto paramTanType = tanSpace->getCanonicalType();
      Requirement req(RequirementKind::SameType, paramType, paramTanType);
      requirements.push_back(req);
    }
  }
  return evaluateOrDefault(
      ctx.evaluator,
      AbstractGenericSignatureRequest{derivativeGenSig.getPointer(),
                                      /*addedGenericParams*/ {},
                                      std::move(requirements)},
      nullptr);
}

// Given the rest of a `Builtin.applyDerivative_{jvp|vjp}` or
// `Builtin.applyTranspose` operation name, attempts to parse the arity and
// throwing-ness from the operation name. Modifies the operation name argument
// in place as substrings get dropped.
static void parseAutoDiffBuiltinCommonConfig(
    StringRef &operationName, unsigned &arity, bool &throws) {
  // Parse '_arity'.
  constexpr char arityPrefix[] = "_arity";
  if (operationName.startswith(arityPrefix)) {
    operationName = operationName.drop_front(sizeof(arityPrefix) - 1);
    auto arityStr = operationName.take_while(llvm::isDigit);
    operationName = operationName.drop_front(arityStr.size());
    auto converted = llvm::to_integer(arityStr, arity);
    assert(converted); (void)converted;
    assert(arity > 0);
  } else {
    arity = 1;
  }
  // Parse '_throws'.
  constexpr char throwsPrefix[] = "_throws";
  if (operationName.startswith(throwsPrefix)) {
    operationName = operationName.drop_front(sizeof(throwsPrefix) - 1);
    throws = true;
  } else {
    throws = false;
  }
}

bool autodiff::getBuiltinApplyDerivativeConfig(
    StringRef operationName, AutoDiffDerivativeFunctionKind &kind,
    unsigned &arity, bool &throws) {
  constexpr char prefix[] = "applyDerivative";
  if (!operationName.startswith(prefix))
    return false;
  operationName = operationName.drop_front(sizeof(prefix) - 1);
  // Parse 'jvp' or 'vjp'.
  constexpr char jvpPrefix[] = "_jvp";
  constexpr char vjpPrefix[] = "_vjp";
  if (operationName.startswith(jvpPrefix))
    kind = AutoDiffDerivativeFunctionKind::JVP;
  else if (operationName.startswith(vjpPrefix))
    kind = AutoDiffDerivativeFunctionKind::VJP;
  operationName = operationName.drop_front(sizeof(jvpPrefix) - 1);
  parseAutoDiffBuiltinCommonConfig(operationName, arity, throws);
  return operationName.empty();
}

bool autodiff::getBuiltinApplyTransposeConfig(
    StringRef operationName, unsigned &arity, bool &throws) {
  constexpr char prefix[] = "applyTranspose";
  if (!operationName.startswith(prefix))
    return false;
  operationName = operationName.drop_front(sizeof(prefix) - 1);
  parseAutoDiffBuiltinCommonConfig(operationName, arity, throws);
  return operationName.empty();
}

bool autodiff::getBuiltinDifferentiableOrLinearFunctionConfig(
    StringRef operationName, unsigned &arity, bool &throws) {
  constexpr char differentiablePrefix[] = "differentiableFunction";
  constexpr char linearPrefix[] = "linearFunction";
  if (operationName.startswith(differentiablePrefix))
    operationName = operationName.drop_front(sizeof(differentiablePrefix) - 1);
  else if (operationName.startswith(linearPrefix))
    operationName = operationName.drop_front(sizeof(linearPrefix) - 1);
  else
    return false;
  parseAutoDiffBuiltinCommonConfig(operationName, arity, throws);
  return operationName.empty();
}

Type TangentSpace::getType() const {
  switch (kind) {
  case Kind::TangentVector:
    return value.tangentVectorType;
  case Kind::Tuple:
    return value.tupleType;
  }
  llvm_unreachable("invalid tangent space kind");
}

CanType TangentSpace::getCanonicalType() const {
  return getType()->getCanonicalType();
}

NominalTypeDecl *TangentSpace::getNominal() const {
  assert(isTangentVector());
  return getTangentVector()->getNominalOrBoundGenericNominal();
}

const char DerivativeFunctionTypeError::ID = '\0';

void DerivativeFunctionTypeError::log(raw_ostream &OS) const {
  OS << "original function type '";
  functionType->print(OS);
  OS << "' ";
  switch (kind) {
  case Kind::NoSemanticResults:
    OS << "has no semantic results ('Void' result)";
    break;
  case Kind::MultipleSemanticResults:
    OS << "has multiple semantic results";
    break;
  case Kind::NoDifferentiabilityParameters:
    OS << "has no differentiability parameters";
    break;
  case Kind::NonDifferentiableDifferentiabilityParameter: {
    auto nonDiffParam = getNonDifferentiableTypeAndIndex();
    OS << "has non-differentiable differentiability parameter "
       << nonDiffParam.second << ": " << nonDiffParam.first;
    break;
  }
  case Kind::NonDifferentiableResult: {
    auto nonDiffResult = getNonDifferentiableTypeAndIndex();
    OS << "has non-differentiable result " << nonDiffResult.second << ": "
       << nonDiffResult.first;
    break;
  }
  }
}

bool swift::operator==(const TangentPropertyInfo::Error &lhs,
                       const TangentPropertyInfo::Error &rhs) {
  if (lhs.kind != rhs.kind)
    return false;
  switch (lhs.kind) {
  case TangentPropertyInfo::Error::Kind::NoDerivativeOriginalProperty:
  case TangentPropertyInfo::Error::Kind::NominalParentNotDifferentiable:
  case TangentPropertyInfo::Error::Kind::OriginalPropertyNotDifferentiable:
  case TangentPropertyInfo::Error::Kind::ParentTangentVectorNotStruct:
  case TangentPropertyInfo::Error::Kind::TangentPropertyNotFound:
  case TangentPropertyInfo::Error::Kind::TangentPropertyNotStored:
    return true;
  case TangentPropertyInfo::Error::Kind::TangentPropertyWrongType:
    return lhs.getType()->isEqual(rhs.getType());
  }
}

void swift::simple_display(llvm::raw_ostream &os, TangentPropertyInfo info) {
  os << "{ ";
  os << "tangent property: "
     << (info.tangentProperty ? info.tangentProperty->printRef() : "null");
  if (info.error) {
    os << ", error: ";
    switch (info.error->kind) {
    case TangentPropertyInfo::Error::Kind::NoDerivativeOriginalProperty:
      os << "'@noDerivative' original property has no tangent property";
      break;
    case TangentPropertyInfo::Error::Kind::NominalParentNotDifferentiable:
      os << "nominal parent does not conform to 'Differentiable'";
      break;
    case TangentPropertyInfo::Error::Kind::OriginalPropertyNotDifferentiable:
      os << "original property type does not conform to 'Differentiable'";
      break;
    case TangentPropertyInfo::Error::Kind::ParentTangentVectorNotStruct:
      os << "'TangentVector' type is not a struct";
      break;
    case TangentPropertyInfo::Error::Kind::TangentPropertyNotFound:
      os << "'TangentVector' struct does not have stored property with the "
            "same name as the original property";
      break;
    case TangentPropertyInfo::Error::Kind::TangentPropertyWrongType:
      os << "tangent property's type is not equal to the original property's "
            "'TangentVector' type";
      break;
    case TangentPropertyInfo::Error::Kind::TangentPropertyNotStored:
      os << "'TangentVector' property '" << info.tangentProperty->getName()
         << "' is not a stored property";
      break;
    }
  }
  os << " }";
}

TangentPropertyInfo
TangentStoredPropertyRequest::evaluate(Evaluator &evaluator,
                                       VarDecl *originalField) const {
  assert(originalField->hasStorage() && originalField->isInstanceMember() &&
         "Expected stored property");
  auto *parentDC = originalField->getDeclContext();
  assert(parentDC->isTypeContext());
  auto parentType = parentDC->getDeclaredTypeInContext();
  auto *moduleDecl = originalField->getModuleContext();
  auto parentTan = parentType->getAutoDiffTangentSpace(
      LookUpConformanceInModule(moduleDecl));
  // Error if parent nominal type does not conform to `Differentiable`.
  if (!parentTan) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::NominalParentNotDifferentiable);
  }
  // Error if original stored property is `@noDerivative`.
  if (originalField->getAttrs().hasAttribute<NoDerivativeAttr>()) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::NoDerivativeOriginalProperty);
  }
  // Error if original property's type does not conform to `Differentiable`.
  auto originalFieldTan = originalField->getType()->getAutoDiffTangentSpace(
      LookUpConformanceInModule(moduleDecl));
  if (!originalFieldTan) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::OriginalPropertyNotDifferentiable);
  }
  auto parentTanType = parentTan->getType();
  auto *parentTanStruct = parentTanType->getStructOrBoundGenericStruct();
  // Error if parent `TangentVector` is not a struct.
  if (!parentTanStruct) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::ParentTangentVectorNotStruct);
  }
  // Find the corresponding field in the tangent space.
  VarDecl *tanField = nullptr;
  // If `TangentVector` is the original struct, then the tangent property is the
  // original property.
  if (parentTanStruct == parentDC->getSelfStructDecl()) {
    tanField = originalField;
  }
  // Otherwise, look up the field by name.
  else {
    auto tanFieldLookup =
        parentTanStruct->lookupDirect(originalField->getName());
    llvm::erase_if(tanFieldLookup,
                   [](ValueDecl *v) { return !isa<VarDecl>(v); });
    // Error if tangent property could not be found.
    if (tanFieldLookup.empty()) {
      return TangentPropertyInfo(
          TangentPropertyInfo::Error::Kind::TangentPropertyNotFound);
    }
    tanField = cast<VarDecl>(tanFieldLookup.front());
  }
  // Error if tangent property's type is not equal to the original property's
  // `TangentVector` type.
  auto originalFieldTanType = originalFieldTan->getType();
  if (!originalFieldTanType->isEqual(tanField->getType())) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::TangentPropertyWrongType,
        originalFieldTanType);
  }
  // Error if tangent property is not a stored property.
  if (!tanField->hasStorage()) {
    return TangentPropertyInfo(
        TangentPropertyInfo::Error::Kind::TangentPropertyNotStored);
  }
  // Otherwise, tangent property is valid.
  return TangentPropertyInfo(tanField);
}