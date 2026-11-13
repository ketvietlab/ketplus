#pragma once
#include "ui/theme/ColorScheme.h"
class QTextDocument;
namespace ketplus {
// The caller supplies a resolved snapshot. Reuses the document's highlighter;
// unchanged snapshots perform no resolution or rehighlight on streaming deltas.
void applySchemeDocumentSyntax(QTextDocument* document, const ColorScheme& resolved,
                               bool enabled = true);
} // namespace ketplus
