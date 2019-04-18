//===--- SemaModule.cpp - Semantic Analysis for Modules -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for modules (C++ modules syntax,
//  Objective-C modules syntax, and Clang header modules).
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/SemaInternal.h"

using namespace clang;
using namespace sema;

static void checkModuleImportContext(Sema &S, Module *M,
                                     SourceLocation ImportLoc, DeclContext *DC,
                                     bool FromInclude = false) {
  SourceLocation ExternCLoc;

  if (auto *LSD = dyn_cast<LinkageSpecDecl>(DC)) {
    switch (LSD->getLanguage()) {
    case LinkageSpecDecl::lang_c:
      if (ExternCLoc.isInvalid())
        ExternCLoc = LSD->getBeginLoc();
      break;
    case LinkageSpecDecl::lang_cxx:
      break;
    }
    DC = LSD->getParent();
  }

  while (isa<LinkageSpecDecl>(DC) || isa<ExportDecl>(DC))
    DC = DC->getParent();

  if (!isa<TranslationUnitDecl>(DC)) {
    S.Diag(ImportLoc, (FromInclude && S.isModuleVisible(M))
                          ? diag::ext_module_import_not_at_top_level_noop
                          : diag::err_module_import_not_at_top_level_fatal)
        << M->getFullModuleName() << DC;
    S.Diag(cast<Decl>(DC)->getBeginLoc(),
           diag::note_module_import_not_at_top_level)
        << DC;
  } else if (!M->IsExternC && ExternCLoc.isValid()) {
    S.Diag(ImportLoc, diag::ext_module_import_in_extern_c)
      << M->getFullModuleName();
    S.Diag(ExternCLoc, diag::note_extern_c_begins_here);
  }
}

Sema::DeclGroupPtrTy
Sema::ActOnGlobalModuleFragmentDecl(SourceLocation ModuleLoc) {
  if (!ModuleScopes.empty() &&
      ModuleScopes.back().Module->Kind == Module::GlobalModuleFragment) {
    // Under -std=c++2a -fmodules-ts, we can find an explicit 'module;' after
    // already implicitly entering the global module fragment. That's OK.
    assert(getLangOpts().CPlusPlusModules && getLangOpts().ModulesTS &&
           "unexpectedly encountered multiple global module fragment decls");
    ModuleScopes.back().BeginLoc = ModuleLoc;
    return nullptr;
  }

  // We start in the global module; all those declarations are implicitly
  // module-private (though they do not have module linkage).
  auto &Map = PP.getHeaderSearchInfo().getModuleMap();
  auto *GlobalModule = Map.createGlobalModuleForInterfaceUnit(ModuleLoc);
  assert(GlobalModule && "module creation should not fail");

  // Enter the scope of the global module.
  ModuleScopes.push_back({});
  ModuleScopes.back().BeginLoc = ModuleLoc;
  ModuleScopes.back().Module = GlobalModule;
  VisibleModules.setVisible(GlobalModule, ModuleLoc);

  // All declarations created from now on are owned by the global module.
  auto *TU = Context.getTranslationUnitDecl();
  TU->setModuleOwnershipKind(Decl::ModuleOwnershipKind::Visible);
  TU->setLocalOwningModule(GlobalModule);

  // FIXME: Consider creating an explicit representation of this declaration.
  return nullptr;
}

Sema::DeclGroupPtrTy
Sema::ActOnModuleDecl(SourceLocation StartLoc, SourceLocation ModuleLoc,
                      ModuleDeclKind MDK, ModuleIdPath Path, bool IsFirstDecl) {
  assert((getLangOpts().ModulesTS || getLangOpts().CPlusPlusModules) &&
         "should only have module decl in Modules TS or C++20");

  // A module implementation unit requires that we are not compiling a module
  // of any kind. A module interface unit requires that we are not compiling a
  // module map.
  switch (getLangOpts().getCompilingModule()) {
  case LangOptions::CMK_None:
    // It's OK to compile a module interface as a normal translation unit.
    break;

  case LangOptions::CMK_ModuleInterface:
    if (MDK != ModuleDeclKind::Implementation)
      break;

    // We were asked to compile a module interface unit but this is a module
    // implementation unit. That indicates the 'export' is missing.
    Diag(ModuleLoc, diag::err_module_interface_implementation_mismatch)
      << FixItHint::CreateInsertion(ModuleLoc, "export ");
    MDK = ModuleDeclKind::Interface;
    break;

  case LangOptions::CMK_ModuleMap:
    Diag(ModuleLoc, diag::err_module_decl_in_module_map_module);
    return nullptr;

  case LangOptions::CMK_HeaderModule:
    Diag(ModuleLoc, diag::err_module_decl_in_header_module);
    return nullptr;
  }

  assert(ModuleScopes.size() <= 1 && "expected to be at global module scope");

  // FIXME: Most of this work should be done by the preprocessor rather than
  // here, in order to support macro import.

  // Only one module-declaration is permitted per source file.
  if (!ModuleScopes.empty() &&
      ModuleScopes.back().Module->Kind == Module::ModuleInterfaceUnit) {
    Diag(ModuleLoc, diag::err_module_redeclaration);
    Diag(VisibleModules.getImportLoc(ModuleScopes.back().Module),
         diag::note_prev_module_declaration);
    return nullptr;
  }

  // Find the global module fragment we're adopting into this module, if any.
  Module *GlobalModuleFragment = nullptr;
  if (!ModuleScopes.empty() &&
      ModuleScopes.back().Module->Kind == Module::GlobalModuleFragment)
    GlobalModuleFragment = ModuleScopes.back().Module;

  // In C++20, the module-declaration must be the first declaration if there
  // is no global module fragment.
  if (getLangOpts().CPlusPlusModules && !IsFirstDecl && !GlobalModuleFragment) {
    Diag(ModuleLoc, diag::err_module_decl_not_at_start);
    SourceLocation BeginLoc =
        ModuleScopes.empty()
            ? SourceMgr.getLocForStartOfFile(SourceMgr.getMainFileID())
            : ModuleScopes.back().BeginLoc;
    if (BeginLoc.isValid()) {
      Diag(BeginLoc, diag::note_global_module_introducer_missing)
          << FixItHint::CreateInsertion(BeginLoc, "module;\n");
    }
  }

  // Flatten the dots in a module name. Unlike Clang's hierarchical module map
  // modules, the dots here are just another character that can appear in a
  // module name.
  std::string ModuleName;
  for (auto &Piece : Path) {
    if (!ModuleName.empty())
      ModuleName += ".";
    ModuleName += Piece.first->getName();
  }

  // If a module name was explicitly specified on the command line, it must be
  // correct.
  if (!getLangOpts().CurrentModule.empty() &&
      getLangOpts().CurrentModule != ModuleName) {
    Diag(Path.front().second, diag::err_current_module_name_mismatch)
        << SourceRange(Path.front().second, Path.back().second)
        << getLangOpts().CurrentModule;
    return nullptr;
  }
  const_cast<LangOptions&>(getLangOpts()).CurrentModule = ModuleName;

  auto &Map = PP.getHeaderSearchInfo().getModuleMap();
  Module *Mod;

  switch (MDK) {
  case ModuleDeclKind::Interface: {
    // We can't have parsed or imported a definition of this module or parsed a
    // module map defining it already.
    if (auto *M = Map.findModule(ModuleName)) {
      Diag(Path[0].second, diag::err_module_redefinition) << ModuleName;
      if (M->DefinitionLoc.isValid())
        Diag(M->DefinitionLoc, diag::note_prev_module_definition);
      else if (const auto *FE = M->getASTFile())
        Diag(M->DefinitionLoc, diag::note_prev_module_definition_from_ast_file)
            << FE->getName();
      Mod = M;
      break;
    }

    // Create a Module for the module that we're defining.
    Mod = Map.createModuleForInterfaceUnit(ModuleLoc, ModuleName,
                                           GlobalModuleFragment);
    assert(Mod && "module creation should not fail");
    break;
  }

  case ModuleDeclKind::Implementation:
    std::pair<IdentifierInfo *, SourceLocation> ModuleNameLoc(
        PP.getIdentifierInfo(ModuleName), Path[0].second);
    Mod = getModuleLoader().loadModule(ModuleLoc, {ModuleNameLoc},
                                       Module::AllVisible,
                                       /*IsIncludeDirective=*/false);
    if (!Mod) {
      Diag(ModuleLoc, diag::err_module_not_defined) << ModuleName;
      // Create an empty module interface unit for error recovery.
      Mod = Map.createModuleForInterfaceUnit(ModuleLoc, ModuleName,
                                             GlobalModuleFragment);
    }
    break;
  }

  if (!GlobalModuleFragment) {
    ModuleScopes.push_back({});
    if (getLangOpts().ModulesLocalVisibility)
      ModuleScopes.back().OuterVisibleModules = std::move(VisibleModules);
  }

  // Switch from the global module fragment (if any) to the named module.
  ModuleScopes.back().BeginLoc = StartLoc;
  ModuleScopes.back().Module = Mod;
  ModuleScopes.back().ModuleInterface = MDK != ModuleDeclKind::Implementation;
  VisibleModules.setVisible(Mod, ModuleLoc);

  // From now on, we have an owning module for all declarations we see.
  // However, those declarations are module-private unless explicitly
  // exported.
  auto *TU = Context.getTranslationUnitDecl();
  TU->setModuleOwnershipKind(Decl::ModuleOwnershipKind::ModulePrivate);
  TU->setLocalOwningModule(Mod);

  // FIXME: Create a ModuleDecl.
  return nullptr;
}

DeclResult Sema::ActOnModuleImport(SourceLocation StartLoc,
                                   SourceLocation ExportLoc,
                                   SourceLocation ImportLoc,
                                   ModuleIdPath Path) {
  // Flatten the module path for a Modules TS module name.
  std::pair<IdentifierInfo *, SourceLocation> ModuleNameLoc;
  if (getLangOpts().ModulesTS) {
    std::string ModuleName;
    for (auto &Piece : Path) {
      if (!ModuleName.empty())
        ModuleName += ".";
      ModuleName += Piece.first->getName();
    }
    ModuleNameLoc = {PP.getIdentifierInfo(ModuleName), Path[0].second};
    Path = ModuleIdPath(ModuleNameLoc);
  }

  Module *Mod =
      getModuleLoader().loadModule(ImportLoc, Path, Module::AllVisible,
                                   /*IsIncludeDirective=*/false);
  if (!Mod)
    return true;

  return ActOnModuleImport(StartLoc, ExportLoc, ImportLoc, Mod, Path);
}

DeclResult Sema::ActOnModuleImport(SourceLocation StartLoc,
                                   SourceLocation ExportLoc,
                                   SourceLocation ImportLoc,
                                   Module *Mod, ModuleIdPath Path) {
  VisibleModules.setVisible(Mod, ImportLoc);

  checkModuleImportContext(*this, Mod, ImportLoc, CurContext);

  // FIXME: we should support importing a submodule within a different submodule
  // of the same top-level module. Until we do, make it an error rather than
  // silently ignoring the import.
  // Import-from-implementation is valid in the Modules TS. FIXME: Should we
  // warn on a redundant import of the current module?
  // FIXME: Import of a module from an implementation partition of the same
  // module is permitted.
  if (Mod->getTopLevelModuleName() == getLangOpts().CurrentModule &&
      (getLangOpts().isCompilingModule() || !getLangOpts().ModulesTS)) {
    Diag(ImportLoc, getLangOpts().isCompilingModule()
                        ? diag::err_module_self_import
                        : diag::err_module_import_in_implementation)
        << Mod->getFullModuleName() << getLangOpts().CurrentModule;
  }

  SmallVector<SourceLocation, 2> IdentifierLocs;
  Module *ModCheck = Mod;
  for (unsigned I = 0, N = Path.size(); I != N; ++I) {
    // If we've run out of module parents, just drop the remaining identifiers.
    // We need the length to be consistent.
    if (!ModCheck)
      break;
    ModCheck = ModCheck->Parent;

    IdentifierLocs.push_back(Path[I].second);
  }

  // If this was a header import, pad out with dummy locations.
  // FIXME: Pass in and use the location of the header-name token in this case.
  if (Path.empty()) {
    for (; ModCheck; ModCheck = ModCheck->Parent) {
      IdentifierLocs.push_back(SourceLocation());
    }
  }

  ImportDecl *Import = ImportDecl::Create(Context, CurContext, StartLoc,
                                          Mod, IdentifierLocs);
  CurContext->addDecl(Import);

  // Sequence initialization of the imported module before that of the current
  // module, if any.
  if (!ModuleScopes.empty())
    Context.addModuleInitializer(ModuleScopes.back().Module, Import);

  // Re-export the module if needed.
  if (!ModuleScopes.empty() && ModuleScopes.back().ModuleInterface) {
    if (ExportLoc.isValid() || Import->isExported())
      getCurrentModule()->Exports.emplace_back(Mod, false);
  } else if (ExportLoc.isValid()) {
    Diag(ExportLoc, diag::err_export_not_in_module_interface);
  }

  return Import;
}

void Sema::ActOnModuleInclude(SourceLocation DirectiveLoc, Module *Mod) {
  checkModuleImportContext(*this, Mod, DirectiveLoc, CurContext, true);
  BuildModuleInclude(DirectiveLoc, Mod);
}

void Sema::BuildModuleInclude(SourceLocation DirectiveLoc, Module *Mod) {
  // Determine whether we're in the #include buffer for a module. The #includes
  // in that buffer do not qualify as module imports; they're just an
  // implementation detail of us building the module.
  //
  // FIXME: Should we even get ActOnModuleInclude calls for those?
  bool IsInModuleIncludes =
      TUKind == TU_Module &&
      getSourceManager().isWrittenInMainFile(DirectiveLoc);

  bool ShouldAddImport = !IsInModuleIncludes;

  // If this module import was due to an inclusion directive, create an
  // implicit import declaration to capture it in the AST.
  if (ShouldAddImport) {
    TranslationUnitDecl *TU = getASTContext().getTranslationUnitDecl();
    ImportDecl *ImportD = ImportDecl::CreateImplicit(getASTContext(), TU,
                                                     DirectiveLoc, Mod,
                                                     DirectiveLoc);
    if (!ModuleScopes.empty())
      Context.addModuleInitializer(ModuleScopes.back().Module, ImportD);
    TU->addDecl(ImportD);
    Consumer.HandleImplicitImportDecl(ImportD);
  }

  getModuleLoader().makeModuleVisible(Mod, Module::AllVisible, DirectiveLoc);
  VisibleModules.setVisible(Mod, DirectiveLoc);
}

void Sema::ActOnModuleBegin(SourceLocation DirectiveLoc, Module *Mod) {
  checkModuleImportContext(*this, Mod, DirectiveLoc, CurContext, true);

  ModuleScopes.push_back({});
  ModuleScopes.back().Module = Mod;
  if (getLangOpts().ModulesLocalVisibility)
    ModuleScopes.back().OuterVisibleModules = std::move(VisibleModules);

  VisibleModules.setVisible(Mod, DirectiveLoc);

  // The enclosing context is now part of this module.
  // FIXME: Consider creating a child DeclContext to hold the entities
  // lexically within the module.
  if (getLangOpts().trackLocalOwningModule()) {
    for (auto *DC = CurContext; DC; DC = DC->getLexicalParent()) {
      cast<Decl>(DC)->setModuleOwnershipKind(
          getLangOpts().ModulesLocalVisibility
              ? Decl::ModuleOwnershipKind::VisibleWhenImported
              : Decl::ModuleOwnershipKind::Visible);
      cast<Decl>(DC)->setLocalOwningModule(Mod);
    }
  }
}

void Sema::ActOnModuleEnd(SourceLocation EomLoc, Module *Mod) {
  if (getLangOpts().ModulesLocalVisibility) {
    VisibleModules = std::move(ModuleScopes.back().OuterVisibleModules);
    // Leaving a module hides namespace names, so our visible namespace cache
    // is now out of date.
    VisibleNamespaceCache.clear();
  }

  assert(!ModuleScopes.empty() && ModuleScopes.back().Module == Mod &&
         "left the wrong module scope");
  ModuleScopes.pop_back();

  // We got to the end of processing a local module. Create an
  // ImportDecl as we would for an imported module.
  FileID File = getSourceManager().getFileID(EomLoc);
  SourceLocation DirectiveLoc;
  if (EomLoc == getSourceManager().getLocForEndOfFile(File)) {
    // We reached the end of a #included module header. Use the #include loc.
    assert(File != getSourceManager().getMainFileID() &&
           "end of submodule in main source file");
    DirectiveLoc = getSourceManager().getIncludeLoc(File);
  } else {
    // We reached an EOM pragma. Use the pragma location.
    DirectiveLoc = EomLoc;
  }
  BuildModuleInclude(DirectiveLoc, Mod);

  // Any further declarations are in whatever module we returned to.
  if (getLangOpts().trackLocalOwningModule()) {
    // The parser guarantees that this is the same context that we entered
    // the module within.
    for (auto *DC = CurContext; DC; DC = DC->getLexicalParent()) {
      cast<Decl>(DC)->setLocalOwningModule(getCurrentModule());
      if (!getCurrentModule())
        cast<Decl>(DC)->setModuleOwnershipKind(
            Decl::ModuleOwnershipKind::Unowned);
    }
  }
}

void Sema::createImplicitModuleImportForErrorRecovery(SourceLocation Loc,
                                                      Module *Mod) {
  // Bail if we're not allowed to implicitly import a module here.
  if (isSFINAEContext() || !getLangOpts().ModulesErrorRecovery ||
      VisibleModules.isVisible(Mod))
    return;

  // Create the implicit import declaration.
  TranslationUnitDecl *TU = getASTContext().getTranslationUnitDecl();
  ImportDecl *ImportD = ImportDecl::CreateImplicit(getASTContext(), TU,
                                                   Loc, Mod, Loc);
  TU->addDecl(ImportD);
  Consumer.HandleImplicitImportDecl(ImportD);

  // Make the module visible.
  getModuleLoader().makeModuleVisible(Mod, Module::AllVisible, Loc);
  VisibleModules.setVisible(Mod, Loc);
}

/// We have parsed the start of an export declaration, including the '{'
/// (if present).
Decl *Sema::ActOnStartExportDecl(Scope *S, SourceLocation ExportLoc,
                                 SourceLocation LBraceLoc) {
  ExportDecl *D = ExportDecl::Create(Context, CurContext, ExportLoc);

  // C++ Modules TS draft:
  //   An export-declaration shall appear in the purview of a module other than
  //   the global module.
  if (ModuleScopes.empty() || !ModuleScopes.back().ModuleInterface)
    Diag(ExportLoc, diag::err_export_not_in_module_interface);

  //   An export-declaration [...] shall not contain more than one
  //   export keyword.
  //
  // The intent here is that an export-declaration cannot appear within another
  // export-declaration.
  if (D->isExported())
    Diag(ExportLoc, diag::err_export_within_export);

  CurContext->addDecl(D);
  PushDeclContext(S, D);
  D->setModuleOwnershipKind(Decl::ModuleOwnershipKind::VisibleWhenImported);
  return D;
}

/// Complete the definition of an export declaration.
Decl *Sema::ActOnFinishExportDecl(Scope *S, Decl *D, SourceLocation RBraceLoc) {
  auto *ED = cast<ExportDecl>(D);
  if (RBraceLoc.isValid())
    ED->setRBraceLoc(RBraceLoc);

  // FIXME: Diagnose export of internal-linkage declaration (including
  // anonymous namespace).

  PopDeclContext();
  return D;
}
