#include "terminal_app/TerminalWindow.h"

#include "terminal/TerminalView.h"
#include "terminal_app/TerminalTab.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>

#include <algorithm>

namespace ketplus {

TerminalWindow::TerminalWindow(ThemeManager& theme, const QString& workingDirectory,
                               const QString& commandPath, QWidget* parent)
    : QMainWindow(parent), theme_(theme), tabs_(new QTabWidget(this)),
      workingDirectory_(
          QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(480, 280);
    resize(920, 580);

    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->tabBar()->setExpanding(false);
    setCentralWidget(tabs_);
    createMenus();
    statusBar()->hide();
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString& message) {
        if (message.isEmpty()) {
            statusBar()->hide();
        }
    });

    const QByteArray savedGeometry =
        QSettings().value(QStringLiteral("window/geometry")).toByteArray();
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    }

    connect(&theme_, &ThemeManager::themeChanged, this, &TerminalWindow::applyTheme);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &TerminalWindow::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            workingDirectory_ = tab->workingDirectory();
            tab->view()->setFocus(Qt::ShortcutFocusReason);
        }
        updateWindowTitle();
    });
    connect(tabs_->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this](const int index) {
        if (index < 0) {
            addTab(workingDirectory_);
        }
    });

    addTab(workingDirectory_, commandPath);
    applyTheme();
}

void TerminalWindow::closeEvent(QCloseEvent* event) {
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    while (tabs_->count() > 0) {
        TerminalTab* tab = qobject_cast<TerminalTab*>(tabs_->widget(0));
        tabs_->removeTab(0);
        delete tab;
    }
    QMainWindow::closeEvent(event);
}

void TerminalWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* newWindowAction = fileMenu->addAction(QStringLiteral("New Window"));
    newWindowAction->setShortcut(QKeySequence::New);
    connect(newWindowAction, &QAction::triggered, this,
            [this] { emit newWindowRequested(workingDirectory_); });

    QAction* newTabAction = fileMenu->addAction(QStringLiteral("New Tab"));
    newTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(newTabAction, &QAction::triggered, this, [this] { addTab(workingDirectory_); });

    QAction* openFolderAction = fileMenu->addAction(QStringLiteral("Open Folder in New Tab…"));
    openFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    connect(openFolderAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Open Terminal in Folder"), workingDirectory_);
        if (!path.isEmpty()) {
            addTab(path);
        }
    });
    fileMenu->addSeparator();
    QAction* closeTabAction = fileMenu->addAction(QStringLiteral("Close Tab"));
    closeTabAction->setShortcut(QKeySequence::Close);
    connect(closeTabAction, &QAction::triggered, this, [this] { closeTab(tabs_->currentIndex()); });
    QAction* closeWindowAction = fileMenu->addAction(QStringLiteral("Close Window"));
    closeWindowAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(closeWindowAction, &QAction::triggered, this, &QWidget::close);
    QAction* quitAction = fileMenu->addAction(QStringLiteral("Quit KetPlus Terminal"));
    quitAction->setMenuRole(QAction::QuitRole);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction* copyAction = editMenu->addAction(QStringLiteral("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->copySelection();
        }
    });
    QAction* pasteAction = editMenu->addAction(QStringLiteral("Paste"));
    pasteAction->setShortcut(QKeySequence::Paste);
    connect(pasteAction, &QAction::triggered, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->pasteClipboard();
        }
    });
    QAction* selectAllAction = editMenu->addAction(QStringLiteral("Select All"));
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAction, &QAction::triggered, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->selectAll();
        }
    });
    editMenu->addSeparator();
    QAction* findAction = editMenu->addAction(QStringLiteral("Find…"));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->openSearch();
        }
    });

    QMenu* shellMenu = menuBar()->addMenu(QStringLiteral("&Shell"));
    QAction* restartAction = shellMenu->addAction(QStringLiteral("Restart Session"));
    restartAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(restartAction, &QAction::triggered, this, &TerminalWindow::restartSession);
    QAction* clearAction = shellMenu->addAction(QStringLiteral("Clear Scrollback"));
    clearAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    connect(clearAction, &QAction::triggered, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->clearScrollback();
        }
    });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* nextTabAction = viewMenu->addAction(QStringLiteral("Next Tab"));
    nextTabAction->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Shift+]")), QKeySequence(QStringLiteral("Ctrl+Tab"))});
    connect(nextTabAction, &QAction::triggered, this, [this] { activateAdjacentTab(1); });
    QAction* previousTabAction = viewMenu->addAction(QStringLiteral("Previous Tab"));
    previousTabAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+[")),
                                     QKeySequence(QStringLiteral("Ctrl+Shift+Tab"))});
    connect(previousTabAction, &QAction::triggered, this, [this] { activateAdjacentTab(-1); });
    viewMenu->addSeparator();
    QAction* zoomInAction = viewMenu->addAction(QStringLiteral("Bigger Text"));
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, this, [this] { changeFontSize(1); });
    QAction* zoomOutAction = viewMenu->addAction(QStringLiteral("Smaller Text"));
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, [this] { changeFontSize(-1); });
    QAction* actualSizeAction = viewMenu->addAction(QStringLiteral("Actual Size"));
    actualSizeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(actualSizeAction, &QAction::triggered, this, [this] {
        fontSizePixels_ = 14;
        lineHeightPixels_ = 22;
        applyTheme();
    });
    viewMenu->addSeparator();
    QAction* fullScreenAction = viewMenu->addAction(QStringLiteral("Enter Full Screen"));
    fullScreenAction->setShortcut(QKeySequence::FullScreen);
    connect(fullScreenAction, &QAction::triggered, this,
            [this] { isFullScreen() ? showNormal() : showFullScreen(); });

    QMenu* appearanceMenu = viewMenu->addMenu(QStringLiteral("Appearance"));
    auto* appearanceGroup = new QActionGroup(this);
    appearanceGroup->setExclusive(true);
    const auto addAppearance = [this, appearanceMenu, appearanceGroup](const QString& label,
                                                                       ThemeManager::Mode mode) {
        QAction* action = appearanceMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(theme_.mode() == mode);
        appearanceGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { theme_.setMode(mode); });
    };
    addAppearance(QStringLiteral("System"), ThemeManager::Mode::System);
    addAppearance(QStringLiteral("Light"), ThemeManager::Mode::Light);
    addAppearance(QStringLiteral("Dark"), ThemeManager::Mode::Dark);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("About KetPlus Terminal"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About KetPlus Terminal"),
            QStringLiteral("KetPlus Terminal %1\nA lightweight native terminal for macOS.")
                .arg(QCoreApplication::applicationVersion()));
    });
}

void TerminalWindow::addTab(const QString& workingDirectory, const QString& commandPath) {
    auto* tab = new TerminalTab(workingDirectory, commandPath, tabs_);
    tab->applyTheme(theme_.palette());
    tab->setTypography(fontSizePixels_, lineHeightPixels_);
    const int index = tabs_->addTab(tab, tab->title());
    tabs_->setCurrentIndex(index);
    workingDirectory_ = tab->workingDirectory();

    connect(tab, &TerminalTab::titleChanged, this, [this, tab](const QString& title) {
        const int tabIndex = tabs_->indexOf(tab);
        if (tabIndex >= 0) {
            tabs_->setTabText(tabIndex, title.left(32));
            tabs_->setTabToolTip(tabIndex, title);
        }
        if (tab == currentTab()) {
            updateWindowTitle();
        }
    });
    connect(tab, &TerminalTab::statusMessageRequested, this, [this](const QString& message) {
        statusBar()->show();
        statusBar()->showMessage(message, 5000);
    });
    tab->start();
    updateWindowTitle();
}

void TerminalWindow::closeTab(const int index) {
    if (index < 0 || index >= tabs_->count()) {
        return;
    }
    TerminalTab* tab = qobject_cast<TerminalTab*>(tabs_->widget(index));
    tabs_->removeTab(index);
    delete tab;
    if (tabs_->count() == 0) {
        close();
    }
}

void TerminalWindow::activateAdjacentTab(const int delta) {
    if (tabs_->count() < 2) {
        return;
    }
    const int next = (tabs_->currentIndex() + delta + tabs_->count()) % tabs_->count();
    tabs_->setCurrentIndex(next);
}

void TerminalWindow::applyTheme() {
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* tab = qobject_cast<TerminalTab*>(tabs_->widget(index))) {
            tab->applyTheme(theme_.palette());
            tab->setTypography(fontSizePixels_, lineHeightPixels_);
        }
    }
}

void TerminalWindow::restartSession() {
    if (TerminalTab* tab = currentTab(); tab != nullptr && !tab->restart()) {
        statusBar()->show();
        statusBar()->showMessage(QStringLiteral("Unable to restart the terminal session."), 5000);
    }
}

void TerminalWindow::changeFontSize(const int delta) {
    fontSizePixels_ = std::clamp(fontSizePixels_ + delta, 8, 48);
    lineHeightPixels_ = std::clamp(fontSizePixels_ + 8, 12, 96);
    applyTheme();
}

void TerminalWindow::updateWindowTitle() {
    const TerminalTab* tab = currentTab();
    const QString title = tab == nullptr
                              ? QStringLiteral("KetPlus Terminal")
                              : QStringLiteral("%1 — KetPlus Terminal").arg(tab->title());
    setWindowTitle(title);
}

TerminalTab* TerminalWindow::currentTab() const {
    return qobject_cast<TerminalTab*>(tabs_->currentWidget());
}

} // namespace ketplus
