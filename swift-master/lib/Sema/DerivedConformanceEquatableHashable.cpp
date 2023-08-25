//===--- DerivedConformanceEquatableHashable.cpp - Derived Equatable & co -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements implicit derivation of the Equatable and Hashable
//  protocols. 
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "DerivedConformances.h"

using namespace swift;

enum NonconformingMemberKind {
  AssociatedValue,
  StoredProperty
};

/// Returns the VarDecl of each stored property in the given struct whose type
/// does not conform to a protocol.
/// \p theStruct The struct whose stored properties should be checked.
/// \p protocol The protocol being requested.
/// \return The VarDecl of each stored property whose type does not conform.
static SmallVector<VarDecl *, 3>
storedPropertiesNotConformingToProtocol(DeclContext *DC, StructDecl *theStruct,
                                        ProtocolDecl *protocol) {
  auto storedProperties = theStruct->getStoredProperties();
  SmallVector<VarDecl *, 3> nonconformingProperties;
  for (auto propertyDecl : storedProperties) {
    if (!propertyDecl->isUserAccessible())
      continue;

    auto type = propertyDecl->getValueInterfaceType();
    if (!type)
      nonconformingProperties.push_back(propertyDecl);

    if (!TypeChecker::conformsToProtocol(DC->mapTypeIntoContext(type), protocol,
                                         DC)) {
      nonconformingProperties.push_back(propertyDecl);
    }
  }
  return nonconformingProperties;
}

/// Returns true if every stored property in the given struct conforms to the
/// protocol (or, vacuously, if it has no stored properties).
/// \p theStruct The struct whose stored properties should be checked.
/// \p protocol The protocol being requested.
/// \return True if all stored properties of the struct conform.
static bool allStoredPropertiesConformToProtocol(DeclContext *DC,
                                                 StructDecl *theStruct,
                                                 ProtocolDecl *protocol) {
  return storedPropertiesNotConformingToProtocol(DC, theStruct, protocol)
      .empty();
}

/// Common preconditions for Equatable and Hashable.
static bool canDeriveConformance(DeclContext *DC,
                                 NominalTypeDecl *target,
                                 ProtocolDecl *protocol) {
  // The type must be an enum or a struct.
  if (auto enumDecl = dyn_cast<EnumDecl>(target)) {
    // The cases must not have associated values, or all associated values must
    // conform to the protocol.
    return DerivedConformance::allAssociatedValuesConformToProtocol(DC, enumDecl, protocol);
  }

  if (auto structDecl = dyn_cast<StructDecl>(target)) {
    // All stored properties of the struct must conform to the protocol.
    return allStoredPropertiesConformToProtocol(DC, structDecl, protocol);
  }

  return false;
}

/// Diagnose failed conformance synthesis caused by a member type not conforming
/// to the same protocol
void diagnoseFailedDerivation(DeclContext *DC, NominalTypeDecl *nominal,
                              ProtocolDecl *protocol) {
  ASTContext &ctx = DC->getASTContext();

  if (auto *enumDecl = dyn_cast<EnumDecl>(nominal)) {
    auto nonconformingAssociatedTypes =
        DerivedConformance::associatedValuesNotConformingToProtocol(DC, enumDecl, protocol);
    for (auto *typeToDiagnose : nonconformingAssociatedTypes) {
      SourceLoc reprLoc;
      if (auto *repr = typeToDiagnose->getTypeRepr())
        reprLoc = repr->getStartLoc();
      ctx.Diags.diagnose(
          reprLoc,
          diag::missing_member_type_conformance_prevents_synthesis,
          NonconformingMemberKind::AssociatedValue,
          typeToDiagnose->getInterfaceType(), protocol->getDeclaredType(),
          nominal->getDeclaredInterfaceType());
    }
  }

  if (auto *structDecl = dyn_cast<StructDecl>(nominal)) {
    auto nonconformingStoredProperties =
        storedPropertiesNotConformingToProtocol(DC, structDecl, protocol);
    for (auto *propertyToDiagnose : nonconformingStoredProperties) {
      ctx.Diags.diagnose(
          propertyToDiagnose->getLoc(),
          diag::missing_member_type_conformance_prevents_synthesis,
          NonconformingMemberKind::StoredProperty,
          propertyToDiagnose->getInterfaceType(), protocol->getDeclaredType(),
          nominal->getDeclaredInterfaceType());
    }
  }

  if (auto *classDecl = dyn_cast<ClassDecl>(nominal)) {
    ctx.Diags.diagnose(classDecl->getLoc(),
                       diag::classes_automatic_protocol_synthesis,
                       protocol->getName().str());
  }
}

static std::pair<BraceStmt *, bool>
deriveBodyEquatable_enum_uninhabited_eq(AbstractFunctionDecl *eqDecl, void *) {
  auto parentDC = eqDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = eqDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  assert(!cast<EnumDecl>(aParam->getType()->getAnyNominal())->hasCases());

  SmallVector<ASTNode, 1> statements;
  SmallVector<ASTNode, 0> cases;

  // switch (a, b) { }
  auto aRef = new (C) DeclRefExpr(aParam, DeclNameLoc(), /*implicit*/ true,
                                  AccessSemantics::Ordinary,
                                  aParam->getType());
  auto bRef = new (C) DeclRefExpr(bParam, DeclNameLoc(), /*implicit*/ true,
                                  AccessSemantics::Ordinary,
                                  bParam->getType());
  TupleTypeElt abTupleElts[2] = { aParam->getType(), bParam->getType() };
  auto abExpr = TupleExpr::create(C, SourceLoc(), {aRef, bRef}, {}, {},
                                  SourceLoc(), /*HasTrailingClosure*/ false,
                                  /*implicit*/ true,
                                  TupleType::get(abTupleElts, C));
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), abExpr,
                                       SourceLoc(), cases, SourceLoc(), C);
  statements.push_back(switchStmt);

  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/true };
}

/// Derive the body for an '==' operator for an enum that has no associated
/// values. This generates code that converts each value to its integer ordinal
/// and compares them, which produces an optimal single icmp instruction.
static std::pair<BraceStmt *, bool>
deriveBodyEquatable_enum_noAssociatedValues_eq(AbstractFunctionDecl *eqDecl,
                                               void *) {
  auto parentDC = eqDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = eqDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());

  // Generate the conversion from the enums to integer indices.
  SmallVector<ASTNode, 6> statements;
  DeclRefExpr *aIndex = DerivedConformance::convertEnumToIndex(statements, parentDC, enumDecl,
                                           aParam, eqDecl, "index_a");
  DeclRefExpr *bIndex = DerivedConformance::convertEnumToIndex(statements, parentDC, enumDecl,
                                           bParam, eqDecl, "index_b");

  // Generate the compare of the indices.
  FuncDecl *cmpFunc = C.getEqualIntDecl();
  assert(cmpFunc && "should have a == for int as we already checked for it");

  auto fnType = cmpFunc->getInterfaceType()->castTo<FunctionType>();

  Expr *cmpFuncExpr;
  if (cmpFunc->getDeclContext()->isTypeContext()) {
    auto contextTy = cmpFunc->getDeclContext()->getSelfInterfaceType();
    Expr *base = TypeExpr::createImplicitHack(SourceLoc(), contextTy, C);
    Expr *ref = new (C) DeclRefExpr(cmpFunc, DeclNameLoc(), /*Implicit*/ true,
                                    AccessSemantics::Ordinary, fnType);

    fnType = fnType->getResult()->castTo<FunctionType>();
    cmpFuncExpr = new (C) DotSyntaxCallExpr(ref, SourceLoc(), base, fnType);
    cmpFuncExpr->setImplicit();
  } else {
    cmpFuncExpr = new (C) DeclRefExpr(cmpFunc, DeclNameLoc(),
                                      /*implicit*/ true,
                                      AccessSemantics::Ordinary,
                                      fnType);
  }

  TupleTypeElt abTupleElts[2] = { aIndex->getType(), bIndex->getType() };
  TupleExpr *abTuple = TupleExpr::create(C, SourceLoc(), { aIndex, bIndex },
                                         { }, { }, SourceLoc(),
                                         /*HasTrailingClosure*/ false,
                                         /*Implicit*/ true,
                                         TupleType::get(abTupleElts, C));

  auto *cmpExpr = new (C) BinaryExpr(
      cmpFuncExpr, abTuple, /*implicit*/ true,
      fnType->castTo<FunctionType>()->getResult());
  statements.push_back(new (C) ReturnStmt(SourceLoc(), cmpExpr));

  BraceStmt *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/true };
}

/// Derive the body for an '==' operator for an enum where at least one of the
/// cases has associated values.
static std::pair<BraceStmt *, bool>
deriveBodyEquatable_enum_hasAssociatedValues_eq(AbstractFunctionDecl *eqDecl,
                                                void *) {
  auto parentDC = eqDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = eqDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  Type enumType = aParam->getType();
  auto enumDecl = cast<EnumDecl>(aParam->getType()->getAnyNominal());

  SmallVector<ASTNode, 6> statements;
  SmallVector<ASTNode, 4> cases;
  unsigned elementCount = 0;

  // For each enum element, generate a case statement matching a pair containing
  // the same case, binding variables for the left- and right-hand associated
  // values.
  for (auto elt : enumDecl->getAllElements()) {
    ++elementCount;

    // .<elt>(let l0, let l1, ...)
    SmallVector<VarDecl*, 3> lhsPayloadVars;
    auto lhsSubpattern = DerivedConformance::enumElementPayloadSubpattern(elt, 'l', eqDecl,
                                                      lhsPayloadVars);
    auto *lhsBaseTE = TypeExpr::createImplicit(enumType, C);
    auto lhsElemPat =
        new (C) EnumElementPattern(lhsBaseTE, SourceLoc(), DeclNameLoc(),
                                   DeclNameRef(), elt, lhsSubpattern);
    lhsElemPat->setImplicit();

    // .<elt>(let r0, let r1, ...)
    SmallVector<VarDecl*, 3> rhsPayloadVars;
    auto rhsSubpattern = DerivedConformance::enumElementPayloadSubpattern(elt, 'r', eqDecl,
                                                      rhsPayloadVars);
    auto *rhsBaseTE = TypeExpr::createImplicit(enumType, C);
    auto rhsElemPat =
        new (C) EnumElementPattern(rhsBaseTE, SourceLoc(), DeclNameLoc(),
                                   DeclNameRef(), elt, rhsSubpattern);
    rhsElemPat->setImplicit();

    auto hasBoundDecls = !lhsPayloadVars.empty();
    Optional<MutableArrayRef<VarDecl *>> caseBodyVarDecls;
    if (hasBoundDecls) {
      // We allocated a direct copy of our lhs var decls for the case
      // body.
      auto copy = C.Allocate<VarDecl *>(lhsPayloadVars.size());
      for (unsigned i : indices(lhsPayloadVars)) {
        auto *vOld = lhsPayloadVars[i];
        auto *vNew = new (C) VarDecl(
            /*IsStatic*/ false, vOld->getIntroducer(), false /*IsCaptureList*/,
            vOld->getNameLoc(), vOld->getName(), vOld->getDeclContext());
        vNew->setHasNonPatternBindingInit();
        vNew->setImplicit();
        copy[i] = vNew;
      }
      caseBodyVarDecls.emplace(copy);
    }

    // case (.<elt>(let l0, let l1, ...), .<elt>(let r0, let r1, ...))
    auto caseTuplePattern = TuplePattern::createImplicit(C, {
      TuplePatternElt(lhsElemPat), TuplePatternElt(rhsElemPat) });
    caseTuplePattern->setImplicit();

    auto labelItem = CaseLabelItem(caseTuplePattern);

    // Generate a guard statement for each associated value in the payload,
    // breaking out early if any pair is unequal. (This is done to avoid
    // constructing long lists of autoclosure-wrapped conditions connected by
    // &&, which the type checker has more difficulty processing.)
    SmallVector<ASTNode, 6> statementsInCase;
    for (size_t varIdx = 0; varIdx < lhsPayloadVars.size(); ++varIdx) {
      auto lhsVar = lhsPayloadVars[varIdx];
      auto lhsExpr = new (C) DeclRefExpr(lhsVar, DeclNameLoc(),
                                         /*implicit*/true);
      auto rhsVar = rhsPayloadVars[varIdx];
      auto rhsExpr = new (C) DeclRefExpr(rhsVar, DeclNameLoc(),
                                         /*Implicit*/true);
      auto guardStmt = DerivedConformance::returnFalseIfNotEqualGuard(C, 
          lhsExpr, rhsExpr);
      statementsInCase.emplace_back(guardStmt);
    }

    // If none of the guard statements caused an early exit, then all the pairs
    // were true.
    // return true
    auto trueExpr = new (C) BooleanLiteralExpr(true, SourceLoc(),
                                               /*Implicit*/true);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), trueExpr);
    statementsInCase.push_back(returnStmt);

    auto body = BraceStmt::create(C, SourceLoc(), statementsInCase,
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     caseBodyVarDecls));
  }

  // default: result = false
  //
  // We only generate this if the enum has more than one case. If it has exactly
  // one case, then that single case statement is already exhaustive.
  if (elementCount > 1) {
    auto defaultPattern = AnyPattern::createImplicit(C);
    auto defaultItem = CaseLabelItem::getDefault(defaultPattern);
    auto falseExpr = new (C) BooleanLiteralExpr(false, SourceLoc(),
                                                /*implicit*/ true);
    auto returnStmt = new (C) ReturnStmt(SourceLoc(), falseExpr);
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(returnStmt),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     defaultItem, SourceLoc(), SourceLoc(),
                                     body,
                                     /*case body var decls*/ None));
  }

  // switch (a, b) { <case statements> }
  auto aRef = new (C) DeclRefExpr(aParam, DeclNameLoc(), /*implicit*/true);
  auto bRef = new (C) DeclRefExpr(bParam, DeclNameLoc(), /*implicit*/true);
  auto abExpr = TupleExpr::create(C, SourceLoc(), { aRef, bRef }, {}, {},
                                  SourceLoc(), /*HasTrailingClosure*/ false,
                                  /*implicit*/ true);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), abExpr,
                                       SourceLoc(), cases, SourceLoc(), C);
  statements.push_back(switchStmt);

  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for an '==' operator for a struct.
static std::pair<BraceStmt *, bool>
deriveBodyEquatable_struct_eq(AbstractFunctionDecl *eqDecl, void *) {
  auto parentDC = eqDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto args = eqDecl->getParameters();
  auto aParam = args->get(0);
  auto bParam = args->get(1);

  auto structDecl = cast<StructDecl>(aParam->getType()->getAnyNominal());

  SmallVector<ASTNode, 6> statements;

  auto storedProperties = structDecl->getStoredProperties();

  // For each stored property element, generate a guard statement that returns
  // false if a property is not pairwise-equal.
  for (auto propertyDecl : storedProperties) {
    if (!propertyDecl->isUserAccessible())
      continue;

    auto aPropertyRef = new (C) DeclRefExpr(propertyDecl, DeclNameLoc(),
                                            /*implicit*/ true);
    auto aParamRef = new (C) DeclRefExpr(aParam, DeclNameLoc(),
                                         /*implicit*/ true);
    auto aPropertyExpr = new (C) DotSyntaxCallExpr(aPropertyRef, SourceLoc(),
                                                   aParamRef);

    auto bPropertyRef = new (C) DeclRefExpr(propertyDecl, DeclNameLoc(),
                                            /*implicit*/ true);
    auto bParamRef = new (C) DeclRefExpr(bParam, DeclNameLoc(),
                                         /*implicit*/ true);
    auto bPropertyExpr = new (C) DotSyntaxCallExpr(bPropertyRef, SourceLoc(),
                                                   bParamRef);

    auto guardStmt = DerivedConformance::returnFalseIfNotEqualGuard(C,
      aPropertyExpr, bPropertyExpr);
    statements.emplace_back(guardStmt);
  }

  // If none of the guard statements caused an early exit, then all the pairs
  // were true.
  // return true
  auto trueExpr = new (C) BooleanLiteralExpr(true, SourceLoc(),
                                             /*Implicit*/true);
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), trueExpr);
  statements.push_back(returnStmt);

  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Derive an '==' operator implementation for an enum or a struct.
static ValueDecl *
deriveEquatable_eq(
    DerivedConformance &derived,
    std::pair<BraceStmt *, bool> (*bodySynthesizer)(AbstractFunctionDecl *,
                                                    void *)) {
  // enum SomeEnum<T...> {
  //   case A, B(Int), C(String, Int)
  //
  //   @derived
  //   @_implements(Equatable, ==(_:_:))
  //   func __derived_enum_equals(a: SomeEnum<T...>,
  //                              b: SomeEnum<T...>) -> Bool {
  //     switch (a, b) {
  //     case (.A, .A):
  //       return true
  //     case (.B(let l0), .B(let r0)):
  //       guard l0 == r0 else { return false }
  //       return true
  //     case (.C(let l0, let l1), .C(let r0, let r1)):
  //       guard l0 == r0 else { return false }
  //       guard l1 == r1 else { return false }
  //       return true
  //     default: return false
  //   }
  // }
  //
  // struct SomeStruct<T...> {
  //   var x: Int
  //   var y: String
  //
  //   @derived
  //   @_implements(Equatable, ==(_:_:))
  //   func __derived_struct_equals(a: SomeStruct<T...>,
  //                                b: SomeStruct<T...>) -> Bool {
  //     guard a.x == b.x else { return false; }
  //     guard a.y == b.y else { return false; }
  //     return true;
  //   }
  // }

  ASTContext &C = derived.Context;

  auto parentDC = derived.getConformanceContext();
  auto selfIfaceTy = parentDC->getDeclaredInterfaceType();

  auto getParamDecl = [&](StringRef s) -> ParamDecl * {
    auto *param = new (C) ParamDecl(SourceLoc(),
                                    SourceLoc(), Identifier(), SourceLoc(),
                                    C.getIdentifier(s), parentDC);
    param->setSpecifier(ParamSpecifier::Default);
    param->setInterfaceType(selfIfaceTy);
    return param;
  };

  ParameterList *params = ParameterList::create(C, {
    getParamDecl("a"),
    getParamDecl("b")
  });

  auto boolTy = C.getBoolDecl()->getDeclaredType();

  Identifier generatedIdentifier;
  if (parentDC->getParentModule()->isResilient()) {
    generatedIdentifier = C.Id_EqualsOperator;
  } else if (selfIfaceTy->getEnumOrBoundGenericEnum()) {
    generatedIdentifier = C.Id_derived_enum_equals;
  } else {
    assert(selfIfaceTy->getStructOrBoundGenericStruct());
    generatedIdentifier = C.Id_derived_struct_equals;
  }

  DeclName name(C, generatedIdentifier, params);
  auto eqDecl =
    FuncDecl::create(C, /*StaticLoc=*/SourceLoc(),
                     StaticSpellingKind::KeywordStatic,
                     /*FuncLoc=*/SourceLoc(), name, /*NameLoc=*/SourceLoc(),
                     /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
                     /*GenericParams=*/nullptr,
                     params,
                     TypeLoc::withoutLoc(boolTy),
                     parentDC);
  eqDecl->setImplicit();
  eqDecl->setUserAccessible(false);

  // Add the @_implements(Equatable, ==(_:_:)) attribute
  if (generatedIdentifier != C.Id_EqualsOperator) {
    auto equatableProto = C.getProtocol(KnownProtocolKind::Equatable);
    auto equatableTy = equatableProto->getDeclaredType();
    auto equatableTyExpr = TypeExpr::createImplicit(equatableTy, C);
    SmallVector<Identifier, 2> argumentLabels = { Identifier(), Identifier() };
    auto equalsDeclName = DeclName(C, DeclBaseName(C.Id_EqualsOperator),
                                   argumentLabels);
    eqDecl->getAttrs().add(new (C) ImplementsAttr(SourceLoc(),
                                                  SourceRange(),
                                                  equatableTyExpr,
                                                  equalsDeclName,
                                                  DeclNameLoc()));
  }

  if (!C.getEqualIntDecl()) {
    derived.ConformanceDecl->diagnose(diag::no_equal_overload_for_int);
    return nullptr;
  }

  eqDecl->setBodySynthesizer(bodySynthesizer);

  eqDecl->copyFormalAccessFrom(derived.Nominal, /*sourceIsParentContext*/ true);

  // Add the operator to the parent scope.
  derived.addMembersToConformanceContext({eqDecl});

  return eqDecl;
}

bool DerivedConformance::canDeriveEquatable(DeclContext *DC,
                                            NominalTypeDecl *type) {
  ASTContext &ctx = DC->getASTContext();
  auto equatableProto = ctx.getProtocol(KnownProtocolKind::Equatable);
  if (!equatableProto) return false;
  return canDeriveConformance(DC, type, equatableProto);
}

ValueDecl *DerivedConformance::deriveEquatable(ValueDecl *requirement) {
  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // Build the necessary decl.
  if (requirement->getBaseName() == "==") {
    if (auto ed = dyn_cast<EnumDecl>(Nominal)) {
      auto bodySynthesizer =
          !ed->hasCases()
              ? &deriveBodyEquatable_enum_uninhabited_eq
              : ed->hasOnlyCasesWithoutAssociatedValues()
                    ? &deriveBodyEquatable_enum_noAssociatedValues_eq
                    : &deriveBodyEquatable_enum_hasAssociatedValues_eq;
      return deriveEquatable_eq(*this, bodySynthesizer);
    } else if (isa<StructDecl>(Nominal))
      return deriveEquatable_eq(*this, &deriveBodyEquatable_struct_eq);
    else
      llvm_unreachable("todo");
  }
  requirement->diagnose(diag::broken_equatable_requirement);
  return nullptr;
}

void DerivedConformance::tryDiagnoseFailedEquatableDerivation(
    DeclContext *DC, NominalTypeDecl *nominal) {
  ASTContext &ctx = DC->getASTContext();
  auto *equatableProto = ctx.getProtocol(KnownProtocolKind::Equatable);
  diagnoseFailedDerivation(DC, nominal, equatableProto);
}

/// Returns a new \c CallExpr representing
///
///   hasher.combine(hashable)
///
/// \param C The AST context to create the expression in.
///
/// \param hasher The parameter decl to make the call on.
///
/// \param hashable The parameter to the call.
static CallExpr *createHasherCombineCall(ASTContext &C,
                                         ParamDecl *hasher,
                                         Expr *hashable) {
  Expr *hasherExpr = new (C) DeclRefExpr(ConcreteDeclRef(hasher),
                                         DeclNameLoc(), /*implicit*/ true);
  // hasher.combine(_:)
  auto *combineCall = UnresolvedDotExpr::createImplicit(
      C, hasherExpr, C.Id_combine, {Identifier()});
  
  // hasher.combine(hashable)
  return CallExpr::createImplicit(C, combineCall, {hashable}, {Identifier()});
}

static FuncDecl *
deriveHashable_hashInto(
    DerivedConformance &derived,
    std::pair<BraceStmt *, bool> (*bodySynthesizer)(AbstractFunctionDecl *,
                                                    void *)) {
  // @derived func hash(into hasher: inout Hasher)

  ASTContext &C = derived.Context;
  auto parentDC = derived.getConformanceContext();

  // Expected type: (Self) -> (into: inout Hasher) -> ()
  // Constructed as:
  //   func type(input: Self,
  //             output: func type(input: inout Hasher,
  //                               output: ()))
  // Created from the inside out:

  auto hasherDecl = C.getHasherDecl();
  if (!hasherDecl) {
    auto hashableProto = C.getProtocol(KnownProtocolKind::Hashable);
    hashableProto->diagnose(diag::broken_hashable_no_hasher);
    return nullptr;
  }
  Type hasherType = hasherDecl->getDeclaredType();

  // Params: self (implicit), hasher
  auto *hasherParamDecl = new (C) ParamDecl(SourceLoc(),
                                            SourceLoc(), C.Id_into, SourceLoc(),
                                            C.Id_hasher, parentDC);
  hasherParamDecl->setSpecifier(ParamSpecifier::InOut);
  hasherParamDecl->setInterfaceType(hasherType);

  ParameterList *params = ParameterList::createWithoutLoc(hasherParamDecl);

  // Return type: ()
  auto returnType = TupleType::getEmpty(C);

  // Func name: hash(into: inout Hasher) -> ()
  DeclName name(C, C.Id_hash, params);
  auto *hashDecl = FuncDecl::create(C,
                                    SourceLoc(), StaticSpellingKind::None,
                                    SourceLoc(), name, SourceLoc(),
                                    /*Throws=*/false, SourceLoc(),
                                    nullptr, params,
                                    TypeLoc::withoutLoc(returnType),
                                    parentDC);
  hashDecl->setImplicit();
  hashDecl->setBodySynthesizer(bodySynthesizer);

  hashDecl->copyFormalAccessFrom(derived.Nominal);

  derived.addMembersToConformanceContext({hashDecl});

  return hashDecl;
}

/// Derive the body for the hash(into:) method when hashValue has a
/// user-supplied implementation.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_compat_hashInto(AbstractFunctionDecl *hashIntoDecl, void *) {
  // func hash(into hasher: inout Hasher) {
  //   hasher.combine(self.hashValue)
  // }
  auto parentDC = hashIntoDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto selfDecl = hashIntoDecl->getImplicitSelfDecl();
  auto selfRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                     /*implicit*/ true);
  auto hashValueExpr = UnresolvedDotExpr::createImplicit(C, selfRef,
                                                         C.Id_hashValue);
  auto hasherParam = hashIntoDecl->getParameters()->get(0);
  auto hasherExpr = createHasherCombineCall(C, hasherParam, hashValueExpr);

  auto body = BraceStmt::create(C, SourceLoc(), {ASTNode(hasherExpr)},
                                SourceLoc(), /*implicit*/ true);
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for the 'hash(into:)' method for an enum by using its raw
/// value.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_enum_rawValue_hashInto(
  AbstractFunctionDecl *hashIntoDecl, void *) {
  // enum SomeEnum: Int {
  //   case A, B, C
  //   @derived func hash(into hasher: inout Hasher) {
  //     hasher.combine(self.rawValue)
  //   }
  // }
  ASTContext &C = hashIntoDecl->getASTContext();

  // generate: self.rawValue
  auto *selfRef = DerivedConformance::createSelfDeclRef(hashIntoDecl);
  auto *rawValueRef = UnresolvedDotExpr::createImplicit(C, selfRef,
                                                        C.Id_rawValue);

  // generate: hasher.combine(discriminator)
  auto hasherParam = hashIntoDecl->getParameters()->get(0);
  ASTNode combineStmt = createHasherCombineCall(C, hasherParam, rawValueRef);

  auto body = BraceStmt::create(C, SourceLoc(), combineStmt, SourceLoc(),
                                /*implicit*/ true);
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for the 'hash(into:)' method for an enum without associated
/// values.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_enum_noAssociatedValues_hashInto(
  AbstractFunctionDecl *hashIntoDecl, void *) {
  // enum SomeEnum {
  //   case A, B, C
  //   @derived func hash(into hasher: inout Hasher) {
  //     let discriminator: Int
  //     switch self {
  //     case A:
  //       discriminator = 0
  //     case B:
  //       discriminator = 1
  //     case C:
  //       discriminator = 2
  //     }
  //     hasher.combine(discriminator)
  //   }
  // }
  auto parentDC = hashIntoDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto enumDecl = parentDC->getSelfEnumDecl();
  auto selfDecl = hashIntoDecl->getImplicitSelfDecl();

  // generate: switch self {...}
  SmallVector<ASTNode, 3> stmts;
  auto discriminatorExpr = DerivedConformance::convertEnumToIndex(stmts, parentDC, enumDecl,
                                              selfDecl, hashIntoDecl,
                                              "discriminator");
  // generate: hasher.combine(discriminator)
  auto hasherParam = hashIntoDecl->getParameters()->get(0);
  auto combineStmt = createHasherCombineCall(C, hasherParam, discriminatorExpr);
  stmts.push_back(combineStmt);

  auto body = BraceStmt::create(C, SourceLoc(), stmts, SourceLoc(),
                                /*implicit*/ true);
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for the 'hash(into:)' method for an enum with associated
/// values.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_enum_hasAssociatedValues_hashInto(
  AbstractFunctionDecl *hashIntoDecl, void *) {
  // enum SomeEnumWithAssociatedValues {
  //   case A, B(Int), C(String, Int)
  //   @derived func hash(into hasher: inout Hasher) {
  //     switch self {
  //     case .A:
  //       hasher.combine(0)
  //     case .B(let a0):
  //       hasher.combine(1)
  //       hasher.combine(a0)
  //     case .C(let a0, let a1):
  //       hasher.combine(2)
  //       hasher.combine(a0)
  //       hasher.combine(a1)
  //     }
  //   }
  // }
  auto parentDC = hashIntoDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto enumDecl = parentDC->getSelfEnumDecl();
  auto selfDecl = hashIntoDecl->getImplicitSelfDecl();

  Type enumType = selfDecl->getType();

  // Extract the decl for the hasher parameter.
  auto hasherParam = hashIntoDecl->getParameters()->get(0);

  unsigned index = 0;
  SmallVector<ASTNode, 4> cases;

  // For each enum element, generate a case statement that binds the associated
  // values so that they can be fed to the hasher.
  for (auto elt : enumDecl->getAllElements()) {
    // case .<elt>(let a0, let a1, ...):
    SmallVector<VarDecl*, 3> payloadVars;
    SmallVector<ASTNode, 3> statements;

    auto payloadPattern = DerivedConformance::enumElementPayloadSubpattern(elt, 'a', hashIntoDecl,
                                                       payloadVars);
    auto pat = new (C) EnumElementPattern(
        TypeExpr::createImplicit(enumType, C), SourceLoc(), DeclNameLoc(),
        DeclNameRef(elt->getBaseIdentifier()), elt, payloadPattern);
    pat->setImplicit();

    auto labelItem = CaseLabelItem(pat);

    // If the enum has no associated values, we use the ordinal as the single
    // hash component, because that is sufficient for a good distribution. If
    // any case does have associated values, then the ordinal is used as the
    // first term fed into the hasher.

    {
      // Generate: hasher.combine(<ordinal>)
      auto ordinalExpr = IntegerLiteralExpr::createFromUnsigned(C, index++);
      auto combineExpr = createHasherCombineCall(C, hasherParam, ordinalExpr);
      statements.emplace_back(ASTNode(combineExpr));
    }

    // Generate a sequence of statements that feed the payloads into hasher.
    for (auto payloadVar : payloadVars) {
      auto payloadVarRef = new (C) DeclRefExpr(payloadVar, DeclNameLoc(),
                                               /*implicit*/ true);
      // Generate: hasher.combine(<payloadVar>)
      auto combineExpr = createHasherCombineCall(C, hasherParam, payloadVarRef);
      statements.emplace_back(ASTNode(combineExpr));
    }

    auto hasBoundDecls = !payloadVars.empty();
    Optional<MutableArrayRef<VarDecl *>> caseBodyVarDecls;
    if (hasBoundDecls) {
      auto copy = C.Allocate<VarDecl *>(payloadVars.size());
      for (unsigned i : indices(payloadVars)) {
        auto *vOld = payloadVars[i];
        auto *vNew = new (C) VarDecl(
            /*IsStatic*/ false, vOld->getIntroducer(), false /*IsCaptureList*/,
            vOld->getNameLoc(), vOld->getName(), vOld->getDeclContext());
        vNew->setHasNonPatternBindingInit();
        vNew->setImplicit();
        copy[i] = vNew;
      }
      caseBodyVarDecls.emplace(copy);
    }

    auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     caseBodyVarDecls,
                                     /*implicit*/ true));
  }

  // generate: switch enumVar { }
  auto enumRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                     /*implicit*/true);
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), enumRef,
                                       SourceLoc(), cases, SourceLoc(), C);

  auto body = BraceStmt::create(C, SourceLoc(), {ASTNode(switchStmt)},
                                SourceLoc());
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for the 'hash(into:)' method for a struct.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_struct_hashInto(AbstractFunctionDecl *hashIntoDecl, void *) {
  // struct SomeStruct {
  //   var x: Int
  //   var y: String
  //   @derived func hash(into hasher: inout Hasher) {
  //     hasher.combine(x)
  //     hasher.combine(y)
  //   }
  // }
  auto parentDC = hashIntoDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  auto structDecl = parentDC->getSelfStructDecl();
  SmallVector<ASTNode, 6> statements;
  auto selfDecl = hashIntoDecl->getImplicitSelfDecl();

  // Extract the decl for the hasher parameter.
  auto hasherParam = hashIntoDecl->getParameters()->get(0);

  auto storedProperties = structDecl->getStoredProperties();

  // Feed each stored property into the hasher.
  for (auto propertyDecl : storedProperties) {
    if (!propertyDecl->isUserAccessible())
      continue;

    auto propertyRef = new (C) DeclRefExpr(propertyDecl, DeclNameLoc(),
                                           /*implicit*/ true);
    auto selfRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                       /*implicit*/ true);
    auto selfPropertyExpr = new (C) DotSyntaxCallExpr(propertyRef, SourceLoc(),
                                                      selfRef);
    // Generate: hasher.combine(self.<property>)
    auto combineExpr = createHasherCombineCall(C, hasherParam, selfPropertyExpr);
    statements.emplace_back(ASTNode(combineExpr));
  }

  auto body = BraceStmt::create(C, SourceLoc(), statements,
                                SourceLoc(), /*implicit*/ true);
  return { body, /*isTypeChecked=*/false };
}

/// Derive the body for the 'hashValue' getter.
static std::pair<BraceStmt *, bool>
deriveBodyHashable_hashValue(AbstractFunctionDecl *hashValueDecl, void *) {
  auto parentDC = hashValueDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  // return _hashValue(for: self)

  // 'self'
  auto selfDecl = hashValueDecl->getImplicitSelfDecl();
  Type selfType = selfDecl->getType();
  auto selfRef = new (C) DeclRefExpr(selfDecl, DeclNameLoc(),
                                     /*implicit*/ true,
                                     AccessSemantics::Ordinary,
                                     selfType);

  // _hashValue(for:)
  auto *hashFunc = C.getHashValueForDecl();
  auto substitutions = SubstitutionMap::get(
      hashFunc->getGenericSignature(),
      [&](SubstitutableType *dependentType) {
        if (auto gp = dyn_cast<GenericTypeParamType>(dependentType)) {
          if (gp->getDepth() == 0 && gp->getIndex() == 0)
            return selfType;
        }

        return Type(dependentType);
      },
      LookUpConformanceInModule(hashValueDecl->getModuleContext()));
  ConcreteDeclRef hashFuncRef(hashFunc, substitutions);

  Type hashFuncType = hashFunc->getInterfaceType().subst(substitutions);
  auto hashExpr = new (C) DeclRefExpr(hashFuncRef, DeclNameLoc(),
                                      /*implicit*/ true,
                                      AccessSemantics::Ordinary,
                                      hashFuncType);
  Type hashFuncResultType =
      hashFuncType->castTo<AnyFunctionType>()->getResult();
  auto callExpr = CallExpr::createImplicit(C, hashExpr,
                                           { selfRef }, { C.Id_for });
  callExpr->setType(hashFuncResultType);

  auto returnStmt = new (C) ReturnStmt(SourceLoc(), callExpr);

  auto body = BraceStmt::create(C, SourceLoc(), {returnStmt}, SourceLoc(),
                                /*implicit*/ true);
  return { body, /*isTypeChecked=*/true };
}

/// Derive a 'hashValue' implementation.
static ValueDecl *deriveHashable_hashValue(DerivedConformance &derived) {
  // @derived var hashValue: Int {
  //   return _hashValue(for: self)
  // }
  ASTContext &C = derived.Context;

  auto parentDC = derived.getConformanceContext();
  Type intType = C.getIntDecl()->getDeclaredType();

  // We can't form a Hashable conformance if Int isn't Hashable or
  // ExpressibleByIntegerLiteral.
  if (TypeChecker::conformsToProtocol(
          intType, C.getProtocol(KnownProtocolKind::Hashable), parentDC)
          .isInvalid()) {
    derived.ConformanceDecl->diagnose(diag::broken_int_hashable_conformance);
    return nullptr;
  }

  ProtocolDecl *intLiteralProto =
      C.getProtocol(KnownProtocolKind::ExpressibleByIntegerLiteral);
  if (TypeChecker::conformsToProtocol(intType, intLiteralProto, parentDC)
          .isInvalid()) {
    derived.ConformanceDecl->diagnose(
      diag::broken_int_integer_literal_convertible_conformance);
    return nullptr;
  }

  VarDecl *hashValueDecl =
    new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Var,
                    /*IsCaptureList*/false, SourceLoc(),
                    C.Id_hashValue, parentDC);
  hashValueDecl->setInterfaceType(intType);

  ParameterList *params = ParameterList::createEmpty(C);

  AccessorDecl *getterDecl = AccessorDecl::create(C,
      /*FuncLoc=*/SourceLoc(), /*AccessorKeywordLoc=*/SourceLoc(),
      AccessorKind::Get, hashValueDecl,
      /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
      /*GenericParams=*/nullptr, params,
      TypeLoc::withoutLoc(intType), parentDC);
  getterDecl->setImplicit();
  getterDecl->setBodySynthesizer(&deriveBodyHashable_hashValue);
  getterDecl->setIsTransparent(false);

  getterDecl->copyFormalAccessFrom(derived.Nominal,
                                   /*sourceIsParentContext*/ true);

  // Finish creating the property.
  hashValueDecl->setImplicit();
  hashValueDecl->setInterfaceType(intType);
  hashValueDecl->setImplInfo(StorageImplInfo::getImmutableComputed());
  hashValueDecl->setAccessors(SourceLoc(), {getterDecl}, SourceLoc());
  hashValueDecl->copyFormalAccessFrom(derived.Nominal,
                                      /*sourceIsParentContext*/ true);

  Pattern *hashValuePat = NamedPattern::createImplicit(C, hashValueDecl);
  hashValuePat->setType(intType);
  hashValuePat = TypedPattern::createImplicit(C, hashValuePat, intType);
  hashValuePat->setType(intType);

  auto *patDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, hashValuePat, /*InitExpr*/ nullptr,
      parentDC);

  derived.addMembersToConformanceContext({hashValueDecl, patDecl});

  return hashValueDecl;
}

static ValueDecl *
getHashValueRequirement(ASTContext &C) {
  auto hashableProto = C.getProtocol(KnownProtocolKind::Hashable);
  for (auto member: hashableProto->getMembers()) {
    if (auto fd = dyn_cast<VarDecl>(member)) {
      if (fd->getBaseName() == C.Id_hashValue)
        return fd;
    }
  }
  return nullptr;
}

static ProtocolConformance *
getHashableConformance(const Decl *parentDecl) {
  ASTContext &C = parentDecl->getASTContext();
  const auto IDC = cast<IterableDeclContext>(parentDecl);
  auto hashableProto = C.getProtocol(KnownProtocolKind::Hashable);
  for (auto conformance: IDC->getLocalConformances()) {
    if (conformance->getProtocol() == hashableProto) {
      return conformance;
    }
  }
  return nullptr;
}

bool DerivedConformance::canDeriveHashable(NominalTypeDecl *type) {
  if (!isa<EnumDecl>(type) && !isa<StructDecl>(type) && !isa<ClassDecl>(type))
    return false;
  // FIXME: This is not actually correct. We cannot promise to always
  // provide a witness here in all cases. Unfortunately, figuring out
  // whether this is actually possible requires a parent decl context.
  // When the answer is no, DerivedConformance::deriveHashable will output
  // its own diagnostics.
  return true;
}

void DerivedConformance::tryDiagnoseFailedHashableDerivation(
    DeclContext *DC, NominalTypeDecl *nominal) {
  ASTContext &ctx = DC->getASTContext();
  auto *hashableProto = ctx.getProtocol(KnownProtocolKind::Hashable);
  diagnoseFailedDerivation(DC, nominal, hashableProto);
}

ValueDecl *DerivedConformance::deriveHashable(ValueDecl *requirement) {
  ASTContext &C = ConformanceDecl->getASTContext();

  // var hashValue: Int
  if (requirement->getBaseName() == C.Id_hashValue) {
    // We always allow hashValue to be synthesized; invalid cases are diagnosed
    // during hash(into:) synthesis.
    return deriveHashable_hashValue(*this);
  }

  // Hashable.hash(into:)
  if (requirement->getBaseName() == C.Id_hash) {
    // Start by resolving hashValue conformance.
    auto hashValueReq = getHashValueRequirement(C);
    auto conformance = getHashableConformance(ConformanceDecl);
    auto hashValueDecl = conformance->getWitnessDecl(hashValueReq);
    if (!hashValueDecl) {
      // We won't derive hash(into:) if hashValue cannot be resolved.
      // The hashValue failure will produce a diagnostic elsewhere.
      return nullptr;
    }
    if (hashValueDecl->isImplicit()) {
      // Neither hashValue nor hash(into:) is explicitly defined; we need to do
      // a full Hashable derivation.
      
      // Refuse to synthesize Hashable if type isn't a struct or enum, or if it
      // has non-Hashable stored properties/associated values.
      auto hashableProto = C.getProtocol(KnownProtocolKind::Hashable);
      if (!canDeriveConformance(getConformanceContext(), Nominal,
                                hashableProto)) {
        ConformanceDecl->diagnose(diag::type_does_not_conform,
                                  Nominal->getDeclaredType(),
                                  hashableProto->getDeclaredType());
        // Ideally, this would be diagnosed in
        // ConformanceChecker::resolveWitnessViaLookup. That doesn't work for
        // Hashable because DerivedConformance::canDeriveHashable returns true
        // even if the conformance can't be derived. See the note there for
        // details.
        auto *dc = ConformanceDecl->getDeclContext();
        tryDiagnoseFailedHashableDerivation(dc, Nominal);
        return nullptr;
      }

      if (checkAndDiagnoseDisallowedContext(requirement))
        return nullptr;

      if (auto ED = dyn_cast<EnumDecl>(Nominal)) {
        std::pair<BraceStmt *, bool> (*bodySynthesizer)(
            AbstractFunctionDecl *, void *);
        if (ED->isObjC())
          bodySynthesizer = deriveBodyHashable_enum_rawValue_hashInto;
        else if (ED->hasOnlyCasesWithoutAssociatedValues())
          bodySynthesizer = deriveBodyHashable_enum_noAssociatedValues_hashInto;
        else
          bodySynthesizer=deriveBodyHashable_enum_hasAssociatedValues_hashInto;
        return deriveHashable_hashInto(*this, bodySynthesizer);
      } else if (isa<StructDecl>(Nominal))
        return deriveHashable_hashInto(*this,
                                       &deriveBodyHashable_struct_hashInto);
      else // This should've been caught by canDeriveHashable above.
        llvm_unreachable("Attempt to derive Hashable for a type other "
                         "than a struct or enum");      
    } else {
      // hashValue has an explicit implementation, but hash(into:) doesn't.
      // Emit a deprecation warning, then derive hash(into:) in terms of
      // hashValue.
      hashValueDecl->diagnose(diag::hashvalue_implementation,
                              Nominal->getDeclaredType());
      return deriveHashable_hashInto(*this,
                                     &deriveBodyHashable_compat_hashInto);
    }
  }

  requirement->diagnose(diag::broken_hashable_requirement);
  return nullptr;
}