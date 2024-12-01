#include <Compiler/Compiler.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>

namespace clice {

static void adjustInvocation(clang::CompilerInvocation& invocation) {
    auto& frontOpts = invocation.getFrontendOpts();
    frontOpts.DisableFree = false;

    clang::LangOptions& langOpts = invocation.getLangOpts();
    langOpts.CommentOpts.ParseAllComments = true;
    langOpts.RetainCommentsFromSystemHeaders = true;

    // FIXME: add more.
}

static auto createInstance(llvm::ArrayRef<const char*> args) {
    auto instance = std::make_unique<clang::CompilerInstance>();

    /// TODO: Figure out `CreateInvocationOptions`.
    clang::CreateInvocationOptions options = {};
    instance->setInvocation(clang::createInvocation(args, options));

    /// TODO: use a thread safe filesystem and our customized `DiagnosticConsumer`.
    instance->createDiagnostics(
        *llvm::vfs::getRealFileSystem(),
        new clang::TextDiagnosticPrinter(llvm::outs(), new clang::DiagnosticOptions()),
        true);

    adjustInvocation(instance->getInvocation());

    return instance;
}

static void applyPreamble(clang::CompilerInstance& instance, CompliationParams& params) {
    auto& PPOpts = instance.getPreprocessorOpts();
    auto& pch = params.pch;
    auto& bounds = params.bounds;
    auto& pcms = params.pcms;
    if(bounds.Size != 0) {
        PPOpts.UsePredefines = false;
        PPOpts.ImplicitPCHInclude = std::move(pch);
        PPOpts.PrecompiledPreambleBytes.first = bounds.Size;
        PPOpts.PrecompiledPreambleBytes.second = bounds.PreambleEndsAtStartOfLine;
        PPOpts.DisablePCHOrModuleValidation = clang::DisableValidationForModuleKind::PCH;
    }

    for(auto& [name, path]: pcms) {
        auto& HSOpts = instance.getHeaderSearchOpts();
        HSOpts.PrebuiltModuleFiles.try_emplace(std::move(name), std::move(path));
    }
}

static llvm::Expected<ASTInfo> ExecuteAction(std::unique_ptr<clang::CompilerInstance> instance,
                                             clang::frontend::ActionKind kind,
                                             bool collectPP = true) {
    std::unique_ptr<clang::ASTFrontendAction> action;
    if(kind == clang::frontend::ActionKind::ParseSyntaxOnly) {
        action = std::make_unique<clang::SyntaxOnlyAction>();
    } else if(kind == clang::frontend::ActionKind::GeneratePCH) {
        action = std::make_unique<clang::GeneratePCHAction>();
    } else if(kind == clang::frontend::ActionKind::GenerateReducedModuleInterface) {
        action = std::make_unique<clang::GenerateReducedModuleInterfaceAction>();
    } else {
        llvm::errs() << "Unsupported action kind\n";
        std::terminate();
    }

    if(!instance->createTarget()) {
        return error("Failed to create target");
    }

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return error("Failed to begin source file");
    }

    // FIXME: clang-tidy, include-fixer, etc?

    // `BeginSourceFile` may create new preprocessor, so all operations related to preprocessor
    // should be done after `BeginSourceFile`.
    if(collectPP) {
        auto& PP = instance->getPreprocessor();
        clang::syntax::TokenCollector collector{PP};

        if(auto error = action->Execute()) {
            return clice::error("Failed to execute action, because {} ", error);
        }

        auto tokBuf = std::make_unique<clang::syntax::TokenBuffer>(std::move(collector).consume());
        tokBuf->indexExpandedTokens();

        return ASTInfo(std::move(action), std::move(instance), std::move(tokBuf));
    } else {
        if(auto error = action->Execute()) {
            return clice::error("Failed to execute action, because {} ", error);
        }

        return ASTInfo(std::move(action), std::move(instance), nullptr);
    }
}

llvm::Expected<ASTInfo> buildAST(CompliationParams& params) {
    auto instance = createInstance(params.args);

    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(params.content);
    instance->getPreprocessorOpts().addRemappedFile(params.path, buffer.release());

    applyPreamble(*instance, params);

    return ExecuteAction(std::move(instance), clang::frontend::ActionKind::ParseSyntaxOnly);
}

llvm::Expected<PCHInfo> buildPCH(CompliationParams& params) {
    auto instance = createInstance(params.args);

    clang::PreambleBounds bounds = {0, false};
    if(params.mainpath == params.path) {
        /// If mainpath is equal to path, just tokenize the content to get preamble bounds.
        bounds = clang::Lexer::ComputePreamble(params.content, {}, false);
    } else {
        /// FIXME: if the mainpath is not equal to path, we need to preprocess the mainpath to get
        /// the preamble bounds.
        std::terminate();
    }

    /// Set options to generate PCH.
    instance->getFrontendOpts().OutputFile = params.outpath;
    instance->getFrontendOpts().ProgramAction = clang::frontend::GeneratePCH;
    instance->getPreprocessorOpts().PrecompiledPreambleBytes = {0, false};
    instance->getPreprocessorOpts().GeneratePreamble = true;
    instance->getLangOpts().CompilingPCH = true;

    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(params.content.substr(0, bounds.Size));
    instance->getPreprocessorOpts().addRemappedFile(params.path, buffer.release());

    if(auto info = ExecuteAction(std::move(instance), clang::frontend::ActionKind::GeneratePCH)) {
        return PCHInfo(std::move(*info), params.outpath, params.content, params.mainpath, bounds);
    } else {
        return info.takeError();
    }
}

llvm::Expected<PCMInfo> buildPCM(CompliationParams& params) {
    auto instance = createInstance(params.args);

    /// Set options to generate PCM.
    instance->getFrontendOpts().OutputFile = params.outpath;
    instance->getFrontendOpts().ProgramAction = clang::frontend::GenerateReducedModuleInterface;

    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(params.content);
    instance->getPreprocessorOpts().addRemappedFile(params.path, buffer.release());

    applyPreamble(*instance, params);

    if(auto info = ExecuteAction(std::move(instance),
                                 clang::frontend::ActionKind::GenerateReducedModuleInterface)) {
        return PCMInfo(std::move(*info), params.outpath);
    } else {
        return info.takeError();
    }
}

llvm::Expected<ASTInfo> codeCompleteAt(CompliationParams& params,
                                       uint32_t line,
                                       uint32_t column,
                                       llvm::StringRef file,
                                       clang::CodeCompleteConsumer* consumer) {
    auto instance = createInstance(params.args);

    /// Set options to run code completion.
    instance->getFrontendOpts().CodeCompletionAt.Line = line;
    instance->getFrontendOpts().CodeCompletionAt.Column = column;
    instance->getFrontendOpts().CodeCompletionAt.FileName = file;
    instance->setCodeCompletionConsumer(consumer);

    auto buffer = llvm::MemoryBuffer::getMemBufferCopy(params.content);
    /// FIXME: Check PPOpts.RetainRemappedFileBuffers.
    instance->getPreprocessorOpts().addRemappedFile(params.path, buffer.release());

    applyPreamble(*instance, params);

    return ExecuteAction(std::move(instance), clang::frontend::ActionKind::ParseSyntaxOnly, false);
}

}  // namespace clice
