#include "agent/TranscriptSyntax.h"
#include "KetSyntax.h"
#include <QTextBlock>
#include <QTextDocument>

namespace ketplus {
namespace {
class TranscriptSyntax final : public QSyntaxHighlighter {
  public:
    TranscriptSyntax(QTextDocument* document, ket::Theme theme)
        : QSyntaxHighlighter(document), theme_(theme), recipe_(ket::syntaxRecipe(theme)) {
        setObjectName(QStringLiteral("ketTranscriptSyntax"));
    }
    void setTheme(ket::Theme theme) {
        if (theme == theme_)
            return;
        theme_ = theme;
        recipe_ = ket::syntaxRecipe(theme);
        rehighlight();
    }

  protected:
    void highlightBlock(const QString& text) override {
        const auto format = currentBlock().blockFormat();
        if (!format.hasProperty(QTextFormat::BlockCodeLanguage)) {
            setCurrentBlockState(0);
            return;
        }
        const auto previous = currentBlock().previous().blockFormat();
        int state = previous.property(QTextFormat::BlockCodeLanguage) ==
                            format.property(QTextFormat::BlockCodeLanguage)
                        ? qMax(0, previousBlockState())
                        : 0;
        const auto spans = ket::highlightLine(
            text, format.property(QTextFormat::BlockCodeLanguage).toString(), state);
        for (const auto& span : spans)
            setFormat(int(span.start), int(span.length), ket::syntaxColor(recipe_, span.role));
        setCurrentBlockState(state);
    }

  private:
    ket::Theme theme_;
    ket::SyntaxRecipe recipe_;
};
} // namespace
void applyTranscriptSyntax(QTextDocument* document, ket::Theme theme) {
    auto* existing = document->findChild<QSyntaxHighlighter*>(QStringLiteral("ketTranscriptSyntax"),
                                                              Qt::FindDirectChildrenOnly);
    if (existing)
        static_cast<TranscriptSyntax*>(existing)->setTheme(theme);
    else
        (new TranscriptSyntax(document, theme))->rehighlight();
}
} // namespace ketplus
