#include "editor/SyntaxRoleAdapter.h"
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
} // namespace
std::array<LexerSchemeStyle, 256> lexillaSchemeStyles(std::string_view lexer,
                                                      const ColorScheme& scheme) {
    const auto resolved = resolveColorScheme(scheme);
    const auto& tokens = syntax::tokenStyles(lexer);
    std::array<LexerSchemeStyle, 256> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto& style = result[i];
        const auto token = tokens[i];
        style = {resolved.editor.value(QStringLiteral("fg")), token.bold, token.italic, false};
        if (const auto* role = roleFor(token.token)) {
            const auto syntax = resolved.syntax.value(QString::fromLatin1(role));
            style = {*syntax.fg, *syntax.bold, *syntax.italic, *syntax.underline};
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
