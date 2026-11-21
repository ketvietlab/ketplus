#include "app/MainWindow.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetIcons.h"
#include "KetRecipes.h"
#include "KetToast.h"
#include "KetTokens.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspace.h"
#include <QStatusBar>

#include <QAbstractButton>
#include <QAction>
#include <QHBoxLayout>
#include <QPainter>

#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <functional>

namespace ketplus {
namespace {

// An activity bar entry as the mock's ActivityButton: a 48 × 40 cell with an icon button in
// the middle, filled like a selected nav item and marked by an accent rule at the left edge.
class ActivityButton final : public QAbstractButton {
  public:
    // `selected` says whether this entry is on screen; it is read at each paint.
    ActivityButton(QStringView icon, const QString& label, std::function<bool()> selected,
                   QWidget* parent)
        : QAbstractButton(parent), icon_(ket::Icons::codeForLucide(icon)),
          selected_(std::move(selected)) {
        setAccessibleName(label);
        setToolTip(label);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        // Checkable, so assistive technology reads the entry on screen as selected.
        setCheckable(true);
        sync();
    }
    QSize sizeHint() const override {
        auto& kit = ket::Appearance::instance();
        return {qRound(kit.scaled(48)), qRound(kit.scaled(40))};
    }
    // The actions are set under signal blockers, so the owner calls this after each change.
    void sync() {
        setChecked(selected_());
        update();
    }

  protected:
    // `selected` decides; a click only triggers the entry.
    void nextCheckState() override {}
    void paintEvent(QPaintEvent*) override {
        auto& kit = ket::Appearance::instance();
        const auto theme = kit.theme();
        // Tab focus only, so the ring marks keyboard focus.
        const ket::InteractionState state{underMouse(), isDown(), isEnabled(), hasFocus()};
        const bool selected = isChecked();
        const auto nav = ket::navItemRecipe(selected, state, theme);
        const auto button = ket::iconButtonRecipe(ket::ControlSize::Medium, state, theme);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(nav.opacity);
        if (selected)
            painter.fillRect(QRect(0, 0, 2, height()), ket::Tokens::color(u"accent", theme));
        const int side = qRound(kit.scaled(button.height));
        const QRectF face((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        painter.setPen(Qt::NoPen);
        painter.setBrush(nav.background);
        painter.drawRoundedRect(face, kit.scaled(nav.radius), kit.scaled(nav.radius));
        if (nav.focusRing.alpha()) {
            painter.setPen(QPen(nav.focusRing, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(face.adjusted(1, 1, -1, -1), kit.scaled(nav.radius),
                                    kit.scaled(nav.radius));
        }
        const int icon = qRound(kit.scaled(nav.iconSize));
        painter.drawPixmap(QPointF(face.center().x() - icon / 2.0, face.center().y() - icon / 2.0),
                           ket::Icons::pixmap(icon_, icon, devicePixelRatioF(), nav.text));
    }

  private:
    QString icon_;
    std::function<bool()> selected_;
};

// The tab strip's end, as the mock: the strip's fill and rules around the terminal toggle.
class TabStripCorner final : public QWidget {
  public:
    explicit TabStripCorner(QWidget* button, QWidget* parent) : QWidget(parent) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 0, 4, 0);
        layout->addWidget(button, 0, Qt::AlignCenter);
    }
    QSize sizeHint() const override {
        auto& kit = ket::Appearance::instance();
        return {QWidget::sizeHint().width(),
                qRound(kit.scaled(ket::tabBarRecipe(kit.theme()).height))};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto strip = ket::tabBarRecipe(ket::Appearance::instance().theme());
        QPainter painter(this);
        painter.fillRect(rect(), strip.background);
        painter.fillRect(QRect(0, 0, 1, height() - 1), strip.border);
        painter.fillRect(QRect(0, height() - 1, width(), 1), strip.border);
    }
};

} // namespace

void MainWindow::createChrome() {
    auto* shell = new QWidget(this);
    auto* row = new QHBoxLayout(shell);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* rail = new QWidget(shell);
    rail->setObjectName(QStringLiteral("liteActivityBar"));
    auto* navigation = new QVBoxLayout(rail);
    const auto add = [rail, navigation](QStringView name, const QString& label,
                                        std::function<bool()> selected) {
        auto* button = new ActivityButton(name, label, std::move(selected), rail);
        button->setFocusPolicy(Qt::TabFocus);
        navigation->addWidget(button);
        return button;
    };
    const auto linkAction = [this, &add](QStringView name, QAction* action) {
        // A side panel is on screen only with the editor surface, not over Settings.
        auto* button = add(name, action->text().remove(QLatin1Char('&')), [this, action] {
            return action->isChecked() && chromePages_->currentWidget() == workspaceSplit_;
        });
        const auto update = [button, action] {
            button->sync();
            button->setEnabled(action->isEnabled());
            button->setToolTip(action->text().remove(QLatin1Char('&')) + QStringLiteral(" (") +
                               action->shortcut().toString(QKeySequence::NativeText) + ')');
        };
        connect(action, &QAction::changed, button, update);
        connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
        update();
    };
    linkAction(u"folder", explorerAction_);
    linkAction(u"search", searchPanelAction_);
    linkAction(u"git-branch", sourceControlAction_);
    navigation->addStretch();
    auto* settings = add(u"settings", tr("Settings"), [this] {
        return settingsPage_ != nullptr && chromePages_->currentWidget() == settingsPage_;
    });
    connect(settings, &QAbstractButton::clicked, this, &MainWindow::showSettings);
    const auto appearance = [rail, navigation] {
        auto& kit = ket::Appearance::instance();
        const auto px = [&kit](int n) { return qRound(kit.scaled(n)); };
        rail->setFixedWidth(px(48));
        navigation->setContentsMargins(0, px(8), 0, px(8));
        navigation->setSpacing(px(4));
        for (auto* button : rail->findChildren<QAbstractButton*>()) {
            button->updateGeometry();
            button->update();
        }
        rail->setStyleSheet(
            QStringLiteral(
                "QWidget#liteActivityBar { background: %1; border-right: 1px solid %2; }")
                .arg(ket::Tokens::color(u"sidebar-bg", kit.theme()).name(QColor::HexArgb),
                     ket::Tokens::color(u"sidebar-border", kit.theme()).name(QColor::HexArgb)));
    };
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, rail, appearance);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, rail, appearance);
    appearance();
    row->addWidget(rail);
    activityBar_ = rail;
    chromePages_ = new QStackedWidget(shell);
    chromePages_->setObjectName(QStringLiteral("liteWorkspacePages"));
    chromePages_->addWidget(workspaceSplit_);
    auto* welcome = new ket::EmptyState(
        ket::Icons::codeForLucide(u"folder"), tr("Welcome to ketPlus"),
        tr("Open a folder to start working, or create a new file."), chromePages_);
    welcome->setObjectName(QStringLiteral("liteWelcome"));
    welcome->setAttribute(Qt::WA_StyledBackground);
    connect(welcome->setAction(tr("Open Folder")), &QAbstractButton::clicked, this,
            &MainWindow::chooseFolder);
    chromePages_->addWidget(welcome);
    chromePages_->setCurrentWidget(welcome);
    row->addWidget(chromePages_, 1);
    // The mock's terminal toggle at the end of the tab strip.
    auto* terminalToggle = new ket::IconButton(
        ket::Icons::icon(ket::Icons::codeForLucide(u"terminal")), tr("Toggle terminal"), tabs_);
    connect(terminalToggle, &QAbstractButton::clicked, terminalAction_, &QAction::trigger);
    auto* corner = new TabStripCorner(terminalToggle, tabs_);
    corner->setObjectName(QStringLiteral("tabStripCorner"));
    tabs_->setCornerWidget(corner, Qt::TopRightCorner);
    setCentralWidget(shell);
    // An open side panel keeps its width in logical pixels when the interface zoom changes,
    // including a width the user dragged; the editor column takes the difference.
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            [this, previous = ket::Appearance::instance().zoom()](const qreal zoom) mutable {
                const qreal factor = zoom / previous;
                previous = zoom;
                auto sizes = workspaceSplit_->sizes();
                const int editor = workspaceSplit_->indexOf(editorColumn_);
                for (int index = 0; index < sizes.size(); ++index) {
                    if (index == editor || sizes[index] == 0 ||
                        workspaceSplit_->widget(index)->isHidden())
                        continue;
                    const int width = qRound(sizes[index] * factor);
                    sizes[editor] = qMax(0, sizes[editor] - (width - sizes[index]));
                    sizes[index] = width;
                }
                workspaceSplit_->setSizes(sizes);
            });
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString& message) {
        if (!message.isEmpty())
            ket::Toast::showMessage(this, message);
    });
}

void MainWindow::refreshActivityBar() {
    if (activityBar_ == nullptr)
        return;
    for (auto* button : activityBar_->findChildren<QAbstractButton*>())
        static_cast<ActivityButton*>(button)->sync();
}

bool MainWindow::panelStaysLeavingSettings(QAction* action, const bool visible) {
    if (visible || settingsPage_ == nullptr || chromePages_->currentWidget() != settingsPage_)
        return visible;
    const QSignalBlocker blocker(action);
    action->setChecked(true);
    return true;
}

void MainWindow::showEditorSurface() {
    if (chromePages_)
        chromePages_->setCurrentWidget(workspaceSplit_);
    // The status bar belongs to the editor surface, as in the mock.
    statusBar()->setVisible(!distractionFree_);
    refreshActivityBar();
}

// Settings as a page of the window, as the Lite mock; changes apply as they are made.
void MainWindow::showSettings() {
    if (settingsPage_ == nullptr) {
        auto* dialog = new SettingsDialog(appearanceSettings_, theme_, this);
        connect(dialog, &SettingsDialog::appearanceSettingsSaved, this,
                &MainWindow::applyAppearanceSettings);
#ifdef Q_OS_MACOS
        const QString shortcut = QStringLiteral("⌘ ,");
#else
        const QString shortcut = QStringLiteral("Ctrl ,");
#endif
        auto* page =
            new SettingsWorkspace(dialog, shortcut, QStringLiteral("KÉT Sans"), chromePages_);
        connect(page, &SettingsWorkspace::dismissed, this, &MainWindow::showEditorSurface);
        chromePages_->addWidget(page);
        settingsPage_ = page;
    }
    chromePages_->setCurrentWidget(settingsPage_);
    statusBar()->hide();
    refreshActivityBar();
}

} // namespace ketplus
