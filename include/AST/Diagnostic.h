#pragma once

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Syntax/Tokens.h>
#include <clang/Lex/PPCallbacks.h>