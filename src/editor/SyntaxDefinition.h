#pragma once

#include <array>
#include <cstddef>

class QString;

namespace ketplus {

inline constexpr std::size_t syntaxKeywordSetCount = 6;

struct SyntaxDefinition final {
    const char* name;
    const char* lexer;
    std::array<const char*, syntaxKeywordSetCount> keywordSets;
};

[[nodiscard]] const SyntaxDefinition& syntaxDefinitionForPath(const QString& filePath);

} // namespace ketplus
