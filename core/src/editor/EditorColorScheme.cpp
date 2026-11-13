#include "editor/EditorWidget.h"
#include "editor/SyntaxRoleAdapter.h"
#include "preview/MarkdownModeController.h"
#include "ui/theme/ColorScheme.h"
#include <Scintilla.h>
#include <ScintillaMessages.h>

namespace ketplus {
namespace {
constexpr unsigned int message(Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}
QColor over(const QColor& color, const QColor& background) {
    if (color.alpha() == 255)
        return color;
    const qreal alpha = color.alphaF();
    return QColor::fromRgbF(color.redF() * alpha + background.redF() * (1 - alpha),
                            color.greenF() * alpha + background.greenF() * (1 - alpha),
                            color.blueF() * alpha + background.blueF() * (1 - alpha));
}
int rgb(const QColor& color) { return color.red() | (color.green() << 8) | (color.blue() << 16); }
} // namespace
void EditorWidget::applyColorScheme(const ColorScheme& scheme) {
    const auto resolved = resolveColorScheme(scheme);
    auto palette = colorSchemePalette(resolved);
    const QColor background =
        over(resolved.editor.value(QStringLiteral("bg")), QColor(palette.panelBackground));
    const auto hex = [&](const QColor& color) { return over(color, background).name(); };
    palette.panelBackground = background.name();
    palette.textMain = hex(resolved.editor.value(QStringLiteral("fg")));
    palette.textMuted = hex(resolved.editor.value(QStringLiteral("gutterFg")));
    palette.panelSubtle = hex(resolved.editor.value(QStringLiteral("gutterBg")));
    palette.accent = hex(resolved.editor.value(QStringLiteral("caret")));
    palette.accentMuted = hex(resolved.editor.value(QStringLiteral("selection")));
    applyTheme(palette);
    colorScheme_ = std::make_shared<const ColorScheme>(resolved);
    if (markdownPresentation_)
        markdownPresentation_->applyColorScheme(resolved);
    const auto styles = lexillaSchemeStyles(lexerName_.toStdString(), resolved);
    for (std::size_t i = 0; i < styles.size(); ++i) {
        // Scintilla reserves these for margins, brace matching and UI, not lexer tokens.
        if (i >= STYLE_DEFAULT && i <= STYLE_LASTPREDEFINED)
            continue;
        const auto& style = styles[i];
        send(message(Scintilla::Message::StyleSetFore), i, rgb(over(style.fg, background)));
        send(message(Scintilla::Message::StyleSetBold), i, style.bold);
        send(message(Scintilla::Message::StyleSetItalic), i, style.italic);
        send(message(Scintilla::Message::StyleSetUnderline), i, style.underline);
    }
    send(message(Scintilla::Message::SetSelFore), 1,
         rgb(over(resolved.editor.value(QStringLiteral("selectionFg")),
                  over(resolved.editor.value(QStringLiteral("selection")), background))));
    const auto highlight = resolved.editor.value(QStringLiteral("lineHighlight"));
    send(message(Scintilla::Message::SetCaretLineBack), rgb(highlight));
    send(message(Scintilla::Message::SetCaretLineBackAlpha), highlight.alpha());
    send(message(Scintilla::Message::Colourise), 0, -1);
    viewport()->update();
}
} // namespace ketplus
