#include "KetNavItem.h"
#include "ui/ChromeGeometry.h"
#include "ui/SymbolIcon.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "workspace/ActivityBar.h"
#include <QButtonGroup>
#include <QLabel>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
namespace ketplus {
void ActivityBar::applyPalette(const ThemePalette& palette, const int interfaceFontSizePixels) {
    if (auto* avatar = findChild<QLabel*>(QStringLiteral("productIdentityAvatar"))) {
        const auto px = [interfaceFontSizePixels](int n) {
            return InterfaceZoomMetrics::geometry(n, interfaceFontSizePixels);
        };
        avatar->setGeometry(px(7), px(11), px(29), px(29));
    }
    const QString wordmark = palette.dark ? QStringLiteral(":/brand/ketplus-logo-dark.png")
                                          : QStringLiteral(":/brand/ketplus-logo-light.png");
    const QPixmap source(wordmark);
    if (!source.isNull()) {
        const QSize size = brandWordmark_->size() * brandWordmark_->devicePixelRatioF();
        QPixmap scaled = source.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(brandWordmark_->devicePixelRatioF());
        brandWordmark_->setPixmap(scaled);
    }
    const QString controlBorder = palette.border;
    const QString sidebarAccent = palette.accentText;
    const QString navSelectedText = palette.accentText;
    const QString sheet = TypeScale::apply(QStringLiteral(R"(
        QWidget#activityBar { border-right: 1px solid %1; background: %2; }
        QScrollArea#productSidebarContext, QScrollArea#productSidebarContext > QWidget > QWidget,
        QWidget#productSidebarContextBody { border: 0; background: transparent; }
        QScrollBar:vertical { width: 5px; border: 0; background: transparent; }
        QScrollBar::handle:vertical { min-height: 24px; border-radius: 2px; background: %10; }
        QScrollBar[scrolling="false"]::handle:vertical { background: transparent; }
        QScrollBar[scrolling="false"]:hover::handle:vertical { background: %10; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QFrame#productSidebarRule { border: 0; background: %1; }
        QToolButton { text-align: left; }
        QToolButton[kvRole="productSearch"] { min-height: 30px; max-height: 30px; padding: 0 8px; border: 1px solid %11; border-radius: 5px; background: %4; color: %5; font-size: {{caption}}px; }
        QToolButton[kvRole="productSearch"]:hover { border-color: %13; }
        QToolButton[kvRole="productNavButton"] { padding: 0; border: 0; border-radius: 5px; background: transparent; }
        QToolButton[kvRole="productNavButton"]:hover { background: %7; }
        QToolButton[kvRole="productNavButton"]:checked { background: %8; }
        QLabel[kvRole="productNavLabel"] { color: %6; font-size: {{body}}px; font-weight: 400; }
        QLabel[kvRole="productNavLabel"][activeNavigation="true"] { color: %9; }
        QLabel[kvRole="productNavLabel"][disabledNavigation="true"] { color: %10; }
        QLabel[kvRole="productSidebarSection"] { color: %5; font-size: {{caption}}px; font-weight: 500; }
        QToolButton[kvRole="productSectionAction"] { padding: 0; border: 0; border-radius: 4px; background: transparent; }
        QToolButton[kvRole="productSectionAction"]:hover { background: %7; }
        QComboBox#productMachineSelect { min-height:28px; padding:0 18px 0 0; border:0; background:transparent; color:%6; font-size:{{caption}}px; font-weight:600; }
        QComboBox#productMachineSelect:hover { color:%3; }
        QComboBox#productMachineSelect:focus { outline:1px solid %12; }
        QComboBox#productMachineSelect::drop-down { width:17px; border:0; background:transparent; }
        QComboBox#productMachineSelect QAbstractItemView { background:%4; color:%3; border:1px solid %11; selection-background-color:%8; }
        QToolButton[kvRole="productRepositoryGroup"] { padding: 0 0 0 8px; border: 0; background: transparent; color: %5; font-size: {{caption}}px; font-weight: 500; }
        QToolButton[kvRole="productRepositoryGroup"]:hover, QToolButton[kvRole="productRepositoryGroup"][activeRepository="true"] { color: %6; }
        QToolButton[kvRole="productShowMoreConversations"] { padding: 0 0 0 8px; border: 0; background: transparent; color: %5; font-size: {{caption}}px; }
        QToolButton[kvRole="productShowMoreConversations"]:hover { color: %6; }
        QToolButton[kvRole="productConversation"] { padding: 0; border: 0; border-radius: 6px; background: transparent; }
        QToolButton[kvRole="productConversation"]:hover, QToolButton[kvRole="productConversation"]:checked { background: %7; }
        QLabel[kvRole="productConversationTitle"] { color: %3; font-size: {{body}}px; font-weight: 400; }
        QLabel[kvRole="productConversationNumber"] { color: %5; font-size: {{caption}}px; font-weight: 500; }
        QLabel[kvRole="productConversationAgentSender"] { color: %5; font-size: {{caption}}px; font-weight: 400; }
        QMenu#productUserMenu { padding: 4px; border: 1px solid %11; border-radius: 6px; background: %4; }
        QMenu#productUserMenu::item { padding: 5px 18px 5px 8px; border-radius: 4px; color: %3; font-size: {{body}}px; }
        QMenu#productUserMenu::item:selected { background: %7; }
        QToolButton[kvRole="productIdentity"] { padding: 7px 7px 7px 43px; border: 0; border-top: 1px solid %1; border-radius: 0; background: transparent; color: %6; font-size: {{caption}}px; }
        QToolButton[kvRole="productIdentity"]:hover { background: %7; }
        QToolButton[kvRole="productUpdateReady"] { margin: 0 2px 4px 2px; padding: 7px 9px; border: 1px solid %12; border-radius: 6px; background: %8; color: %12; font-size: {{caption}}px; font-weight: 550; }
        QToolButton[kvRole="productUpdateReady"]:hover { background: %7; }
        QLabel#productIdentityAvatar { border: 1px solid %11; border-radius: 14px; background: %4; color: %12; font-size: {{micro}}px; font-weight: 600; }
        QLabel#quickSearchShortcut { color: %5; font-size: {{micro}}px; }
        QLabel#sourceControlCountBadge { border: 1px solid %11; border-radius: 4px; color: %6; font-size: {{micro}}px; }
    )"),
                                           interfaceFontSizePixels);
    setStyleSheet(sheet.arg(
        palette.border, palette.sidebarBackground, palette.textMain, palette.panelBackground,
        palette.textMuted, palette.textSecondary, palette.surfaceHover, palette.accentSubtle,
        navSelectedText, palette.textDisabled, controlBorder, sidebarAccent, palette.borderStrong));
    chrome_geometry::apply(this, interfaceFontSizePixels);
    repositoryList_->applyZoom(interfaceFontSizePixels);
    iconMainColor_ = palette.textMain;
    iconMutedColor_ = palette.textMuted;
    iconDisabledColor_ = palette.textDisabled;
    iconAccentColor_ = sidebarAccent;
    iconPositiveColor_ = palette.positive;
    iconWarningColor_ = palette.warning;
    iconDangerColor_ = palette.danger;
    refreshIcons();
}

void ActivityBar::refreshIcons() {
    for (auto* button : findChildren<QToolButton*>()) {
        const auto body = button->property("symbolName").toByteArray();
        if (body.isEmpty())
            continue;
        QString color = iconMutedColor_;
        if (button->property("symbolColorRole").toString() == QStringLiteral("main"))
            color = iconMainColor_;
        if (group_->buttons().contains(button) && button->isChecked())
            color = iconAccentColor_;
        button->setIcon(QIcon(symbol_icon::pixmap(body, QColor(color), button->iconSize())));
    }
    for (auto* label : findChildren<QLabel*>()) {
        const auto body = label->property("symbolName").toByteArray();
        if (label->property("kvRole").toString() == QStringLiteral("productNavLabel")) {
            auto* button = qobject_cast<QToolButton*>(label->parentWidget());
            label->setProperty("activeNavigation", button != nullptr && button->isChecked());
            label->setProperty("disabledNavigation", button != nullptr && !button->isEnabled());
            label->style()->unpolish(label);
            label->style()->polish(label);
        }
        if (body.isEmpty())
            continue;
        const QString role = label->property("symbolColorRole").toString();
        QString color = iconMutedColor_;
        auto* navigationButton = qobject_cast<QToolButton*>(label->parentWidget());
        if (label->objectName() == QStringLiteral("productNavIcon") && navigationButton != nullptr)
            color = navigationButton->isEnabled()
                        ? (navigationButton->isChecked() ? iconAccentColor_ : iconMutedColor_)
                        : iconDisabledColor_;
        else if (role == QStringLiteral("accent"))
            color = iconAccentColor_;
        else if (role == QStringLiteral("main"))
            color = iconMainColor_;
        else if (role == QStringLiteral("positive"))
            color = iconPositiveColor_;
        else if (role == QStringLiteral("warning"))
            color = iconWarningColor_;
        else if (role == QStringLiteral("danger"))
            color = iconDangerColor_;
        label->setPixmap(symbol_icon::pixmap(body, QColor(color), label->size()));
    }
}

ActivityBar::Mode ActivityBar::mode() const noexcept { return mode_; }

} // namespace ketplus
