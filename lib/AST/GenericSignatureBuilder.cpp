//===--- GenericSignatureBuilder.cpp - Generic Requirement Builder --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Support for collecting a set of generic requirements, both explicitly stated
// and inferred, and computing the archetypes and required witness tables from
// those requirements.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using llvm::DenseMap;

namespace {
  typedef GenericSignatureBuilder::PotentialArchetype PotentialArchetype;
} // end anonymous namespace

struct GenericSignatureBuilder::Implementation {
  /// Function used to look up conformances.
  std::function<GenericFunction> LookupConformance;

  /// The generic parameters that this generic signature builder is working
  /// with.
  SmallVector<GenericTypeParamType *, 4> GenericParams;

  /// The potential archetypes for the generic parameters in \c GenericParams.
  SmallVector<PotentialArchetype *, 4> PotentialArchetypes;

  /// The number of nested types that haven't yet been resolved to archetypes.
  /// Once all requirements have been added, this will be zero in well-formed
  /// code.
  unsigned NumUnresolvedNestedTypes = 0;

  /// The nested types that have been renamed.
  SmallVector<PotentialArchetype *, 4> RenamedNestedTypes;

  /// The requirement sources used in this generic signature builder.
  llvm::FoldingSet<RequirementSource> RequirementSources;

#ifndef NDEBUG
  /// Whether we've already finalized the builder.
  bool finalized = false;
#endif
};

#pragma mark Requirement sources
bool RequirementSource::isAcceptableStorageKind(Kind kind,
                                                StorageKind storageKind) {
  switch (kind) {
  case Explicit:
    switch (storageKind) {
    case StorageKind::None:
    case StorageKind::TypeRepr:
    case StorageKind::RequirementRepr:
      return true;

    case StorageKind::ProtocolDecl:
    case StorageKind::ProtocolConformance:
      return false;
    }

  case Inferred:
    switch (storageKind) {
    case StorageKind::None:
    case StorageKind::TypeRepr:
      return true;

    case StorageKind::ProtocolDecl:
    case StorageKind::ProtocolConformance:
    case StorageKind::RequirementRepr:
      return false;
    }

  case NestedTypeNameMatch:
  case Parent:
    switch (storageKind) {
    case StorageKind::None:
        return true;

    case StorageKind::TypeRepr:
    case StorageKind::ProtocolDecl:
    case StorageKind::ProtocolConformance:
    case StorageKind::RequirementRepr:
      return false;
    }

  case RequirementSignatureSelf:
  case ProtocolRequirement:
    switch (storageKind) {
    case StorageKind::ProtocolDecl:
      return true;

    case StorageKind::None:
    case StorageKind::TypeRepr:
    case StorageKind::ProtocolConformance:
    case StorageKind::RequirementRepr:
      return false;
    }

  case Superclass:
  case Concrete:
    switch (storageKind) {
    case StorageKind::ProtocolConformance:
      return true;

    case StorageKind::None:
    case StorageKind::ProtocolDecl:
    case StorageKind::TypeRepr:
    case StorageKind::RequirementRepr:
      return false;
    }
  }
}

const void *RequirementSource::getOpaqueStorage() const {
  switch (storageKind) {
  case StorageKind::None:
    // Note: always null.
    return storage.typeRepr;

  case StorageKind::TypeRepr:
    return storage.typeRepr;

  case StorageKind::RequirementRepr:
    return storage.requirementRepr;

  case StorageKind::ProtocolConformance:
    return storage.conformance;

  case StorageKind::ProtocolDecl:
    return storage.protocol;
  }
}

bool RequirementSource::isDerivedRequirement() const {
  switch (kind) {
  case Explicit:
  case Inferred:
    return false;

  case NestedTypeNameMatch:
  case Parent:
  case Superclass:
  case Concrete:
  case RequirementSignatureSelf:
    return true;

  case ProtocolRequirement:
    // Requirements based on protocol requirements are derived unless they are
    // direct children of the requirement-signature source, in which case we
    // need to keep them for the requirement signature.
    return parent->kind != RequirementSignatureSelf;
  }
}

bool RequirementSource::isDerivedViaConcreteConformance() const {
  for (auto source = this; source; source = source->parent) {
    switch (source->kind) {
    case Explicit:
    case Inferred:
    case NestedTypeNameMatch:
    case RequirementSignatureSelf:
      return false;

    case Parent:
    case ProtocolRequirement:
      continue;

    case Superclass:
    case Concrete:
      return true;
    }
  }

  return false;
}

#define REQUIREMENT_SOURCE_FACTORY_BODY(SourceKind, Parent, Storage)       \
  llvm::FoldingSetNodeID nodeID;                                           \
  Profile(nodeID, Kind::SourceKind, Parent, Storage);                      \
                                                                           \
  void *insertPos = nullptr;                                               \
  if (auto known =                                                         \
        builder.Impl->RequirementSources.FindNodeOrInsertPos(nodeID,       \
                                                            insertPos))    \
    return known;                                                          \
                                                                           \
  auto result = new RequirementSource(Kind::SourceKind, Parent, Storage);  \
  builder.Impl->RequirementSources.InsertNode(result, insertPos);          \
  return result

#define REQUIREMENT_SOURCE_FACTORY_BODY_NOSTORAGE(SourceKind, Parent)      \
  llvm::FoldingSetNodeID nodeID;                                           \
  Profile(nodeID, Kind::SourceKind, Parent, nullptr);                      \
                                                                           \
  void *insertPos = nullptr;                                               \
  if (auto known =                                                         \
        builder.Impl->RequirementSources.FindNodeOrInsertPos(nodeID,       \
                                                            insertPos))    \
    return known;                                                          \
                                                                           \
  auto result = new RequirementSource(Kind::SourceKind, Parent);           \
  builder.Impl->RequirementSources.InsertNode(result, insertPos);          \
  return result

const RequirementSource *RequirementSource::forAbstract(
                                             GenericSignatureBuilder &builder) {
  REQUIREMENT_SOURCE_FACTORY_BODY_NOSTORAGE(Explicit, nullptr);
}

const RequirementSource *RequirementSource::forExplicit(
                                             GenericSignatureBuilder &builder,
                                             const TypeRepr *typeRepr) {
  REQUIREMENT_SOURCE_FACTORY_BODY(Explicit, nullptr, typeRepr);
}

const RequirementSource *RequirementSource::forExplicit(
                                      GenericSignatureBuilder &builder,
                                      const RequirementRepr *requirementRepr) {
  REQUIREMENT_SOURCE_FACTORY_BODY(Explicit, nullptr, requirementRepr);
}

const RequirementSource *RequirementSource::forInferred(
                                              GenericSignatureBuilder &builder,
                                              const TypeRepr *typeRepr) {
  REQUIREMENT_SOURCE_FACTORY_BODY(Inferred, nullptr, typeRepr);
}

const RequirementSource *RequirementSource::forRequirementSignature(
                                              GenericSignatureBuilder &builder,
                                              ProtocolDecl *protocol) {
  REQUIREMENT_SOURCE_FACTORY_BODY(RequirementSignatureSelf, nullptr, protocol);
}

const RequirementSource *RequirementSource::forNestedTypeNameMatch(
                                             GenericSignatureBuilder &builder) {
  REQUIREMENT_SOURCE_FACTORY_BODY_NOSTORAGE(NestedTypeNameMatch, nullptr);
}

const RequirementSource *RequirementSource::viaAbstractProtocolRequirement(
                                               GenericSignatureBuilder &builder,
                                               ProtocolDecl *protocol) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(ProtocolRequirement, this, protocol);
}

const RequirementSource *RequirementSource::viaSuperclass(
                                      GenericSignatureBuilder &builder,
                                      ProtocolConformance *conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(Superclass, this, conformance);
}

const RequirementSource *RequirementSource::viaConcrete(
                                      GenericSignatureBuilder &builder,
                                      ProtocolConformance *conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(Concrete, this, conformance);
}

const RequirementSource *RequirementSource::viaParent(
                                      GenericSignatureBuilder &builder) const {
  REQUIREMENT_SOURCE_FACTORY_BODY_NOSTORAGE(Parent, this);
}

#undef REQUIREMENT_SOURCE_FACTORY_BODY_NOSTORAGE
#undef REQUIREMENT_SOURCE_FACTORY_BODY

ProtocolDecl *RequirementSource::getProtocolDecl() const {
  switch (storageKind) {
  case StorageKind::None:
  case StorageKind::TypeRepr:
  case StorageKind::RequirementRepr:
    return nullptr;

  case StorageKind::ProtocolDecl:
    return storage.protocol;

  case StorageKind::ProtocolConformance:
    if (storage.conformance)
      return storage.conformance->getProtocol();

    return nullptr;
  }
}

SourceLoc RequirementSource::getLoc() const {
  if (auto typeRepr = getTypeRepr())
    return typeRepr->getStartLoc();
  if (auto requirementRepr = getRequirementRepr()) {
    switch (requirementRepr->getKind()) {
    case RequirementReprKind::LayoutConstraint:
    case RequirementReprKind::TypeConstraint:
      return requirementRepr->getColonLoc();

    case RequirementReprKind::SameType:
      return requirementRepr->getEqualLoc();
    }
  }
  if (parent)
    return parent->getLoc();
  if (kind == RequirementSignatureSelf)
    return getProtocolDecl()->getLoc();

  return SourceLoc();
}

int RequirementSource::compare(const RequirementSource *other) const {
  // Prefer the derived option, if there is one.
  bool thisIsDerived = this->isDerivedRequirement();
  bool otherIsDerived = other->isDerivedRequirement();
  if (thisIsDerived != otherIsDerived)
    return thisIsDerived ? -1 : +1;

  // FIXME: Arbitrary hack to allow later requirement sources to stomp on
  // earlier ones. We need a proper ordering here.
  return +1;
}

void RequirementSource::dump() const {
  dump(llvm::errs(), nullptr, 0);
  llvm::errs() << "\n";
}

/// Dump the constraint source.
void RequirementSource::dump(llvm::raw_ostream &out, SourceManager *srcMgr,
                             unsigned indent) const {
  // FIXME: Implement for real, so we actually dump the structure.
  out.indent(indent);
  print(out, srcMgr);
}

void RequirementSource::print() const {
  print(llvm::errs(), nullptr);
}

void RequirementSource::print(llvm::raw_ostream &out,
                              SourceManager *srcMgr) const {
  if (parent) {
    parent->print(out, srcMgr);
    out << " -> ";
  }

  switch (kind) {
  case Concrete:
    out << "Concrete";
    break;

  case Explicit:
    out << "Explicit";
    break;

  case Inferred:
    out << "Inferred";
    break;

  case NestedTypeNameMatch:
    out << "Nested type match";
    break;

  case Parent:
    out << "Parent";
    break;

  case ProtocolRequirement:
    out << "Protocol requirement";
    break;

  case RequirementSignatureSelf:
    out << "Requirement signature self";
    break;

  case Superclass:
    out << "Superclass";
    break;
  }

  // Local function to dump a source location, if we can.
  auto dumpSourceLoc = [&](SourceLoc loc) {
    if (!srcMgr) return;
    if (loc.isInvalid()) return;

    unsigned bufferID = srcMgr->findBufferContainingLoc(loc);

    auto lineAndCol = srcMgr->getLineAndColumn(loc, bufferID);
    out << " @ " << lineAndCol.first << ':' << lineAndCol.second;
  };

  switch (storageKind) {
  case StorageKind::None:
    break;

  case StorageKind::TypeRepr:
  case StorageKind::RequirementRepr:
    dumpSourceLoc(getLoc());
    break;

  case StorageKind::ProtocolDecl:
    if (storage.protocol)
      out << " (" << storage.protocol->getName() << ")";
    break;

  case StorageKind::ProtocolConformance:
    if (storage.conformance) {
      out << " (" << storage.conformance->getType()->getString() << ": "
          << storage.conformance->getProtocol()->getName() << ")";
    }
    break;
  }
}

GenericSignatureBuilder::PotentialArchetype::~PotentialArchetype() {
  for (const auto &nested : NestedTypes) {
    for (auto pa : nested.second) {
      if (pa != this)
        delete pa;
    }
  }
}

std::string GenericSignatureBuilder::PotentialArchetype::getDebugName() const {
  llvm::SmallString<64> result;

  auto parent = getParent();
  if (!parent) {
    return GenericTypeParamType::get(getGenericParamKey().Depth,
                                     getGenericParamKey().Index,
                                     getBuilder()->getASTContext())->getName()
             .str();
  }

  // Nested types.
  result += parent->getDebugName();

  // When building the name for debugging purposes, include the protocol into
  // which the associated type or type alias was resolved.
  ProtocolDecl *proto = nullptr;
  if (auto assocType = getResolvedAssociatedType()) {
    proto = assocType->getProtocol();
  } else if (auto typeAlias = getTypeAliasDecl()) {
    proto = typeAlias->getParent()->getAsProtocolOrProtocolExtensionContext();
  }

  if (proto) {
    result.push_back('[');
    result.push_back('.');
    result.append(proto->getName().str().begin(), proto->getName().str().end());
    result.push_back(']');
  }

  result.push_back('.');
  result.append(getNestedName().str().begin(), getNestedName().str().end());

  return result.str().str();
}

unsigned GenericSignatureBuilder::PotentialArchetype::getNestingDepth() const {
  unsigned Depth = 0;
  for (auto P = getParent(); P; P = P->getParent())
    ++Depth;
  return Depth;
}

void GenericSignatureBuilder::PotentialArchetype::resolveAssociatedType(
       AssociatedTypeDecl *assocType,
       GenericSignatureBuilder &builder) {
  assert(!getResolvedAssociatedType() && "associated type is already resolved");
  isUnresolvedNestedType = false;
  identifier.assocTypeOrAlias = assocType;
  assert(assocType->getName() == getNestedName());
  assert(builder.Impl->NumUnresolvedNestedTypes > 0 &&
         "Mismatch in number of unresolved nested types");
  --builder.Impl->NumUnresolvedNestedTypes;
}

const RequirementSource *
PotentialArchetype::findAnyConcreteTypeSourceAsWritten() const {
  // If we have a concrete type source, use that.
  if (ConcreteTypeSource && ConcreteTypeSource->getLoc().isValid())
    return ConcreteTypeSource;

  // If we don't have a concrete type, there's no source.
  auto rep = getRepresentative();
  if (!rep->isConcreteType()) return nullptr;

  // Otherwise, go look for the source.
  for (auto pa : rep->getEquivalenceClass()) {
    if (pa->ConcreteTypeSource &&
        pa->ConcreteTypeSource->getLoc().isValid())
      return pa->ConcreteTypeSource;
  }

  return nullptr;
}

bool GenericSignatureBuilder::updateRequirementSource(
                                      const RequirementSource *&existingSource,
                                      const RequirementSource *newSource) {
  if (int result = newSource->compare(existingSource)) {
    if (result < 0) {
      existingSource = newSource;
      return true;
    }

    return false;
  }

  assert(existingSource == newSource && "incomparable requirement sources");
  return false;
}

const RequirementSource *GenericSignatureBuilder::resolveSuperConformance(
                      GenericSignatureBuilder::PotentialArchetype *pa,
                      ProtocolDecl *proto,
                      const RequirementSource *&protoSource) {
  // Get the superclass constraint.
  Type superclass = pa->getSuperclass();
  if (!superclass) return nullptr;

  // Lookup the conformance of the superclass to this protocol.
  auto conformance =
    getLookupConformanceFn()(pa->getDependentType({ }, /*allowUnresolved=*/true)
                               ->getCanonicalType(),
                             superclass,
                             proto->getDeclaredInterfaceType()
                               ->castTo<ProtocolType>());
  if (!conformance) return nullptr;

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  auto superclassSource =
    pa->getSuperclassSource()->viaSuperclass(*this, conformance->getConcrete());
  updateRequirementSource(protoSource, superclassSource);
  return superclassSource;
}

struct GenericSignatureBuilder::ResolvedType {
  llvm::PointerUnion<PotentialArchetype *, Type> paOrT;

  explicit ResolvedType(PotentialArchetype *pa) : paOrT(pa) {}
  explicit ResolvedType(Type ty) : paOrT(ty) {}

public:
  static ResolvedType forConcreteType(Type t) {
    assert(!t->isTypeParameter() &&
           "concrete type with parameter should've been resolved");
    return ResolvedType(t);
  }

  // FIXME: this probably shouldn't exist, the potential archetype modelling of
  // generic typealiases is fundamentally broken (aka they're not modelled at
  // all), but some things with them mostly work, so we just maintain that,
  // despite this causing crashes and weird behavior.
  static ResolvedType forConcreteTypeFromGenericTypeAlias(Type t) {
    return ResolvedType(t);
  }

  static ResolvedType forPotentialArchetype(PotentialArchetype *pa) {
    return ResolvedType(pa);
  }

  static ResolvedType forNewTypeAlias(PotentialArchetype *pa) {
    assert(pa->getParent() && pa->getTypeAliasDecl() &&
           pa->ConcreteType.isNull() && pa->getEquivalenceClass().size() == 1 &&
           "not a new typealias");
    return ResolvedType(pa);
  }

  Type getType() const { return paOrT.dyn_cast<Type>(); }
  PotentialArchetype *getPotentialArchetype() const {
    return paOrT.dyn_cast<PotentialArchetype *>();
  }
};

/// If there is a same-type requirement to be added for the given nested type
/// due to a superclass constraint on the parent type, add it now.
static void maybeAddSameTypeRequirementForNestedType(
              GenericSignatureBuilder::PotentialArchetype *nestedPA,
              const RequirementSource *superSource,
              GenericSignatureBuilder &builder) {
  // If there's no super conformance, we're done.
  if (!superSource) return;

  auto assocType = nestedPA->getResolvedAssociatedType();
  assert(assocType && "Not resolved to an associated type?");

  // Dig out the type witness.
  auto superConformance = superSource->getProtocolConformance();
  auto concreteType =
    superConformance->getTypeWitness(assocType, builder.getLazyResolver())
      .getReplacement();
  if (!concreteType) return;

  // Add the same-type constraint.
  auto nestedSource = superSource->viaParent(builder);
  concreteType = superConformance->getDeclContext()
      ->mapTypeOutOfContext(concreteType);

  builder.addSameTypeRequirement(nestedPA, concreteType, nestedSource);
}

/// Walk the members of a protocol.
///
/// This is essentially just a call to \c proto->getMembers(), except that
/// for Objective-C-imported protocols we can simply return an empty declaration
/// range because the generic signature builder only cares about nested types (which
/// Objective-C protocols don't have).
static DeclRange getProtocolMembers(ProtocolDecl *proto) {
  if (proto->hasClangNode())
    return DeclRange(DeclIterator(), DeclIterator());

  return proto->getMembers();
}

bool GenericSignatureBuilder::PotentialArchetype::addConformance(
       ProtocolDecl *proto, 
       bool updateExistingSource,
       const RequirementSource *source,
       GenericSignatureBuilder &builder) {
  auto rep = getRepresentative();
  if (rep != this)
    return rep->addConformance(proto, updateExistingSource, source, builder);

  // Check whether we already know about this conformance.
  auto known = ConformsTo.find(proto);
  if (known != ConformsTo.end()) {
    // We already have this requirement. Update the requirement source
    // appropriately.
    if (updateExistingSource)
      builder.updateRequirementSource(known->second, source);
    return false;
  }

  // Add this conformance.
  auto inserted = ConformsTo.insert(std::make_pair(proto, source)).first;

  // Determine whether there is a superclass constraint where the
  // superclass conforms to this protocol.
  auto superSource = getBuilder()->resolveSuperConformance(this, proto,
                                                           inserted->second);

  // Check whether any associated types in this protocol resolve
  // nested types of this potential archetype.
  for (auto member : getProtocolMembers(proto)) {
    auto assocType = dyn_cast<AssociatedTypeDecl>(member);
    if (!assocType)
      continue;

    auto known = NestedTypes.find(assocType->getName());
    if (known == NestedTypes.end())
      continue;

    // If the nested type was not already resolved, do so now.
    if (!known->second.front()->getResolvedAssociatedType()) {
      known->second.front()->resolveAssociatedType(assocType, builder);

      // If there's a superclass constraint that conforms to the protocol,
      // add the appropriate same-type relationship.
      maybeAddSameTypeRequirementForNestedType(known->second.front(),
                                               superSource,
                                               builder);
      continue;
    }

    // Otherwise, create a new potential archetype for this associated type
    // and make it equivalent to the first potential archetype we encountered.
    auto otherPA = new PotentialArchetype(this, assocType);
    known->second.push_back(otherPA);
    auto sameNamedSource = RequirementSource::forNestedTypeNameMatch(builder);
    builder.addSameTypeRequirement(known->second.front(), otherPA,
                                   sameNamedSource);

    // If there's a superclass constraint that conforms to the protocol,
    // add the appropriate same-type relationship.
    maybeAddSameTypeRequirementForNestedType(otherPA, superSource, builder);
  }

  return true;
}

auto GenericSignatureBuilder::PotentialArchetype::getRepresentative() const
                                             -> PotentialArchetype *{
  // Find the representative.
  PotentialArchetype *Result = Representative;
  while (Result != Result->Representative)
    Result = Result->Representative;

  // Perform (full) path compression.
  const PotentialArchetype *FixUp = this;
  while (FixUp != FixUp->Representative) {
    const PotentialArchetype *Next = FixUp->Representative;
    FixUp->Representative = Result;
    FixUp = Next;
  }

  return Result;
}

/// Canonical ordering for dependent types in generic signatures.
static int compareDependentTypes(PotentialArchetype * const* pa,
                                 PotentialArchetype * const* pb) {
  auto a = *pa, b = *pb;

  // Fast-path check for equality.
  if (a == b)
    return 0;

  // Typealiases must be ordered *after* everything else, to ensure they
  // don't become representatives in the case where a typealias is equated
  // with an associated type.
  if (a->getParent() && b->getParent() &&
      !!a->getTypeAliasDecl() != !!b->getTypeAliasDecl())
    return a->getTypeAliasDecl() ? +1 : -1;

  // Types that are equivalent to concrete types follow types that are still
  // type parameters.
  if (a->isConcreteType() != b->isConcreteType())
    return a->isConcreteType() ? +1 : -1;

  // Ordering is as follows:
  // - Generic params
  if (a->isGenericParam() && b->isGenericParam())
    return a->getGenericParamKey() < b->getGenericParamKey() ? -1 : +1;

  // A generic parameter is always ordered before a nested type.
  if (a->isGenericParam() != b->isGenericParam())
    return a->isGenericParam() ? -1 : +1;

  // - Dependent members
  auto ppa = a->getParent();
  auto ppb = b->getParent();

  // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
  if (int compareBases = compareDependentTypes(&ppa, &ppb))
    return compareBases;

  // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
  if (int compareNames = a->getNestedName().str().compare(
                                                      b->getNestedName().str()))
    return compareNames;

  if (auto *aa = a->getResolvedAssociatedType()) {
    if (auto *ab = b->getResolvedAssociatedType()) {
      // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
      auto protoa = aa->getProtocol();
      auto protob = ab->getProtocol();
      if (int compareProtocols
            = ProtocolType::compareProtocols(&protoa, &protob))
        return compareProtocols;

      // Error case: if we have two associated types with the same name in the
      // same protocol, just tie-break based on address.
      if (aa != ab)
        return aa < ab ? -1 : +1;
    } else {
      // A resolved archetype is always ordered before an unresolved one.
      return -1;
    }
  } else {
    // A resolved archetype is always ordered before an unresolved one.
    if (b->getResolvedAssociatedType())
      return +1;
  }

  // Make sure typealiases are properly ordered, to avoid crashers.
  if (auto *aa = a->getTypeAliasDecl()) {
    auto *ab = b->getTypeAliasDecl();
    assert(ab != nullptr && "Should have handled this case above");

    // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
    auto protoa =
      aa->getDeclContext()->getAsProtocolOrProtocolExtensionContext();
    auto protob =
      ab->getDeclContext()->getAsProtocolOrProtocolExtensionContext();

    if (int compareProtocols
          = ProtocolType::compareProtocols(&protoa, &protob))
      return compareProtocols;

    if (aa != ab)
      return aa < ab ? -1 : +1;
  }

  // Along the error path where one or both of the potential archetypes was
  // renamed due to typo correction,
  if (a->wasRenamed() || b->wasRenamed()) {
    if (a->wasRenamed() != b->wasRenamed())
      return a->wasRenamed() ? +1 : -1;

    if (int compareNames = a->getOriginalName().str().compare(
                                                    b->getOriginalName().str()))
      return compareNames;
  }

  llvm_unreachable("potential archetype total order failure");
}

/// Rebuild the given potential archetype based on anchors.
static GenericSignatureBuilder::PotentialArchetype*rebuildPotentialArchetypeAnchor(
                                    GenericSignatureBuilder::PotentialArchetype *pa,
                                    GenericSignatureBuilder &builder) {
  if (auto parent = pa->getParent()) {
    auto parentAnchor =
      rebuildPotentialArchetypeAnchor(parent->getArchetypeAnchor(builder),
                                      builder);
    if (parent == parentAnchor) return pa;

    if (auto assocType = pa->getResolvedAssociatedType())
      return parentAnchor->getNestedType(assocType, builder);

    return parentAnchor->getNestedType(pa->getNestedName(), builder);
  }

  return pa;
}

auto GenericSignatureBuilder::PotentialArchetype::getArchetypeAnchor(
                                                      GenericSignatureBuilder &builder)
       -> PotentialArchetype * {
  // Rebuild the potential archetype anchor for this type, so the equivalence
  // class will contain the anchor.
  (void)rebuildPotentialArchetypeAnchor(this, builder);

  // Find the best archetype within this equivalence class.
  PotentialArchetype *rep = getRepresentative();
  auto anchor = rep;
  for (auto pa : rep->getEquivalenceClass()) {
    if (compareDependentTypes(&pa, &anchor) < 0)
      anchor = pa;
  }

#ifndef NDEBUG
  // Make sure that we did, in fact, get one that is better than all others.
  for (auto pa : anchor->getEquivalenceClass()) {
    assert((pa == anchor || compareDependentTypes(&anchor, &pa) < 0) &&
           compareDependentTypes(&pa, &anchor) >= 0 &&
           "archetype anchor isn't a total order");
  }
#endif

  return anchor;
}

// Give a nested type the appropriately resolved concrete type, based off a
// parent PA that has a concrete type.
static void concretizeNestedTypeFromConcreteParent(
    GenericSignatureBuilder::PotentialArchetype *parent,
    const RequirementSource *parentConcreteSource,
    GenericSignatureBuilder::PotentialArchetype *nestedPA,
    GenericSignatureBuilder &builder,
    llvm::function_ref<ProtocolConformanceRef(ProtocolDecl *)>
        lookupConformance) {
  auto concreteParent = parent->getConcreteType();
  assert(concreteParent &&
         "attempting to resolve concrete nested type of non-concrete PA");

  // These requirements are all implied based on the parent's concrete
  // conformance.
  auto source = parentConcreteSource->viaConcrete(builder, /*FIXME: */nullptr)
    ->viaParent(builder);
  auto assocType = nestedPA->getResolvedAssociatedType();
  if (!assocType) return;

  // FIXME: Get the conformance from the parent.
  auto conformance = lookupConformance(assocType->getProtocol());

  Type witnessType;
  if (conformance.isConcrete()) {
    witnessType = conformance.getConcrete()
                      ->getTypeWitness(assocType, builder.getLazyResolver())
                      .getReplacement();
  } else {
    witnessType = DependentMemberType::get(concreteParent, assocType);
  }

  builder.addSameTypeRequirement(nestedPA, witnessType, source,
     [&](Type type1, Type type2) {
       builder.getASTContext().Diags.diagnose(
                              source->getLoc(),
                              diag::requires_same_type_conflict,
                              nestedPA->getDependentType(/*FIXME: */{ }, true),
                              type1, type2);
     });
}

auto GenericSignatureBuilder::PotentialArchetype::getNestedType(
       Identifier nestedName,
       GenericSignatureBuilder &builder) -> PotentialArchetype * {
  // If we already have a nested type with this name, return it.
  if (!NestedTypes[nestedName].empty()) {
    return NestedTypes[nestedName].front();
  }

  // Find the same nested type within the representative (unless we are
  // the representative, of course!).
  PotentialArchetype *repNested = nullptr;
  auto rep = getRepresentative();
  if (rep != this)
    repNested = rep->getNestedType(nestedName, builder);

  auto sameNestedTypeSource =
    RequirementSource::forNestedTypeNameMatch(builder);

  // Attempt to resolve this nested type to an associated type
  // of one of the protocols to which the parent potential
  // archetype conforms.
  SmallVector<std::pair<ProtocolDecl *, const RequirementSource *>, 4>
    conformsTo(rep->ConformsTo.begin(), rep->ConformsTo.end());
  for (auto &conforms : conformsTo) {
    auto proto = conforms.first;

    for (auto member : proto->lookupDirect(nestedName)) {
      PotentialArchetype *pa;
      std::function<void(Type, Type)> diagnoseMismatch;

      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        // Resolve this nested type to this associated type.
        pa = new PotentialArchetype(this, assocType);

        diagnoseMismatch = [&](Type first, Type second) {
          llvm_unreachable(
              "associated type shouldn't result in new mismatches");
        };
      } else if (auto alias = dyn_cast<TypeAliasDecl>(member)) {
        // Resolve this nested type to this type alias.
        pa = new PotentialArchetype(this, alias);

        diagnoseMismatch = [&](Type first, Type second) {
          if (auto NAT = dyn_cast<NameAliasType>(first.getPointer())) {
            if (NAT->getDecl() == member) {
              // If we have typealias T = Foo and Foo is completely concrete
              // (e.g. Array<Int?>), then the subst will leave the NameAliasType
              // intact. However, this means, if there's a
              // concrete-type-mismatch at the top level, the default error
              // message will be "ProtocolName.T (aka Foo)", but the "T" bit is
              // already in the error message so it's better to print only
              // "Foo".
              first = NAT->getSinglyDesugaredType();
            }
          }
          builder.Diags.diagnose(member->getLoc(),
                                 diag::protocol_typealias_conflict,
                                 member->getName(), first, second);
        };

        // FIXME (recursive decl validation): if the alias doesn't have an
        // interface type when getNestedType is called while building a
        // protocol's generic signature (i.e. during validation), then it'll
        // fail completely, because building that alias's interface type
        // requires the protocol to be validated. This seems to occur when the
        // alias's RHS involves archetypes from the protocol.
        if (!alias->hasInterfaceType())
          builder.getLazyResolver()->resolveDeclSignature(alias);
        if (!alias->hasInterfaceType())
          continue;

        // The protocol typealias has an underlying type written in terms
        // of the protocol's 'Self' type.
        auto type = alias->getDeclaredInterfaceType();

        // Substitute in the type of the current PotentialArchetype in
        // place of 'Self' here.
        auto subMap = SubstitutionMap::getProtocolSubstitutions(
          proto, getDependentType(/*genericParams=*/{},
                                  /*allowUnresolved=*/true),
          ProtocolConformanceRef(proto));
        type = type.subst(subMap, SubstFlags::UseErrorType);

        builder.addSameTypeRequirement(ResolvedType::forNewTypeAlias(pa),
                                       builder.resolve(type),
                                       sameNestedTypeSource, diagnoseMismatch);
      } else
        continue;

      // If we have resolved this nested type to more than one associated
      // type, create same-type constraints between them.
      llvm::TinyPtrVector<PotentialArchetype *> &nested =
          NestedTypes[nestedName];
      if (!nested.empty()) {
        nested.push_back(pa);

        // Produce a same-type constraint between the two same-named
        // potential archetypes.
        builder.addSameTypeRequirement(pa, nested.front(), sameNestedTypeSource,
                                       diagnoseMismatch);
      } else {
        nested.push_back(pa);

        if (repNested) {
          builder.addSameTypeRequirement(pa, repNested, sameNestedTypeSource,
                                         diagnoseMismatch);
        }
      }

      // If there's a superclass constraint that conforms to the protocol,
      // add the appropriate same-type relationship.
      auto superSource = builder.resolveSuperConformance(this, conforms.first,
                                                         conforms.second);
      maybeAddSameTypeRequirementForNestedType(pa, superSource, builder);
    }
  }

  // We couldn't resolve the nested type yet, so create an
  // unresolved associated type.
  llvm::TinyPtrVector<PotentialArchetype *> &nested = NestedTypes[nestedName];
  if (nested.empty()) {
    nested.push_back(new PotentialArchetype(this, nestedName));
    ++builder.Impl->NumUnresolvedNestedTypes;
  }

  auto nestedPA = nested.front();

  // We know something concrete about the parent PA, so we need to propagate
  // that information to this new archetype.
  if (isConcreteType()) {
    for (auto equivT : rep->EquivalenceClass) {
      concretizeNestedTypeFromConcreteParent(
          equivT, sameNestedTypeSource, nestedPA, builder,
          [&](ProtocolDecl *proto) -> ProtocolConformanceRef {
            auto depTy = nestedPA->getDependentType({},
                                                    /*allowUnresolved=*/true)
                             ->getCanonicalType();
            auto protocolTy =
                proto->getDeclaredInterfaceType()->castTo<ProtocolType>();
            auto conformance = builder.getLookupConformanceFn()(
                depTy, getConcreteType(), protocolTy);
            assert(conformance &&
                   "failed to find PA's conformance to known protocol");
            return *conformance;
          });
    }
  }

  return nestedPA;
}

auto GenericSignatureBuilder::PotentialArchetype::getNestedType(
                            AssociatedTypeDecl *assocType,
                            GenericSignatureBuilder &builder) -> PotentialArchetype * {
  // Add the requirement that this type conform to the protocol of the
  // associated type. We treat this as "inferred" because it comes from the
  // structure of the type---there will be an explicit or implied requirement
  // somewhere else.
  bool failed = builder.addConformanceRequirement(
                  this, assocType->getProtocol(),
                  RequirementSource::forInferred(builder, nullptr));
  (void)failed;

  // Trigger the construction of nested types with this name.
  auto fallback = getNestedType(assocType->getName(), builder);

  // Find the nested type that resolved to this associated type.
  for (const auto &nested : NestedTypes[assocType->getName()]) {
    if (nested->getResolvedAssociatedType() == assocType) return nested;
  }

  assert(failed && "unable to find nested type that we know is there");
  return fallback;
}

Type GenericSignatureBuilder::PotentialArchetype::getTypeInContext(
                                               GenericSignatureBuilder &builder,
                                               GenericEnvironment *genericEnv) {
  ArrayRef<GenericTypeParamType *> genericParams =
    genericEnv->getGenericParams();

  // Retrieve the archetype from the archetype anchor in this equivalence class.
  // The anchor must not have any concrete parents (otherwise we would just
  // use the representative).
  auto archetypeAnchor = getArchetypeAnchor(builder);
  if (archetypeAnchor != this)
    return archetypeAnchor->getTypeInContext(builder, genericEnv);

  auto representative = getRepresentative();
  ASTContext &ctx = genericEnv->getGenericSignature()->getASTContext();

  // Return a concrete type or archetype we've already resolved.
  if (Type concreteType = representative->getConcreteType()) {
    // Otherwise, substitute in the archetypes in the environment.
    // If this has a recursive type, return an error type.
    if (representative->RecursiveConcreteType) {
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));
    }

    return genericEnv->mapTypeIntoContext(concreteType,
                                          builder.getLookupConformanceFn());
  }

  // Local function to check whether we have a generic parameter that has
  // already been recorded
  auto getAlreadyRecoveredGenericParam = [&]() -> Type {
    if (!isGenericParam()) return Type();

    auto type = genericEnv->getMappingIfPresent(getGenericParamKey());
    if (!type) return Type();

    // We already have a mapping for this generic parameter in the generic
    // environment. Return it.
    return *type;
  };

  AssociatedTypeDecl *assocType = nullptr;
  ArchetypeType *ParentArchetype = nullptr;
  if (auto parent = getParent()) {
    // For nested types, first substitute into the parent so we can form the
    // proper nested type.
    auto parentTy = parent->getTypeInContext(builder, genericEnv);
    if (!parentTy)
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));

    ParentArchetype = parentTy->getAs<ArchetypeType>();
    if (!ParentArchetype) {
      LazyResolver *resolver = ctx.getLazyResolver();
      assert(resolver && "need a lazy resolver");
      (void) resolver;

      // Resolve the member type.
      auto type = getDependentType(genericParams, /*allowUnresolved=*/false);
      if (type->hasError())
        return type;

      auto depMemberType = type->castTo<DependentMemberType>();
      Type memberType =
        depMemberType->substBaseType(parentTy,
                                     builder.getLookupConformanceFn());

      // If the member type maps to an archetype, resolve that archetype.
      if (auto memberPA = builder.resolveArchetype(memberType)) {
        if (memberPA->getRepresentative() != representative) {
          return memberPA->getTypeInContext(builder, genericEnv);
        }

        llvm_unreachable("we have no parent archetype");
      }


      // Otherwise, it's a concrete type.
      return genericEnv->mapTypeIntoContext(memberType,
                                            builder.getLookupConformanceFn());
    }

    // Check whether the parent already has a nested type with this name. If
    // so, return it directly.
    if (auto nested = ParentArchetype->getNestedTypeIfKnown(getNestedName()))
      return *nested;

    // We will build the archetype below.
    assocType = getResolvedAssociatedType();
  } else if (auto result = getAlreadyRecoveredGenericParam()) {
    return result;
  }

  // Determine the superclass for the archetype. If it exists and involves
  // type parameters, substitute them.
  Type superclass = representative->getSuperclass();
  if (superclass && superclass->hasTypeParameter()) {
    if (representative->RecursiveSuperclassType) {
      superclass = ErrorType::get(superclass);
    } else {
      superclass = genericEnv->mapTypeIntoContext(
                                              superclass,
                                              builder.getLookupConformanceFn());

      // We might have recursively recorded the archetype; if so, return early.
      // FIXME: This should be detectable before we end up building archetypes.
      if (auto result = getAlreadyRecoveredGenericParam())
        return result;
    }
  }

  LayoutConstraint layout = representative->getLayout();

  // Build a new archetype.

  // Collect the protocol conformances for the archetype.
  SmallVector<ProtocolDecl *, 4> Protos;
  for (const auto &conforms : representative->getConformsTo()) {
    if (conforms.second->kind != RequirementSource::Superclass)
      Protos.push_back(conforms.first);
  }

  // Create the archetype.
  //
  // Note that we delay the computation of the superclass until after we
  // create the archetype, in case the superclass references the archetype
  // itself.
  ArchetypeType *arch;
  if (ParentArchetype) {
    // If we were unable to resolve this as an associated type, produce an
    // error type.
    if (!assocType) {
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));
    }

    // Create a nested archetype.
    arch = ArchetypeType::getNew(ctx, ParentArchetype, assocType, Protos,
                                 superclass, layout);

    // Register this archetype with its parent.
    ParentArchetype->registerNestedType(getNestedName(), arch);
  } else {
    // Create a top-level archetype.
    Identifier name =
      genericParams[getGenericParamKey().findIndexIn(genericParams)]->getName();
    arch = ArchetypeType::getNew(ctx, genericEnv, name, Protos,
                                 superclass, layout);

    // Register the archetype with the generic environment.
    genericEnv->addMapping(getGenericParamKey(), arch);
  }

  return arch;
}

void ArchetypeType::resolveNestedType(
                                    std::pair<Identifier, Type> &nested) const {
  auto genericEnv = getGenericEnvironment();
  auto &builder = *genericEnv->getGenericSignatureBuilder();

  Type interfaceType =
    genericEnv->mapTypeOutOfContext(const_cast<ArchetypeType *>(this));
  auto parentPA = builder.resolveArchetype(interfaceType);
  auto memberPA = parentPA->getNestedType(nested.first, builder);
  auto result = memberPA->getTypeInContext(builder, genericEnv);
  assert(!nested.second ||
         nested.second->isEqual(result) ||
         (nested.second->hasError() && result->hasError()));
  nested.second = result;
}

Type GenericSignatureBuilder::PotentialArchetype::getDependentType(
                                ArrayRef<GenericTypeParamType *> genericParams,
                                bool allowUnresolved) {
  if (auto parent = getParent()) {
    Type parentType = parent->getDependentType(genericParams,
                                               allowUnresolved);
    if (parentType->hasError())
      return parentType;

    // If we've resolved to an associated type, use it.
    if (auto assocType = getResolvedAssociatedType())
      return DependentMemberType::get(parentType, assocType);

    // If we don't allow unresolved dependent member types, fail.
    if (!allowUnresolved)
      return ErrorType::get(getDependentType(genericParams,
                                             /*allowUnresolved=*/true));

    return DependentMemberType::get(parentType, getNestedName());
  }
  
  assert(isGenericParam() && "Not a generic parameter?");

  // FIXME: This is a temporary workaround.
  if (genericParams.empty())
    genericParams = getBuilder()->Impl->GenericParams;

  unsigned index = getGenericParamKey().findIndexIn(genericParams);
  return genericParams[index];
}

void GenericSignatureBuilder::PotentialArchetype::dump() const {
  dump(llvm::errs(), nullptr, 0);
}

void GenericSignatureBuilder::PotentialArchetype::dump(llvm::raw_ostream &Out,
                                                       SourceManager *SrcMgr,
                                                       unsigned Indent) const {
  // Print name.
  if (Indent == 0 || isGenericParam())
    Out << getDebugName();
  else
    Out.indent(Indent) << getNestedName();

  // Print superclass.
  if (Superclass) {
    Out << " : ";
    Superclass.print(Out);
    Out << " ";
    if (!SuperclassSource->isDerivedRequirement())
      Out << "*";
    Out << "[";
    SuperclassSource->print(Out, SrcMgr);
    Out << "]";
  }

  // Print concrete type.
  if (ConcreteType) {
    Out << " == ";
    ConcreteType.print(Out);
    if (ConcreteTypeSource) {
      Out << " ";
      if (!ConcreteTypeSource->isDerivedRequirement())
        Out << "*";
      Out << "[";
      ConcreteTypeSource->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print requirements.
  if (!ConformsTo.empty()) {
    Out << " : ";

    bool First = true;
    for (const auto &ProtoAndSource : ConformsTo) {
      if (First)
        First = false;
      else
        Out << " & ";

      Out << ProtoAndSource.first->getName().str() << " ";
      if (!ProtoAndSource.second->isDerivedRequirement())
        Out << "*";
      Out << "[";
      ProtoAndSource.second->print(Out, SrcMgr);
      Out << "]";
    }
  }

  if (Representative != this) {
    Out << " [represented by " << getRepresentative()->getDebugName() << "]";
  }

  if (EquivalenceClass.size() > 1) {
    Out << " [equivalence class ";
    bool isFirst = true;
    for (auto equiv : EquivalenceClass) {
      if (equiv == this) continue;

      if (isFirst) isFirst = false;
      else Out << ", ";

      Out << equiv->getDebugName();
    }
    Out << "]";
  }

  Out << "\n";

  // Print nested types.
  for (const auto &nestedVec : NestedTypes) {
    for (auto nested : nestedVec.second) {
      nested->dump(Out, SrcMgr, Indent + 2);
    }
  }
}

GenericSignatureBuilder::GenericSignatureBuilder(
                               ASTContext &ctx,
                               std::function<GenericFunction> lookupConformance)
  : Context(ctx), Diags(Context.Diags), Impl(new Implementation) {
  Impl->LookupConformance = std::move(lookupConformance);
}

GenericSignatureBuilder::GenericSignatureBuilder(GenericSignatureBuilder &&) = default;

GenericSignatureBuilder::~GenericSignatureBuilder() {
  if (!Impl)
    return;

  for (auto PA : Impl->PotentialArchetypes)
    delete PA;
}

std::function<GenericFunction>
GenericSignatureBuilder::getLookupConformanceFn() const {
  return Impl->LookupConformance;
}

LazyResolver *GenericSignatureBuilder::getLazyResolver() const { 
  return Context.getLazyResolver();
}

auto GenericSignatureBuilder::resolveArchetype(Type type) -> PotentialArchetype * {
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    unsigned index = GenericParamKey(genericParam).findIndexIn(
                                                           Impl->GenericParams);
    if (index < Impl->GenericParams.size())
      return Impl->PotentialArchetypes[index];

    return nullptr;
  }

  if (auto dependentMember = type->getAs<DependentMemberType>()) {
    auto base = resolveArchetype(dependentMember->getBase());
    if (!base)
      return nullptr;

    if (auto assocType = dependentMember->getAssocType())
      return base->getNestedType(assocType, *this);

    return base->getNestedType(dependentMember->getName(), *this);
  }

  return nullptr;
}

auto GenericSignatureBuilder::resolve(UnresolvedType paOrT,
                                      bool hackTypeFromGenericTypeAlias)
    -> ResolvedType {
  auto pa = paOrT.dyn_cast<PotentialArchetype *>();
  if (auto type = paOrT.dyn_cast<Type>()) {
    pa = resolveArchetype(type);
    if (!pa) {
      if (hackTypeFromGenericTypeAlias)
        return ResolvedType::forConcreteTypeFromGenericTypeAlias(type);
      return ResolvedType::forConcreteType(type);
    }
  }

  auto rep = pa->getRepresentative();
  if (!rep->getParent() || !rep->getTypeAliasDecl())
    return ResolvedType::forPotentialArchetype(pa);

  // We're assuming that an equivalence class with a type alias representative
  // doesn't have a "true" (i.e. associated type) potential archetype.
  assert(llvm::all_of(rep->getEquivalenceClass(),
                      [&](PotentialArchetype *pa) {
                        return pa->getParent() && pa->getTypeAliasDecl();
                      }) &&
         "unexpected typealias representative with non-typealias equivalent");

  // Recursively resolve the concrete type.
  if (auto concrete = pa->getConcreteType())
    return resolve(concrete);

  return ResolvedType::forPotentialArchetype(pa);
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamDecl *GenericParam) {
  addGenericParameter(
     GenericParam->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
}

bool GenericSignatureBuilder::addGenericParameterRequirements(
                                           GenericTypeParamDecl *GenericParam) {
  GenericParamKey Key(GenericParam);
  auto PA = Impl->PotentialArchetypes[Key.findIndexIn(Impl->GenericParams)];
  
  // Add the requirements from the declaration.
  llvm::SmallPtrSet<ProtocolDecl *, 8> visited;
  return addInheritedRequirements(GenericParam, PA, nullptr, visited);
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamType *GenericParam) {
  GenericParamKey Key(GenericParam);
  assert(Impl->GenericParams.empty() ||
         ((Key.Depth == Impl->GenericParams.back()->getDepth() &&
           Key.Index == Impl->GenericParams.back()->getIndex() + 1) ||
          (Key.Depth > Impl->GenericParams.back()->getDepth() &&
           Key.Index == 0)));

  // Create a potential archetype for this type parameter.
  auto PA = new PotentialArchetype(this, GenericParam);
  Impl->GenericParams.push_back(GenericParam);
  Impl->PotentialArchetypes.push_back(PA);
}

bool GenericSignatureBuilder::addConformanceRequirement(PotentialArchetype *PAT,
                                                 ProtocolDecl *Proto,
                                                 const RequirementSource *Source) {
  llvm::SmallPtrSet<ProtocolDecl *, 8> Visited;
  return addConformanceRequirement(PAT, Proto, Source, Visited);
}

/// Visit all of the types that show up in the list of inherited
/// types.
///
/// \returns true if any of the invocations of \c visitor returned true.
static bool visitInherited(
                   ArrayRef<TypeLoc> inheritedTypes,
                   llvm::function_ref<bool(Type, const TypeRepr *)> visitor) {
  // Local function that (recursively) adds inherited types.
  bool isInvalid = false;
  std::function<void(Type, const TypeRepr *)> visitInherited;
  visitInherited = [&](Type inheritedType, const TypeRepr *typeRepr) {
    // Decompose protocol compositions.
    auto composition = dyn_cast_or_null<CompositionTypeRepr>(typeRepr);
    if (auto compositionType
          = inheritedType->getAs<ProtocolCompositionType>()) {
      unsigned index = 0;
      for (auto protoType : compositionType->getProtocols()) {
        if (composition && index < composition->getTypes().size())
          visitInherited(protoType, composition->getTypes()[index]);
        else
          visitInherited(protoType, typeRepr);

        ++index;
      }
      return;
    }

    isInvalid |= visitor(inheritedType, typeRepr);
  };

  // Visit all of the inherited types.
  for (auto inherited : inheritedTypes) {
    visitInherited(inherited.getType(), inherited.getTypeRepr());
  }

  return isInvalid;
}

bool GenericSignatureBuilder::addConformanceRequirement(PotentialArchetype *PAT,
                                                 ProtocolDecl *Proto,
                                                 const RequirementSource *Source,
                               llvm::SmallPtrSetImpl<ProtocolDecl *> &Visited) {
  // Add the requirement to the representative.
  auto T = PAT->getRepresentative();

  // Add the requirement, if we haven't done so already.
  if (!T->addConformance(Proto, /*updateExistingSource=*/true, Source, *this))
    return false;

  bool inserted = Visited.insert(Proto).second;
  assert(inserted);
  (void) inserted;
  SWIFT_DEFER {
    Visited.erase(Proto);
  };

  // Use the requirement signature to avoid rewalking the entire protocol.  This
  // cannot compute the requirement signature directly, because that may be
  // infinitely recursive: this code is also used to construct it.
  if (Proto->isRequirementSignatureComputed()) {
    auto reqSig = Proto->getRequirementSignature();

    auto concreteSelf = T->getDependentType({}, /*allowUnresolved=*/true);
    auto subMap = SubstitutionMap::getProtocolSubstitutions(
        Proto, concreteSelf, ProtocolConformanceRef(Proto));

    auto innerSource = Source->viaAbstractProtocolRequirement(*this, Proto);
    for (auto rawReq : reqSig->getRequirements()) {
      auto req = rawReq.subst(subMap);
      assert(req && "substituting Self in requirement shouldn't fail");
      if (addRequirement(*req, innerSource, Visited))
        return true;
    }

    return false;
  }

  // Add all of the inherited protocol requirements, recursively.
  if (auto resolver = getLazyResolver())
    resolver->resolveInheritedProtocols(Proto);

  if (addInheritedRequirements(Proto, PAT, Source, Visited))
    return true;

  // Add requirements for each of the associated types.
  for (auto Member : getProtocolMembers(Proto)) {
    if (auto AssocType = dyn_cast<AssociatedTypeDecl>(Member)) {
      // Add requirements placed directly on this associated type.
      auto AssocPA = T->getNestedType(AssocType, *this);

      if (AssocPA != T) {
        if (addInheritedRequirements(AssocType, AssocPA, Source, Visited))
          return true;
      }
    } else if (auto TypeAlias = dyn_cast<TypeAliasDecl>(Member)) {
        // FIXME: this should check that the typealias is makes sense (e.g. has
        // the same/compatible type as typealiases in parent protocols) and
        // set-up any same type requirements required. Forcing the PA to be
        // created with getNestedType is currently worse than useless due to the
        // 'recursive decl validation' FIXME in that function: it creates an
        // unresolved PA that prints an error later.
      (void)TypeAlias;
    }

    // FIXME: Requirement declarations.
  }

  return false;
}

bool GenericSignatureBuilder::addLayoutRequirement(PotentialArchetype *PAT,
                                            LayoutConstraint Layout,
                                            const RequirementSource *Source) {
  // Add the requirement to the representative.
  auto T = PAT->getRepresentative();

  if (T->Layout) {
    if (T->Layout == Layout) {
      // Update the source.
      T->LayoutSource = Source;
      return false;
    }
    // There is an existing layout constraint for this archetype.
    Diags.diagnose(Source->getLoc(), diag::mutiple_layout_constraints,
                   Layout, T->Layout);
    Diags.diagnose(T->LayoutSource->getLoc(),
                   diag::previous_layout_constraint, T->Layout);
    T->setInvalid();

    return true;
  }

  T->Layout = Layout;
  T->LayoutSource = Source;

  return false;
}

bool GenericSignatureBuilder::addSuperclassRequirement(PotentialArchetype *T,
                                                Type Superclass,
                                                const RequirementSource *Source) {
  T = T->getRepresentative();

  // Make sure the concrete type fulfills the superclass requirement
  // of the archetype.
  if (T->isConcreteType()) {
    Type concrete = T->getConcreteType();
    if (!Superclass->isExactSuperclassOf(concrete, getLazyResolver())) {
      if (auto source = T->findAnyConcreteTypeSourceAsWritten()) {
        Diags.diagnose(source->getLoc(), diag::type_does_not_inherit,
                       T->getDependentType(/*FIXME:*/{ },
                                           /*allowUnresolved=*/true),
                       concrete, Superclass)
          .highlight(Source->getLoc());
      }
      return true;
    }

    return false;
  }

  // Local function to handle the update of superclass conformances
  // when the superclass constraint changes.
  auto updateSuperclassConformances = [&] {
    for (auto &conforms : T->ConformsTo) {
      if (auto superSource = resolveSuperConformance(T, conforms.first,
                                                     conforms.second)) {
        for (auto req : getProtocolMembers(conforms.first)) {
          auto assocType = dyn_cast<AssociatedTypeDecl>(req);
          if (!assocType) continue;

          const auto &nestedTypes = T->getNestedTypes();
          auto nested = nestedTypes.find(assocType->getName());
          if (nested == nestedTypes.end()) continue;

          for (auto nestedPA : nested->second) {
            if (nestedPA->getResolvedAssociatedType() == assocType)
              maybeAddSameTypeRequirementForNestedType(nestedPA, superSource,
                                                       *this);
          }
        }
      }
    }
  };

  // If T already has a superclass, make sure it's related.
  if (T->Superclass) {
    // TODO: In principle, this could be isBindableToSuperclassOf instead of
    // isExactSubclassOf. If you had:
    //
    //   class Foo<T>
    //   class Bar: Foo<Int>
    //
    //   func foo<T, U where U: Foo<T>, U: Bar>(...) { ... }
    //
    // then the second constraint should be allowed, constraining U to Bar
    // and secondarily imposing a T == Int constraint.
    if (T->Superclass->isExactSuperclassOf(Superclass, nullptr)) {
      T->Superclass = Superclass;

      // We've strengthened the bound, so update superclass conformances.
      updateSuperclassConformances();
    // TODO: Similar to the above, a more general isBindableToSuperclassOf
    // base class constraint could potentially introduce secondary constraints.
    // If you had:
    //
    //   class Foo<T>
    //   class Bar: Foo<Int>
    //
    //   func foo<T, U where U: Bar, U: Foo<T>>(...) { ... }
    //
    // then the second `U: Foo<T>` constraint introduces a `T == Int`
    // constraint.
    } else if (!Superclass->isExactSuperclassOf(T->Superclass, nullptr)) {
      if (Source->getLoc().isValid()) {
        // Figure out what kind of subject we have; it will affect the
        // diagnostic.
        auto subjectType = T->getDependentType(/*FIXME: */{ }, true);
        unsigned kind;
        if (auto gp = subjectType->getAs<GenericTypeParamType>()) {
          if (gp->getDecl() &&
              isa<ProtocolDecl>(gp->getDecl()->getDeclContext())) {
            kind = 1;
            subjectType = cast<ProtocolDecl>(gp->getDecl()->getDeclContext())
                            ->getDeclaredInterfaceType();
          } else {
            kind = 0;
          }
        } else {
          kind = 2;
        }

        Diags.diagnose(Source->getLoc(), diag::requires_superclass_conflict,
                       kind, subjectType, T->Superclass, Superclass)
          .highlight(T->SuperclassSource->getLoc());
      }
      return true;
    }

    updateRequirementSource(T->SuperclassSource, Source);
    return false;
  }

  // Set the superclass.
  T->Superclass = Superclass;
  T->SuperclassSource = Source;

  // Update based on these conformances.
  updateSuperclassConformances();
  return false;
}

void GenericSignatureBuilder::PotentialArchetype::addSameTypeConstraint(
                                             PotentialArchetype *otherPA,
                                             const RequirementSource *source) {
  // If the types are the same, there's nothing to do.
  if (this == otherPA) return;

  // Update the same-type constraints of this PA to reference the other PA.
  auto insertedIntoThis = SameTypeConstraints.insert({otherPA, source});
  if (!insertedIntoThis.second) {
    getBuilder()->updateRequirementSource(insertedIntoThis.first->second,
                                          source);
  }

  // Update the same-type constraints of the other PA to reference this PA.
  auto insertedIntoOther = otherPA->SameTypeConstraints.insert({this, source});
  if (!insertedIntoOther.second) {
    getBuilder()->updateRequirementSource(insertedIntoOther.first->second,
                                          source);
  }
}

bool GenericSignatureBuilder::addSameTypeRequirementBetweenArchetypes(
       PotentialArchetype *OrigT1,
       PotentialArchetype *OrigT2,
       const RequirementSource *Source) 
{
  // Record the same-type constraint.
  OrigT1->addSameTypeConstraint(OrigT2, Source);

  // Operate on the representatives
  auto T1 = OrigT1->getRepresentative();
  auto T2 = OrigT2->getRepresentative();

  // If the representatives are already the same, we're done.
  if (T1 == T2)
    return false;

  // Decide which potential archetype is to be considered the representative.
  // It doesn't specifically matter which we use, but it's a minor optimization
  // to prefer the canonical type.
  if (compareDependentTypes(&T2, &T1) < 0)
    std::swap(T1, T2);

  // Merge any concrete constraints.
  Type concrete1 = T1->getConcreteType();
  Type concrete2 = T2->getConcreteType();
  
  if (concrete1 && concrete2) {
    bool mismatch = addSameTypeRequirement(
        concrete1, concrete2, Source, [&](Type type1, Type type2) {
          Diags.diagnose(Source->getLoc(),
                         diag::requires_same_type_conflict,
                         T1->getDependentType(/*FIXME: */{ }, true), type1,
                         type2);
        });

    if (mismatch) return true;
  } else if (concrete2) {
    assert(!T1->ConcreteType
           && "already formed archetype for concrete-constrained parameter");
    T1->ConcreteType = concrete2;
  }

  // Don't mark requirements as redundant if they come from one of our
  // child archetypes. This is a targeted fix -- more general cases
  // continue to break. In general, we need to detect cycles in the
  // archetype graph and not propagate requirement source information
  // along back edges.
  bool updateExistingSource = true;
  auto T2Parent = T2;
  while (T2Parent != nullptr) {
    if (T2Parent->getRepresentative() == T1)
      updateExistingSource = false;
    T2Parent = T2Parent->getParent();
  }

  // Another targeted fix -- don't drop conformances from generic
  // parameters.
  if (T1->getParent() == nullptr)
    updateExistingSource = false;

  // Make T1 the representative of T2, merging the equivalence classes.
  T2->Representative = T1;
  for (auto equiv : T2->EquivalenceClass)
    T1->EquivalenceClass.push_back(equiv);

  // Superclass requirements.
  if (T2->Superclass) {
    addSuperclassRequirement(T1, T2->getSuperclass(),
                             T2->getSuperclassSource());
  }

  // Add all of the protocol conformance requirements of T2 to T1.
  for (auto conforms : T2->ConformsTo) {
    T1->addConformance(conforms.first, updateExistingSource,
                       conforms.second, *this);
  }

  // Recursively merge the associated types of T2 into T1.
  auto sameNestedTypeSource = RequirementSource::forNestedTypeNameMatch(*this);
  for (auto equivT2 : T2->EquivalenceClass) {
    for (auto T2Nested : equivT2->NestedTypes) {
      auto T1Nested = T1->getNestedType(T2Nested.first, *this);
      if (addSameTypeRequirement(T1Nested, T2Nested.second.front(),
                                 sameNestedTypeSource,
                                 [&](Type type1, Type type2) {
            Diags.diagnose(Source->getLoc(),
                           diag::requires_same_type_conflict,
                           T1Nested->getDependentType(/*FIXME: */{ }, true),
                           type1, type2);
            }))
        return true;
    }
  }

  return false;
}

bool GenericSignatureBuilder::addSameTypeRequirementToConcrete(
       PotentialArchetype *T,
       Type Concrete,
       const RequirementSource *Source) {
  auto rep = T->getRepresentative();

  // If there is an existing source on this potential archetype, make sure
  // we have the same type.
  // FIXME: Delay until finalize().
  if (auto existingSource = T->ConcreteTypeSource) {
    bool mismatch = addSameTypeRequirement(
        T->ConcreteType, Concrete, Source, [&](Type type1, Type type2) {
          Diags.diagnose(Source->getLoc(),
                         diag::requires_same_type_conflict,
                         T->getDependentType(/*FIXME: */{ }, true), type1,
                         type2);
        });

    if (mismatch) return true;

    // If this is a better source, record it.
    updateRequirementSource(T->ConcreteTypeSource, Source);

    if (!rep->ConcreteType)
      rep->ConcreteType = Concrete;

    return false;
  }

  // If we've already been bound to a type, we're either done, or we have a
  // problem.
  // FIXME: Move, to finalize().
  if (T != rep) {
    if (auto oldConcrete = rep->getConcreteType()) {
      bool mismatch = addSameTypeRequirement(
          oldConcrete, Concrete, Source, [&](Type type1, Type type2) {
            Diags.diagnose(Source->getLoc(),
                           diag::requires_same_type_conflict,
                           T->getDependentType(/*FIXME: */{ }, true), type1,
                           type2);

          });

      if (mismatch) return true;
      return false;
    }
  }

  // Record the concrete type and its source.
  T->ConcreteType = Concrete;
  T->ConcreteTypeSource = Source;

  // Make sure the concrete type fulfills the requirements on the archetype.
  // FIXME: Move later...
  DenseMap<ProtocolDecl *, ProtocolConformanceRef> conformances;
  CanType depTy = rep->getDependentType({ }, /*allowUnresolved=*/true)
                    ->getCanonicalType();
  for (auto &conforms : rep->getConformsTo()) {
    auto protocol = conforms.first;
    auto conformance =
      getLookupConformanceFn()(depTy, Concrete,
                               protocol->getDeclaredInterfaceType()
                                 ->castTo<ProtocolType>());
    if (!conformance) {
      Diags.diagnose(Source->getLoc(),
                     diag::requires_generic_param_same_type_does_not_conform,
                     Concrete, protocol->getName());
      return true;
    }

    conformances.insert({protocol, *conformance});

    // Abstract conformances are acceptable for existential types.
    assert(conformance->isConcrete() || Concrete->isExistentialType());

    // Update the requirement source now that we know it's concrete.
    // FIXME: Bad concrete source info.
    auto concreteSource = Source->viaConcrete(*this,
                                              conformance->isConcrete()
                                                ? conformance->getConcrete()
                                                : nullptr);
    updateRequirementSource(conforms.second, concreteSource);
  }

  // Record the requirement.
  rep->ConcreteType = Concrete;

  // Make sure the concrete type fulfills the superclass requirement
  // of the archetype.
  if (rep->Superclass) {
    if (!rep->Superclass->isExactSuperclassOf(Concrete, getLazyResolver())) {
      Diags.diagnose(Source->getLoc(), diag::type_does_not_inherit,
                     rep->getDependentType(/*FIXME: */{ },
                                         /*allowUnresolved=*/true),
                     Concrete, rep->Superclass)
        .highlight(rep->SuperclassSource->getLoc());
      return true;
    }

    // The superclass requirement is made redundant by the concrete type
    // assignment.
    auto concreteSource = Source->viaConcrete(*this, nullptr);
    updateRequirementSource(rep->SuperclassSource, concreteSource);
  }

  // Eagerly resolve any existing nested types to their concrete forms (others
  // will be "concretized" as they are constructed, in getNestedType).
  for (auto equivT : rep->EquivalenceClass) {
    for (auto nested : equivT->getNestedTypes()) {
      concretizeNestedTypeFromConcreteParent(
          equivT, Source, nested.second.front(), *this,
          [&](ProtocolDecl *proto) -> ProtocolConformanceRef {
            return conformances.find(proto)->second;
          });
    }
  }

  return false;
}

bool GenericSignatureBuilder::addSameTypeRequirementBetweenConcrete(
    Type type1, Type type2, const RequirementSource *source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  // Local class to handle matching the two sides of the same-type constraint.
  class ReqTypeMatcher : public TypeMatcher<ReqTypeMatcher> {
    GenericSignatureBuilder &builder;
    const RequirementSource *source;
    Type outerType1, outerType2;
    llvm::function_ref<void(Type, Type)> diagnoseMismatch;

  public:
    ReqTypeMatcher(GenericSignatureBuilder &builder, const RequirementSource *source,
                   Type outerType1, Type outerType2,
                   llvm::function_ref<void(Type, Type)> diagnoseMismatch)
        : builder(builder), source(source), outerType1(outerType1),
          outerType2(outerType2), diagnoseMismatch(diagnoseMismatch) {}

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      // If the mismatch was in the first layer (i.e. what was fed to
      // addSameTypeRequirementBetweenConcrete), then this is a fundamental
      // mismatch, and we need to diagnose it. This is what breaks the mutual
      // recursion between addSameTypeRequirement and
      // addSameTypeRequirementBetweenConcrete.
      if (outerType1->isEqual(firstType) && outerType2->isEqual(secondType)) {
        diagnoseMismatch(sugaredFirstType, secondType);
        return false;
      }
      auto failed = builder.addSameTypeRequirement(
          sugaredFirstType, Type(secondType), source, diagnoseMismatch);
      return !failed;
    }
  } matcher(*this, source, type1, type2, diagnoseMismatch);

  return !matcher.match(type1, type2);
}

bool GenericSignatureBuilder::addSameTypeRequirement(UnresolvedType paOrT1,
                                                     UnresolvedType paOrT2,
                                                     const RequirementSource *source) {
  return addSameTypeRequirement(resolve(paOrT1), resolve(paOrT2), source);
}
bool GenericSignatureBuilder::addSameTypeRequirement(
    UnresolvedType paOrT1, UnresolvedType paOrT2, const RequirementSource *source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  return addSameTypeRequirement(resolve(paOrT1), resolve(paOrT2), source,
                                diagnoseMismatch);
}
bool GenericSignatureBuilder::addSameTypeRequirement(ResolvedType paOrT1,
                                                     ResolvedType paOrT2,
                                                     const RequirementSource *source) {
  return addSameTypeRequirement(paOrT1, paOrT2, source,
                                [&](Type type1, Type type2) {
    Diags.diagnose(source->getLoc(), diag::requires_same_concrete_type,
                   type1, type2);
  });
}

bool GenericSignatureBuilder::addSameTypeRequirement(
    ResolvedType paOrT1, ResolvedType paOrT2, const RequirementSource *source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  auto pa1 = paOrT1.getPotentialArchetype();
  auto pa2 = paOrT2.getPotentialArchetype();
  auto t1 = paOrT1.getType();
  auto t2 = paOrT2.getType();

  // If both sides of the requirement are type parameters, equate them.
  if (pa1 && pa2) {
    return addSameTypeRequirementBetweenArchetypes(pa1, pa2, source);
    // If just one side is a type parameter, map it to a concrete type.
  } else if (pa1) {
    return addSameTypeRequirementToConcrete(pa1, t2, source);
  } else if (pa2) {
    return addSameTypeRequirementToConcrete(pa2, t1, source);
  } else {
    return addSameTypeRequirementBetweenConcrete(t1, t2, source,
                                                 diagnoseMismatch);
  }
}

// Local function to mark the given associated type as recursive,
// diagnosing it if this is the first such occurrence.
void GenericSignatureBuilder::markPotentialArchetypeRecursive(
    PotentialArchetype *pa, ProtocolDecl *proto, const RequirementSource *source) {
  if (pa->isRecursive())
    return;
  pa->setIsRecursive();

  // FIXME: Drop this protocol.
  pa->addConformance(proto, /*updateExistingSource=*/true, source, *this);
  if (!pa->getParent())
    return;

  auto assocType = pa->getResolvedAssociatedType();
  if (!assocType || assocType->isInvalid())
    return;

  Diags.diagnose(assocType->getLoc(), diag::recursive_requirement_reference);

  // Silence downstream errors referencing this associated type.
  assocType->setInvalid();
}

bool GenericSignatureBuilder::addInheritedRequirements(
                             TypeDecl *decl,
                             PotentialArchetype *pa,
                             const RequirementSource *parentSource,
                             llvm::SmallPtrSetImpl<ProtocolDecl *> &visited) {
  if (isa<AssociatedTypeDecl>(decl) &&
      decl->hasInterfaceType() &&
      decl->getInterfaceType()->is<ErrorType>())
    return false;

  // Walk the 'inherited' list to identify requirements.
  if (auto resolver = getLazyResolver())
    resolver->resolveInheritanceClause(decl);

  return visitInherited(
                    decl->getInherited(),
                    [&](Type inheritedType, const TypeRepr *typeRepr) -> bool {
    // Local function to get the source.
    auto getSource = [&] {
      if (parentSource) {
        if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
          // FIXME: Pass along the typeRepr!
          auto proto = assocType->getProtocol();
          return parentSource->viaAbstractProtocolRequirement(*this, proto);
        }

        // FIXME: Pass along the typeRepr.
        auto proto = cast<ProtocolDecl>(decl);
        return parentSource->viaAbstractProtocolRequirement(*this, proto);
      }

      // Explicit requirement.
      if (typeRepr)
        return RequirementSource::forExplicit(*this, typeRepr);

      // An abstract explicit requirement.
      return RequirementSource::forAbstract(*this);
    };

    // Protocol requirement.
    if (auto protocolType = inheritedType->getAs<ProtocolType>()) {
      if (visited.count(protocolType->getDecl())) {
        markPotentialArchetypeRecursive(pa, protocolType->getDecl(),
                                        getSource());

        return true;
      }

      return addConformanceRequirement(pa, protocolType->getDecl(), getSource(),
                                       visited);
    }

    // Superclass requirement.
    if (inheritedType->getClassOrBoundGenericClass()) {
      return addSuperclassRequirement(pa, inheritedType, getSource());
    }

    // Note: anything else is an error, to be diagnosed later.
    return false;
  });
}

bool GenericSignatureBuilder::addRequirement(const RequirementRepr *Req) {
  auto source = RequirementSource::forExplicit(*this, Req);

  switch (Req->getKind()) {
  case RequirementReprKind::LayoutConstraint: {
    // FIXME: Need to do something here.
    PotentialArchetype *PA = resolveArchetype(Req->getSubject());
    if (!PA) {
      // FIXME: Poor location information.
      // FIXME: Delay diagnostic until after type validation?
      Diags.diagnose(Req->getColonLoc(), diag::requires_not_suitable_archetype,
                     0, Req->getSubjectLoc(), 0);
      return true;
    }

    if (addLayoutRequirement(PA, Req->getLayoutConstraint(), source))
      return true;

    return false;
  }

  case RequirementReprKind::TypeConstraint: {
    PotentialArchetype *PA = resolveArchetype(Req->getSubject());
    if (!PA) {
      // FIXME: Poor location information.
      // FIXME: Delay diagnostic until after type validation?
      Diags.diagnose(Req->getColonLoc(), diag::requires_not_suitable_archetype,
                     0, Req->getSubjectLoc(), 0);
      return true;
    }

    // Check whether this is a supertype requirement.
    if (Req->getConstraint()->getClassOrBoundGenericClass()) {
      return addSuperclassRequirement(PA, Req->getConstraint(), source);
    }

    SmallVector<ProtocolDecl *, 4> ConformsTo;
    if (!Req->getConstraint()->isExistentialType(ConformsTo)) {
      // FIXME: Diagnose this failure here, rather than over in type-checking.
      return true;
    }

    // Add each of the protocols.
    for (auto Proto : ConformsTo)
      if (addConformanceRequirement(PA, Proto, source))
        return true;

    return false;
  }

  case RequirementReprKind::SameType:
    // Require that at least one side of the requirement contain a type
    // parameter.
    if (!Req->getFirstType()->hasTypeParameter() &&
        !Req->getSecondType()->hasTypeParameter()) {
      Diags.diagnose(Req->getEqualLoc(), diag::requires_no_same_type_archetype)
        .highlight(Req->getFirstTypeLoc().getSourceRange())
        .highlight(Req->getSecondTypeLoc().getSourceRange());
      return true;
    }

    return addRequirement(Requirement(RequirementKind::SameType,
                                      Req->getFirstType(),
                                      Req->getSecondType()),
                          source);
  }

  llvm_unreachable("Unhandled requirement?");
}

bool GenericSignatureBuilder::addRequirement(const Requirement &req,
                                      const RequirementSource *source) {
  llvm::SmallPtrSet<ProtocolDecl *, 8> Visited;
  return addRequirement(req, source, Visited);
}

bool GenericSignatureBuilder::addRequirement(
    const Requirement &req, const RequirementSource *source,
    llvm::SmallPtrSetImpl<ProtocolDecl *> &Visited) {
  switch (req.getKind()) {
  case RequirementKind::Superclass: {
    // FIXME: Diagnose this.
    PotentialArchetype *pa = resolveArchetype(req.getFirstType());
    if (!pa) return false;

    assert(req.getSecondType()->getClassOrBoundGenericClass());
    return addSuperclassRequirement(pa, req.getSecondType(), source);
  }

  case RequirementKind::Layout: {
    // FIXME: Diagnose this.
    PotentialArchetype *pa = resolveArchetype(req.getFirstType());
    if (!pa) return false;

    return addLayoutRequirement(pa, req.getLayoutConstraint(), source);
  }

  case RequirementKind::Conformance: {
    // FIXME: Diagnose this.
    PotentialArchetype *pa = resolveArchetype(req.getFirstType());
    if (!pa) return false;

    SmallVector<ProtocolDecl *, 4> conformsTo;
    (void)req.getSecondType()->isExistentialType(conformsTo);

    // Add each of the protocols.
    for (auto proto : conformsTo) {
      if (Visited.count(proto)) {
        markPotentialArchetypeRecursive(pa, proto, source);
        continue;
      }
      if (addConformanceRequirement(pa, proto, source, Visited)) return true;
    }

    return false;
  }

  case RequirementKind::SameType:
    return addSameTypeRequirement(
        req.getFirstType(), req.getSecondType(), source,
        [&](Type type1, Type type2) {
          if (source->getLoc().isValid())
            Diags.diagnose(source->getLoc(), diag::requires_same_concrete_type,
                           type1, type2);
        });
  }

  llvm_unreachable("Unhandled requirement?");
}

/// AST walker that infers requirements from type representations.
class GenericSignatureBuilder::InferRequirementsWalker : public TypeWalker {
  GenericSignatureBuilder &Builder;
  TypeRepr *typeRepr;
  unsigned MinDepth;
  unsigned MaxDepth;

  /// We cannot add requirements to archetypes from outer generic parameter
  /// lists.
  bool isOuterArchetype(PotentialArchetype *PA) {
    unsigned ParamDepth = PA->getRootGenericParamKey().Depth;
    assert(ParamDepth <= MaxDepth);
    (void) MaxDepth;
    return ParamDepth < MinDepth;
  }

public:
  InferRequirementsWalker(GenericSignatureBuilder &builder,
                          TypeRepr *typeRepr,
                          unsigned MinDepth,
                          unsigned MaxDepth)
    : Builder(builder), typeRepr(typeRepr), MinDepth(MinDepth),
      MaxDepth(MaxDepth) { }

  Action walkToTypePost(Type ty) override {
    auto boundGeneric = ty->getAs<BoundGenericType>();
    if (!boundGeneric)
      return Action::Continue; 

    auto genericSig = boundGeneric->getDecl()->getGenericSignature();
    if (!genericSig)
      return Action::Stop;

    /// Retrieves the type substitution.
    auto args = boundGeneric->getGenericArgs();
    auto genericSigDepth =
      genericSig->getInnermostGenericParams().front()->getDepth();
    auto getTypeSubstitution = [&](SubstitutableType *dependentType) -> Type {
      if (auto gp = dyn_cast<GenericTypeParamType>(dependentType)) {
        if (gp->getDepth() == genericSigDepth)
          return args[gp->getIndex()];

        return gp;
      }

      return dependentType;
    };

    // Handle the requirements.
    // FIXME: Inaccurate TypeReprs.
    auto source = RequirementSource::forInferred(Builder, typeRepr);
    for (const auto &rawReq : genericSig->getRequirements()) {
      if (auto req = rawReq.subst(getTypeSubstitution,
                                  Builder.getLookupConformanceFn()))
        Builder.addRequirement(*req, source);
    }

    return Action::Continue;
  }
};

void GenericSignatureBuilder::inferRequirements(TypeLoc type,
                                         unsigned minDepth,
                                         unsigned maxDepth) {
  if (!type.getType())
    return;
  // FIXME: Crummy source-location information.
  InferRequirementsWalker walker(*this, type.getTypeRepr(),
                                 minDepth, maxDepth);
  type.getType().walk(walker);
}

void GenericSignatureBuilder::inferRequirements(ParameterList *params,
                                         GenericParamList *genericParams) {
  if (genericParams == nullptr)
    return;

  unsigned depth = genericParams->getDepth();
  for (auto P : *params)
    inferRequirements(P->getTypeLoc(),
                      /*minDepth=*/depth,
                      /*maxDepth=*/depth);
}

/// Perform typo correction on the given nested type, producing the
/// corrected name (if successful).
static Identifier typoCorrectNestedType(
                    GenericSignatureBuilder::PotentialArchetype *pa) {
  StringRef name = pa->getNestedName().str();

  // Look through all of the associated types of all of the protocols
  // to which the parent conforms.
  llvm::SmallVector<Identifier, 2> bestMatches;
  unsigned bestEditDistance = 0;
  unsigned maxScore = (name.size() + 1) / 3;
  for (const auto &conforms : pa->getParent()->getConformsTo()) {
    auto proto = conforms.first;
    for (auto member : getProtocolMembers(proto)) {
      auto assocType = dyn_cast<AssociatedTypeDecl>(member);
      if (!assocType)
        continue;

      unsigned dist = name.edit_distance(assocType->getName().str(),
                                         /*AllowReplacements=*/true,
                                         maxScore);
      assert(dist > 0 && "nested type should have matched associated type");
      if (bestEditDistance == 0 || dist == bestEditDistance) {
        bestEditDistance = dist;
        maxScore = bestEditDistance;
        bestMatches.push_back(assocType->getName());
      } else if (dist < bestEditDistance) {
        bestEditDistance = dist;
        maxScore = bestEditDistance;
        bestMatches.clear();
        bestMatches.push_back(assocType->getName());
      }
    }
  }

  // FIXME: Look through the superclass.

  // If we didn't find any matches at all, fail.
  if (bestMatches.empty())
    return Identifier();

  // Make sure that we didn't find more than one match at the best
  // edit distance.
  for (auto other : llvm::makeArrayRef(bestMatches).slice(1)) {
    if (other != bestMatches.front())
      return Identifier();
  }

  return bestMatches.front();
}

void
GenericSignatureBuilder::finalize(SourceLoc loc,
                           ArrayRef<GenericTypeParamType *> genericParams,
                           bool allowConcreteGenericParams) {
  assert(!Impl->finalized && "Already finalized builder");
#ifndef NDEBUG
  Impl->finalized = true;
#endif

  // Local function (+ cache) describing the set of potential archetypes
  // directly referenced by the concrete same-type constraint of the given
  // potential archetype. Both the inputs and results are the representatives
  // of their equivalence classes.
  llvm::DenseMap<PotentialArchetype *,
                 SmallPtrSet<PotentialArchetype *, 4>> concretePAs;
  auto getConcreteReferencedPAs
      = [&](PotentialArchetype *pa) -> SmallPtrSet<PotentialArchetype *, 4> {
    assert(pa == pa->getRepresentative() && "Only use with representatives");
    auto known = concretePAs.find(pa);
    if (known != concretePAs.end())
      return known->second;

    SmallPtrSet<PotentialArchetype *, 4> referencedPAs;
    if (!pa->isConcreteType() || !pa->getConcreteType()->hasTypeParameter())
      return referencedPAs;

    if (auto concreteType = pa->getConcreteType()) {
      if (concreteType->hasTypeParameter()) {
        concreteType.visit([&](Type type) {
          if (type->isTypeParameter()) {
            if (auto referencedPA = resolveArchetype(type)) {
              referencedPAs.insert(referencedPA->getRepresentative());
            }
          }
        });
      }
    }

    concretePAs[pa] = referencedPAs;
    return referencedPAs;
  };

  /// Check whether the given type references the archetype.
  auto isRecursiveConcreteType = [&](PotentialArchetype *archetype,
                                     bool isSuperclass) {
    SmallPtrSet<PotentialArchetype *, 4> visited;
    SmallVector<PotentialArchetype *, 4> stack;
    stack.push_back(archetype);
    visited.insert(archetype);

    // Check whether the specific type introduces recursion.
    auto checkTypeRecursion = [&](Type type) {
      if (!type->hasTypeParameter()) return false;

      return type.findIf([&](Type type) {
        if (type->isTypeParameter()) {
          if (auto referencedPA = resolveArchetype(type)) {
            referencedPA = referencedPA->getRepresentative();
            if (referencedPA == archetype) return true;

            if (visited.insert(referencedPA).second)
              stack.push_back(referencedPA);
          }
        }

        return false;
      });
    };

    while (!stack.empty()) {
      auto pa = stack.back();
      stack.pop_back();

      // If we're checking superclasses, do so now.
      if (isSuperclass) {
        if (auto superclass = pa->getSuperclass()) {
          if (checkTypeRecursion(superclass)) return true;
        }
      }

      // Otherwise, look for the potential archetypes referenced by
      // same-type constraints.
      for (auto referencedPA : getConcreteReferencedPAs(pa)) {
        // If we found a reference to the original archetype, it's recursive.
        if (referencedPA == archetype) return true;

        if (visited.insert(referencedPA).second)
          stack.push_back(referencedPA);
      }
    }

    return false;
  };

  // Check for recursive same-type bindings and superclass constraints.
  visitPotentialArchetypes([&](PotentialArchetype *archetype) {
    if (archetype != archetype->getRepresentative()) return;

    // Check for recursive same-type bindings.
    if (archetype->isConcreteType()) {
      if (isRecursiveConcreteType(archetype, /*isSuperclass=*/false)) {
        if (auto source = archetype->findAnyConcreteTypeSourceAsWritten()) {
          Diags.diagnose(source->getLoc(),
                         diag::recursive_same_type_constraint,
                         archetype->getDependentType(genericParams,
                                                     /*allowUnresolved=*/true),
                         archetype->getConcreteType());
        }

        archetype->RecursiveConcreteType = true;
      }
    }

    // Check for recursive superclass bindings.
    if (archetype->getSuperclass()) {
      if (isRecursiveConcreteType(archetype, /*isSuperclass=*/true)) {
        if (archetype->SuperclassSource->getLoc().isValid())
          Diags.diagnose(archetype->SuperclassSource->getLoc(),
                         diag::recursive_superclass_constraint,
                         archetype->getDependentType(genericParams,
                                                     /*allowUnresolved=*/true),
                         archetype->getSuperclass());

        archetype->RecursiveSuperclassType = true;
      }
    }
  });

  SmallPtrSet<PotentialArchetype *, 4> visited;

  // Check for generic parameters which have been made concrete or equated
  // with each other.
  if (!allowConcreteGenericParams) {
    unsigned depth = 0;
    for (const auto &gp : Impl->GenericParams)
      depth = std::max(depth, gp->getDepth());

    for (const auto pa : Impl->PotentialArchetypes) {
      auto rep = pa->getRepresentative();

      if (pa->getRootGenericParamKey().Depth < depth)
        continue;

      if (!visited.insert(rep).second)
        continue;

      // Don't allow a generic parameter to be equivalent to a concrete type,
      // because then we don't actually have a parameter.
      if (rep->getConcreteType()) {
        if (auto source = rep->findAnyConcreteTypeSourceAsWritten())
          Diags.diagnose(source->getLoc(),
                         diag::requires_generic_param_made_equal_to_concrete,
                         rep->getDependentType(genericParams,
                                               /*allowUnresolved=*/true));
        continue;
      }

      // Don't allow two generic parameters to be equivalent, because then we
      // don't actually have two parameters.
      for (auto other : rep->getEquivalenceClass()) {
        // If it isn't a generic parameter, skip it.
        if (other == pa || other->getParent() != nullptr) continue;

        SourceLoc constraintLoc;
        for (const auto &sameType : pa->getSameTypeConstraints()) {
          SourceLoc sameTypeLoc = sameType.second->getLoc();
          if (sameTypeLoc.isInvalid()) continue;

          if (sameType.first == other) {
            constraintLoc = sameTypeLoc;
            break;
          }

          if (constraintLoc.isInvalid())
            constraintLoc = sameTypeLoc;
        }

        if (constraintLoc.isInvalid())
          constraintLoc = loc;

        Diags.diagnose(constraintLoc,
                       diag::requires_generic_params_made_equal,
                       pa->getDependentType(genericParams, true),
                       other->getDependentType(genericParams, true));
        break;
      }
    }
  }

  // If any nested types remain unresolved, produce diagnostics.
  if (Impl->NumUnresolvedNestedTypes > 0) {
    visitPotentialArchetypes([&](PotentialArchetype *pa) {
      // We only care about nested types that haven't been resolved.
      if (pa->getParent() == nullptr || pa->getResolvedAssociatedType() ||
          pa->getTypeAliasDecl() ||
          /* FIXME: Should be able to handle this earlier */pa->getSuperclass())
        return;

      // Try to typo correct to a nested type name.
      Identifier correction = typoCorrectNestedType(pa);
      if (correction.empty()) {
        pa->setInvalid();
        return;
      }

      // Note that this is being renamed.
      pa->saveNameForRenaming();
      Impl->RenamedNestedTypes.push_back(pa);
      
      // Resolve the associated type and merge the potential archetypes.
      auto replacement = pa->getParent()->getNestedType(correction, *this);
      pa->resolveAssociatedType(replacement->getResolvedAssociatedType(),
                                *this);
      addSameTypeRequirement(
          pa, replacement,
          RequirementSource::forNestedTypeNameMatch(*this));
    });
  }
}

bool GenericSignatureBuilder::diagnoseRemainingRenames(
                              SourceLoc loc,
                              ArrayRef<GenericTypeParamType *> genericParams) {
  bool invalid = false;

  for (auto pa : Impl->RenamedNestedTypes) {
    if (pa->alreadyDiagnosedRename()) continue;

    Diags.diagnose(loc, diag::invalid_member_type_suggest,
                   pa->getParent()->getDependentType(genericParams,
                                                     /*allowUnresolved=*/true),
                   pa->getOriginalName(), pa->getNestedName());
    invalid = true;
  }

  return invalid;
}

template<typename F>
void GenericSignatureBuilder::visitPotentialArchetypes(F f) {
  // Stack containing all of the potential archetypes to visit.
  SmallVector<PotentialArchetype *, 4> stack;
  llvm::SmallPtrSet<PotentialArchetype *, 4> visited;

  // Add top-level potential archetypes to the stack.
  for (const auto pa : Impl->PotentialArchetypes) {
    if (visited.insert(pa).second)
      stack.push_back(pa);
  }

  // Visit all of the potential archetypes.
  while (!stack.empty()) {
    PotentialArchetype *pa = stack.back();
    stack.pop_back();
    f(pa);

    // Visit nested potential archetypes.
    for (const auto &nested : pa->getNestedTypes()) {
      for (auto nestedPA : nested.second) {
        if (visited.insert(nestedPA).second) {
          stack.push_back(nestedPA);
        }
      }
    }
  }
}

/// Perform a depth-first search from the given potential archetype through
/// the *implicit* same-type constraints.
///
/// \param pa The potential archetype to visit.
/// \param visited The set of potential archetypes that have already been
/// seen.
/// \param found Used to record each potential archetype visited
static void sameTypeDFS(PotentialArchetype *pa,
                        SmallPtrSetImpl<PotentialArchetype *> &visited,
                        SmallVectorImpl<PotentialArchetype *> &found) {
  // If we've already visited this potential archetype, we're done.
  if (!visited.insert(pa).second) return;

  // Note that we've found this potential archetype.
  found.push_back(pa);

  // Visit its adjacent potential archetypes.
  for (const auto &sameType : pa->getSameTypeConstraints()) {
    switch (sameType.second->kind) {
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::RequirementSignatureSelf:
      // Skip explicit constraints.
      continue;

    case RequirementSource::Concrete:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::Superclass:
      break;
    }

    sameTypeDFS(sameType.first, visited, found);
  }
}

namespace {
  /// Describes a component in the (implied) same-type constraint graph.
  struct SameTypeComponent {
    /// The potential archetype that acts as the anchor for this component.
    PotentialArchetype * anchor;

    /// The (best) requirement source within the component that makes the
    /// potential archetypes in this component equivalent to the concrete type.
    const RequirementSource * concreteTypeSource;

    friend bool operator<(const SameTypeComponent &lhs,
                          const SameTypeComponent &rhs) {
      return compareDependentTypes(&lhs.anchor, &rhs.anchor) < 0;
    }
  };
}

/// Computes the ordered set of archetype anchors required to form a minimum
/// spanning tree among the connected components formed by only the implied
/// same-type requirements within the equivalence class of \c rep.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) contains all potential archetypes that are made equivalent by
/// the known set of same-type constraints, which includes both directly-
/// stated same-type constraints (e.g., \c T.A == T.B) as well as same-type
/// constraints that are implied either because the names coincide (e.g.,
/// \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) is formed from a graph whose vertices are the potential archetypes
/// and whose edges are the same-type constraints. These edges include both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
/// The equivalence class forms a single connected component.
///
/// Within that graph is a subgraph that includes only those edges that are
/// implied (and, therefore, excluding those edges that were explicitly stated).
/// The connected components within that subgraph describe the potential
/// archetypes that would be equivalence even with all of the (explicit)
/// same-type constraints removed.
///
/// The entire equivalence class can be restored by introducing edges between
/// the connected components. This function computes a minimal, canonicalized
/// set of edges (same-type constraints) needed to describe the equivalence
/// class, which is suitable for the generation of the canonical generic
/// signature.
///
/// The resulting set of "edges" is returned as a set of vertices, one per
/// connected component (of the subgraph). Each is the anchor for that
/// connected component (as determined by \c compareDependentTypes()), and the
/// set itself is ordered by \c compareDependentTypes(). The actual set of
/// canonical edges connects vertex i to vertex i+1 for i in 0..<size-1.
static SmallVector<SameTypeComponent, 2> getSameTypeComponents(
                                                     PotentialArchetype *rep) {
  SmallPtrSet<PotentialArchetype *, 8> visited;
  SmallVector<SameTypeComponent, 2> components;
  for (auto pa : rep->getEquivalenceClass()) {
    // If we've already seen this potential archetype, there's nothing else to
    // do.
    if (visited.count(pa) != 0) continue;

    // Find all of the potential archetypes within this connected component.
    SmallVector<PotentialArchetype *, 2> component;
    sameTypeDFS(pa, visited, component);

    // Find the best anchor and concrete type source for this component.
    PotentialArchetype *anchor = component[0];
    auto bestConcreteTypeSource = anchor->getConcreteTypeSourceAsWritten();

    for (auto componentPA : ArrayRef<PotentialArchetype *>(component).slice(1)){
      // Update the anchor.
      if (compareDependentTypes(&componentPA, &anchor) < 0)
        anchor = componentPA;

      // If this potential archetype has a better concrete type source than
      // the best we've seen, take it.
      if (auto concreteSource = componentPA->getConcreteTypeSourceAsWritten()) {
        if (!bestConcreteTypeSource ||
            concreteSource->compare(bestConcreteTypeSource) < 0)
          bestConcreteTypeSource = concreteSource;
      }
    }

    // Record the anchor.
    components.push_back({anchor, bestConcreteTypeSource});
  }

  llvm::array_pod_sort(components.begin(), components.end());

  return components;
}

void GenericSignatureBuilder::enumerateRequirements(llvm::function_ref<
                     void (RequirementKind kind,
                           PotentialArchetype *archetype,
                           GenericSignatureBuilder::RequirementRHS constraint,
                           const RequirementSource *source)> f) {
  // Collect all archetypes.
  SmallVector<PotentialArchetype *, 8> archetypes;
  visitPotentialArchetypes([&](PotentialArchetype *archetype) {
    archetypes.push_back(archetype);
  });

  // Remove any invalid potential archetypes or archetypes whose parents are
  // concrete; they have no requirements.
  archetypes.erase(
    std::remove_if(archetypes.begin(), archetypes.end(),
      [&](PotentialArchetype *archetype) -> bool {
        // Invalid archetypes are never representatives in well-formed or
        // corrected signature, so we don't need to visit them.
        if (archetype->isInvalid())
          return true;

        // Keep it.
        return false;
      }),
    archetypes.end());

  // Sort the archetypes in canonical order.
  llvm::array_pod_sort(archetypes.begin(), archetypes.end(),
                       compareDependentTypes);

  // Track the anchors for each of the implied connected components within the
  // equivalence class of each representative.
  llvm::DenseMap<PotentialArchetype *, SmallVector<SameTypeComponent, 2>>
    sameTypeComponents;
  auto getSameTypeComponents =
    [&](PotentialArchetype *rep) -> ArrayRef<SameTypeComponent> {
      assert(rep->getRepresentative() == rep);
      auto known = sameTypeComponents.find(rep);
      if (known != sameTypeComponents.end())
        return known->second;

      return sameTypeComponents.insert(
               {rep, ::getSameTypeComponents(rep) }).first->second;
    };

  for (auto *archetype : archetypes) {
    // Check whether this archetype is one of the anchors within its
    // connected component. If so, we may need to emit a same-type constraint.
    //
    // FIXME: O(n) in the number of implied connected components within the
    // equivalence class. The equivalence class should be small, but...
    auto rep = archetype->getRepresentative();
    auto components = getSameTypeComponents(rep);
    auto knownAnchor = std::find_if(components.begin(),
                                    components.end(),
                                    [&](const SameTypeComponent &component) {
                                      return component.anchor == archetype;
                                    });
    std::function<void()> deferredSameTypeRequirement;

    if (knownAnchor != components.end()) {
      // If this equivalence class is bound to a concrete type, equate the
      // anchor with a concrete type.
      if (Type concreteType = rep->getConcreteType()) {
        auto source =
          knownAnchor->concreteTypeSource
            ? knownAnchor->concreteTypeSource
            : RequirementSource::forAbstract(*this);

        f(RequirementKind::SameType, archetype, concreteType, source);
        continue;
      }

      // If we're at the last anchor in the component, do nothing;
      auto nextAnchor = knownAnchor;
      ++nextAnchor;
      if (nextAnchor != components.end()) {
        // Form a same-type constraint from this anchor within the component
        // to the next.
        // FIXME: Distinguish between explicit and inferred here?
        auto otherPA = nextAnchor->anchor;
        deferredSameTypeRequirement = [&f, archetype, otherPA, this] {
          f(RequirementKind::SameType, archetype, otherPA,
            RequirementSource::forAbstract(*this));
        };
      }
    }
    SWIFT_DEFER {
      if (deferredSameTypeRequirement) deferredSameTypeRequirement();
    };

    // If this is not the archetype anchor, we're done.
    if (archetype != archetype->getArchetypeAnchor(*this))
      continue;

    // If we have a superclass, produce a superclass requirement
    if (Type superclass = rep->getSuperclass()) {
      f(RequirementKind::Superclass, archetype, superclass,
        rep->getSuperclassSource());
    }

    // If we have a layout constraint, produce a layout requirement.
    if (LayoutConstraint Layout = archetype->getLayout()) {
      f(RequirementKind::Layout, archetype, Layout,
        archetype->getLayoutSource());
    }

    // Enumerate conformance requirements.
    SmallVector<ProtocolDecl *, 4> protocols;
    DenseMap<ProtocolDecl *, const RequirementSource *> protocolSources;
    for (const auto &conforms : rep->getConformsTo()) {
      protocols.push_back(conforms.first);
      assert(protocolSources.count(conforms.first) == 0 && 
             "redundant protocol requirement?");
      protocolSources.insert({conforms.first, conforms.second});
    }

    // Sort the protocols in canonical order.
    llvm::array_pod_sort(protocols.begin(), protocols.end(), 
                         ProtocolType::compareProtocols);

    // Enumerate the conformance requirements.
    for (auto proto : protocols) {
      assert(protocolSources.count(proto) == 1 && "Missing conformance?");
      f(RequirementKind::Conformance, archetype, 
        proto->getDeclaredInterfaceType(),
        protocolSources.find(proto)->second);
    }
  };
}

void GenericSignatureBuilder::dump() {
  dump(llvm::errs());
}

void GenericSignatureBuilder::dump(llvm::raw_ostream &out) {
  out << "Requirements:";
  enumerateRequirements([&](RequirementKind kind,
                            PotentialArchetype *archetype,
                            GenericSignatureBuilder::RequirementRHS constraint,
                            const RequirementSource *source) {
    switch (kind) {
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
      out << "\n  ";
      out << archetype->getDebugName() << " : " 
          << constraint.get<Type>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::Layout:
      out << "\n  ";
      out << archetype->getDebugName() << " : "
          << constraint.get<LayoutConstraint>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::SameType:
      out << "\n  ";
      out << archetype->getDebugName() << " == " ;
      if (auto secondType = constraint.dyn_cast<Type>()) {
        out << secondType.getString();
      } else {
        out << constraint.get<PotentialArchetype *>()->getDebugName();
      }
      out << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    }
  });
  out << "\n";

  out << "Potential archetypes:\n";
  for (auto pa : Impl->PotentialArchetypes) {
    pa->dump(out, &Context.SourceMgr, 2);
  }
  out << "\n";
}

void GenericSignatureBuilder::addGenericSignature(GenericSignature *sig) {
  if (!sig) return;

  for (auto param : sig->getGenericParams())
    addGenericParameter(param);

  auto source = RequirementSource::forAbstract(*this);
  for (auto &reqt : sig->getRequirements()) {
    addRequirement(reqt, source);
  }
}

/// Collect the set of requirements placed on the given generic parameters and
/// their associated types.
static void collectRequirements(GenericSignatureBuilder &builder,
                                ArrayRef<GenericTypeParamType *> params,
                                SmallVectorImpl<Requirement> &requirements) {
  builder.enumerateRequirements([&](RequirementKind kind,
          GenericSignatureBuilder::PotentialArchetype *archetype,
          GenericSignatureBuilder::RequirementRHS type,
          const RequirementSource *source) {
    // Filter out derived requirements.
    if (source->isDerivedRequirement()) return;

    auto depTy = archetype->getDependentType(params,
                                             /*allowUnresolved=*/false);

    if (depTy->hasError())
      return;

    Type repTy;
    if (auto concreteTy = type.dyn_cast<Type>()) {
      // Maybe we were equated to a concrete type...
      repTy = concreteTy;

      // Drop requirements involving concrete types containing
      // unresolved associated types.
      if (repTy.findIf([](Type t) -> bool {
            if (auto *depTy = dyn_cast<DependentMemberType>(t.getPointer()))
              if (depTy->getAssocType() == nullptr)
                return true;
            return false;
          })) {
        return;
      }
    } else if (auto layoutConstraint = type.dyn_cast<LayoutConstraint>()) {
      requirements.push_back(Requirement(kind, depTy, layoutConstraint));
      return;
    } else {
      // ...or to a dependent type.
      repTy = type.get<GenericSignatureBuilder::PotentialArchetype *>()
          ->getDependentType(params, /*allowUnresolved=*/false);
    }

    if (repTy->hasError())
      return;

    requirements.push_back(Requirement(kind, depTy, repTy));
  });
}

GenericSignature *GenericSignatureBuilder::getGenericSignature() {
  assert(Impl->finalized && "Must finalize builder first");

  // Collect the requirements placed on the generic parameter types.
  SmallVector<Requirement, 4> requirements;
  collectRequirements(*this, Impl->GenericParams, requirements);

  auto sig = GenericSignature::get(Impl->GenericParams, requirements);
  return sig;
}
