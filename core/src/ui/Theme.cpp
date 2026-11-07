#include "ui/Theme.h"
#include "ui/InterfaceZoom.h"

#include "KetAppearance.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "KetStyle.h"
#include "KetSyntax.h"
#include "KetTokens.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QSettings>
#include <QToolTip>

#include <utility>

namespace ketplus {
namespace {

// One Két DS token in a theme, opaque: a translucent token (dark borders) is composited
// over the panel, so stylesheets, HTML and QML read the same #RRGGBB everywhere.
QString tokenColor(const QString& name, const ket::Theme theme) {
    QColor color = ket::Tokens::color(name, theme);
    if (color.alpha() < 255) {
        const QColor base = ket::Tokens::color(QStringLiteral("panel-bg"), theme);
        const qreal alpha = color.alphaF();
        color = QColor::fromRgbF(color.redF() * alpha + base.redF() * (1 - alpha),
                                 color.greenF() * alpha + base.greenF() * (1 - alpha),
                                 color.blueF() * alpha + base.blueF() * (1 - alpha));
    }
    return color.name(QColor::HexRgb).toUpper();
}

// The palette of a theme, from Két DS tokens (ket::Tokens) and the kit's syntax recipe;
// the roles keep their names so the widgets that read them do not change.
ThemePalette paletteFor(const ket::Theme theme) {
    const auto token = [theme](const char* name) {
        return tokenColor(QString::fromLatin1(name), theme);
    };
    const ket::SyntaxRecipe syntax = ket::syntaxRecipe(theme);
    const auto hex = [](const QColor& color) { return color.name(QColor::HexRgb).toUpper(); };
    return {
        .dark = theme == ket::Theme::Dark,
        .appBackground = token("page-bg"),
        .pageBackground = token("page-bg"),
        .sidebarBackground = token("sidebar-bg"),
        .panelBackground = token("panel-bg"),
        .panelSubtle = token("panel-bg-subtle"),
        .surfaceRaised = token("surface-raised"),
        .surfaceHover = token("surface-hover"),
        .textMain = token("text-main"),
        .textSecondary = token("text-secondary"),
        .textMuted = token("text-muted"),
        .textDisabled = token("text-disabled"),
        .border = token("panel-border"),
        .borderStrong = token("border-strong"),
        .interactiveBackground = token("interactive-bg"),
        .interactiveHover = token("interactive-hover"),
        .interactiveActive = token("interactive-active"),
        .accent = token("accent"),
        .accentHover = token("accent-hover"),
        .accentActive = token("accent-active"),
        .accentSubtle = token("accent-subtle"),
        .accentMuted = token("accent-muted"),
        .focus = token("color-focus"),
        .tabActiveText = token("tab-active-text"),
        .accentText = token("accent-text"),
        .actionPrimary = token("action-primary-bg"),
        .actionPrimaryHover = token("action-primary-hover"),
        .actionPrimaryActive = token("action-primary-active"),
        .actionPrimaryInk = token("action-primary-ink"),
        .positive = token("positive"),
        .warning = token("warning"),
        .danger = token("danger"),
        .info = token("info"),
        .syntaxKeyword = hex(syntax.keyword),
        .syntaxString = hex(syntax.string),
        .syntaxNumber = hex(syntax.number),
        .syntaxType = hex(syntax.type),
        .syntaxComment = hex(syntax.comment),
        .syntaxAttribute = hex(syntax.attribute),
    };
}

QString modeName(const ThemeManager::Mode mode) {
    switch (mode) {
    case ThemeManager::Mode::Light:
        return QStringLiteral("light");
    case ThemeManager::Mode::Dark:
        return QStringLiteral("dark");
    case ThemeManager::Mode::System:
        return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

ThemeManager::Mode modeFromName(const QString& name) {
    if (name == QStringLiteral("light")) {
        return ThemeManager::Mode::Light;
    }
    if (name == QStringLiteral("dark")) {
        return ThemeManager::Mode::Dark;
    }
    return ThemeManager::Mode::System;
}

} // namespace

QString themeTokenColor(const QString& name, bool dark) {
    return tokenColor(name, dark ? ket::Theme::Dark : ket::Theme::Light);
}

ThemePalette currentThemePalette() { return paletteFor(ket::Appearance::instance().theme()); }

// The tooltip size of the kit's recipe. The base QWidget rule would otherwise set tooltip
// labels to the interface size: a stylesheet font wins over QToolTip::setFont.
int tooltipFontPixels() {
    const ket::Appearance& appearance = ket::Appearance::instance();
    return qRound(appearance.scaled(ket::tooltipRecipe(appearance.theme()).fontPixels));
}

namespace {

QString styleSheetFor(const ThemePalette& palette, const int interfaceFontSizePixels) {
    const QString terminalFontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    QString style = QStringLiteral(R"QSS(
QToolTip {
    font-size: {{tooltipFontSize}}px;
}

QMainWindow, QDialog, QMessageBox,
QStackedWidget#liteWorkspacePages, QWidget#liteWelcome {
    background: {{pageBackground}};
}

QMenuBar {
    min-height: 30px;
    border-bottom: 1px solid {{border}};
    background: {{panelSubtle}};
    color: {{textSecondary}};
    spacing: 2px;
}

QMenuBar::item {
    padding: 5px 9px;
    border-radius: 5px;
    background: transparent;
}

QMenuBar::item:selected {
    background: {{accentSubtle}};
    color: {{accentText}};
}

QToolButton[kvRole="tabClose"] {
    min-width: 20px;
    max-width: 20px;
    min-height: 20px;
    max-height: 20px;
    margin-left: 5px;
    padding: 0;
    border: 0;
    border-radius: 4px;
    background: transparent;
    color: {{textMuted}};
    font-size: 16px;
    font-weight: 400;
}

QToolButton[kvRole="tabClose"]:hover {
    background: {{surfaceHover}};
    color: {{danger}};
}

QSplitter::handle {
    background: {{border}};
}

QSplitter::handle:hover {
    background: {{focus}};
}

QWidget[kvRole="activityBar"] {
    border-right: 1px solid {{border}};
    background: {{sidebarBackground}};
}

QToolButton[kvRole="activityButton"] {
    min-width: 44px;
    max-width: 44px;
    min-height: 42px;
    max-height: 42px;
    padding: 0;
    border: 0;
    border-left: 2px solid transparent;
    border-radius: 0;
    background: transparent;
    color: {{textMuted}};
    font-size: 17px;
    font-weight: 600;
}

QToolButton[kvRole="activityButton"]:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QToolButton[kvRole="activityButton"]:checked {
    border-left-color: {{accent}};
    background: {{panelBackground}};
    color: {{accentText}};
}

QToolButton[kvRole="activityButton"]:disabled {
    background: transparent;
    color: {{textDisabled}};
}

QWidget[kvRole="explorer"] {
    background: {{sidebarBackground}};
}

QWidget[kvRole="markdownPreview"] {
    background: {{panelBackground}};
}

QWidget[kvRole="terminalPanel"] {
    border-top: 1px solid {{border}};
    background: {{panelSubtle}};
}

QWidget[kvRole="terminalHeader"] {
    min-height: 34px;
    max-height: 34px;
    border-bottom: 1px solid {{border}};
    background: {{panelSubtle}};
}

QLabel[kvRole="terminalHeading"] {
    color: {{textSecondary}};
    font-size: 10px;
    font-weight: 700;
}

QLabel[kvRole="terminalHeading"][terminalState="running"] {
    color: {{tabActiveText}};
}

QLabel[kvRole="terminalHeading"][terminalState="exited"] {
    color: {{textMuted}};
}

QLabel[kvRole="terminalPath"] {
    color: {{textMuted}};
    font-size: 10px;
}

QAbstractScrollArea[kvRole="terminalView"] {
    border: 0;
    background: {{panelSubtle}};
    font-family: "{{terminalFontFamily}}";
}

QToolButton[kvRole="terminalAction"] {
    min-width: 24px;
    max-width: 24px;
    min-height: 24px;
    max-height: 24px;
    padding: 0;
    border: 0;
    background: transparent;
    color: {{textMuted}};
    font-size: 16px;
}

QToolButton[kvRole="terminalAction"]:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QWidget[kvRole="previewHeader"] {
    min-height: 34px;
    max-height: 34px;
    border-bottom: 1px solid {{border}};
    background: {{panelSubtle}};
}

QLabel[kvRole="previewHeading"] {
    color: {{textSecondary}};
    font-size: 11px;
    font-weight: 700;
}

QLabel[kvRole="previewHint"] {
    color: {{textMuted}};
    font-size: 11px;
}

QToolButton[kvRole="previewClose"] {
    min-width: 24px;
    max-width: 24px;
    min-height: 24px;
    max-height: 24px;
    padding: 0;
    border: 0;
    background: transparent;
    color: {{textMuted}};
    font-size: 17px;
}

QToolButton[kvRole="previewClose"]:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QLabel[kvRole="previewNotice"] {
    padding: 8px 12px;
    border-bottom: 1px solid {{border}};
    background: {{accentSubtle}};
    color: {{textSecondary}};
    font-size: 11px;
}

QLabel[kvRole="previewNotice"][noticeState="error"] {
    color: {{danger}};
}

QLabel[kvRole="sidebarHeading"] {
    color: {{textSecondary}};
    font-size: 11px;
    font-weight: 700;
}

QLabel[kvRole="sidebarFolder"] {
    color: {{textMuted}};
    font-size: 11px;
}

QLabel[kvRole="sourceControlHeading"] {
    color: {{textMain}};
    font-size: 11px;
    font-weight: 600;
}

QLabel[kvRole="gitRepositoryName"] {
    color: {{textMain}};
    font-size: 14px;
    font-weight: 600;
}

QLabel[kvRole="gitRepositoryBranch"] {
    color: {{textMuted}};
    font-size: 12px;
    font-weight: 400;
}

QLabel[kvRole="emptyTitle"] {
    color: {{textMuted}};
    font-size: 12px;
}

QTreeView {
    border: 0;
    outline: 0;
    background: {{sidebarBackground}};
    color: {{textSecondary}};
    font-size: 12px;
}

QTreeView::item {
    min-height: 26px;
    padding: 0 5px;
    border: 0;
}

QTreeView::item:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QTreeView::item:selected {
    background: {{accentSubtle}};
    color: {{accentText}};
}

QStatusBar {
    min-height: 25px;
    border-top: 1px solid {{border}};
    background: {{pageBackground}};
    color: {{textMuted}};
    font-size: 11px;
}

QStatusBar::item {
    border: 0;
}

QLabel[kvRole="status"] {
    padding: 0 9px;
    border-left: 1px solid {{border}};
    color: {{textMuted}};
    font-size: 11px;
}

QToolButton[kvRole="statusButton"]::menu-indicator {
    image: none;
    width: 0;
}

QToolButton[kvRole="statusGit"],
QToolButton[kvRole="statusButton"] {
    min-width: 0;
    min-height: 22px;
    max-height: 22px;
    margin: 0;
    padding: 0 9px;
    border: 0;
    border-left: 1px solid {{border}};
    border-radius: 0;
    background: transparent;
    color: {{textMuted}};
    font-size: 11px;
}

QToolButton[kvRole="statusGit"]:hover,
QToolButton[kvRole="statusGit"]:pressed,
QToolButton[kvRole="statusButton"]:hover,
QToolButton[kvRole="statusButton"]:pressed {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QWidget[kvRole="gitDiff"] {
    background: {{panelBackground}};
}

QWidget[kvRole="diffHeader"] {
    min-height: 48px;
    border-bottom: 1px solid {{border}};
    background: {{panelSubtle}};
}

QLabel[kvRole="diffFile"] {
    color: {{textMain}};
    font-size: 13px;
    font-weight: 700;
}

QLabel[kvRole="diffPath"] {
    color: {{textMuted}};
    font-size: 11px;
}

QLabel[kvRole="diffScope"] {
    min-height: 18px;
    padding: 0 7px;
    border: 1px solid {{borderStrong}};
    border-radius: 4px;
    background: {{surfaceRaised}};
    color: {{textSecondary}};
    font-size: 9px;
    font-weight: 700;
}

QLabel[kvRole="diffScope"][diffMode="staged"] {
    border-color: {{positive}};
    color: {{positive}};
}

QLabel[kvRole="diffScope"][diffMode="unstaged"] {
    border-color: {{accent}};
    color: {{accentText}};
}

QLabel[kvRole="diffStats"] {
    color: {{textMuted}};
    font-size: 11px;
}

QToolButton[kvRole="diffAction"] {
    min-height: 26px;
    max-height: 26px;
    padding: 0 9px;
    border-color: {{border}};
    background: {{panelBackground}};
    color: {{textSecondary}};
    font-size: 11px;
}

QToolButton[kvRole="diffAction"]:hover {
    border-color: {{borderStrong}};
    background: {{surfaceHover}};
    color: {{textMain}};
}

QTableView[kvRole="diffTable"] {
    border: 0;
    outline: 0;
    background: {{panelBackground}};
    color: {{textSecondary}};
    selection-background-color: {{accentMuted}};
    selection-color: {{textMain}};
}

QTableView[kvRole="diffTable"]::item {
    min-height: 23px;
    padding: 0 6px;
    border: 0;
}

QTableView[kvRole="diffTable"]::item:selected {
    selection-background-color: {{accentMuted}};
    selection-color: {{textMain}};
}

QWidget[kvRole="findBar"] {
    border-top: 1px solid {{border}};
    background: {{panelSubtle}};
}

QLabel[kvRole="fieldLabel"] {
    min-width: 50px;
    color: {{textSecondary}};
    font-size: 12px;
    font-weight: 600;
}

QLabel[kvRole="searchStatus"] {
    color: {{textMuted}};
    font-size: 11px;
}

QLabel[kvRole="searchStatus"][searchState="empty"] {
    color: {{danger}};
}

QCheckBox {
    spacing: 6px;
    color: {{textSecondary}};
    font-size: 12px;
}

QPushButton, QToolButton, QComboBox, QLineEdit, QSpinBox {
    min-height: 32px;
    padding: 0 12px;
    border: 1px solid {{borderStrong}};
    border-radius: 5px;
    background: {{interactiveBackground}};
    color: {{textSecondary}};
}

QPushButton:hover, QToolButton:hover, QComboBox:hover, QLineEdit:hover, QSpinBox:hover {
    background: {{interactiveHover}};
    color: {{textMain}};
}

QPushButton:pressed, QToolButton:pressed {
    background: {{interactiveActive}};
}

QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus, QSpinBox:focus {
    border-color: {{focus}};
}

QDialog[kvRole="settingsDialog"] {
    background: {{pageBackground}};
}

QWidget[kvRole="settingsHeader"], QWidget[kvRole="settingsFooter"] {
    min-height: 54px;
    border-bottom: 1px solid {{border}};
    background: {{panelSubtle}};
}

QWidget[kvRole="settingsFooter"] {
    min-height: 48px;
    border-top: 1px solid {{border}};
    border-bottom: 0;
}

QLabel[kvRole="settingsHeaderTitle"] {
    color: {{textMain}};
    font-size: 14px;
    font-weight: 650;
}

QListWidget#settingsNavigation {
    padding: 12px 9px;
    border: 0;
    border-right: 1px solid {{border}};
    background: {{sidebarBackground}};
    outline: 0;
}

QListWidget#settingsNavigation::item,
QListWidget#settingsNavigation::item:hover,
QListWidget#settingsNavigation::item:selected {
    padding: 0;
    border: 0;
    background: transparent;
    color: transparent;
}

QScrollArea#appearanceSettingsScroll, QScrollArea#typographySettingsScroll {
    border: 0;
    background: {{pageBackground}};
}

QLabel[kvRole="settingsEyebrow"] {
    color: {{textMuted}};
    font-size: 10px;
    font-weight: 700;
}

QLabel[kvRole="settingsTitle"] {
    color: {{textMain}};
    font-size: 22px;
    font-weight: 700;
}

QLabel[kvRole="settingsDescription"] {
    color: {{textSecondary}};
    font-size: 12px;
}

QFrame[kvRole="settingsCard"] {
    border: 1px solid {{border}};
    border-radius: 8px;
    background: {{panelBackground}};
}

QLabel[kvRole="settingsFieldLabel"] {
    min-width: 92px;
    padding-top: 7px;
    color: {{textMain}};
    font-size: 12px;
    font-weight: 600;
}

QLabel[kvRole="settingsFieldHelp"] {
    padding-bottom: 7px;
    color: {{textMuted}};
    font-size: 10px;
}

QLabel[kvRole="settingsAutomaticValue"] {
    min-height: 30px;
    border: 1px solid {{border}};
    border-radius: 5px;
    background: {{panelSubtle}};
    color: {{textMuted}};
    font-size: 10px;
}

QWidget[kvRole="settingsPreview"] {
    border: 1px solid {{border}};
    border-radius: 8px;
    background: {{panelBackground}};
    color: {{textMain}};
}

QPushButton[kvRole="primaryAction"] {
    border-color: {{actionPrimary}};
    background: {{actionPrimary}};
    color: {{actionPrimaryInk}};
    font-weight: 600;
}

QPushButton[kvRole="primaryAction"]:hover {
    border-color: {{actionPrimaryHover}};
    background: {{actionPrimaryHover}};
    color: {{actionPrimaryInk}};
}

QPushButton[kvRole="primaryAction"]:pressed {
    border-color: {{actionPrimaryActive}};
    background: {{actionPrimaryActive}};
    color: {{actionPrimaryInk}};
}

QPushButton[kvRole="primaryAction"]:disabled {
    border-color: {{border}};
    background: {{accentMuted}};
    color: {{textMuted}};
}

QPushButton[kvRole="settingsReset"] {
    border-color: transparent;
    background: transparent;
    color: {{textMuted}};
}

QPushButton[kvRole="settingsReset"]:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}

QLineEdit QToolButton {
    min-width: 16px;
    max-width: 16px;
    min-height: 16px;
    max-height: 16px;
    margin: 0 4px;
    padding: 0;
    border: 0;
    background: transparent;
}

QScrollBar:vertical {
    width: 10px;
    margin: 0;
    border: 0;
    background: {{panelBackground}};
}

QScrollBar::handle:vertical {
    min-height: 28px;
    margin: 2px;
    border-radius: 3px;
    background: {{borderStrong}};
}

QScrollBar::handle:vertical:hover {
    background: {{textMuted}};
}

QScrollBar:horizontal {
    height: 10px;
    margin: 0;
    border: 0;
    background: {{panelBackground}};
}

QScrollBar::handle:horizontal {
    min-width: 28px;
    margin: 2px;
    border-radius: 3px;
    background: {{borderStrong}};
}

QScrollBar::add-line, QScrollBar::sub-line,
QScrollBar::add-page, QScrollBar::sub-page {
    width: 0;
    height: 0;
    background: transparent;
}

QPushButton[kvRole="findClose"] {
    min-width: 28px;
    max-width: 28px;
    padding: 0;
    border-color: transparent;
    background: transparent;
    color: {{textMuted}};
    font-size: 17px;
}

QPushButton[kvRole="findClose"]:hover {
    background: {{surfaceHover}};
    color: {{danger}};
}

QToolButton[kvRole="sidebarAction"] {
    min-width: 24px;
    max-width: 24px;
    min-height: 24px;
    max-height: 24px;
    padding: 0;
    border: 0;
    border-radius: 4px;
    background: transparent;
    color: {{textMuted}};
    font-size: 14px;
}

QToolButton[kvRole="sidebarAction"]:hover {
    background: {{surfaceHover}};
    color: {{textMain}};
}
)QSS");

    // Scale legacy product text once, before token expansion. Kit controls own
    // their fonts; the tooltip token deliberately keeps its recipe size.
    static const QRegularExpression fontSize(QStringLiteral(R"((font-size:\s*)(\d+)px)"));
    auto matches = fontSize.globalMatch(style);
    QList<QRegularExpressionMatch> sizes;
    while (matches.hasNext())
        sizes.append(matches.next());
    for (auto it = sizes.crbegin(); it != sizes.crend(); ++it)
        style.replace(it->capturedStart(2), it->capturedLength(2),
                      QString::number(InterfaceZoomMetrics::scale(it->captured(2).toInt(),
                                                                  interfaceFontSizePixels)));

    const std::pair<const char*, QString> replacements[] = {
        {"{{terminalFontFamily}}", terminalFontFamily},
        {"{{interfaceFontSize}}", QString::number(interfaceFontSizePixels)},
        {"{{tooltipFontSize}}",
         QString::number(qRound(ket::tooltipRecipe(ket::Appearance::instance().theme()).fontPixels *
                                interfaceFontSizePixels / InterfaceZoomMetrics::designedAtPixels))},
        {"{{pageBackground}}", palette.pageBackground},
        {"{{panelBackground}}", palette.panelBackground},
        {"{{panelSubtle}}", palette.panelSubtle},
        {"{{sidebarBackground}}", palette.sidebarBackground},
        {"{{surfaceRaised}}", palette.surfaceRaised},
        {"{{surfaceHover}}", palette.surfaceHover},
        {"{{textMain}}", palette.textMain},
        {"{{textSecondary}}", palette.textSecondary},
        {"{{textMuted}}", palette.textMuted},
        {"{{border}}", palette.border},
        {"{{borderStrong}}", palette.borderStrong},
        {"{{interactiveBackground}}", palette.interactiveBackground},
        {"{{interactiveHover}}", palette.interactiveHover},
        {"{{interactiveActive}}", palette.interactiveActive},
        {"{{accent}}", palette.accent},
        {"{{accentHover}}", palette.accentHover},
        {"{{accentActive}}", palette.accentActive},
        {"{{accentSubtle}}", palette.accentSubtle},
        {"{{accentMuted}}", palette.accentMuted},
        {"{{focus}}", palette.focus},
        {"{{tabActiveText}}", palette.tabActiveText},
        {"{{accentText}}", palette.accentText},
        {"{{actionPrimaryHover}}", palette.actionPrimaryHover},
        {"{{actionPrimaryActive}}", palette.actionPrimaryActive},
        {"{{actionPrimaryInk}}", palette.actionPrimaryInk},
        {"{{actionPrimary}}", palette.actionPrimary},
        {"{{danger}}", palette.danger},
    };

    for (const auto& [token, value] : replacements) {
        style.replace(QString::fromLatin1(token), value);
    }
    return style;
}

} // namespace

ThemeManager::ThemeManager(QApplication& application, QObject* parent)
    : QObject(parent), application_(application) {
    // The kit's style draws menus, tooltips, tab bars, select popups, progress bars and
    // sliders from Két DS recipes, and keeps the application palette on ket::Appearance;
    // the stylesheet below styles the rest. The profile goes first: the style reads it.
    auto profile = ket::Profile::ketPlus();
    profile.codePixels = 14;
    profile.conversationNumberPixels = 14;
    profile.usageRingSize = 15;
    profile.usageRingStroke = 2;
    profile.conversationTitleWeight = 500;
    profile.conversationSelectedTitleWeight = 500;
    profile.conversationMetaWeight = 500;
    profile.navItemActiveWeight = 500;
    ket::Profile::setCurrent(profile);
    ket::Appearance::instance().setZoom(qreal(interfaceFontSizePixels_) /
                                        InterfaceZoomMetrics::designedAtPixels);
    ket::Style::install(application_);
    loadInterfaceFont();

    QSettings settings;
    mode_ = modeFromName(
        settings.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString());

    // ket::Appearance owns light, dark and following the system; QML (KetTheme), the kit's
    // style and KétPlus Terminal read it, so the palette follows it too.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            [this] { apply(); });
    syncAppearance();
}

ThemeManager::Mode ThemeManager::mode() const noexcept { return mode_; }

const ThemePalette& ThemeManager::palette() const noexcept { return palette_; }

int ThemeManager::interfaceFontSizePixels() const noexcept { return interfaceFontSizePixels_; }

void ThemeManager::setMode(const Mode mode) {
    const bool needsApply = mode_ != mode || previewMode_.has_value();
    mode_ = mode;
    previewMode_.reset();
    QSettings().setValue(QStringLiteral("appearance/theme"), modeName(mode_));
    if (!needsApply) {
        return;
    }
    syncAppearance();
}

void ThemeManager::previewMode(const Mode mode) {
    if (previewMode_ == mode) {
        return;
    }
    previewMode_ = mode;
    syncAppearance();
}

void ThemeManager::cancelPreview() {
    if (!previewMode_.has_value()) {
        return;
    }
    previewMode_.reset();
    syncAppearance();
}

void ThemeManager::setInterfaceFontSizePixels(const int fontSizePixels) {
    const int normalized = qBound(8, fontSizePixels, 24);
    if (interfaceFontSizePixels_ == normalized) {
        return;
    }
    interfaceFontSizePixels_ = normalized;
    // Finish QApplication/QSS propagation before notifying kit widgets. Qt's
    // stylesheet repolish resets child fonts, including explicit recipe fonts.
    QFont font = application_.font();
    font.setPixelSize(interfaceFontSizePixels_);
    setApplicationFont(font);
    apply(false);
    ket::Appearance::instance().setZoom(qreal(normalized) / InterfaceZoomMetrics::designedAtPixels);
    emit themeChanged();
}

void ThemeManager::syncAppearance() {
    const Mode mode = effectiveMode();
    const auto previousTheme = ket::Appearance::instance().theme();
    ket::Appearance::instance().setMode(mode == Mode::Dark    ? ket::Appearance::Mode::Dark
                                        : mode == Mode::Light ? ket::Appearance::Mode::Light
                                                              : ket::Appearance::Mode::System);
    // A changed theme already calls apply() before the kit widgets' observers.
    // Reapplying afterwards would overwrite the fonts they just refreshed.
    if (previousTheme == ket::Appearance::instance().theme())
        apply();
}

void ThemeManager::apply(const bool notify) {
    palette_ = paletteFor(ket::Appearance::instance().theme());

    const auto sheet = styleSheetFor(palette_, interfaceFontSizePixels_);
    if (application_.styleSheet() != sheet)
        application_.setStyleSheet(sheet);
    restoreTooltipFont();

    if (notify)
        emit themeChanged();
}

ThemeManager::Mode ThemeManager::effectiveMode() const noexcept {
    return previewMode_.value_or(mode_);
}

void ThemeManager::loadInterfaceFont() {
    uiFontFamily_ = QFontDatabase::hasFamily(QStringLiteral("KET Sans"))
                        ? QStringLiteral("KET Sans")
                        : QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();

    QFont font(uiFontFamily_);
    font.setPixelSize(interfaceFontSizePixels_);
    font.setWeight(QFont::Normal);
    setApplicationFont(font);
}

void ThemeManager::setApplicationFont(const QFont& font) {
    // QApplication::setFont without a class drops the per-class fonts, including the
    // tooltip font ket::Style sets; put it back as the style does on a theme change.
    application_.setFont(font);
    restoreTooltipFont();
}

void ThemeManager::restoreTooltipFont() {
    QFont tooltipFont = ket::Style::interfaceFont();
    tooltipFont.setPixelSize(tooltipFontPixels());
    QToolTip::setFont(tooltipFont);
}

} // namespace ketplus
