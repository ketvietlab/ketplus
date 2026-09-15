#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace ketplus {

struct DocumentSymbol final {
    QString name;
    QString kind;
    int line{0};
    int depth{0};
};

inline constexpr int defaultSymbolLimit = 5000;

// Finds top-level declarations with lightweight per-line patterns for the given
// syntax name (see SyntaxDefinition). Lines are 1-based; depth follows indentation
// or Markdown heading level. This is a heuristic outline, not a parser.
[[nodiscard]] QList<DocumentSymbol> extractDocumentSymbols(const QByteArray& text,
                                                           const QString& syntaxName,
                                                           int maximumSymbols = defaultSymbolLimit);

} // namespace ketplus
