#include "terminal/TerminalPanel.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "terminal/TerminalTab.h"
#include "terminal/TerminalView.h"
#include <QDir>
#include <QHBoxLayout>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
namespace ketplus {
TerminalPanel::TerminalPanel(QWidget* parent)
    : QWidget(parent), tabs_(new QTabWidget(this)), palette_(currentThemePalette()) {
    setProperty("kvRole", QStringLiteral("terminalPanel"));
    setMinimumHeight(120);
    tabs_->setObjectName(QStringLiteral("terminalPanelTabs"));
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->tabBar()->setExpanding(false);
    auto* actions = new QWidget(tabs_);
    auto* row = new QHBoxLayout(actions);
    row->setContentsMargins(0, 0, 4, 0);
    row->setSpacing(0);
    const auto add = [actions, row](QStringView icon, const QString& label) {
        auto* button =
            new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(icon)), label, actions);
        row->addWidget(button);
        return button;
    };
    auto* create = add(u"plus", tr("New terminal"));
    create->setObjectName(QStringLiteral("newTerminalTab"));
    auto* find = add(u"search", tr("Find in terminal"));
    auto* clear = add(u"trash-2", tr("Clear terminal scrollback"));
    auto* restart = add(u"refresh-cw", tr("Restart terminal session"));
    auto* hide = add(u"x", tr("Hide terminal"));
    tabs_->setCornerWidget(actions, Qt::TopRightCorner);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs_);
    connect(create, &QAbstractButton::clicked, this, [this] { addTab(true); });
    connect(find, &QAbstractButton::clicked, this, [this] {
        if (auto* tab = currentTab())
            tab->view()->openSearch();
    });
    connect(clear, &QAbstractButton::clicked, this, [this] {
        if (auto* tab = currentTab())
            tab->view()->clearScrollback();
    });
    connect(restart, &QAbstractButton::clicked, this, [this] {
        if (auto* tab = currentTab(); tab && !tab->restart())
            emit statusMessageRequested(tr("Unable to restart terminal session."));
    });
    connect(hide, &QAbstractButton::clicked, this, &TerminalPanel::closeRequested);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto* tab = tabs_->widget(index);
        tabs_->removeTab(index);
        delete tab;
        if (!tabs_->count()) {
            addTab(false);
            emit closeRequested();
        }
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (auto* tab = currentTab())
            setFocusProxy(tab->view());
    });
    addTab(false);
}
TerminalTab* TerminalPanel::currentTab() const {
    return qobject_cast<TerminalTab*>(tabs_->currentWidget());
}
void TerminalPanel::addTab(bool startSession) {
    auto* tab = new TerminalTab(workingDirectory_, {}, tabs_);
    tab->applyTheme(palette_);
    tab->setTypography(fontSizePixels_, lineHeightPixels_, fontFamily_);
    const int index = tabs_->addTab(tab, tab->title());
    tabs_->setTabToolTip(index, tab->workingDirectory());
    tabs_->setCurrentIndex(index);
    connect(tab, &TerminalTab::statusMessageRequested, this,
            &TerminalPanel::statusMessageRequested);
    connect(tab, &TerminalTab::titleChanged, this, [this, tab](const QString& title) {
        const int index = tabs_->indexOf(tab);
        if (index >= 0)
            tabs_->setTabText(index, title);
    });
    if (startSession)
        tab->start();
}
void TerminalPanel::start(const QString& path) {
    workingDirectory_ = QDir::cleanPath(path.isEmpty() ? QDir::homePath() : path);
    if (auto* tab = currentTab()) {
        tab->setDirectoryBeforeStart(workingDirectory_);
        tabs_->setTabToolTip(tabs_->currentIndex(), tab->workingDirectory());
        tab->start();
    }
}
void TerminalPanel::typeText(const QString& text) {
    if (auto* tab = currentTab())
        tab->typeText(text);
}

void TerminalPanel::typeTextInNewTab(const QString& text) {
    if (currentTab() != nullptr)
        addTab(true);
    typeText(text);
}

void TerminalPanel::focusTerminal() {
    auto* tab = currentTab();
    if (!tab)
        return;
    const QPointer<TerminalView> terminal = tab->view();
    terminal->setFocus(Qt::ShortcutFocusReason);
    QTimer::singleShot(0, terminal, [terminal] {
        if (terminal && terminal->isVisible()) {
            terminal->window()->activateWindow();
            terminal->setFocus(Qt::ShortcutFocusReason);
        }
    });
}
void TerminalPanel::applyTheme(const ThemePalette& palette) {
    palette_ = palette;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* tab = qobject_cast<TerminalTab*>(tabs_->widget(i)))
            tab->applyTheme(palette);
}
void TerminalPanel::setTypography(int fontSize, int lineHeight, const QString& family) {
    fontSizePixels_ = fontSize;
    lineHeightPixels_ = lineHeight;
    fontFamily_ = family;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* tab = qobject_cast<TerminalTab*>(tabs_->widget(i)))
            tab->setTypography(fontSize, lineHeight, family);
}
bool TerminalPanel::isSessionRunning() const {
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* tab = qobject_cast<TerminalTab*>(tabs_->widget(i)); tab && tab->isRunning())
            return true;
    return false;
}
} // namespace ketplus
