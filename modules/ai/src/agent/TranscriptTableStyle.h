#pragma once

class QTextDocument;
namespace ket {
struct MarkdownRecipe;
}

namespace ketplus {
void styleTranscriptTables(QTextDocument* document, const ket::MarkdownRecipe& recipe, double zoom,
                           double codePadding);
}
