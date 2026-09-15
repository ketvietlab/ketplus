#pragma once

#include <array>
#include <cstddef>
#include <vector>

class QString;

namespace ketplus {

inline constexpr std::size_t syntaxKeywordSetCount = 6;

struct SyntaxDefinition final {
    const char* name;
    const char* lexer;
    std::array<const char*, syntaxKeywordSetCount> keywordSets;
};

struct SyntaxChoice final {
    const char* name;
    const char* displayName;
};

[[nodiscard]] const SyntaxDefinition& syntaxDefinitionForPath(const QString& filePath);
[[nodiscard]] const SyntaxDefinition* syntaxDefinitionByName(const QString& name);
// Languages users can pick manually, sorted by display name.
[[nodiscard]] const std::vector<SyntaxChoice>& syntaxChoices();
[[nodiscard]] QString syntaxDisplayName(const QString& name);

} // namespace ketplus
