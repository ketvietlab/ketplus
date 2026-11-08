#pragma once

#include "KetTokens.h"
class QTextDocument;

namespace ketplus {
// Colour only fenced code; never rewrite source, links or Markdown blocks.
void applyTranscriptSyntax(QTextDocument* document, ket::Theme theme);
} // namespace ketplus
