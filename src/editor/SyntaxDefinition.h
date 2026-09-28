#pragma once

#include <ketplus/syntax/SyntaxDefinition.h>

#include <cstddef>
#include <vector>

class QString;

namespace ketplus {

// The syntax tables live in ketplus-syntax, shared with the KetPlus phone app; these take Qt
// strings.
inline constexpr std::size_t syntaxKeywordSetCount = syntax::keywordSetCount;

using SyntaxDefinition = syntax::SyntaxDefinition;
using SyntaxChoice = syntax::SyntaxChoice;

[[nodiscard]] const SyntaxDefinition& syntaxDefinitionForPath(const QString& filePath);
[[nodiscard]] const SyntaxDefinition* syntaxDefinitionByName(const QString& name);
// Languages users can pick manually, sorted by display name.
[[nodiscard]] const std::vector<SyntaxChoice>& syntaxChoices();
[[nodiscard]] QString syntaxDisplayName(const QString& name);

} // namespace ketplus
