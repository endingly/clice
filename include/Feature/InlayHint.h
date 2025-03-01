#pragma once

#include "Basic/Document.h"
#include "Basic/SourceCode.h"
#include "Index/Shared.h"
#include "Support/JSON.h"

namespace clice {

namespace proto {

// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#inlayHintParams
struct InlayHintParams {
    /// The text document.
    TextDocumentIdentifier textDocument;

    /// The visible document range for which inlay hints should be computed.
    Range range;
};

// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#inlayHintLabelPart
struct InlayHintLablePart {
    /// The label of the inlay hint.
    string value;

    /// The tooltip text when you hover over this label part.  Depending on
    /// the client capability `inlayHint.resolveSupport` clients might resolve
    /// this property late using the resolve request.
    MarkupContent tooltip;

    /// An optional source code location that represents this label part.
    Location Location;

    /// TODO:
    // Command command;
};

// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#inlayHintKind
struct InlayHintKind : refl::Enum<InlayHintKind> {
    enum Kind : uint8_t {
        Invalid = 0,
        Type = 1,
        Parameter = 2,
    };

    using Enum::Enum;

    constexpr static auto InvalidEnum = Invalid;
};

// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#inlayHint
struct InlayHint {
    /// The position of this hint.
    Position position;

    /// The label of this hint.
    std::vector<InlayHintLablePart> lable;

    /// The kind of this hint.
    InlayHintKind kind;

    /// TODO:
    /// Optional text edits that are performed when accepting this inlay hint.
    // std::vector<TextEdit> textEdits;

    /// Render padding before the hint.
    bool paddingLeft = false;

    /// Render padding before the hint.
    bool paddingRight = false;

    /// TODO:
    // LspAny data;
};

using InlayHintsResult = std::vector<InlayHint>;

}  // namespace proto

namespace config {

/// Options for inlay hints, from table `inlay-hint` in `clice.toml`.
struct InlayHintOption {
    /// Max length of hint string, the extra part will be replaced with `...`. Keep entire text if
    /// it's zero.
    uint16_t maxLength = 30;

    /// How many elements to show in initializer-list. Hint for all elements if it's zero.
    uint16_t maxArrayElements = 3;

    /// Hint for `auto` declaration, structure binding, if/for statement with initializer.
    bool dedcucedType : 1 = true;

    /// Hint for function / lambda return type.
    ///     auto f |-> int| { return 1; }
    ///     []() |-> bool| { return true; }
    bool returnType : 1 = true;

    /// Hint after '}', including if/switch/while/for/namespace/class/function end.
    bool blockEnd : 1 = false;

    /// Hint for function arguments. e.g.
    ///     void f(int a, int b);
    ///     f(|a:|1, |b:|2);
    bool paramName : 1 = true;

    /// Diaplay the value of `sizeof()` and `alignof()` for a struct/class defination. e.g.
    ///    struct Example |size: 4, align: 4| { int x; };
    bool structSizeAndAlign : 1 = true;

    /// TODO:
    /// Display the value of `sizeof()` and `offsetof()` for a non-static member for a struct/class
    /// defination. e.g.
    ///     struct Example {
    ///         int x; |size: 4, offset: 0|
    ///         int y: |size: 4, offset: 4|
    ///     }
    bool memberSizeAndOffset : 1 = true;

    /// TODO:
    /// Hint for implicit cast like `1 |as int|`.
    bool implicitCast : 1 = false;

    /// TODO:
    /// Hint for function return type in multiline chaind-call. e.g.
    ///     a()
    ///     .to_b() |ClassB|
    ///     .to_c() |ClassC|
    ///     .to_d() |ClassD|
    bool chainCall : 1 = false;

    /// TODO:
    /// Hint for a magic number literal to hex format. e.g.
    ///     uint32 magic = 31|0x1F|;
    bool numberLiteralToHex : 1 = false;

    /// TODO:
    /// Hint for a string literal length. e.g.
    ///     const char* str = "123456"|len: 6|;
    /// Too short string (len <= 8) will not be hinted.
    bool cstrLength : 1 = false;
};

}  // namespace config

class ASTInfo;
class SourceConverter;

namespace feature::inlay_hint {

json::Value capability(json::Value clientCapabilities);

/// For each hint, we record extra kind tag more than LSP used, which is used to distinguish
/// different cases coresponding to the item in `InlayHintOptions`.
struct InlayHintKind : refl::Enum<InlayHintKind> {
    enum Kind : uint8_t {
        Invalid = 0,

        AutoDecl,
        StructureBinding,

        Parameter,
        Constructor,

        FunctionReturnType,
        LambdaReturnType,

        IfBlockEnd,
        SwitchBlockEnd,
        WhileBlockEnd,
        ForBlockEnd,

        NamespaceEnd,
        TagDeclEnd,
        FunctionEnd,
        LambdaBodyEnd,

        ArrayIndex,

        StructSizeAndAlign,
        /// TODO:
        /// The below items is still TODO.
        MemberSizeAndOffset,
        ImplicitCast,
        ChainCall,
        NumberLiteralToHex,
        CStrLength,
    };

    using Enum::Enum;

    constexpr static auto InvalidEnum = Invalid;

    /// Check if the kind is related to type kind in LSP.
    constexpr bool isLspTypeKind() {
        return is_one_of(AutoDecl,
                         StructureBinding,
                         FunctionReturnType,
                         LambdaReturnType,
                         ImplicitCast,
                         ChainCall);
    }

    /// Check if the kind is related to parameter kind in LSP.
    constexpr bool isLspParameterKind() {
        return !isLspTypeKind();
    }
};

constexpr proto::InlayHintKind toLspType(InlayHintKind kind) {
    return kind.isLspTypeKind() ? proto::InlayHintKind::Type : proto::InlayHintKind::Parameter;
}

/// We don't store the document URI in each `Lable` object, it's always same in the given document
/// of `ASTInfo`.
struct Lable {
    std::string value;
    LocalSourceRange location;

    /// TODO: Should we store tooltip field in index ?
    /// MarkupContent tooltip;
};

/// Different with `proto::InlayHint`, this struct is used for index. It doesn't store many lable
/// part but only one.
struct InlayHint {
    /// The position of this hint.
    InlayHintKind kind;

    /// The offset of hint position.
    std::uint32_t offset;

    /// Currently, there is only 1 lable is recorded during the collection of InlayHints. If it's
    /// necessary to store multiple lable parts, replace typpe of `lable` with `std::vector<Lable>`.
    Lable lable;
};

using Result = std::vector<InlayHint>;

/// Compute inlay hints for MainfileID in given range and config.
Result inlayHints(proto::InlayHintParams param, ASTInfo& info, const SourceConverter& converter,
                  const config::InlayHintOption& config);

/// Same with `inlayHints` but including all fileID, and all options in `config::InlayHintOption`
/// will be enabled to support index.
index::Shared<Result> inlayHints(proto::DocumentUri uri, ASTInfo& info,
                                 const SourceConverter& converter);

/// Convert `Result` to `proto::InlayHintResult`.  If an option is provided, use the option to
/// filter result. By default, all hints will be converted.
proto::InlayHintsResult toLspType(llvm::ArrayRef<InlayHint> result, llvm::StringRef docuri,
                                  std::optional<config::InlayHintOption> config,
                                  llvm::StringRef content, const SourceConverter& SC);

}  // namespace feature::inlay_hint

}  // namespace clice
