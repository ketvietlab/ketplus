#include "editor/SyntaxRoleAdapter.h"
#include <SciLexer.h>
#include <ketplus/syntax/Tokens.h>

namespace ketplus {
namespace {
const char* roleFor(syntax::Token token) {
    switch (token) {
    case syntax::Token::keyword:
        return "keyword";
    case syntax::Token::string:
        return "string";
    case syntax::Token::number:
        return "number";
    case syntax::Token::type:
        return "type";
    case syntax::Token::comment:
        return "comment";
    case syntax::Token::attribute:
        return "attribute";
    default:
        return nullptr;
    }
}
const char* extendedRole(std::string_view lexer, int style) {
    if (lexer == "cpp") {
        if (style == SCE_C_OPERATOR)
            return "operator";
        if (style == SCE_C_IDENTIFIER)
            return "variable";
    } else if (lexer == "python") {
        if (style == SCE_P_DEFNAME)
            return "function";
        if (style == SCE_P_OPERATOR)
            return "operator";
        if (style == SCE_P_IDENTIFIER)
            return "variable";
    } else if (lexer == "bash") {
        if (style == SCE_SH_OPERATOR)
            return "operator";
        if (style == SCE_SH_SCALAR || style == SCE_SH_PARAM)
            return "variable";
    } else if (lexer == "json" && style == SCE_JSON_OPERATOR)
        return "operator";
    else if (lexer == "yaml" && style == SCE_YAML_OPERATOR)
        return "operator";
    else if (lexer == "lua") {
        if (style == SCE_LUA_OPERATOR)
            return "operator";
        if (style == SCE_LUA_IDENTIFIER)
            return "variable";
    } else if (lexer == "rust") {
        if (style == SCE_RUST_OPERATOR)
            return "operator";
        if (style == SCE_RUST_IDENTIFIER)
            return "variable";
    } else if (lexer == "sql" && style == SCE_SQL_OPERATOR)
        return "operator";
    else if (lexer == "css") {
        if (style == SCE_CSS_OPERATOR)
            return "operator";
        if (style == SCE_CSS_VARIABLE)
            return "variable";
    }
    // Function calls and constants generally share an identifier style. Do not
    // infer semantic roles from spelling or color unrelated identifiers.
    return nullptr;
}
} // namespace
std::array<LexerSchemeStyle, 256> lexillaSchemeStyles(std::string_view lexer,
                                                      const ColorScheme& scheme) {
    const auto resolved = resolveColorScheme(scheme);
    const auto& tokens = syntax::tokenStyles(lexer);
    const auto base = builtinColorScheme(resolved.extends == u"builtin-dark" ? SchemeKind::Dark
                                                                             : SchemeKind::Light);
    const bool legacyFonts = (resolved.id == u"builtin-light" || resolved.id == u"builtin-dark") &&
                             resolved.syntax == base.syntax;
    std::array<LexerSchemeStyle, 256> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto& style = result[i];
        const auto token = tokens[i];
        style = {resolved.editor.value(QStringLiteral("fg")), token.bold, token.italic, false};
        const auto* role = legacyFonts ? roleFor(token.token) : extendedRole(lexer, int(i));
        if (!role)
            role = roleFor(token.token);
        if (role) {
            const auto syntax = resolved.syntax.value(QString::fromLatin1(role));
            style.fg = *syntax.fg;
            if (!legacyFonts) {
                style.bold = *syntax.bold;
                style.italic = *syntax.italic;
                style.underline = *syntax.underline;
            }
            continue;
        }
        switch (token.token) {
        case syntax::Token::danger:
            style.fg = resolved.ui.value(QStringLiteral("chrome.danger"));
            break;
        case syntax::Token::added:
            style.fg = resolved.ui.value(QStringLiteral("chrome.positive"));
            break;
        case syntax::Token::changed:
            style.fg = resolved.ui.value(QStringLiteral("chrome.warning"));
            break;
        case syntax::Token::secondary:
            style.fg = resolved.ui.value(QStringLiteral("chrome.textSecondary"));
            break;
        default:
            break;
        }
    }
    return result;
}
} // namespace ketplus
