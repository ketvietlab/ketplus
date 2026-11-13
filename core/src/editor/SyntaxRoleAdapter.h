#pragma once
#include "ui/theme/ColorScheme.h"
#include <array>
#include <string_view>

namespace ketplus {
struct LexerSchemeStyle {
    QColor fg;
    bool bold{false};
    bool italic{false};
    bool underline{false};
};
// Uses the pinned syntax library's explicit style tables for every supported lexer.
// Roles a lexer cannot emit remain separate scheme data, with plain text fallback.
[[nodiscard]] std::array<LexerSchemeStyle, 256> lexillaSchemeStyles(std::string_view lexer,
                                                                    const ColorScheme& scheme);
} // namespace ketplus
