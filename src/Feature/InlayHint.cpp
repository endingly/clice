#include "Basic/SourceConverter.h"
#include "Compiler/Compilation.h"
#include "Feature/InlayHint.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice {

namespace {

/// TODO:
/// Replace blank tooltip to something useful.

/// Create a blank markup content as a place holder.
proto::MarkupContent blank() {
    return {
        .value = "",
    };
}

using Kind = feature::inlay_hint::InlayHintKind;
using feature::inlay_hint::Lable;
using feature::inlay_hint::InlayHint;
using feature::inlay_hint::Result;

/// Compute inlay hints for a AST. There is two kind of collection:
///     A. Only collect hints in MainFileID.
///     B. Collect hints in each files, used for header context.
/// The result is always stored in a densmap<FileID, vector<InlayHint>>, and return as needed.
struct InlayHintCollector : clang::RecursiveASTVisitor<InlayHintCollector> {

    using Base = clang::RecursiveASTVisitor<InlayHintCollector>;

    /// The result of inlay hints for given AST.
    using Storage = llvm::DenseMap<clang::FileID, Result>;

    const clang::SourceManager& src;

    const SourceConverter& cvtr;

    /// The restrict range of request.
    const LocalSourceRange limit;

    /// The config of inlay hints collector.
    const config::InlayHintOption config;

    /// Indicate that only hints in main file should be collected (mode A).
    const bool onlyMain;

    /// The result of inlay hints.
    Storage result;

    /// The printing policy of AST.
    const clang::PrintingPolicy policy;

    /// Whole source code text in main file.
    const llvm::StringRef code;

    /// Do not produce inlay hints if either range ends is not within the main file.
    bool needFilter(clang::SourceRange range) {
        // skip invalid range or not in main file
        if(range.isInvalid())
            return true;

        if(!onlyMain)
            return false;

        if(!src.isInMainFile(range.getBegin()) || !src.isInMainFile(range.getEnd()))
            return true;

        // not involved in restrict range
        auto begin = src.getDecomposedLoc(range.getBegin()).second;
        auto end = src.getDecomposedLoc(range.getEnd()).second;
        if(end < limit.begin || begin > limit.end)
            return true;

        return false;
    }

    /// Shrink the hint text to the max length.
    static std::string shrinkHintText(std::string text, size_t maxLength) {
        if(text.size() > maxLength)
            text.resize(maxLength - 3), text.append("...");

        text.shrink_to_fit();
        return text;
    }

    std::string tryShrinkHintText(std::string text) {
        return onlyMain ? shrinkHintText(text, config.maxLength) : text;
    }

    /// Collect hint for variable declared with `auto` keywords.
    /// The hint string wiil be placed at the right side of identifier, starting with ':' character.
    /// The `originDeclRange` will be used as the link of hint string.
    void collectAutoDeclTypeHint(clang::QualType deduced, clang::SourceRange identRange,
                                 std::optional<clang::SourceRange> linkDeclRange, Kind kind) {

        // For lambda expression, `getAsString` return a text like `(lambda at main.cpp:2:10)`
        //      auto lambda = [](){ return 1; };
        // Use a short text instead.
        std::string typeName = deduced.getAsString(policy);
        if(typeName.contains("lambda"))
            typeName = "(lambda)";

        Lable lable{
            .value = tryShrinkHintText(std::format(": {}", typeName)),
        };

        if(linkDeclRange.has_value())
            lable.location = cvtr.toLocalRange(*linkDeclRange, src);

        InlayHint hint{
            .kind = kind,
            .offset = src.getDecomposedLoc(identRange.getEnd()).second,
            .lable = lable,
        };

        clang::FileID fid = onlyMain ? src.getMainFileID() : src.getFileID(identRange.getBegin());
        result[fid].push_back(std::move(hint));
    }

    // If `expr` spells a single unqualified identifier, return that name, otherwise, return an
    // empty string.
    static llvm::StringRef takeExprIdentifier(const clang::Expr* expr) {
        auto spelled = expr->IgnoreUnlessSpelledInSource();
        if(auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(spelled))
            if(!declRef->getQualifier())
                return declRef->getDecl()->getName();
        if(auto* member = llvm::dyn_cast<clang::MemberExpr>(spelled))
            if(!member->getQualifier() && member->isImplicitAccess())
                return member->getMemberDecl()->getName();
        return {};
    }

    /// Check if there is any comment like /*paramName*/ before a argument.
    bool hasHandWriteComment(clang::SourceRange argument) {
        auto [fid, offset] = src.getDecomposedLoc(argument.getBegin());
        if(fid != src.getMainFileID())
            return false;

        // Get source text until the argument and drop end whitespace.
        llvm::StringRef content = code.substr(0, offset).rtrim();

        // Any comment ends with `*/` is considered meaningful.
        return content.ends_with("*/");
    }

    bool needHintArgument(const clang::ParmVarDecl* param, const clang::Expr* arg) {
        auto name = param->getName();

        // Skip anonymous parameters.
        if(name.empty())
            return false;

        // Skip if the argument is a single name and it matches the parameter exactly.
        if(name.equals_insensitive(takeExprIdentifier(arg)))
            return false;

        // Skip if the argument is preceded by any hand-written hint /*paramName*/.
        if(hasHandWriteComment(arg->getSourceRange()))
            return false;

        return true;
    }

    bool isPassedAsMutableLValueRef(const clang::ParmVarDecl* param) {
        auto qual = param->getType();
        return qual->isLValueReferenceType() && !qual.getNonReferenceType().isConstQualified();
    }

    void collectArgumentHint(llvm::ArrayRef<const clang::ParmVarDecl*> params,
                             llvm::ArrayRef<const clang::Expr*> args, Kind kind) {
        for(size_t i = 0; i < params.size() && i < args.size(); ++i) {
            // Pack expansion and default argument is always the tail of arguments.
            if(llvm::isa<clang::PackExpansionExpr>(args[i]) ||
               llvm::isa<clang::CXXDefaultArgExpr>(args[i]))
                break;

            if(!needHintArgument(params[i], args[i]))
                continue;

            // Only hint reference for mutable lvalue reference.
            const bool hintRef = isPassedAsMutableLValueRef(params[i]);

            auto parmName = std::format("{}{}:", params[i]->getName(), hintRef ? "&" : "");
            Lable lable{
                .value = tryShrinkHintText(std::move(parmName)),
                .location = cvtr.toLocalRange(params[i]->getSourceRange(), src),
            };

            auto argBeginLoc = args[i]->getSourceRange().getBegin();
            InlayHint hint{
                .kind = kind,
                .offset = src.getDecomposedLoc(argBeginLoc).second,
                .lable = std::move(lable),
            };

            clang::FileID fid = onlyMain ? src.getMainFileID() : src.getFileID(argBeginLoc);
            result[fid].push_back(std::move(hint));
        }
    }

    bool TraverseDecl(clang::Decl* decl) {
        if(!decl || needFilter(decl->getSourceRange()))
            return true;

        return Base::TraverseDecl(decl);
    }

    bool VisitVarDecl(const clang::VarDecl* decl) {
        // Hint local variable, global variable, and structure binding.
        if(!decl->isLocalVarDecl() && !decl->isFileVarDecl())
            return true;

        if(!config.dedcucedType)
            return true;

        // Hint for indivadual element of structure binding.
        if(auto bind = llvm::dyn_cast<clang::DecompositionDecl>(decl)) {
            for(auto* binding: bind->bindings()) {
                // Hint for used variable only.
                if(auto type = binding->getType(); !type.isNull() && !type->isDependentType()) {
                    // Hint at the end position of identifier.
                    auto name = binding->getName();
                    collectAutoDeclTypeHint(type.getCanonicalType(),
                                            binding->getBeginLoc().getLocWithOffset(name.size()),
                                            decl->getSourceRange(),
                                            Kind::StructureBinding);
                }
            }
            return true;
        }

        /// skip dependent type.
        clang::QualType qty = decl->getType();
        if(qty.isNull() || qty->isDependentType())
            return true;

        if(const auto at = qty->getContainedAutoType()) {
            // Use most recent decl as the link of hint string.
            /// FIXME:
            /// Shall we use the first decl as the link of hint string?
            std::optional<clang::SourceRange> originDeclRange;
            if(const auto mrd = decl->getMostRecentDecl())
                originDeclRange = mrd->getSourceRange();

            auto tailOfIdentifier = decl->getLocation().getLocWithOffset(decl->getName().size());
            collectAutoDeclTypeHint(qty, tailOfIdentifier, originDeclRange, Kind::AutoDecl);
        }
        return true;
    }

    static bool isBuiltinFnCall(const clang::CallExpr* expr) {
        namespace btin = clang::Builtin;
        switch(expr->getBuiltinCallee()) {
            case btin::BIaddressof:
            case btin::BIas_const:
            case btin::BIforward:
            case btin::BImove:
            case btin::BImove_if_noexcept: return true;
            default: return false;
        }
    }

    /// Try find the FunctionProtoType of a CallExpr which callee is a function pointer.
    static auto detectCallViaFnPointer(const clang::Expr* call)
        -> std::optional<clang::FunctionProtoTypeLoc> {

        auto nake = call->IgnoreParenCasts();
        clang::TypeLoc target;

        if(auto* tydef = nake->getType().getTypePtr()->getAs<clang::TypedefType>())
            target = tydef->getDecl()->getTypeSourceInfo()->getTypeLoc();
        else if(auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(nake))
            if(auto* varDecl = llvm::dyn_cast<clang::VarDecl>(declRef->getDecl()))
                target = varDecl->getTypeSourceInfo()->getTypeLoc();

        if(!target)
            return std::nullopt;

        // Unwrap types that may be wrapping the function type.
        while(true) {
            if(auto P = target.getAs<clang::PointerTypeLoc>())
                target = P.getPointeeLoc();
            else if(auto A = target.getAs<clang::AttributedTypeLoc>())
                target = A.getModifiedLoc();
            else if(auto P = target.getAs<clang::ParenTypeLoc>())
                target = P.getInnerLoc();
            else
                break;
        }

        if(auto proto = target.getAs<clang::FunctionProtoTypeLoc>())
            return proto;

        return std::nullopt;
    }

    bool VisitCallExpr(const clang::CallExpr* call) {
        if(!config.paramName)
            return true;

        // Don't hint for UDL operator like `operaotr ""_str` , and builtin funtion.
        if(!call || llvm::isa<clang::UserDefinedLiteral>(call) || isBuiltinFnCall(call))
            return true;

        // They were handled in  `VisitCXXMemberCallExpr`, `VisitCXXOperatorCallExpr`.
        if(llvm::isa<clang::CXXMemberCallExpr>(call) || llvm::isa<clang::CXXOperatorCallExpr>(call))
            return true;

        // For a CallExpr, there are 2 case of Callee:
        //     1. An object which has coresponding FunctionDecl, free function or method.
        //     2. A function pointer, which has no FunctionDecl but FunctionProtoTypeLoc.

        // Use FunctionDecl if callee is a free function or method.
        const clang::FunctionDecl* fndecl = nullptr;
        const clang::Decl* calleeDecl = call->getCalleeDecl();
        if(auto decl = llvm::dyn_cast<clang::FunctionDecl>(calleeDecl))
            fndecl = decl;
        else if(auto tfndecl = llvm::dyn_cast<clang::FunctionTemplateDecl>(calleeDecl))
            fndecl = tfndecl->getTemplatedDecl();

        llvm::ArrayRef<const clang::Expr*> arguments = {call->getArgs(), call->getNumArgs()};
        if(fndecl)
            // free function
            collectArgumentHint(fndecl->parameters(), arguments, Kind::Parameter);
        else if(auto proto = detectCallViaFnPointer(call->getCallee()); proto.has_value())
            // function pointer
            collectArgumentHint(proto->getParams(), arguments, Kind::Parameter);

        return true;
    }

    bool VisitCXXOperatorCallExpr(const clang::CXXOperatorCallExpr* call) {
        if(!config.paramName)
            return true;

        // Do not hint paramters for operator overload except `operator()`, and `operator[]` with
        // only one parameter.
        auto opkind = call->getOperator();
        if(opkind == clang::OO_Call || opkind == clang::OO_Subscript && call->getNumArgs() != 1) {
            auto method = llvm::dyn_cast<clang::CXXMethodDecl>(call->getCalleeDecl());

            llvm::ArrayRef<const clang::ParmVarDecl*> params{method->parameters()};
            llvm::ArrayRef<const clang::Expr*> args{call->getArgs(), call->getNumArgs()};

            // Skip `this` parameter declaration if callee is CXXMethodDecl.
            if(!method->hasCXXExplicitFunctionObjectParameter())
                args = args.drop_front();

            collectArgumentHint(params, args, Kind::Parameter);
        }

        return true;
    }

    static bool isSimpleSetter(const clang::CXXMethodDecl* md) {
        if(md->getNumParams() != 1)
            return false;

        auto name = md->getName();
        if(!name.starts_with_insensitive("set"))
            return false;

        // Check that the part after "set" matches the name of the parameter (ignoring case). The
        // idea here is that if the parameter name differs, it may contain extra information that
        // may be useful to show in a hint, as in:
        //   void setTimeout(int timeoutMillis);
        // The underscores in FunctionName and Parameter will be ignored.
        llvm::SmallString<32> param, fnname;
        for(auto c: name.drop_front(3))
            if(c != '_')
                fnname.push_back(c);

        for(auto c: md->getParamDecl(0)->getName())
            if(c != '_')
                param.push_back(c);

        return fnname.equals_insensitive(param);
    }

    bool VisitCXXMemberCallExpr(const clang::CXXMemberCallExpr* call) {
        if(!config.paramName)
            return true;

        auto callee = llvm::dyn_cast<clang::FunctionDecl>(call->getCalleeDecl());

        // Do not hint move / copy constructor call.
        if(auto ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(callee))
            if(ctor->isCopyOrMoveConstructor())
                return true;

        // Do not hint simple setter function call. e.g. `setX(1)`.
        if(auto md = llvm::dyn_cast<clang::CXXMethodDecl>(callee))
            if(isSimpleSetter(md))
                return true;

        llvm::ArrayRef<const clang::ParmVarDecl*> params{callee->parameters()};
        llvm::ArrayRef<const clang::Expr*> args{call->getArgs(), call->getNumArgs()};

        // Skip `this` parameter declaration if callee is CXXMethodDecl.
        if(auto md = llvm::dyn_cast<clang::CXXMethodDecl>(callee))
            if(md->hasCXXExplicitFunctionObjectParameter())
                args = args.drop_front();

        collectArgumentHint(params, args, Kind::Parameter);
        return true;
    }

    bool VisitCXXConstructExpr(const clang::CXXConstructExpr* ctor) {
        if(!config.paramName)
            return true;

        // Skip constructor call without an argument list, by checking the validity of
        // getParenOrBraceRange(). Also skip std::initializer_list constructors.
        if(!ctor->getParenOrBraceRange().isValid() || ctor->isStdInitListInitialization())
            return true;

        if(const auto decl = ctor->getConstructor())
            collectArgumentHint(decl->parameters(),
                                {ctor->getArgs(), ctor->getNumArgs()},
                                Kind::Constructor);

        return true;
    }

    void collectReturnTypeHint(clang::SourceLocation hintLoc, clang::QualType retType,
                               clang::SourceRange retTypeDeclRange, Kind kind) {
        Lable lable{
            .value = tryShrinkHintText(std::format("-> {}", retType.getAsString(policy))),
            .location = cvtr.toLocalRange(retTypeDeclRange, src),
        };
        InlayHint hint{
            .kind = kind,
            .offset = src.getDecomposedLoc(hintLoc).second,
            .lable = std::move(lable),
        };

        clang::FileID fid = onlyMain ? src.getMainFileID() : src.getFileID(hintLoc);
        result[fid].push_back(std::move(hint));
    }

    bool VisitFunctionDecl(const clang::FunctionDecl* decl) {
        // 1. Hint block end.
        if(config.blockEnd && decl->isThisDeclarationADefinition() &&
           isMultiLineRange(decl->getSourceRange())) {
            /// FIXME:
            /// Use a proper name such as simplified signature of funtion.
            auto typeLoc = decl->getTypeSourceInfo()->getTypeLoc().getSourceRange();
            auto begin = src.getCharacterData(typeLoc.getBegin());
            auto end = src.getCharacterData(typeLoc.getEnd());
            llvm::StringRef piece{begin, static_cast<size_t>(end - begin) + 1};

            // Right side of '}'
            collectBlockEndHint(decl->getBodyRBrace().getLocWithOffset(1),
                                std::format("// {}", piece),
                                decl->getSourceRange(),
                                Kind::FunctionEnd,
                                DecideDuplicated::Ignore);
        }

        // 2. Hint return type.
        if(!config.returnType)
            return true;

        if(auto proto = llvm::dyn_cast<clang::FunctionProtoType>(decl->getType().getTypePtr()))
            if(proto->hasTrailingReturn())
                return true;

        if(auto fnTypeLoc = decl->getFunctionTypeLoc())
            // Hint for function declaration with `auto` or `decltype(...)` return type.
            if(fnTypeLoc.getReturnLoc().getContainedAutoTypeLoc())
                // Right side of ')' in parameter list.
                collectReturnTypeHint(fnTypeLoc.getRParenLoc().getLocWithOffset(1),
                                      decl->getReturnType(),
                                      decl->getSourceRange(),
                                      Kind::FunctionReturnType);

        return true;
    }

    bool VisitLambdaExpr(const clang::LambdaExpr* expr) {
        // 1. Hint block end.
        if(config.blockEnd && isMultiLineRange(expr->getBody()->getSourceRange()))
            collectBlockEndHint(
                expr->getEndLoc().getLocWithOffset(1),
                std::format("// lambda #{}", expr->getLambdaClass()->getLambdaManglingNumber()),
                expr->getSourceRange(),
                Kind::LambdaBodyEnd,
                DecideDuplicated::Replace);

        // 2. Hint return type.
        if(!config.returnType)
            return true;

        clang::FunctionDecl* decl = expr->getCallOperator();
        if(expr->hasExplicitResultType())
            return true;

        // where to place the hint position, in default it is an invalid value.
        clang::SourceLocation hintLoc = {};
        if(!expr->hasExplicitParameters())
            // right side of ']' after the capture list.
            hintLoc = expr->getIntroducerRange().getEnd().getLocWithOffset(1);
        else if(auto fnTypeLoc = decl->getFunctionTypeLoc())
            // right side of ')'.
            hintLoc = fnTypeLoc.getRParenLoc().getLocWithOffset(1);

        if(hintLoc.isValid())
            collectReturnTypeHint(hintLoc,
                                  decl->getReturnType(),
                                  decl->getSourceRange(),
                                  Kind::LambdaReturnType);

        return true;
    }

    void collectArrayElemIndexHint(int index, clang::SourceLocation location) {
        Lable lable{
            .value = std::format("[{}]=", index),  // This shouldn't be shrinked.
            .location = cvtr.toLocalRange(location, src),
        };

        InlayHint hint{
            .kind = Kind::ArrayIndex,
            .offset = src.getDecomposedLoc(location).second,
            .lable = std::move(lable),
        };

        clang::FileID fid = onlyMain ? src.getMainFileID() : src.getFileID(location);
        result[fid].push_back(std::move(hint));
    }

    bool VisitInitListExpr(const clang::InitListExpr* Syn) {
        int count = 0;
        for(auto init: Syn->inits()) {
            if(llvm::isa<clang::DesignatedInitExpr>(init) ||
               hasHandWriteComment(init->getSourceRange()))
                continue;

            // Only hint for the first config.maxArrayElements elements.
            if(count++ >= config.maxArrayElements)
                break;

            collectArrayElemIndexHint(count, init->getBeginLoc());
        }
        return true;
    }

    bool isMultiLineRange(const clang::SourceRange range) {
        return range.isValid() && src.getPresumedLineNumber(range.getBegin()) <
                                      src.getPresumedLineNumber(range.getEnd());
    }

    llvm::StringRef remainTextOfThatLine(clang::SourceLocation location) {
        auto [_, offset] = src.getDecomposedLoc(location);
        auto remain = code.substr(offset).split('\n').first;
        return remain.ltrim();
    }

    /// This enum decide how to handle the duplicated block-end hint in the same line.
    enum class DecideDuplicated {
        // Accept all hints.
        AcceptBoth,

        // Drop the old hint, and accept the new hint. Commonly use the inner one.
        //      namespace out::in {
        //      } |// namespace in|
        Replace,

        // Ignore the new hint, and keep the old hint. Commonly use the outer one.
        //      struct Out {
        //          struct In {
        //      }} |// struct Out|;
        Ignore,
    };

    void collectBlockEndHint(clang::SourceLocation location, std::string text,
                             clang::SourceRange linkRange, Kind kind, DecideDuplicated decision) {
        // Already has a comment in that line.
        if(auto remain = remainTextOfThatLine(location);
           remain.starts_with("/*") || remain.starts_with("//"))
            return;

        auto& state = result[onlyMain ? src.getMainFileID() : src.getFileID(location)];
        if(decision != DecideDuplicated::AcceptBoth && !state.empty()) {
            // Already has a duplicated hint in that line, use the newer hint instead.
            auto lastHintLine = cvtr.toPosition(code, state.back().offset).line;
            auto thatLine = cvtr.toPosition(location, src).line;
            if(lastHintLine == thatLine) {
                if(decision == DecideDuplicated::Replace)
                    state.pop_back();
                else
                    return;  // use the old one.
            }
        }

        Lable lable{
            .value = tryShrinkHintText(std::move(text)),
            .location = cvtr.toLocalRange(linkRange, src),
        };

        InlayHint hint{
            .kind = kind,
            .offset = src.getDecomposedLoc(location).second,
            .lable = std::move(lable),
        };

        state.push_back(std::move(hint));
    }

    bool VisitNamespaceDecl(const clang::NamespaceDecl* decl) {
        if(!config.blockEnd)
            return true;

        auto range = decl->getSourceRange();
        if(decl->isAnonymousNamespace() || !isMultiLineRange(range))
            return true;

        collectBlockEndHint(decl->getRBraceLoc().getLocWithOffset(1),
                            std::format("// namespace {}", decl->getName()),
                            range,
                            Kind::NamespaceEnd,
                            DecideDuplicated::Replace);
        return true;
    }

    void collectStructSizeAndAlign(const clang::TagDecl* decl) {
        if(!decl->isStruct() && !decl->isClass())
            return;

        auto& ctx = decl->getASTContext();
        auto qual = decl->getTypeForDecl()->getCanonicalTypeInternal();

        auto size = ctx.getTypeSizeInChars(qual).getQuantity();
        auto align = ctx.getTypeAlignInChars(qual).getQuantity();

        Lable lable{
            .value = tryShrinkHintText(std::format("size: {}, align: {}", size, align)),
            .location = cvtr.toLocalRange(decl->getSourceRange(), src),
        };

        // right side of identifier.
        auto tail = decl->getLocation().getLocWithOffset(decl->getName().size());
        InlayHint hint{
            .kind = Kind::StructSizeAndAlign,
            .offset = src.getDecomposedLoc(tail).second,
            .lable = std::move(lable),
        };

        auto fid = onlyMain ? src.getMainFileID() : src.getFileID(tail);
        result[fid].push_back(std::move(hint));
    }

    bool VisitTagDecl(const clang::TagDecl* decl) {
        if(!decl->isThisDeclarationADefinition())
            return true;

        if(config.blockEnd && isMultiLineRange(decl->getBraceRange())) {
            std::string hintText = std::format("// {}", decl->getKindName().str());
            // Add a tail flag for enum declaration as clangd's do.
            if(const auto* enumDecl = llvm::dyn_cast<clang::EnumDecl>(decl);
               enumDecl && enumDecl->isScoped())
                hintText += enumDecl->isScopedUsingClassTag() ? " class" : " struct";

            // Format text to 'struct Example' or `class Example` or `enum class Example`
            hintText.append(" ").append(decl->getName());
            collectBlockEndHint(decl->getBraceRange().getEnd().getLocWithOffset(1),
                                std::move(hintText),
                                decl->getSourceRange(),
                                Kind::TagDeclEnd,
                                DecideDuplicated::Ignore);
        }

        if(config.structSizeAndAlign)
            collectStructSizeAndAlign(decl);

        return true;
    }

    /// TODO:
    /// Find proper end location of cast expression.
    // bool VisitImplicitCastExpr(const clang::ImplicitCastExpr* stmt) {
    //     if(!config.implicitCast)
    //         return true;
    //     if(auto* expr = llvm::dyn_cast<clang::ImplicitCastExpr>(stmt)) {
    //         Lable lable{
    //             .value = shrinkText(std::format("as {}", expr->getType().getAsString(policy))),
    //             .tooltip = blank(),
    //         };
    //         // right side of that expr.
    //         InlayHint hint{
    //             .position = cvtr.toPosition(stmt->getEndLoc()),
    //             .lable = {std::move(lable)},
    //             .kind = proto::InlayHintKind::Parameter,
    //         };
    //         result.push_back(std::move(hint));
    //     }
    //     return true;
    // }
};

using feature::inlay_hint::InlayHintKind;

bool isAvailableWithOption(InlayHintKind kind, const config::InlayHintOption& config) {
    using enum InlayHintKind::Kind;

    switch(kind.kind()) {
        case Invalid: return false;

        case AutoDecl:
        case StructureBinding: return config.dedcucedType;

        case Parameter:
        case Constructor: return config.paramName;

        case FunctionReturnType:
        case LambdaReturnType: return config.returnType;

        case IfBlockEnd:
        case SwitchBlockEnd:
        case WhileBlockEnd:
        case ForBlockEnd:
        case NamespaceEnd:
        case TagDeclEnd:
        case FunctionEnd:
        case LambdaBodyEnd: return config.blockEnd;

        case ArrayIndex: return config.maxArrayElements > 0;
        case StructSizeAndAlign: return config.structSizeAndAlign;
        case MemberSizeAndOffset: return config.memberSizeAndOffset;
        case ImplicitCast: return config.implicitCast;
        case ChainCall: return config.chainCall;
        case NumberLiteralToHex: return config.numberLiteralToHex;
        case CStrLength: return config.cstrLength;
    }
}

}  // namespace

namespace feature::inlay_hint {

json::Value inlayHintCapability(json::Value InlayHintClientCapabilities) {
    return {};
}

/// Convert `Lable` to `proto:":InlayHintLablePart`. the hint text will be shrinked to the
/// `maxHintLength` if it's not zero.
proto::InlayHintLablePart toLspType(const Lable& lable, size_t maxHintLength,
                                    llvm::StringRef docuri, llvm::StringRef content,
                                    const SourceConverter& SC) {
    proto::InlayHintLablePart lspLable;
    lspLable.value = InlayHintCollector::shrinkHintText(lable.value, maxHintLength);
    lspLable.tooltip = blank();
    lspLable.Location = {
        .uri = docuri.str(),
        .range = SC.toRange(lable.location, content),
    };
    return lspLable;
}

/// Convert `InlayHint` to `proto::proto::InlayHint`.
proto::InlayHint toLspType(const InlayHint& hint, size_t maxHintLength, llvm::StringRef docuri,
                           llvm::StringRef content, const SourceConverter& SC) {
    proto::InlayHint lspHint;
    /// Use hint.lable as the only element of `proto::InlayHint::lable`.
    lspHint.lable = {toLspType(hint.lable, maxHintLength, docuri, content, SC)};
    lspHint.kind = toLspType(hint.kind);
    lspHint.position = SC.toPosition(content, hint.offset);
    return lspHint;
}

Result inlayHints(proto::InlayHintParams param, ASTInfo& info, const SourceConverter& converter,
                  const config::InlayHintOption& config) {
    const clang::SourceManager& src = info.srcMgr();

    llvm::StringRef codeText = src.getBufferData(src.getMainFileID());

    // Take 0-0 based Lsp Location from `param.range` and convert it to offset pair.
    LocalSourceRange requestRange{
        .begin = static_cast<uint32_t>(converter.toOffset(codeText, param.range.start)),
        .end = static_cast<uint32_t>(converter.toOffset(codeText, param.range.end)),
    };

    // If request range is invalid, use the whole main file as the restrict range.
    if(requestRange.begin >= requestRange.end) {
        clang::FileID main = src.getMainFileID();
        requestRange.begin = src.getDecomposedSpellingLoc(src.getLocForStartOfFile(main)).second;
        requestRange.end = src.getDecomposedSpellingLoc(src.getLocForEndOfFile(main)).second;
    }

    /// TODO:
    /// Check and fix invalid options before collect hints.
    InlayHintCollector collector{
        .src = src,
        .cvtr = converter,
        .limit = requestRange,
        .config = config,
        .onlyMain = true,
        .result = InlayHintCollector::Storage{},
        .policy = info.context().getPrintingPolicy(),
        .code = codeText,
    };

    collector.TraverseTranslationUnitDecl(info.tu());

    return std::move(collector.result[src.getMainFileID()]);
}

index::Shared<Result> inlayHints(proto::DocumentUri uri, ASTInfo& info,
                                 const SourceConverter& converter) {
    const clang::SourceManager& src = info.srcMgr();

    config::InlayHintOption enableAll;
    enableAll.maxLength = 0;
    enableAll.maxArrayElements = 0;
    enableAll.blockEnd = true;
    enableAll.implicitCast = true;
    enableAll.chainCall = true;
    enableAll.numberLiteralToHex = true;
    enableAll.cstrLength = true;

    InlayHintCollector collector{
        .src = src,
        .cvtr = converter,
        .config = enableAll,
        .onlyMain = false,
        .result = InlayHintCollector::Storage{},
        .policy = info.context().getPrintingPolicy(),
        .code = src.getBufferData(src.getMainFileID()),
    };

    collector.TraverseTranslationUnitDecl(info.tu());
    return std::move(collector.result);
}

proto::InlayHintsResult toLspType(llvm::ArrayRef<InlayHint> result, llvm::StringRef docuri,
                                  std::optional<config::InlayHintOption> config,
                                  llvm::StringRef content, const SourceConverter& SC) {
    proto::InlayHintsResult lspRes;
    lspRes.reserve(result.size());

    /// NOTICE:
    /// During converting hints from `InlayHint` to `proto::InlayHint`, the
    /// `config::maxArrayElements` will be ignored because we can't recover the parent-child
    /// relationship of AST node from `InlayHint`.

    for(auto& hint: result) {
        if(config.has_value() && !isAvailableWithOption(hint.kind, *config))
            continue;

        lspRes.push_back(toLspType(hint, config->maxLength, docuri, content, SC));
    }

    lspRes.shrink_to_fit();
    return lspRes;
}

}  // namespace feature::inlay_hint

}  // namespace clice
