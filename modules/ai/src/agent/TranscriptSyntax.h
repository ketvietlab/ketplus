#pragma once

#include "KetTokens.h"
class QTextDocument;

namespace ketplus {
struct ColorScheme;
// Colour only fenced code; never rewrite source, links or Markdown blocks.
void applyTranscriptSyntax(QTextDocument* document, ket::Theme theme,
                           const ColorScheme* resolved = nullptr);
} // namespace ketplus
