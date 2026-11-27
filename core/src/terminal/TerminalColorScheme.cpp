#include "terminal/TerminalSession.h"
#include "terminal/TerminalView.h"
#include "ui/theme/ColorScheme.h"

namespace ketplus {
namespace {
QColor over(const QColor& color, const QColor& background) {
    if (color.alpha() == 255)
        return color;
    const qreal alpha = color.alphaF();
    return QColor::fromRgbF(color.redF() * alpha + background.redF() * (1 - alpha),
                            color.greenF() * alpha + background.greenF() * (1 - alpha),
                            color.blueF() * alpha + background.blueF() * (1 - alpha));
}
} // namespace
void TerminalView::applyColorScheme(const ColorScheme& scheme) {
    const auto resolved = resolveColorScheme(scheme);
    const auto palette = colorSchemePalette(resolved);
    applyTheme(palette);
    background_ =
        over(resolved.terminal.value(QStringLiteral("bg")), QColor(palette.panelBackground));
    foreground_ = over(resolved.terminal.value(QStringLiteral("fg")), background_);
    cursorColor_ = over(resolved.terminal.value(QStringLiteral("cursor")), background_);
    selectionColor_ = resolved.terminal.value(QStringLiteral("selection"));
    for (int index = 0; index < 16; ++index) {
        const auto color =
            over(resolved.terminal.value(QStringLiteral("ansi%1").arg(index)), background_);
        session_.setAnsiColor(index, color.red(), color.green(), color.blue());
    }
    session_.setDefaultColors(foreground_.red(), foreground_.green(), foreground_.blue(),
                              background_.red(), background_.green(), background_.blue());
    auto colors = viewport()->palette();
    colors.setColor(QPalette::Window, background_);
    colors.setColor(QPalette::Base, background_);
    colors.setColor(QPalette::Text, foreground_);
    viewport()->setPalette(colors);
    viewport()->update();
}
} // namespace ketplus
