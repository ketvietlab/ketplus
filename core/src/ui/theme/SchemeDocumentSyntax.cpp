#include "ui/theme/SchemeDocumentSyntax.h"
#include "KetSyntax.h"
#include <QTextBlock>
#include <QTextDocument>
namespace ketplus {
namespace {
QString roleName(ket::SyntaxRole role) {
    switch (role) {
    case ket::SyntaxRole::Keyword:
        return QStringLiteral("keyword");
    case ket::SyntaxRole::String:
        return QStringLiteral("string");
    case ket::SyntaxRole::Number:
        return QStringLiteral("number");
    case ket::SyntaxRole::Type:
        return QStringLiteral("type");
    case ket::SyntaxRole::Comment:
        return QStringLiteral("comment");
    case ket::SyntaxRole::Attribute:
        return QStringLiteral("attribute");
    }
    return {};
}
class SchemeDocumentSyntax final : public QSyntaxHighlighter {
  public:
    explicit SchemeDocumentSyntax(QTextDocument* document) : QSyntaxHighlighter(document) {
        setObjectName(QStringLiteral("ketSchemeDocumentSyntax"));
    }
    void update(const ColorScheme& scheme, bool enabled) {
        if (styles_ == scheme.syntax && foreground_ == scheme.editor.value(QStringLiteral("fg")) &&
            enabled_ == enabled)
            return;
        styles_ = scheme.syntax;
        // Callers may retain a mutable reference to an authored role across a
        // preview. Keep our cache independent of that reference as well.
        styles_.detach();
        foreground_ = scheme.editor.value(QStringLiteral("fg"));
        enabled_ = enabled;
        rehighlight();
    }

  protected:
    void highlightBlock(const QString& text) override {
        const auto format = currentBlock().blockFormat();
        if (!enabled_ || !format.hasProperty(QTextFormat::BlockCodeLanguage)) {
            setCurrentBlockState(0);
            return;
        }
        const auto previous = currentBlock().previous().blockFormat();
        int state = previous.property(QTextFormat::BlockCodeLanguage) ==
                            format.property(QTextFormat::BlockCodeLanguage)
                        ? qMax(0, previousBlockState())
                        : 0;
        setFormat(0, int(text.size()), foreground_);
        const auto spans = ket::highlightLine(
            text, format.property(QTextFormat::BlockCodeLanguage).toString(), state);
        for (const auto& span : spans) {
            const auto style = styles_.value(roleName(span.role));
            QTextCharFormat format;
            if (style.fg)
                format.setForeground(*style.fg);
            // Fresh ranges remove former overrides; unstyled built-ins retain
            // their original code-block font (including its medium weight).
            if (style.bold.value_or(false))
                format.setFontWeight(QFont::Bold);
            if (style.italic.value_or(false))
                format.setFontItalic(true);
            if (style.underline.value_or(false))
                format.setFontUnderline(true);
            setFormat(int(span.start), int(span.length), format);
        }
        setCurrentBlockState(state);
    }

  private:
    QMap<QString, SyntaxStyle> styles_;
    QColor foreground_;
    bool enabled_{false};
};
} // namespace
void applySchemeDocumentSyntax(QTextDocument* document, const ColorScheme& scheme, bool enabled) {
    auto* highlighter = document->findChild<QSyntaxHighlighter*>(
        QStringLiteral("ketSchemeDocumentSyntax"), Qt::FindDirectChildrenOnly);
    if (!highlighter)
        highlighter = new SchemeDocumentSyntax(document);
    static_cast<SchemeDocumentSyntax*>(highlighter)->update(scheme, enabled);
}
} // namespace ketplus
