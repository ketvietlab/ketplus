#include "agent/TranscriptTableStyle.h"
#include "KetAppearance.h"
#include "KetRichRecipes.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>

namespace ketplus {
void styleTranscriptTables(QTextDocument* document, const ket::MarkdownRecipe& recipe,
                           const double zoom, const double codePadding) {
    const auto px = [zoom](qreal value) { return ket::Appearance::scale(value, zoom); };
    for (auto* frame : document->rootFrame()->childFrames()) {
        auto* table = qobject_cast<QTextTable*>(frame);
        if (!table)
            continue;
        auto first = table->cellAt(0, 0);
        const auto preceding = first.firstCursorPosition().block().previous();
        const bool followsCode = preceding.isValid() && preceding.blockFormat().hasProperty(
                                                            QTextFormat::BlockCodeLanguage);
        // Qt 6.11's Markdown importer can prepend a blank paragraph to the first
        // populated cell after a fence (including a later column after an empty
        // header). Remove only that artifact, never a genuinely empty cell.
        if (followsCode) {
            for (int row = 0; row < table->rows(); ++row) {
                for (int column = 0; column < table->columns(); ++column) {
                    auto cell = table->cellAt(row, column);
                    const auto block = cell.firstCursorPosition().block();
                    if (!block.text().isEmpty() || !block.next().isValid() ||
                        block.next().text().isEmpty() ||
                        table->cellAt(block.next().position()) != cell)
                        continue;
                    const auto contentFormat = block.next().blockFormat();
                    const auto contentCharacters = block.next().charFormat();
                    QTextCursor cursor(block);
                    cursor.deleteChar();
                    // Joining blocks otherwise keeps the empty paragraph's normal
                    // font and drops the Markdown header's inherited emphasis.
                    cursor.setBlockFormat(contentFormat);
                    cursor.setBlockCharFormat(contentCharacters);
                    // Markdown's first row is the header; this Qt importer does
                    // not set headerRowCount and loses emphasis in the affected cell.
                    if (row == 0) {
                        cursor = cell.firstCursorPosition();
                        cursor.setPosition(cell.lastCursorPosition().position(),
                                           QTextCursor::KeepAnchor);
                        QTextCharFormat emphasis;
                        emphasis.setFontWeight(recipe.strongWeight);
                        cursor.mergeCharFormat(emphasis);
                    }
                }
            }
        }
        auto format = table->format();
        format.setBorderBrush(recipe.tableBorder);
        format.setCellPadding(px(recipe.tableCellPadding));
        // Qt does not reserve the preceding paragraph's outer code-frame padding.
        format.setTopMargin(px(recipe.paragraphSpacing) + (followsCode ? codePadding : 0));
        if (format != table->format())
            table->setFormat(format);
    }
}
} // namespace ketplus
