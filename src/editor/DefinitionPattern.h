#pragma once

#include <QString>

namespace ketplus {

// A regular expression that matches where `symbol` is most likely declared in files of the
// given syntax (see SyntaxDefinition). Returns an empty string when the symbol is not a name.
// This is a heuristic, like SymbolExtractor: it finds declarations, not the exact binding.
[[nodiscard]] QString definitionExpression(const QString& symbol, const QString& syntaxName);

// True when the token looks like a file reference, such as "./theme.css" or "app/main.h".
[[nodiscard]] bool looksLikeFileReference(const QString& token);

// Relative paths to try for a file reference: the token itself, then the extensions an
// import without one may mean, then the index file of a folder. A ".js" specifier also
// offers its TypeScript sources, which is how TypeScript projects refer to them.
[[nodiscard]] QStringList fileReferenceCandidates(const QString& token);

// The module a line brings a name in from: the path in `import x from "./a.js"`,
// `require("./a")`, `#include "a.h"` or `from .a import x`. Empty when the line imports
// nothing, and never a package name such as "react" that no file in the tree holds.
[[nodiscard]] QString importedModuleOnLine(const QString& line);

} // namespace ketplus
