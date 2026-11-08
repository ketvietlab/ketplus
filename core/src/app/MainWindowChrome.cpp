#include "app/MainWindow.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetIcons.h"
#include "KetTokens.h"

#include <QAction>
#include <QHBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ketplus {

void MainWindow::createChrome() {
    auto* shell = new QWidget(this);
    auto* row = new QHBoxLayout(shell);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* rail = new QWidget(shell);
    rail->setObjectName(QStringLiteral("liteActivityBar"));
    auto* navigation = new QVBoxLayout(rail);
    navigation->setSpacing(4);
    navigation->setContentsMargins(4, 8, 4, 8);
    const auto add = [rail, navigation](QStringView name, const QString& label) {
        auto* button =
            new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(name)), label, rail);
        navigation->addWidget(button, 0, Qt::AlignHCenter);
        return button;
    };
    const auto linkAction = [this, &add](QStringView name, QAction* action) {
        auto* button = add(name, action->text().remove(QLatin1Char('&')));
        button->setCheckable(true);
        const auto update = [button, action] {
            button->setChecked(action->isChecked());
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
    auto* settings = add(u"settings", tr("Settings"));
    connect(settings, &QAbstractButton::clicked, this, &MainWindow::showSettings);
    const auto appearance = [rail, navigation] {
        auto& kit = ket::Appearance::instance();
        const auto px = [&kit](int n) { return qRound(kit.scaled(n)); };
        rail->setFixedWidth(px(48));
        navigation->setContentsMargins(px(4), px(8), px(4), px(8));
        navigation->setSpacing(px(4));
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
    chromePages_ = new QStackedWidget(shell);
    chromePages_->addWidget(mainSplit_);
    auto* welcome = new ket::EmptyState(
        ket::Icons::codeForLucide(u"folder"), tr("Welcome to KétPlus"),
        tr("Open a folder to start working, or create a new file."), chromePages_);
    welcome->setObjectName(QStringLiteral("liteWelcome"));
    connect(welcome->setAction(tr("Open Folder")), &QAbstractButton::clicked, this,
            &MainWindow::chooseFolder);
    chromePages_->addWidget(welcome);
    chromePages_->setCurrentWidget(welcome);
    row->addWidget(chromePages_, 1);
    setCentralWidget(shell);
}

void MainWindow::showEditorSurface() {
    if (chromePages_)
        chromePages_->setCurrentWidget(mainSplit_);
}

} // namespace ketplus
