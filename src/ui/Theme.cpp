#include "ui/Theme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>

#include <utility>

namespace ketplus {
namespace {

ThemePalette lightPalette() {
    return {
        .dark = false,
        .appBackground = QStringLiteral("#F7F5F5"),
        .pageBackground = QStringLiteral("#F7F5F5"),
        .sidebarBackground = QStringLiteral("#F7F5F5"),
        .panelBackground = QStringLiteral("#FFFFFF"),
        .panelSubtle = QStringLiteral("#F7F5F5"),
        .surfaceRaised = QStringLiteral("#FBFAFA"),
        .surfaceHover = QStringLiteral("#F1EFF0"),
        .textMain = QStringLiteral("#24262A"),
        .textSecondary = QStringLiteral("#5A5C5E"),
        .textMuted = QStringLiteral("#717373"),
        .textDisabled = QStringLiteral("#9A9EA2"),
        .border = QStringLiteral("#E9E7E8"),
        .borderStrong = QStringLiteral("#CCCCCD"),
        .interactiveBackground = QStringLiteral("#FFFFFF"),
        .interactiveHover = QStringLiteral("#F7F5F5"),
        .interactiveActive = QStringLiteral("#EFEDEE"),
        .accent = QStringLiteral("#5968DF"),
        .accentHover = QStringLiteral("#4F5ED0"),
        .accentActive = QStringLiteral("#4557BC"),
        .accentSubtle = QStringLiteral("#EEF0FB"),
        .accentMuted = QStringLiteral("#DDE2F7"),
        .focus = QStringLiteral("#7485E8"),
        .tabActiveText = QStringLiteral("#394985"),
        .positive = QStringLiteral("#1D5D3B"),
        .warning = QStringLiteral("#8B5C12"),
        .danger = QStringLiteral("#B42318"),
        .info = QStringLiteral("#5968DF"),
    };
}

ThemePalette darkPalette() {
    return {
        .dark = true,
        .appBackground = QStringLiteral("#15181D"),
        .pageBackground = QStringLiteral("#1B1F24"),
        .sidebarBackground = QStringLiteral("#171B20"),
        .panelBackground = QStringLiteral("#1D2228"),
        .panelSubtle = QStringLiteral("#171B20"),
        .surfaceRaised = QStringLiteral("#23282F"),
        .surfaceHover = QStringLiteral("#292F37"),
        .textMain = QStringLiteral("#CDD2D8"),
        .textSecondary = QStringLiteral("#B4BAC4"),
        .textMuted = QStringLiteral("#858D99"),
        .textDisabled = QStringLiteral("#505862"),
        .border = QStringLiteral("#292D33"),
        .borderStrong = QStringLiteral("#3A4048"),
        .interactiveBackground = QStringLiteral("#20252C"),
        .interactiveHover = QStringLiteral("#292F37"),
        .interactiveActive = QStringLiteral("#313844"),
        .accent = QStringLiteral("#5968DF"),
        .accentHover = QStringLiteral("#7485E8"),
        .accentActive = QStringLiteral("#4F5ED0"),
        .accentSubtle = QStringLiteral("#252B42"),
        .accentMuted = QStringLiteral("#2B3356"),
        .focus = QStringLiteral("#7485E8"),
        .tabActiveText = QStringLiteral("#AAB6ED"),
        .positive = QStringLiteral("#40C97B"),
        .warning = QStringLiteral("#E5A93C"),
        .danger = QStringLiteral("#EF665C"),
        .info = QStringLiteral("#5968DF"),
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

QString styleSheetFor(const ThemePalette& palette, const QString& fontFamily) {
    const QString terminalFontFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    QString style = QStringLiteral(R"QSS(
QWidget {
    color: {{textMain}};
    font-family: "{{fontFamily}}";
    font-size: 14px;
    selection-background-color: {{accentMuted}};
    selection-color: {{textMain}};
}

QMainWindow, QDialog, QMessageBox {
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
    color: {{accentActive}};
}

QMenu {
    padding: 5px;
    border: 1px solid {{borderStrong}};
    border-radius: 7px;
    background: {{panelBackground}};
    color: {{textMain}};
}

QMenu::item {
    min-height: 24px;
    padding: 4px 28px 4px 10px;
    border-radius: 5px;
}

QMenu::item:selected {
    background: {{accentSubtle}};
    color: {{accentActive}};
}

QMenu::separator {
    height: 1px;
    margin: 5px 7px;
    background: {{border}};
}

QTabWidget::pane {
    border: 0;
    background: {{panelBackground}};
}

QTabBar {
    qproperty-drawBase: 0;
    background: {{pageBackground}};
}

QTabBar::tab {
    min-width: 104px;
    min-height: 40px;
    padding: 0 14px;
    border: 0;
    border-right: 1px solid {{border}};
    border-bottom: 2px solid transparent;
    background: {{pageBackground}};
    color: {{textMuted}};
    font-size: 12px;
    font-weight: 500;
}

QTabBar::tab:hover:!selected {
    background: {{surfaceHover}};
    color: {{textSecondary}};
}

QTabBar::tab:selected {
    border-bottom: 2px solid {{accent}};
    background: {{panelBackground}};
    color: {{tabActiveText}};
}

QTabBar::close-button {
    margin-left: 7px;
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
    color: {{accent}};
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
    color: {{accentActive}};
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

QToolButton[kvRole="statusGit"] {
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
QToolButton[kvRole="statusGit"]:pressed {
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
    color: {{accentActive}};
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

QWidget[kvRole="settingsPreview"] {
    border: 1px solid {{border}};
    border-radius: 8px;
    background: {{panelBackground}};
    color: {{textMain}};
}

QPushButton[kvRole="primaryAction"] {
    border-color: {{accent}};
    background: {{accent}};
    color: #FFFFFF;
    font-weight: 600;
}

QPushButton[kvRole="primaryAction"]:hover {
    border-color: {{accentHover}};
    background: {{accentHover}};
    color: #FFFFFF;
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

QToolTip {
    padding: 6px 8px;
    border: 1px solid {{borderStrong}};
    border-radius: 5px;
    background: {{surfaceRaised}};
    color: {{textMain}};
    font-size: 12px;
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

    const std::pair<const char*, QString> replacements[] = {
        {"{{fontFamily}}", fontFamily},
        {"{{terminalFontFamily}}", terminalFontFamily},
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
    application_.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    loadInterFont();

    QSettings settings;
    mode_ = modeFromName(
        settings.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString());

    connect(application_.styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
                if (mode_ == Mode::System) {
                    apply();
                }
            });
    apply();
}

ThemeManager::Mode ThemeManager::mode() const noexcept { return mode_; }

const ThemePalette& ThemeManager::palette() const noexcept { return palette_; }

void ThemeManager::setMode(const Mode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    QSettings().setValue(QStringLiteral("appearance/theme"), modeName(mode_));
    apply();
}

void ThemeManager::apply() {
    palette_ = useDarkPalette() ? darkPalette() : lightPalette();

    QPalette nativePalette;
    nativePalette.setColor(QPalette::Window, QColor(palette_.pageBackground));
    nativePalette.setColor(QPalette::WindowText, QColor(palette_.textMain));
    nativePalette.setColor(QPalette::Base, QColor(palette_.panelBackground));
    nativePalette.setColor(QPalette::AlternateBase, QColor(palette_.panelSubtle));
    nativePalette.setColor(QPalette::Text, QColor(palette_.textMain));
    nativePalette.setColor(QPalette::Button, QColor(palette_.interactiveBackground));
    nativePalette.setColor(QPalette::ButtonText, QColor(palette_.textSecondary));
    nativePalette.setColor(QPalette::Highlight, QColor(palette_.accentMuted));
    nativePalette.setColor(QPalette::HighlightedText, QColor(palette_.textMain));
    application_.setPalette(nativePalette);
    application_.setStyleSheet(styleSheetFor(palette_, uiFontFamily_));

    emit themeChanged();
}

bool ThemeManager::useDarkPalette() const {
    if (mode_ == Mode::Dark) {
        return true;
    }
    if (mode_ == Mode::Light) {
        return false;
    }
    return application_.styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void ThemeManager::loadInterFont() {
    uiFontFamily_ = QStringLiteral("Inter");
    const int fontId =
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/InterVariable.ttf"));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) {
        uiFontFamily_ = families.constFirst();
    }

    QFont font(uiFontFamily_);
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    application_.setFont(font);
}

} // namespace ketplus
