#include "ui/InterfaceFont.h"

#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStringList>
#include <QToolTip>

static void initializeInterfaceFontResources() {
    Q_INIT_RESOURCE(ketplus_ui_fonts);
    Q_INIT_RESOURCE(ketplus_ui_fonts_mono);
}

namespace ketplus {
namespace {

QString loadedKetSansFamily() {
    static const QString family = [] {
        initializeInterfaceFontResources();
        for (const auto* style : {"Light", "Regular", "Medium", "Bold"}) {
            const auto path =
                QStringLiteral(":/ketplus/fonts/TTF/KETSans-%1.ttf").arg(QLatin1String(style));
            const int id = QFontDatabase::addApplicationFont(path);
            if (id < 0 || !QFontDatabase::applicationFontFamilies(id).contains("KET Sans")) {
                qWarning("Unable to register embedded KET Sans %s", style);
                return QString{};
            }
        }
        // KÉT Sans Mono Medium ships beside it for code and the terminal (#141).
        if (QFontDatabase::addApplicationFont(
                QStringLiteral(":/ketplus/fonts/KETSansMono-Medium.ttf")) < 0) {
            qWarning("Unable to register embedded KET Sans Mono");
        }
        return QStringLiteral("KET Sans");
    }();
    return family;
}

} // namespace

QString InterfaceUiFont::resolvedFamily() {
    const auto ketSans = loadedKetSansFamily();
    if (!ketSans.isEmpty())
        return ketSans;
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

QString InterfaceUiFont::styleSheetFamilyList() {
    QStringList quoted;
    const auto ketSans = loadedKetSansFamily();
    if (!ketSans.isEmpty())
        quoted.append(QStringLiteral("\"%1\"").arg(ketSans));
    const QString system = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (!system.isEmpty() && system != ketSans)
        quoted.append(QStringLiteral("\"%1\"").arg(system));
    quoted.append(QStringLiteral("sans-serif"));
    quoted.removeDuplicates();
    return quoted.join(QStringLiteral(", "));
}

QString InterfaceUiFont::settingsLabel() {
    const QString family = resolvedFamily();
    if (family == QStringLiteral("KET Sans"))
        return QStringLiteral("KÉT Sans");
    if (family.contains(QStringLiteral("SF Pro"), Qt::CaseInsensitive) ||
        family.contains(QStringLiteral("AppleSystemUIFont"), Qt::CaseInsensitive) ||
        family.startsWith(QStringLiteral(".SF")))
        return QStringLiteral("SF Pro");
    return QStringLiteral("System");
}

void InterfaceUiFont::apply(QApplication& application, const int pixelSize) {
    QFont font(resolvedFamily());
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::Normal);
    application.setFont(font);

    // ThemeManager resolves and scales the compatibility sheet once. Rewriting
    // it here would rescale existing windows and override kit-owned label fonts.
    QFont tooltip = font;
    tooltip.setPixelSize(tooltipFontPixels());
    QToolTip::setFont(tooltip);
}

} // namespace ketplus
