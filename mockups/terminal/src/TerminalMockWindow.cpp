// SPDX-License-Identifier: MPL-2.0
#include "TerminalMockWindow.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "KetToast.h"
#include "KetTokens.h"
#include "terminal/TerminalView.h"
#include "terminal_app/TerminalTab.h"
#include "ui/Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QTabBar>
#include <QTabWidget>

namespace {

QString token(QStringView name, ket::Theme theme) {
    return ket::Tokens::color(name, theme).name(QColor::HexArgb);
}

// The roles TerminalView reads, taken from Két Design System tokens instead of the KetPlus
// palette: the grid on the page background, the cursor in the accent, matches in warning.
ketplus::ThemePalette terminalPalette(ket::Theme theme) {
    ketplus::ThemePalette palette;
    palette.dark = theme == ket::Theme::Dark;
    palette.textMain = token(u"text-main", theme);
    palette.panelSubtle = token(u"page-bg", theme);
    palette.accent = token(u"accent", theme);
    palette.accentMuted = token(u"accent-muted", theme);
    palette.warning = token(u"warning", theme);
    return palette;
}

// The tab strip's trailing area: the strip background and its bottom edge, so the actions sit
// on the strip instead of the page.
class TabActions final : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent*) override {
        const ket::TabBarRecipe recipe = ket::tabBarRecipe(ket::Appearance::instance().theme());
        QPainter painter(this);
        painter.fillRect(rect(), recipe.background);
        painter.fillRect(QRect(0, height() - 1, width(), 1), recipe.border);
    }
};

ket::IconButton* actionButton(const QString& code, const QString& label, QWidget* parent) {
    auto* button = new ket::IconButton(ket::Icons::icon(code), label, parent);
    button->setToolTip(label);
    return button;
}

} // namespace

TerminalMockWindow::TerminalMockWindow(QString workingDirectory, QWidget* parent)
    : QMainWindow(parent), tabs_(new QTabWidget(this)),
      workingDirectory_(QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath()
                                                                   : std::move(workingDirectory))) {
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(480, 280);
    resize(920, 580);
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->tabBar()->setExpanding(false);
    setCentralWidget(tabs_);

    // The embedded panel's actions (core/src/terminal/TerminalPanel), beside the tabs.
    auto* actions = new TabActions(tabs_);
    auto* actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(4, 0, 6, 0);
    actionsLayout->setSpacing(2);
    auto* newTab = actionButton(QStringLiteral("plus"), QStringLiteral("New tab"), actions);
    auto* find =
        actionButton(QStringLiteral("search"), QStringLiteral("Find in terminal"), actions);
    auto* clear =
        actionButton(QStringLiteral("trash"), QStringLiteral("Clear terminal scrollback"), actions);
    auto* restart = actionButton(QStringLiteral("refresh"),
                                 QStringLiteral("Restart terminal session"), actions);
    for (QWidget* button : {static_cast<QWidget*>(newTab), static_cast<QWidget*>(find),
                            static_cast<QWidget*>(clear), static_cast<QWidget*>(restart)})
        actionsLayout->addWidget(button);
    tabs_->setCornerWidget(actions, Qt::TopRightCorner);
    connect(newTab, &QAbstractButton::clicked, this, [this] { addTab(workingDirectory_); });
    connect(find, &QAbstractButton::clicked, this, [this] {
        if (ketplus::TerminalTab* tab = currentTab())
            tab->view()->openSearch();
    });
    connect(clear, &QAbstractButton::clicked, this, [this] {
        if (ketplus::TerminalTab* tab = currentTab())
            tab->view()->clearScrollback();
    });
    connect(restart, &QAbstractButton::clicked, this, [this] {
        if (ketplus::TerminalTab* tab = currentTab(); tab && !tab->restart())
            showStatus(QStringLiteral("Unable to restart the terminal session."));
    });

    createMenus();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &TerminalMockWindow::applyTheme);
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, actions,
            qOverload<>(&QWidget::update));
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &TerminalMockWindow::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (ketplus::TerminalTab* tab = currentTab()) {
            workingDirectory_ = tab->workingDirectory();
            tab->view()->setFocus(Qt::ShortcutFocusReason);
        }
        updateWindowTitle();
    });
    connect(tabs_->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0)
            addTab(workingDirectory_);
    });
}

void TerminalMockWindow::createMenus() {
    const auto add = [this](QMenu* menu, const QString& text, const QKeySequence& shortcut,
                            auto&& slot) {
        QAction* action = menu->addAction(text);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    const auto onView = [this](auto method) {
        return [this, method] {
            if (ketplus::TerminalTab* tab = currentTab())
                (tab->view()->*method)();
        };
    };

    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    add(file, QStringLiteral("New Window"), QKeySequence::New,
        [this] { emit newWindowRequested(workingDirectory_); });
    add(file, QStringLiteral("New Tab"), QKeySequence(QStringLiteral("Ctrl+T")),
        [this] { addTab(workingDirectory_); });
    add(file, QStringLiteral("Open Folder in New Tab…"), QKeySequence(QStringLiteral("Ctrl+O")),
        [this] {
            const QString path = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Open Terminal in Folder"), workingDirectory_);
            if (!path.isEmpty())
                addTab(path);
        });
    file->addSeparator();
    add(file, QStringLiteral("Close Tab"), QKeySequence::Close,
        [this] { closeTab(tabs_->currentIndex()); });
    add(file, QStringLiteral("Close Window"), QKeySequence(QStringLiteral("Ctrl+Shift+W")),
        [this] { close(); });
    QAction* quit = add(file, QStringLiteral("Quit KetPlus Terminal"), QKeySequence::Quit,
                        [] { QApplication::closeAllWindows(); });
    quit->setMenuRole(QAction::QuitRole);

    QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    add(edit, QStringLiteral("Copy"), QKeySequence::Copy,
        onView(&ketplus::TerminalView::copySelection));
    add(edit, QStringLiteral("Paste"), QKeySequence::Paste,
        onView(&ketplus::TerminalView::pasteClipboard));
    add(edit, QStringLiteral("Select All"), QKeySequence::SelectAll,
        onView(&ketplus::TerminalView::selectAll));
    edit->addSeparator();
    add(edit, QStringLiteral("Find…"), QKeySequence::Find,
        onView(&ketplus::TerminalView::openSearch));

    QMenu* shell = menuBar()->addMenu(QStringLiteral("&Shell"));
    add(shell, QStringLiteral("Restart Session"), QKeySequence(QStringLiteral("Ctrl+Shift+R")),
        [this] {
            if (ketplus::TerminalTab* tab = currentTab(); tab && !tab->restart())
                showStatus(QStringLiteral("Unable to restart the terminal session."));
        });
    add(shell, QStringLiteral("Clear Scrollback"), QKeySequence(QStringLiteral("Ctrl+K")),
        onView(&ketplus::TerminalView::clearScrollback));

    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* next =
        add(view, QStringLiteral("Next Tab"), QKeySequence(QStringLiteral("Ctrl+Shift+]")),
            [this] { activateAdjacentTab(1); });
    next->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Shift+]")), QKeySequence(QStringLiteral("Ctrl+Tab"))});
    QAction* previous =
        add(view, QStringLiteral("Previous Tab"), QKeySequence(QStringLiteral("Ctrl+Shift+[")),
            [this] { activateAdjacentTab(-1); });
    previous->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+[")),
                            QKeySequence(QStringLiteral("Ctrl+Shift+Tab"))});
    view->addSeparator();
    add(view, QStringLiteral("Bigger Text"), QKeySequence::ZoomIn, [this] { changeFontSize(1); });
    add(view, QStringLiteral("Smaller Text"), QKeySequence::ZoomOut,
        [this] { changeFontSize(-1); });
    add(view, QStringLiteral("Actual Size"), QKeySequence(QStringLiteral("Ctrl+0")), [this] {
        fontSizePixels_ = 14;
        lineHeightPixels_ = 22;
        applyTheme();
    });
    view->addSeparator();
    add(view, QStringLiteral("Enter Full Screen"), QKeySequence::FullScreen,
        [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    QMenu* appearance = view->addMenu(QStringLiteral("Appearance"));
    auto* group = new QActionGroup(this);
    for (const auto& [label, mode] :
         {std::pair{QStringLiteral("System"), ket::Appearance::Mode::System},
          std::pair{QStringLiteral("Light"), ket::Appearance::Mode::Light},
          std::pair{QStringLiteral("Dark"), ket::Appearance::Mode::Dark}}) {
        QAction* action = appearance->addAction(label);
        action->setCheckable(true);
        action->setChecked(ket::Appearance::instance().mode() == mode);
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [mode] { ket::Appearance::instance().setMode(mode); });
    }

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* about = help->addAction(QStringLiteral("About KetPlus Terminal"));
    about->setMenuRole(QAction::AboutRole);
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("About KetPlus Terminal"));
        box.setIconPixmap(windowIcon().pixmap(64, 64));
        box.setText(
            QStringLiteral("KetPlus Terminal %1").arg(QCoreApplication::applicationVersion()));
        box.setInformativeText(QStringLiteral("A lightweight native terminal for macOS."));
        box.exec();
    });
}

void TerminalMockWindow::addTab(const QString& workingDirectory, const QString& commandPath) {
    auto* tab = new ketplus::TerminalTab(workingDirectory, commandPath, tabs_);
    tab->applyTheme(terminalPalette(ket::Appearance::instance().theme()));
    applyTypography(tab);
    const int index = tabs_->addTab(tab, tab->title());
    tabs_->setCurrentIndex(index);
    workingDirectory_ = tab->workingDirectory();
    connect(tab, &ketplus::TerminalTab::titleChanged, this, [this, tab](const QString& title) {
        if (const int tabIndex = tabs_->indexOf(tab); tabIndex >= 0) {
            tabs_->setTabText(tabIndex, title.left(32));
            tabs_->setTabToolTip(tabIndex, title);
        }
        if (tab == currentTab())
            updateWindowTitle();
    });
    connect(tab, &ketplus::TerminalTab::statusMessageRequested, this,
            &TerminalMockWindow::showStatus);
    tab->start();
    updateWindowTitle();
}

ketplus::TerminalTab* TerminalMockWindow::currentTab() const {
    return qobject_cast<ketplus::TerminalTab*>(tabs_->currentWidget());
}

void TerminalMockWindow::closeTab(int index) {
    if (index < 0 || index >= tabs_->count())
        return;
    QWidget* tab = tabs_->widget(index);
    tabs_->removeTab(index);
    delete tab;
    if (tabs_->count() == 0)
        close();
}

void TerminalMockWindow::activateAdjacentTab(int delta) {
    if (tabs_->count() > 1)
        tabs_->setCurrentIndex((tabs_->currentIndex() + delta + tabs_->count()) % tabs_->count());
}

void TerminalMockWindow::changeFontSize(int delta) {
    fontSizePixels_ = qBound(10, fontSizePixels_ + delta, 28);
    lineHeightPixels_ = qRound(fontSizePixels_ * 22.0 / 14.0);
    applyTheme();
    showStatus(QStringLiteral("Text size %1 px").arg(fontSizePixels_));
}

void TerminalMockWindow::applyTheme() {
    const ketplus::ThemePalette palette = terminalPalette(ket::Appearance::instance().theme());
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* tab = qobject_cast<ketplus::TerminalTab*>(tabs_->widget(index))) {
            tab->applyTheme(palette);
            applyTypography(tab);
        }
    }
}

// KÉT Sans Mono from the profile; only its Medium face is bundled, so that is the one drawn.
void TerminalMockWindow::applyTypography(ketplus::TerminalTab* tab) const {
    tab->setTypography(fontSizePixels_, lineHeightPixels_, ket::Profile::current().monoFamily());
}

void TerminalMockWindow::showStatus(const QString& message) {
    ket::Toast::showMessage(this, message);
}

void TerminalMockWindow::updateWindowTitle() {
    const ketplus::TerminalTab* tab = currentTab();
    setWindowTitle(tab ? QStringLiteral("%1 — KetPlus Terminal").arg(tab->title())
                       : QStringLiteral("KetPlus Terminal"));
}
