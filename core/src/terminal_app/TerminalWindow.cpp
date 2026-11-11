#include "terminal_app/TerminalWindow.h"

#include "terminal/TerminalTab.h"
#include "terminal/TerminalView.h"
#include "ui/MessageDialog.h"
#include "ui/Theme.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "KetToast.h"
#include "KetTokens.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>

#include <array>
#include <utility>

namespace ketplus {

namespace {

constexpr auto appearanceKey = "appearance/theme";
constexpr auto fontFamilyKey = "terminal/fontFamily";
constexpr auto interfaceZoomKey = "appearance/interfaceZoom";

// The names ThemeManager saved, so an existing preference survives the move to the kit.
constexpr std::array<std::pair<const char*, ket::Appearance::Mode>, 3> appearanceModes{{
    {"system", ket::Appearance::Mode::System},
    {"light", ket::Appearance::Mode::Light},
    {"dark", ket::Appearance::Mode::Dark},
}};

QString token(QStringView name, const ket::Theme theme) {
    return ket::Tokens::color(name, theme).name(QColor::HexArgb);
}

// The roles TerminalView reads, from Két DS tokens: the grid on the page background, the cursor
// in the accent, matches in warning. The ANSI colours come from the tokens inside the view.
ThemePalette terminalPalette(const ket::Theme theme) {
    ThemePalette palette;
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

void restoreTerminalAppearance() {
    const qreal savedZoom = QSettings().value(QLatin1StringView(interfaceZoomKey), 1.0).toDouble();
    ket::Appearance::instance().setZoom(qBound(0.75, savedZoom, 2.0));
    const QString saved = QSettings().value(QLatin1StringView(appearanceKey)).toString();
    for (const auto& [name, mode] : appearanceModes) {
        if (saved == QLatin1StringView(name)) {
            ket::Appearance::instance().setMode(mode);
            return;
        }
    }
}

TerminalWindow::TerminalWindow(const QString& workingDirectory, const QString& commandPath,
                               QWidget* parent)
    : QMainWindow(parent), tabs_(new QTabWidget(this)),
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

    // The embedded panel's actions (core/src/terminal/TerminalPanel), beside the tabs.
    auto* actions = new TabActions(tabs_);
    actions->setObjectName(QStringLiteral("terminalTabActions"));
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
                            static_cast<QWidget*>(clear), static_cast<QWidget*>(restart)}) {
        actionsLayout->addWidget(button);
    }
    tabs_->setCornerWidget(actions, Qt::TopRightCorner);
    connect(newTab, &QAbstractButton::clicked, this, [this] { addTab(workingDirectory_); });
    connect(find, &QAbstractButton::clicked, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->openSearch();
        }
    });
    connect(clear, &QAbstractButton::clicked, this, [this] {
        if (TerminalTab* tab = currentTab()) {
            tab->view()->clearScrollback();
        }
    });
    connect(restart, &QAbstractButton::clicked, this, &TerminalWindow::restartSession);

    createMenus();

    const QByteArray savedGeometry =
        QSettings().value(QStringLiteral("window/geometry")).toByteArray();
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    }

    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &TerminalWindow::applyTheme);
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, actions,
            qOverload<>(&QWidget::update));
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
    const auto add = [this](QMenu* menu, const QString& text, const QKeySequence& shortcut,
                            auto&& slot) {
        QAction* action = menu->addAction(text);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    const auto onView = [this](auto method) {
        return [this, method] {
            if (TerminalTab* tab = currentTab()) {
                (tab->view()->*method)();
            }
        };
    };

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    add(fileMenu, QStringLiteral("New Window"), QKeySequence::New,
        [this] { emit newWindowRequested(workingDirectory_); });
    add(fileMenu, QStringLiteral("New Tab"), QKeySequence(QStringLiteral("Ctrl+T")),
        [this] { addTab(workingDirectory_); });
    add(fileMenu, QStringLiteral("Open Folder in New Tab…"), QKeySequence(QStringLiteral("Ctrl+O")),
        [this] {
            const QString path = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Open Terminal in Folder"), workingDirectory_);
            if (!path.isEmpty()) {
                addTab(path);
            }
        });
    fileMenu->addSeparator();
    add(fileMenu, QStringLiteral("Close Tab"), QKeySequence::Close,
        [this] { closeTab(tabs_->currentIndex()); });
    add(fileMenu, QStringLiteral("Close Window"), QKeySequence(QStringLiteral("Ctrl+Shift+W")),
        [this] { close(); });
    QAction* quitAction = add(fileMenu, QStringLiteral("Quit KétPlus Terminal"), QKeySequence::Quit,
                              [] { QApplication::closeAllWindows(); });
    quitAction->setMenuRole(QAction::QuitRole);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    add(editMenu, QStringLiteral("Copy"), QKeySequence::Copy, onView(&TerminalView::copySelection));
    add(editMenu, QStringLiteral("Paste"), QKeySequence::Paste,
        onView(&TerminalView::pasteClipboard));
    add(editMenu, QStringLiteral("Select All"), QKeySequence::SelectAll,
        onView(&TerminalView::selectAll));
    editMenu->addSeparator();
    add(editMenu, QStringLiteral("Find…"), QKeySequence::Find, onView(&TerminalView::openSearch));

    QMenu* shellMenu = menuBar()->addMenu(QStringLiteral("&Shell"));
    add(shellMenu, QStringLiteral("Restart Session"), QKeySequence(QStringLiteral("Ctrl+Shift+R")),
        [this] { restartSession(); });
    add(shellMenu, QStringLiteral("Clear Scrollback"), QKeySequence(QStringLiteral("Ctrl+K")),
        onView(&TerminalView::clearScrollback));

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* nextTabAction = add(viewMenu, QStringLiteral("Next Tab"), QKeySequence(),
                                 [this] { activateAdjacentTab(1); });
    nextTabAction->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Shift+]")), QKeySequence(QStringLiteral("Ctrl+Tab"))});
    QAction* previousTabAction = add(viewMenu, QStringLiteral("Previous Tab"), QKeySequence(),
                                     [this] { activateAdjacentTab(-1); });
    previousTabAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+[")),
                                     QKeySequence(QStringLiteral("Ctrl+Shift+Tab"))});
    viewMenu->addSeparator();
    add(viewMenu, QStringLiteral("Bigger Text"), QKeySequence::ZoomIn,
        [this] { changeFontSize(1); });
    add(viewMenu, QStringLiteral("Smaller Text"), QKeySequence::ZoomOut,
        [this] { changeFontSize(-1); });
    add(viewMenu, QStringLiteral("Actual Size"), QKeySequence(QStringLiteral("Ctrl+0")), [this] {
        fontSizePixels_ = 14;
        lineHeightPixels_ = 22;
        applyTheme();
    });
    viewMenu->addSeparator();
    add(viewMenu, QStringLiteral("Enter Full Screen"), QKeySequence::FullScreen,
        [this] { isFullScreen() ? showNormal() : showFullScreen(); });

    QMenu* zoomMenu = viewMenu->addMenu(QStringLiteral("Interface Zoom"));
    auto* zoomGroup = new QActionGroup(this);
    zoomGroup->setExclusive(true);
    for (int percent : {75, 100, 125, 150, 200}) {
        QAction* zoom = zoomMenu->addAction(QStringLiteral("%1%").arg(percent));
        zoom->setObjectName(QStringLiteral("terminalInterfaceZoom%1").arg(percent));
        zoom->setCheckable(true);
        zoomGroup->addAction(zoom);
        const qreal factor = percent / 100.0;
        const auto synchronize = [zoom, factor] {
            zoom->setChecked(qFuzzyCompare(ket::Appearance::instance().zoom(), factor));
        };
        synchronize();
        connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, zoom, synchronize);
        connect(zoom, &QAction::triggered, this, [factor] {
            QSettings().setValue(QLatin1StringView(interfaceZoomKey), factor);
            ket::Appearance::instance().setZoom(factor);
        });
    }

    QMenu* appearanceMenu = viewMenu->addMenu(QStringLiteral("Appearance"));
    auto* appearanceGroup = new QActionGroup(this);
    appearanceGroup->setExclusive(true);
    for (const auto& [name, mode] : appearanceModes) {
        QString label = QString::fromLatin1(name);
        label[0] = label[0].toUpper();
        QAction* action = appearanceMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(ket::Appearance::instance().mode() == mode);
        appearanceGroup->addAction(action);
        connect(action, &QAction::triggered, this, [name, mode] {
            ket::Appearance::instance().setMode(mode);
            QSettings().setValue(QLatin1StringView(appearanceKey), QLatin1StringView(name));
        });
    }

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("About KétPlus Terminal"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, [this] {
        message_dialog::information(
            this, QStringLiteral("About KétPlus Terminal"),
            QStringLiteral("KétPlus Terminal %1\n\nA lightweight native terminal for macOS.\n\n"
                           "Icons: Két Symbol © 2026 Két Việt, CC BY 4.0.")
                .arg(QCoreApplication::applicationVersion()));
    });
}

void TerminalWindow::addTab(const QString& workingDirectory, const QString& commandPath) {
    auto* tab = new TerminalTab(workingDirectory, commandPath, tabs_);
    tab->applyTheme(terminalPalette(ket::Appearance::instance().theme()));
    applyTypography(tab);
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
    connect(tab, &TerminalTab::statusMessageRequested, this, &TerminalWindow::showStatus);
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
    const ThemePalette palette = terminalPalette(ket::Appearance::instance().theme());
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* tab = qobject_cast<TerminalTab*>(tabs_->widget(index))) {
            tab->applyTheme(palette);
            applyTypography(tab);
        }
    }
}

// The family saved under terminal/fontFamily, else the profile's monospace (KÉT Sans Mono when
// it is installed, then the platform's).
void TerminalWindow::applyTypography(TerminalTab* tab) const {
    QString family = QSettings().value(QLatin1StringView(fontFamilyKey)).toString().trimmed();
    if (family.isEmpty()) {
        family = ket::Profile::current().monoFamily();
    }
    tab->setTypography(fontSizePixels_, lineHeightPixels_, family);
}

void TerminalWindow::restartSession() {
    if (TerminalTab* tab = currentTab(); tab != nullptr && !tab->restart()) {
        showStatus(QStringLiteral("Unable to restart the terminal session."));
    }
}

void TerminalWindow::changeFontSize(const int delta) {
    fontSizePixels_ = qBound(10, fontSizePixels_ + delta, 28);
    lineHeightPixels_ = qRound(fontSizePixels_ * 22.0 / 14.0);
    applyTheme();
    showStatus(QStringLiteral("Text size %1 px").arg(fontSizePixels_));
}

void TerminalWindow::showStatus(const QString& message) { ket::Toast::showMessage(this, message); }

void TerminalWindow::updateWindowTitle() {
    const TerminalTab* tab = currentTab();
    const QString title = tab == nullptr
                              ? QStringLiteral("KétPlus Terminal")
                              : QStringLiteral("%1 — KétPlus Terminal").arg(tab->title());
    setWindowTitle(title);
}

TerminalTab* TerminalWindow::currentTab() const {
    return qobject_cast<TerminalTab*>(tabs_->currentWidget());
}

} // namespace ketplus
