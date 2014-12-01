// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2012-2014 David Anderson
//
// This file is part of SourcePawn.
//
// SourcePawn is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
// 
// SourcePawn is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SourcePawn. If not, see http://www.gnu.org/licenses/.
#include "compile-context.h"
#include "name-resolver.h"

using namespace ke;
using namespace sp;

NameResolver::NameResolver(CompileContext &cc)
 : cc_(cc),
   pool_(cc.pool()),
   tr_(cc),
   layout_scope_(nullptr)
{
  atom_String_ = cc_.add("String");
  atom_Float_ = cc_.add("Float");
  atom_any_ = cc_.add("any");
  atom_Function_ = cc_.add("Function");
  atom_bool_ = cc_.add("bool");
}

void
NameResolver::OnEnterParser()
{
  globals_ = GlobalScope::New(pool_);

  OnEnterScope(Scope::Global);
  env_[0].setScope(globals_);

  declareSystemTypes(globals_);
}

void
NameResolver::OnLeaveParser()
{
  OnLeaveScope();
  assert(env_.empty());

  if (!cc_.phasePassed())
    return;

  if (cc_.options().SkipResolution)
    return;

  resolveUnknownTags();
  resolveUnboundNames();

  if (!cc_.phasePassed())
    return;

  tr_.analyze();
}

void
NameResolver::declareSystemTypes(Scope *scope)
{
  declareSystemType(scope, "float", PrimitiveType::Float);
  declareSystemType(scope, "int", PrimitiveType::Int32);
  declareSystemType(scope, "bool", PrimitiveType::Bool);
  declareSystemType(scope, "char", PrimitiveType::Char);
  declareSystemType(scope, "void", cc_.types()->getVoid());

  // These are pseudo-deprecated, but we still have them for compatibility.
  declareSystemType(scope, "_", PrimitiveType::Int32);
  declareSystemType(scope, "any", cc_.types()->getUnchecked());
  declareSystemType(scope, "Function", cc_.types()->getMetaFunction());
}

void
NameResolver::declareSystemType(Scope *scope, const char *name, PrimitiveType prim)
{
  declareSystemType(scope, name, cc_.types()->getPrimitive(prim));
}

void
NameResolver::declareSystemType(Scope *scope, const char *name, Type *type)
{
  Atom *tag = cc_.add(name);

  TypeSymbol *sym = new (pool_) TypeSymbol(nullptr, scope, tag, type);
  scope->addSymbol(sym);
}

void
NameResolver::OnEnterScope(Scope::Kind kind)
{
  env_.append(SymbolEnv(kind));
}

Scope *
NameResolver::OnLeaveScope()
{
  SymbolEnv &env = env_.back();
  SymbolEnv *prev = env_.length() >= 2
                    ? &env_[env_.length() - 2]
                    : nullptr;
  Scope *scope = env.scope();
  if (scope) {
    // Fix up children.
    for (size_t i = 0; i < env.children().length(); i++)
      env.children()[i]->setParent(scope);
        
    // Add us to our parent scope.
    if (prev)
      prev->addChild(scope);
  } else {
    // We didn't have a scope. Transfer any children to our parent. Note that
    // we must have a parent, since we stop at the global scope.
    prev->children().extend(ke::Move(env.children()));
  }
  env_.pop();
  return scope;
}

void
NameResolver::OnLeaveOrphanScope()
{
  // Don't connect the current up to anything.
  env_.pop();
}

Scope *
NameResolver::getOrCreateScope()
{
  SymbolEnv &env = env_.back();
  if (env.scope())
    return env.scope();

  switch (env.kind()) {
    case Scope::Block:
      env.setScope(BlockScope::New(pool_));
      break;
    case Scope::Function:
      env.setScope(FunctionScope::New(pool_));
      break;
    default:
      assert(false);
  }

  return env.scope();
}

Symbol *
NameResolver::lookup(Atom *name)
{
  for (size_t i = env_.length(); i > 0; i--) {
    SymbolEnv &env = env_[i - 1];
    if (!env.scope())
      continue;
    if (Symbol *sym = env.scope()->localLookup(name))
      return sym;
  }
  return nullptr;
}

void
NameResolver::reportRedeclaration(Symbol *sym, Symbol *other)
{
  if (other->node()) {
    cc_.report(sym->node()->loc(), rmsg::redeclared_name)
      << sym->name()
      << (cc_.note(other->node()->loc(), rmsg::previous_location));
  } else {
    cc_.report(sym->node()->loc(), rmsg::redeclared_builtin)
      << sym->name();
  }
}

bool
NameResolver::registerSymbol(Symbol *sym)
{
  Scope *scope = getOrCreateScope();

  if (Symbol *other = scope->localLookup(sym->name())) {
    // Report, but allow errors to continue.
    reportRedeclaration(sym, other);
    return true;
  }

  return scope->addSymbol(sym);
}

void
NameResolver::resolveUnknownTags()
{
  for (AtomMap<NameProxy *>::iterator iter = user_tags_.iter(); !iter.empty(); iter.next()) {
    Atom *atom = iter->key;
    if (globals_->lookup(atom))
      continue;

    NameProxy *origin = iter->value;
    EnumType *type = cc_.types()->newEnum(atom);
    Symbol *sym = new (pool_) TypeSymbol(origin, globals_, atom, type);
    globals_->addSymbol(sym);
  }
}

void
NameResolver::resolveUnboundNames()
{
  // Resolve unresolved global names.
  AtomSet seen;
  for (size_t i = 0; i < unresolved_names_.length(); i++) {
    NameProxy *proxy = unresolved_names_[i];
    Symbol *sym = globals_->lookup(proxy->name());
    if (!sym) {
      AtomSet::Insert p = seen.findForAdd(proxy->name());
      if (p.found())
        continue;
      seen.add(p, proxy->name());

      cc_.report(proxy->loc(), rmsg::name_not_found)
        << proxy->name();
      continue;
    }

    proxy->bind(sym);
  }
}

void
NameResolver::OnNameProxy(NameProxy *proxy)
{
  if (Symbol *sym = lookup(proxy->name())) {
    proxy->bind(sym);
  } else {
    // Place this symbol in the unresolved list, in case it binds to a
    // global we haven't seen yet.
    unresolved_names_.append(proxy);
  }
}

void
NameResolver::OnTagProxy(NameProxy *proxy)
{
  if (Symbol *sym = lookup(proxy->name())) {
    proxy->bind(sym);
    return;
  }

  // SourcePawn 1 compatibility: we don't go as far as keeping separate type
  // and variable/fun symbol tables, but we do lazily create tags that don't
  // exist if there are no other bindings available.
  AtomMap<NameProxy *>::Insert p = user_tags_.findForAdd(proxy->name());
  if (!p.found())
    user_tags_.add(p, proxy->name(), proxy);
  unresolved_names_.append(proxy);
}

void
NameResolver::OnEnumDecl(EnumStatement *node)
{
  Scope *scope = getOrCreateScope();

  // Note: we do not let enums override methodmap declarations. Once a
  // methodmap has been declared, if no enum had been seen, we cannot
  // add enum values after the fact. This is handled implicitly by
  // registerSymbol(), and the fact that methodmaps define an enum.
  if (node->name()) {
    TypeSymbol *sym = new (pool_) TypeSymbol(node, scope, node->name());
    registerSymbol(sym);
    node->setSymbol(sym);
  } else {
    // If the enum does not have a name, we give it an anonymous symbol.
    Atom *name = cc_.createAnonymousName(node->loc());
    node->setSymbol(new (pool_) TypeSymbol(node, scope, name));
  }

  Type *type;
  if (node->name()) {
    type = cc_.types()->newEnum(node->name());
  } else {
    type = cc_.types()->getPrimitive(PrimitiveType::Int32);
  }
  node->sym()->setType(type);

  tr_.addPending(node);
}

void
NameResolver::OnEnumValueDecl(EnumConstant *cs)
{
  ConstantSymbol *sym = new (pool_) ConstantSymbol(cs, getOrCreateScope(), cs->name());
  registerSymbol(sym);
  cs->setSymbol(sym);

  // Note: we don't need to add to the type resolver, it'll be handled by the
  // parent enum.
}

VarDecl *
NameResolver::HandleVarDecl(NameToken name, TypeSpecifier &spec, Expression *init)
{
  Scope *scope = getOrCreateScope();

  // See the comment in TypeResolver::visitVarDecl for why we do not want to
  // infer sizes from literals for arguments.
  TypeExpr te;
  if (init &&
      !scope->isFunction() &&
      ((init->isArrayLiteral() && init->asArrayLiteral()->isFixedArrayLiteral()) ||
       (init->isStringLiteral())))
  {
    // Wait until the type resolution pass to figure this out. We still have
    // to precompute the base though.
    if (Type *type = resolveBase(spec))
      spec.setResolvedBaseType(type);
    te = TypeExpr(new (pool_) TypeSpecifier(spec));
  } else {
    te = resolve(spec);
  }

  // :TODO: set variadic info
  VarDecl *var = new (pool_) VarDecl(name, te, init);

  // Note: the parser has already bound |var->init()| at this point, meaning
  // that aside from globals it should be impossible to self-initialize like:
  //    int x = x;
  //
  // :TODO: do check this for globals.
  VariableSymbol *sym = new (pool_) VariableSymbol(var, scope, var->name());
  registerSymbol(sym);
  var->setSymbol(sym);

  if (te.resolved())
    sym->setType(te.resolved());

  // Even if we were able to resolve the type, if we have to resolve a constant
  // value, we'll have to add it to the resolver queue.
  if (!te.resolved() || sym->canUseInConstExpr())
    tr_.addPending(var);

  return var;
}

void
NameResolver::OnEnterMethodmap(MethodmapDecl *methodmap)
{
  // Methodmaps cannot be nested anywhere.
  assert(!layout_scope_);
  layout_scope_ = LayoutScope::New(pool_);

  bool canDefine = canDefineMethodmap(methodmap);
  if (!methodmap->sym()) {
    TypeSymbol *sym = new (pool_) TypeSymbol(methodmap, globals_, methodmap->name());
    if (canDefine)
      registerSymbol(sym);
    sym->setType(cc_.types()->newEnum(methodmap->name()));
    methodmap->setSymbol(sym);
  }

  // We always add the methodmap to the resolver, to avoid duplicating the
  // parent-resolving logic.
  tr_.addPending(methodmap);

  // Note that we do not insert the layout scope into the scope chain. For
  // simplicity SP1 did not, and we can't break that.
  methodmap->setScope(layout_scope_);
}

void
NameResolver::OnLeaveMethodmap(MethodmapDecl *methodmap)
{
  layout_scope_ = nullptr;
}

bool
NameResolver::canDefineMethodmap(MethodmapDecl *methodmap)
{
  // Methodmaps are only allowed in the global scope. They have very odd
  // semantics (by design, as part of the transitional syntax): they
  // create an enum, or they extend an existing enum, in any declaration
  // order.
  //
  // If the symbol already exists, it must be a direct enum type. We do
  // not accept typedefs.
  assert(getOrCreateScope() == globals_);

  Symbol *prev = globals_->lookup(methodmap->name());
  if (!prev)
    return true;

  TypeSymbol *sym = prev->asType();
  if (!sym) {
    cc_.report(methodmap->loc(), rmsg::methodmap_on_non_type)
      << sym->name();
    return false;
  }

  // Builtin types do not have AST nodes.
  if (!sym->node()) {
    cc_.report(methodmap->loc(), rmsg::methodmap_on_non_enum)
      << sym->name();
    return false;
  }

  EnumStatement *stmt = sym->node()->asEnumStatement();
  if (!stmt) {
    if (sym->node()->asMethodmapDecl()) {
      // We had something like:
      //   methodmap X {}
      //   methodmap X {}
      //
      // We can give a slightly more specific error for this case.
      cc_.report(methodmap->loc(), rmsg::methodmap_already_defined)
        << methodmap->name();
    } else {
      cc_.report(methodmap->loc(), rmsg::methodmap_on_non_enum)
        << sym->name();
    }
    return false;
  }

  // Mark that our enum statement has a methodmap.
  stmt->setMethodmap(methodmap);

  // Point the layout at the enum type.
  methodmap->setSymbol(sym);

  // Return false - symbol is already defined.
  return false;
}

void
NameResolver::OnEnterRecordDecl(RecordDecl *decl)
{
  TypeSymbol *sym = new (pool_) TypeSymbol(decl, getOrCreateScope(), decl->name());
  registerSymbol(sym);
  decl->setSymbol(sym);

  RecordType *type = nullptr;
  switch (decl->token()) {
    case TOK_UNION:
      type = cc_.types()->newUnion(decl->name());
      break;

    case TOK_STRUCT:
      type = cc_.types()->newStruct(decl->name());
      break;

    default:
      assert(false);
  }
  sym->setType(type);

  // Record-types cannot nest yet.
  assert(!layout_scope_);
  layout_scope_ = LayoutScope::New(pool_);

  // Record types cannot have methods yet, so there is no need to link this
  // scope into the scope chain.
  decl->setScope(layout_scope_);
}

void
NameResolver::OnLeaveRecordDecl(RecordDecl *decl)
{
  if (decl->token() == TOK_UNION) {
    if (layout_scope_->hasMixedAnonymousFields()) {
      cc_.report(decl->loc(), rmsg::union_cannot_mix_fields);
      return;
    }
  } else {
    // Parser should disallow this.
    assert(!layout_scope_->anonymous_fields());
  }

  layout_scope_ = nullptr;
}

PropertyDecl *
NameResolver::EnterPropertyDecl(const SourceLocation &begin,
                                const NameToken &nameToken,
                                TypeSpecifier &spec)
{
  TypeExpr te = resolve(spec);
  PropertyDecl *decl = new (pool_) PropertyDecl(begin, nameToken, te);

  PropertySymbol *sym = new (pool_) PropertySymbol(decl, layout_scope_, decl->name());
  decl->setSymbol(sym);

  if (Symbol *other = layout_scope_->localLookup(decl->name())) {
    cc_.report(decl->loc(), rmsg::redefined_layout_decl)
      << "property"
      << decl->name()
      << other->kindName()
      << cc_.note(other->node()->loc(), rmsg::previous_location);
  } else {
    layout_scope_->addSymbol(sym);
  }

  return decl;
}

void
NameResolver::LeavePropertyDecl(PropertyDecl *decl)
{
  if (decl->te().resolved())
    decl->sym()->setType(decl->te().resolved());

  FunctionOrAlias *getter = decl->getter();
  FunctionOrAlias *setter = decl->setter();
  if (!decl->te().resolved() ||
      (getter->fun() && !getter->fun()->signature()->isResolved()) ||
      (setter->fun() && !setter->fun()->signature()->isResolved()))
  {
    tr_.addPending(decl);
  }
}

FieldDecl *
NameResolver::HandleFieldDecl(const SourceLocation &pos,
                              const NameToken &nameToken,
                              TypeSpecifier &spec)
{
  TypeExpr te = resolve(spec);
  FieldDecl *decl = new (pool_) FieldDecl(pos, nameToken, te);

  if (!te.resolved())
    tr_.addPending(decl);

  Atom *name = nameToken.atom;
  if (!name) {
    // Originally, SourcePawn had a concept called "funcenums" to work around
    // the lack of a true top type. They were untagged unions of function
    // types. Sourceawn 1.7 future-proofed this syntax the best it could by
    // introducing something we interpret as a "block union". A block union
    // simply has a list of types, none of them named.
    layout_scope_->addAnonymousField(decl);
    return decl;
  }

  FieldSymbol *sym = new (pool_) FieldSymbol(decl, layout_scope_, name);
  decl->setSymbol(sym);
  if (te.resolved())
    sym->setType(te.resolved());

  if (Symbol *other = layout_scope_->localLookup(name)) {
    cc_.report(decl->loc(), rmsg::redefined_layout_decl)
      << "field"
      << name
      << other->kindName()
      << cc_.note(other->node()->loc(), rmsg::previous_location);
  } else {
    layout_scope_->addSymbol(sym);
  }

  return decl;
}

MethodDecl *
NameResolver::EnterMethodDecl(const SourceLocation &begin,
                              const NameToken &nameToken,
                              TypeSpecifier *spec,
                              TypeExpr *te)
{
  MethodDecl *decl = new (pool_) MethodDecl(begin, nameToken, FunctionOrAlias());

  if (spec)
    *te = resolve(*spec);

  MethodSymbol *sym =
    new (pool_) MethodSymbol(decl, layout_scope_, decl->name());
  decl->setSymbol(sym);

  // Once we support overloading, this will have to change.
  if (Symbol *other = layout_scope_->localLookup(decl->name())) {
    cc_.report(decl->loc(), rmsg::redefined_layout_decl)
      << "method"
      << decl->name()
      << other->kindName()
      << cc_.note(other->node()->loc(), rmsg::previous_location);
  } else {
    layout_scope_->addSymbol(sym);
  }

  return decl;
}

void
NameResolver::LeaveMethodDecl(MethodDecl *decl)
{
  FunctionNode *fun = decl->method()->fun();
  if (!fun)
    return;

  // Now that we've parsed the function, check if we were able to resolve its
  // signature.
  if (!fun->signature()->isResolved())
    tr_.addPending(decl);
}

FunctionSignature *
NameResolver::HandleFunctionSignature(TypeSpecifier &spec,
                                      ParameterList *params,
                                      bool canResolveEagerly)
{
  TypeExpr te = resolve(spec);
  return HandleFunctionSignature(te, params, canResolveEagerly);
}

FunctionSignature *
NameResolver::HandleFunctionSignature(const TypeExpr &te,
                                      ParameterList *params,
                                      bool canResolveEagerly)
{
  FunctionSignature *sig = new (pool_) FunctionSignature(te, params);
  if (te.resolved() && canResolveEagerly) {
#if defined(DEBUG)
    for (size_t i = 0; i < params->length(); i++)
      assert(params->at(i)->sym()->type());
#endif
    sig->setResolved();
  }

  return sig;
}

void
NameResolver::registerFunction(FunctionSymbol *sym)
{
  Scope *scope = sym->scope();
  assert(scope == globals_);

  Symbol *other = scope->localLookup(sym->name());
  if (!other) {
    scope->addSymbol(sym);
    return;
  }

  // If |other| is not a function, it's an error.
  FunctionSymbol *orig = other->asFunction();
  if (!orig) {
    reportRedeclaration(sym, other);
    return;
  }

  // If both have bodies, it's an error.
  FunctionStatement *sym_node = sym->node()->toFunctionStatement();
  FunctionStatement *orig_node = orig->node()->toFunctionStatement();
  if (sym_node->body() && orig_node->body()) {
    reportRedeclaration(sym, other);
    return;
  }

  // Build a shadow list, containing all symbols with this name.
  if (!orig->shadows()) {
    orig->setShadows(new (pool_) PoolList<Symbol *>());
    orig->shadows()->append(orig);
  }
  orig->shadows()->append(sym);
  sym_node->setShadowed(orig);
}

void
NameResolver::OnEnterFunctionDecl(FunctionStatement *node)
{
  // Currently, we only allow these at global scope.
  Scope *scope = getOrCreateScope();
  assert(scope == globals_);

  FunctionSymbol *sym =
    new (pool_) FunctionSymbol(node, scope, node->name());
  registerFunction(sym);
  node->setSymbol(sym);

  encountered_return_value_ = false;
}

void
NameResolver::OnLeaveFunctionDecl(FunctionStatement *node)
{
  FunctionSignature *sig = node->signature();

  // Function statements have quirky semantics around their return type. For
  // reasonable compatibility with SP1, we use the following heuristics for
  // when no explicit type is declared.
  TypeExpr &rt = sig->returnType();
  if ((rt.resolved() && rt.resolved()->isImplicitInt()) ||
      (rt.spec() && rt.spec()->resolver() == TOK_IMPLICIT_INT))
  {
    bool cell_required = (node->token() == TOK_FORWARD || node->token() == TOK_NATIVE);
    if (cell_required || encountered_return_value_)
      rt.setResolved(cc_.types()->getPrimitive(PrimitiveType::Int32));
    else
      rt.setResolved(cc_.types()->getVoid());
  }

  if (!sig->isResolved())
    tr_.addPending(node);
}

void
NameResolver::OnReturnStmt(ReturnStatement *stmt)
{
  if (stmt->expression())
    encountered_return_value_ = true;
}

TypedefDecl *
NameResolver::HandleTypedefDecl(const SourceLocation &begin,
                                Atom *name,
                                TypeSpecifier &spec)
{
  TypeExpr te = resolve(spec);
  TypedefDecl *node = new (pool_) TypedefDecl(begin, name, te);

  TypeSymbol *sym = new (pool_) TypeSymbol(node, getOrCreateScope(), node->name());
  TypedefType *type = cc_.types()->newTypedef(node->name());
  sym->setType(type);

  registerSymbol(sym);
  node->setSymbol(sym);

  if (te.resolved())
    type->resolve(te.resolved());
  else
    tr_.addPending(node);
  return node;
}

UnsafeCastExpr *
NameResolver::HandleUnsafeCast(const SourceLocation &pos, TypeSpecifier &spec, Expression *expr)
{
  TypeExpr te = resolve(spec);
  UnsafeCastExpr *node = new (pool_) UnsafeCastExpr(pos, te, expr);

  if (!te.resolved())
    tr_.addPending(node);
  return node;
}

CallNewExpr *
NameResolver::HandleCallNewExpr(const SourceLocation &pos,
                                TypeSpecifier &spec,
                                ExpressionList *args)
{
  TypeExpr te = resolve(spec);
  CallNewExpr *node = new (pool_) CallNewExpr(pos, te, args);

  if (!te.resolved())
    tr_.addPending(node);
  return node;
}

// Indicate that a type specifier can't be resolved it, so whatever is
// consuming it should add it to the resolver queue.
TypeExpr
NameResolver::delay(const TypeSpecifier &spec)
{
  return TypeExpr(new (pool_) TypeSpecifier(spec));
}

Type *
NameResolver::resolveBase(TypeSpecifier &spec)
{
  switch (spec.resolver()) {
    // These are the most common cases - either a primitive type or a signature
    // containing primitive types. In some cases we could have already resolved
    // the type even earlier, for example, the parser does this for certain =
    // builtin tags.
    case TOK_VOID:
      return cc_.types()->getVoid();
    case TOK_IMPLICIT_INT:
    case TOK_INT:
      return cc_.types()->getPrimitive(PrimitiveType::Int32);
    case TOK_BOOL:
      return cc_.types()->getPrimitive(PrimitiveType::Bool);
    case TOK_CHAR:
      return cc_.types()->getPrimitive(PrimitiveType::Char);
    case TOK_FLOAT:
      return cc_.types()->getPrimitive(PrimitiveType::Float);
    case TOK_DEFINED:
      return spec.getResolvedBase();
    case TOK_FUNCTION:
    {
      FunctionSignature *sig = spec.signature();
      if (!sig->isResolved())
        return nullptr;
      return FunctionType::New(sig);
    }

    case TOK_LABEL:
    case TOK_NAME:
    {
      NameProxy *proxy = spec.proxy();
      Symbol *sym = proxy->sym();
      if (!sym) {
        // This can happen if we use a type before it's been defined. We wait
        // until type resolution to try again.
        return nullptr;
      }

      TypeSymbol *ts = sym->asType();
      if (!ts) {
        cc_.report(proxy->loc(), rmsg::not_a_type) << sym->name();
        return nullptr;
      }

      // If we resolved a TypeSymbol, we must have allocated a Type object
      // (even if it's incomplete).
      assert(ts->type());
      return ts->type();
    }

    default:
      return nullptr;
  }
}

// We make a reasonable effort to resolve types early, since TypeSpecifier is
// quite a large structure (48 bytes on x86, as of this writing, and it will
// only get bigger). We want to eliminate it from the AST, as well as reduce
// dependence on TypeResolver which is a rather expensive pass.
TypeExpr
NameResolver::resolve(TypeSpecifier &spec)
{
  Type *type = resolveBase(spec);
  if (!type)
    return delay(spec);

  // Note: we are only updating the base type! We can't overwrite the whole
  // spec because it gets reused for parsing some decls.
  spec.setResolvedBaseType(type);

  // If the base type is an unresolved typedef, we have to wait.
  if (type->isUnresolvedTypedef())
    return delay(spec);

  // Error this early so we don't have to check it in each branch below.
  if (spec.rank()) {
    if (type->isVoid())
      cc_.report(spec.arrayLoc(), rmsg::array_of_void);
  }

  if (spec.dims()) {
    // If we have explicit dimension sizes, we have to bail out and wait for
    // type resolution (which also does constant resolution). We do special
    // case the very common integer literal, single-dimension case.
    if (spec.rank() != 1)
      return delay(spec);
    Expression *expr = spec.sizeOfRank(0);
    if (!expr || !expr->isIntegerLiteral())
      return delay(spec);
    IntegerLiteral *lit = expr->toIntegerLiteral();
    if (lit->value() < 1 || lit->value() > ArrayType::kMaxSize)
      return delay(spec);

    type = cc_.types()->newArray(type, (int32_t)lit->value());
  } else if (spec.rank()) {
    for (size_t i = 0; i < spec.rank(); i++)
      type = cc_.types()->newArray(type, ArrayType::kUnsized);
  }

  if (spec.isByRef()) {
    if (type->canUseInReferenceType()) {
      type = cc_.types()->newReference(type);
    } else {
      cc_.report(spec.byRefLoc(), rmsg::type_cannot_be_ref)
        << type;
    }
  }

  if (spec.isConst())
    type = cc_.types()->newQualified(type, Qualifiers::Const);

  return TypeExpr(type);
}