#include "ui/InterfaceFont.h"

#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStringList>

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

QString loadedInterFamily() {
    if (QFontDatabase::hasFamily(QStringLiteral("Inter")))
        return QStringLiteral("Inter");
    const QStringList families = QFontDatabase::families();
    for (const QString& family : families) {
        if (family.startsWith(QStringLiteral("Inter")))
            return family;
    }
    return {};
}

QStringList namedSfProFamilies() {
    return {QStringLiteral("SF Pro Text"), QStringLiteral("SF Pro Display"),
            QStringLiteral("SF Pro")};
}

bool usableFamily(const QString& family) {
    if (family.isEmpty())
        return false;
    if (family.startsWith(QLatin1Char('.')))
        return true;
    return QFontDatabase::hasFamily(family);
}

// macOS ships SF Pro as the system UI face under private names; treat those as
// SF Pro so the cascade stays SF Pro → Inter → system, not Inter-first.
bool isSfProSystemFamily(const QString& family) {
    if (family.isEmpty() || family == QStringLiteral("Sans Serif"))
        return false;
    if (family.contains(QStringLiteral("SF Pro"), Qt::CaseInsensitive))
        return true;
    if (family.contains(QStringLiteral("AppleSystemUIFont"), Qt::CaseInsensitive))
        return true;
    if (family.startsWith(QStringLiteral(".SF")))
        return true;
    return false;
}

} // namespace

QString InterfaceUiFont::resolvedFamily() {
    const auto ketSans = loadedKetSansFamily();
    if (!ketSans.isEmpty())
        return ketSans;
    for (const QString& family : namedSfProFamilies()) {
        if (usableFamily(family))
            return family;
    }
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        const QString macSystem = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
        if (isSfProSystemFamily(macSystem))
            return macSystem;
    }
#endif
    const QString inter = loadedInterFamily();
    if (usableFamily(inter))
        return inter;
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

QString InterfaceUiFont::styleSheetFamilyList() {
    QStringList quoted;
    const auto ketSans = loadedKetSansFamily();
    if (!ketSans.isEmpty())
        quoted.append(QStringLiteral("\"%1\"").arg(ketSans));
    for (const QString& family : namedSfProFamilies())
        quoted.append(QStringLiteral("\"%1\"").arg(family));
    const QString inter = loadedInterFamily();
    if (!inter.isEmpty())
        quoted.append(QStringLiteral("\"%1\"").arg(inter));
    const QString system = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (!system.isEmpty() && system != inter)
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
    if (family.contains(QStringLiteral("Inter"), Qt::CaseInsensitive))
        return QStringLiteral("Inter");
    return QStringLiteral("System");
}

void InterfaceUiFont::apply(QApplication& application, const int pixelSize) {
    QFont font(resolvedFamily());
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::Normal);
    application.setFont(font);

    QString sheet = application.styleSheet();
    const int pos = sheet.indexOf(QStringLiteral("font-family: \""));
    if (pos >= 0) {
        const int end = sheet.indexOf(QLatin1Char(';'), pos);
        if (end >= pos) {
            sheet.replace(pos, end - pos + 1,
                          QStringLiteral("font-family: %1;").arg(styleSheetFamilyList()));
        }
    }
    // ThemeManager already expanded the base QWidget size. Protect that rule
    // while scaling Medium-designed literals in the remaining CM stylesheet.
    static const QRegularExpression baseRule(
        QStringLiteral(R"((QWidget\s*\{[^}]*font-size:\s*)\d+px)"));
    sheet.replace(baseRule, QStringLiteral("\\1{{resolvedInterfaceSize}}px"));
    // Tooltips keep the kit's recipe size (ThemeManager); do not scale it as a CM literal.
    static const QRegularExpression tooltipRule(
        QStringLiteral(R"((QToolTip\s*\{[^}]*font-size:\s*)\d+px)"));
    sheet.replace(tooltipRule, QStringLiteral("\\1{{tooltipSize}}px"));
    sheet = TypeScale::scaleLiteralFontSizes(sheet, pixelSize);
    sheet.replace(QStringLiteral("{{resolvedInterfaceSize}}"), QString::number(pixelSize));
    sheet.replace(QStringLiteral("{{tooltipSize}}"), QString::number(tooltipFontPixels()));
    application.setStyleSheet(sheet);
}

} // namespace ketplus
