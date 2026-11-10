#include "workspace/ExplorerPanel.h"
#include "KetBanner.h"
#include "KetConversationRow.h"
#include "KetProfile.h"
#include "workspace/ActivityBar.h"
#include "workspace/SymbolIcons.h"

#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <algorithm>

class ExplorerPanelTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsOnlyAfterReceivingARoot();
    void opensFilesFromTheTree();
    void togglesFoldersFromTheTree();
    void switchesActivityModes();
    void collapsesAndPreviewsRepositoryConversations();
    void conversationStatusMarksUnreadCompletionAndRotatesWorkingIcon();
    void runningAgentTurnNamesItsSender();
    void ordersRepositoryConversationsByLatestTurnAcrossWorktrees();
    void ordersRepositoriesByTheirNewestConversation();
    void scalesSidebarTypeWithInterfaceZoom();
    void filtersTagsAndUsesAuthoritativeStorageWarning();
};

void ExplorerPanelTest::loadsOnlyAfterReceivingARoot() {
    ketplus::ExplorerPanel explorer;
    QCOMPARE(explorer.rootPath(), QString());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    QFile file(directory.filePath(QStringLiteral("readme.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("KetPlus"), 7);
    file.close();

    explorer.setRootPath(directory.path());
    QCOMPARE(explorer.rootPath(), directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->rowCount(tree->rootIndex()) >= 2, 3000);
    QVERIFY(model->index(file.fileName()).isValid());
}

void ExplorerPanelTest::opensFilesFromTheTree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("main.cpp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    ketplus::ExplorerPanel explorer;
    explorer.resize(280, 500);
    explorer.show();
    explorer.setRootPath(directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->index(path).isValid(), 3000);

    const QModelIndex index = model->index(path);
    QSignalSpy opened(&explorer, &ketplus::ExplorerPanel::fileOpenRequested);
    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.constFirst().constFirst().toString(), path);
}

void ExplorerPanelTest::togglesFoldersFromTheTree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    const QString path = directory.filePath(QStringLiteral("nested"));

    ketplus::ExplorerPanel explorer;
    explorer.setRootPath(directory.path());

    auto* tree = explorer.findChild<QTreeView*>(QStringLiteral("explorerTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(model->index(path).isValid(), 3000);

    const QModelIndex index = model->index(path);
    QSignalSpy opened(&explorer, &ketplus::ExplorerPanel::fileOpenRequested);
    QVERIFY(!tree->isExpanded(index));

    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));
    QVERIFY(tree->isExpanded(index));
    QCOMPARE(opened.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                      Q_ARG(QModelIndex, index)));
    QVERIFY(!tree->isExpanded(index));
    QCOMPARE(opened.count(), 0);
}

void ExplorerPanelTest::switchesActivityModes() {
    ketplus::ActivityBar bar;
    QSignalSpy requested(&bar, &ketplus::ActivityBar::modeRequested);
    QVERIFY(bar.findChild<QToolButton*>(QStringLiteral("atlasActivityButton")) == nullptr);
    auto* agent = bar.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
    auto* source = bar.findChild<QAbstractButton*>(QStringLiteral("sourceControlActivityButton"));
    auto* remote = bar.findChild<QAbstractButton*>(QStringLiteral("remoteActivityButton"));
    QVERIFY(agent != nullptr);
    QVERIFY(source != nullptr);
    QVERIFY(remote != nullptr);

    agent->click();
    QCOMPARE(bar.mode(), ketplus::ActivityBar::Mode::AgentWorkspace);
    QCOMPARE(requested.count(), 1);
    remote->click();
    QCOMPARE(bar.mode(), ketplus::ActivityBar::Mode::Remote);
    QVERIFY(remote->isChecked());
    QVERIFY(!agent->isChecked());
    QCOMPARE(requested.count(), 2);

    bar.setSourceControlAvailable(false);
    QVERIFY(!source->isEnabled());
    bar.setSourceControlAvailable(true);
    source->click();
    QCOMPARE(bar.mode(), ketplus::ActivityBar::Mode::SourceControl);
    QCOMPARE(requested.count(), 3);
    remote->click();
    QCOMPARE(requested.count(), 4);
    QCOMPARE(bar.mode(), ketplus::ActivityBar::Mode::Remote);
    QVERIFY(!source->isChecked());

    // Repository > conversation; each conversation names the worktree it runs in.
    const QVariantList conversations = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("thread-1")},
                    {QStringLiteral("title"), QStringLiteral("Refine navigation")},
                    {QStringLiteral("status"), QStringLiteral("Completed")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("selected"), true},
                    {QStringLiteral("lastTurnStartedAt"), 20},
                    {QStringLiteral("path"), QStringLiteral("/repo/develop")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("thread-2")},
                    {QStringLiteral("title"), QStringLiteral("Review startup")},
                    {QStringLiteral("status"), QStringLiteral("Reasoning")},
                    {QStringLiteral("running"), true},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("lastTurnStartedAt"), 10},
                    {QStringLiteral("path"), QStringLiteral("/repo/feature")}}};
    QSignalSpy conversationRequested(&bar, &ketplus::ActivityBar::conversationRequested);
    QSignalSpy conversationMenuRequested(&bar,
                                         &ketplus::ActivityBar::conversationContextMenuRequested);
    QSignalSpy repositoryActivation(&bar, &ketplus::ActivityBar::repositoryActivationRequested);
    QSignalSpy newConversation(&bar, &ketplus::ActivityBar::newConversationRequested);
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, conversations},
                         {QStringLiteral("/other"), QStringLiteral("other"), false, {}}});
    QVERIFY(bar.findChildren<QToolButton*>(QStringLiteral("productWorktreeButton")).isEmpty());
    const auto groups =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productRepositoryGroupButton"));
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.at(0)->accessibleName(), QStringLiteral("repo"));
    QVERIFY(groups.at(0)->property("activeRepository").toBool());
    const auto rows =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton"));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0)->height(), rows.at(0)->sizeHint().height());
    QVERIFY(rows.at(0)->isChecked());
    QVERIFY(!rows.at(1)->isChecked());
    QCOMPARE(rows.at(0)->accessibleDescription(), QStringLiteral("Completed"));
    QCOMPARE(rows.at(1)->accessibleDescription(), QStringLiteral("Working"));
    QCOMPARE(rows.at(1)->toolTip(), QStringLiteral("Review startup\nfeature"));

    rows.at(1)->click();
    QCOMPARE(conversationRequested.count(), 1);
    QCOMPARE(conversationRequested.constFirst().at(0).toString(), QStringLiteral("/repo/feature"));
    QCOMPARE(conversationRequested.constFirst().at(1).toString(), QStringLiteral("thread-2"));
    QVERIFY(!rows.at(1)->isChecked());
    QCOMPARE(rows.at(0)->contextMenuPolicy(), Qt::CustomContextMenu);
    QVERIFY(QMetaObject::invokeMethod(rows.at(0), "customContextMenuRequested",
                                      Q_ARG(QPoint, QPoint(4, 4))));
    QCOMPARE(conversationMenuRequested.count(), 1);
    QCOMPARE(conversationMenuRequested.constFirst().at(0).toString(),
             QStringLiteral("/repo/develop"));
    QCOMPARE(conversationMenuRequested.constFirst().at(1).toString(), QStringLiteral("thread-1"));

    // Repositories without conversations open their folder; there is no collapse icon.
    QVERIFY(bar.findChildren<QLabel*>(QStringLiteral("productRepositoryCollapsedIcon")).isEmpty());
    groups.at(1)->click();
    QCOMPARE(repositoryActivation.count(), 1);
    QCOMPARE(repositoryActivation.constFirst().constFirst().toString(), QStringLiteral("/other"));

    // Every repository offers a new conversation on hover and names itself in the request.
    const auto create =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productNewConversationButton"));
    QCOMPARE(create.size(), 2);
    QVERIFY(create.constFirst()->isHidden());
    create.at(1)->click();
    QCOMPARE(newConversation.count(), 1);
    QCOMPARE(newConversation.constFirst().constFirst().toString(), QStringLiteral("/other"));

    // Double-clicking a conversation asks to rename that conversation in its own worktree.
    QSignalSpy renameRequested(&bar, &ketplus::ActivityBar::conversationRenameRequested);
    QTest::mouseDClick(rows.at(1), Qt::LeftButton);
    QCOMPARE(renameRequested.count(), 1);
    QCOMPARE(renameRequested.constFirst().at(0).toString(), QStringLiteral("/repo/feature"));
    QCOMPARE(renameRequested.constFirst().at(1).toString(), QStringLiteral("thread-2"));

    // Quota is gone from the sidebar; Settings lives in the user menu.
    QVERIFY(bar.findChild<QProgressBar*>(QStringLiteral("productQuotaProgress")) == nullptr);
    QVERIFY(bar.findChild<QToolButton*>(QStringLiteral("productSettingsButton")) == nullptr);
    QSignalSpy settingsRequested(&bar, &ketplus::ActivityBar::settingsRequested);
    auto* settingsAction = bar.findChild<QAction*>(QStringLiteral("productSettingsAction"));
    QVERIFY(settingsAction);
    settingsAction->trigger();
    QCOMPARE(settingsRequested.count(), 1);

    // The sidebar is resizable within its bounds.
    QSignalSpy widthChanged(&bar, &ketplus::ActivityBar::sidebarWidthChanged);
    bar.setSidebarWidth(1000);
    QCOMPARE(bar.width(), ketplus::ActivityBar::kMaxWidth);
    bar.setSidebarWidth(10);
    QCOMPARE(bar.width(), ketplus::ActivityBar::kMinWidth);
    QCOMPARE(widthChanged.count(), 2);

    // Interface zoom scales the shown width and keeps the unscaled choice.
    ketplus::ThemeManager theme(*qApp);
    bar.setSidebarWidth(ketplus::ActivityBar::kWidth);
    widthChanged.clear();
    const int large = 125;
    bar.applyPalette(theme.palette(), large);
    const int scaled = ketplus::InterfaceZoomMetrics::geometry(ketplus::ActivityBar::kWidth, large);
    QVERIFY(scaled > ketplus::ActivityBar::kWidth);
    QCOMPARE(bar.width(), scaled);
    QCOMPARE(bar.sidebarWidth(), ketplus::ActivityBar::kWidth);
    QCOMPARE(widthChanged.count(), 1);
    QCOMPARE(widthChanged.last().first().toInt(), scaled);
    bar.applyPalette(theme.palette(), ketplus::InterfaceZoomMetrics::defaultPercent);
    QCOMPARE(bar.width(), ketplus::ActivityBar::kWidth);
}

void ExplorerPanelTest::collapsesAndPreviewsRepositoryConversations() {
    QTemporaryDir settings;
    QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    QVariantList conversations;
    for (int index = 0; index < 7; ++index) {
        conversations.append(QVariantMap{{"id", QStringLiteral("thread-%1").arg(index)},
                                         {"title", QStringLiteral("Conversation %1").arg(index)},
                                         {"status", QStringLiteral("Completed")},
                                         {"started", true},
                                         {"selected", index == 6},
                                         {"running", index == 5},
                                         {"lastTurnStartedAt", 100 - index},
                                         {"path", QStringLiteral("/repo/develop")}});
    }
    ketplus::ActivityBar bar;
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, conversations},
                         {QStringLiteral("/other"), QStringLiteral("other"), false, {}}});
    const auto rows =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton"));
    QCOMPARE(rows.size(), 7);
    const auto visibleIds = [&rows] {
        QSet<QString> ids;
        for (auto* row : rows) {
            if (!row->isHidden())
                ids.insert(row->property("conversationId").toString());
        }
        return ids;
    };
    QCOMPARE(visibleIds().size(), 5);
    QVERIFY(visibleIds().contains(QStringLiteral("thread-0")));
    QVERIFY(visibleIds().contains(QStringLiteral("thread-5")));
    QVERIFY(visibleIds().contains(QStringLiteral("thread-6")));
    QVERIFY(!visibleIds().contains(QStringLiteral("thread-3")));
    QVERIFY(!visibleIds().contains(QStringLiteral("thread-4")));

    auto* showMore = bar.findChild<QToolButton*>(QStringLiteral("productShowMoreConversations"));
    QVERIFY(showMore);
    QVERIFY(!showMore->isHidden());
    showMore->click();
    QCOMPARE(visibleIds().size(), 7);
    QVERIFY(showMore->isHidden());

    const auto groups =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productRepositoryGroupButton"));
    QCOMPARE(groups.size(), 2);
    QSignalSpy activation(&bar, &ketplus::ActivityBar::repositoryActivationRequested);
    groups.at(0)->click();
    QCOMPARE(activation.count(), 0);
    QCOMPARE(visibleIds().size(), 0);
    QCOMPARE(groups.at(0)->accessibleDescription(), QStringLiteral("Collapsed"));
    QCOMPARE(QSettings().value(QStringLiteral("sidebar/collapsedRepositories")).toStringList(),
             QStringList{QStringLiteral("/repo")});
    groups.at(0)->click();
    QCOMPARE(activation.count(), 0);
    QCOMPARE(visibleIds().size(), 5);
    QCOMPARE(groups.at(0)->accessibleDescription(), QStringLiteral("Expanded"));
    groups.at(1)->click();
    QCOMPARE(activation.count(), 1);
    QCOMPARE(activation.constFirst().constFirst().toString(), QStringLiteral("/other"));
}

void ExplorerPanelTest::conversationStatusMarksUnreadCompletionAndRotatesWorkingIcon() {
    ketplus::ActivityBar bar;
    const QVariantList conversations{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("seen")},
                    {QStringLiteral("title"), QStringLiteral("Already read")},
                    {QStringLiteral("status"), QStringLiteral("Completed")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("seen"), true},
                    {QStringLiteral("selected"), true},
                    {QStringLiteral("path"), QStringLiteral("/repo/develop")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("persisted")},
                    {QStringLiteral("title"), QStringLiteral("Read before relaunch")},
                    {QStringLiteral("status"), QStringLiteral("Completed")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("seen"), true},
                    {QStringLiteral("path"), QStringLiteral("/repo/persisted")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("unread")},
                    {QStringLiteral("title"), QStringLiteral("Background result")},
                    {QStringLiteral("status"), QStringLiteral("Completed")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("seen"), false},
                    {QStringLiteral("path"), QStringLiteral("/repo/feature")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("working")},
                    {QStringLiteral("title"), QStringLiteral("In progress")},
                    {QStringLiteral("status"), QStringLiteral("Working")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("running"), true},
                    {QStringLiteral("path"), QStringLiteral("/repo/topic")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("waiting")},
                    {QStringLiteral("title"), QStringLiteral("Awaiting reply")},
                    {QStringLiteral("status"), QStringLiteral("Waiting for your answer…")},
                    {QStringLiteral("started"), true},
                    {QStringLiteral("running"), true},
                    {QStringLiteral("path"), QStringLiteral("/repo/waiting")}}};
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, conversations}});

    const auto rows =
        bar.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton"));
    const auto statusFor = [&rows](const QString& id) {
        const auto row = std::find_if(rows.cbegin(), rows.cend(), [&id](QAbstractButton* button) {
            return button->property("conversationId").toString() == id;
        });
        return row == rows.cend() ? nullptr : qobject_cast<ket::ConversationRow*>(*row);
    };
    auto* seen = statusFor(QStringLiteral("seen"));
    auto* persisted = statusFor(QStringLiteral("persisted"));
    auto* unread = statusFor(QStringLiteral("unread"));
    auto* working = statusFor(QStringLiteral("working"));
    auto* waiting = statusFor(QStringLiteral("waiting"));
    QVERIFY(seen);
    QVERIFY(persisted);
    QVERIFY(unread);
    QVERIFY(working);
    QVERIFY(waiting);
    QVERIFY(!seen->property("unread").toBool());
    QVERIFY(!persisted->property("unread").toBool());
    QVERIFY(unread->property("unread").toBool());
    QCOMPARE(working->status(), ket::ConversationStatus::Working);
    QCOMPARE(waiting->status(), ket::ConversationStatus::NeedsInput);
    const auto unreadRow = std::find_if(rows.cbegin(), rows.cend(), [](QAbstractButton* button) {
        return button->property("conversationId").toString() == QStringLiteral("unread");
    });
    QVERIFY(unreadRow != rows.cend());
    (*unreadRow)->click();
    QVERIFY(!unread->property("unread").toBool());

    // A later daemon projection can clear the read marker after a background turn.
    QVariantList refreshed = conversations;
    refreshed[2] = QVariantMap{{QStringLiteral("id"), QStringLiteral("unread")},
                               {QStringLiteral("title"), QStringLiteral("Background result")},
                               {QStringLiteral("status"), QStringLiteral("Completed")},
                               {QStringLiteral("started"), true},
                               {QStringLiteral("seen"), false},
                               {QStringLiteral("path"), QStringLiteral("/repo/feature")}};
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, refreshed}});
    QVERIFY(statusFor(QStringLiteral("unread"))->property("unread").toBool());
}

void ExplorerPanelTest::runningAgentTurnNamesItsSender() {
    ketplus::ActivityBar bar;
    QVariantMap woken{{QStringLiteral("id"), QStringLiteral("woken")},
                      {QStringLiteral("title"), QStringLiteral("Sidebar")},
                      {QStringLiteral("number"), QStringLiteral("1093")},
                      {QStringLiteral("agentTurnNumber"), QStringLiteral("4821")},
                      {QStringLiteral("agentTurnTitle"), QStringLiteral("Release checklist")},
                      {QStringLiteral("started"), true},
                      {QStringLiteral("running"), true},
                      {QStringLiteral("path"), QStringLiteral("/repo/sidebar")}};
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, {woken}}});
    const auto row = [&bar] {
        return bar.findChild<QAbstractButton*>(QStringLiteral("productConversationButton"));
    };
    QVERIFY(row());
    auto* conversation = qobject_cast<ket::ConversationRow*>(row());
    QVERIFY(conversation);
    QCOMPARE(conversation->number(), QStringLiteral("1093"));
    ketplus::ThemeManager theme(*qApp);
    bar.applyPalette(theme.palette());
    bar.resize(bar.width(), 400);
    bar.show();
    const auto capture = qEnvironmentVariable("KETPLUS_SIDEBAR_CAPTURE");
    if (!capture.isEmpty())
        QVERIFY(bar.grab().save(capture));
    QVERIFY(conversation->statusLabel().contains(QStringLiteral("from #4821")));
    QVERIFY(row()->toolTip().contains(QStringLiteral("Message from #4821 · Release checklist")));
    // The label belongs to the running turn; it goes when the turn ends.
    woken.insert(QStringLiteral("running"), false);
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, {woken}}});
    QVERIFY(!qobject_cast<ket::ConversationRow*>(row())->statusLabel().contains("from #"));
    QVERIFY(!row()->toolTip().contains(QStringLiteral("#4821")));
}

void ExplorerPanelTest::ordersRepositoryConversationsByLatestTurnAcrossWorktrees() {
    ketplus::ActivityBar bar;
    const auto conversation = [](const QString& id, const QString& path, const qint64 startedAt) {
        return QVariantMap{
            {QStringLiteral("id"), id},        {QStringLiteral("title"), id},
            {QStringLiteral("started"), true}, {QStringLiteral("seen"), true},
            {QStringLiteral("path"), path},    {QStringLiteral("lastTurnStartedAt"), startedAt}};
    };
    const auto order = [&bar] {
        QStringList ids;
        for (auto* row :
             bar.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton")))
            ids << row->property("conversationId").toString();
        return ids;
    };
    // Projected worktree by worktree; `legacy` has no recorded turn and keeps its place last.
    QVariantList conversations{
        conversation(QStringLiteral("alpha"), QStringLiteral("/repo/a"), 20),
        conversation(QStringLiteral("legacy"), QStringLiteral("/repo/a"), 0),
        conversation(QStringLiteral("beta"), QStringLiteral("/repo/b"), 30),
        conversation(QStringLiteral("gamma"), QStringLiteral("/repo/b"), 10)};
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, conversations}});
    QCOMPARE(order(), (QStringList{QStringLiteral("beta"), QStringLiteral("alpha"),
                                   QStringLiteral("gamma"), QStringLiteral("legacy")}));

    // A new message in `gamma` starts a turn and moves it to the top of the repository.
    conversations[3] = conversation(QStringLiteral("gamma"), QStringLiteral("/repo/b"), 40);
    bar.setRepositories({{QStringLiteral("/repo"), QStringLiteral("repo"), true, conversations}});
    QCOMPARE(order(), (QStringList{QStringLiteral("gamma"), QStringLiteral("beta"),
                                   QStringLiteral("alpha"), QStringLiteral("legacy")}));
}

void ExplorerPanelTest::ordersRepositoriesByTheirNewestConversation() {
    ketplus::ActivityBar bar;
    const auto conversation = [](const QString& id, const QString& title, const qint64 startedAt) {
        return QVariantMap{{QStringLiteral("id"), id},
                           {QStringLiteral("title"), title},
                           {QStringLiteral("started"), true},
                           {QStringLiteral("seen"), true},
                           {QStringLiteral("path"), QStringLiteral("/work/") + id},
                           {QStringLiteral("lastTurnStartedAt"), startedAt}};
    };
    const auto repositoryOrder = [&bar] {
        QStringList paths;
        for (auto* group : bar.findChildren<QWidget*>(QStringLiteral("productRepositoryGroup")))
            paths << group->property("repositoryPath").toString();
        return paths;
    };
    const auto conversationOrder = [&bar] {
        QStringList ids;
        for (auto* row :
             bar.findChildren<QAbstractButton*>(QStringLiteral("productConversationButton")))
            ids << row->property("conversationId").toString();
        return ids;
    };
    // Phones apply this order too. Being the open repository does not move one, and
    // repositories without a turn follow by name.
    bar.setRepositories({{QStringLiteral("/work/ketviet"),
                          QStringLiteral("ketviet"),
                          true,
                          {conversation(QStringLiteral("crm"), QStringLiteral("CRM"), 10)}},
                         {QStringLiteral("/work/zeta"), QStringLiteral("zeta"), false, {}},
                         {QStringLiteral("/work/ketplus"),
                          QStringLiteral("KetPlus"),
                          false,
                          {conversation(QStringLiteral("untitled-b"), QStringLiteral("b draft"), 0),
                           conversation(QStringLiteral("untitled-a"), QStringLiteral("A draft"), 0),
                           conversation(QStringLiteral("sidebar"), QStringLiteral("Sidebar"), 30)}},
                         {QStringLiteral("/work/atlas"), QStringLiteral("atlas"), false, {}}});
    QCOMPARE(repositoryOrder(),
             (QStringList{QStringLiteral("/work/ketplus"), QStringLiteral("/work/ketviet"),
                          QStringLiteral("/work/atlas"), QStringLiteral("/work/zeta")}));
    QCOMPARE(conversationOrder(),
             (QStringList{QStringLiteral("sidebar"), QStringLiteral("untitled-a"),
                          QStringLiteral("untitled-b"), QStringLiteral("crm")}));
}

void ExplorerPanelTest::filtersTagsAndUsesAuthoritativeStorageWarning() {
    ketplus::ActivityBar bar;
    bar.resize(260, 680);
    bar.show();
    const QVariantList conversations{
        QVariantMap{{"id", "one"},
                    {"title", "One"},
                    {"tags", QVariantList{QVariantMap{{"label", "UI"}, {"color", "#3377cc"}}}}},
        QVariantMap{{"id", "two"},
                    {"title", "Two"},
                    {"tags", QVariantList{QVariantMap{{"label", "Review"}}}}}};
    bar.setRepositories({{"/synthetic", "Repo", true, conversations}});
    auto* filter = bar.findChild<QComboBox*>("conversationTagFilter");
    QVERIFY(filter && filter->isVisible());
    filter->setCurrentIndex(filter->findData("UI"));
    const auto rows = bar.findChildren<ket::ConversationRow*>("productConversationButton");
    QCOMPARE(rows.size(), 2);
    for (auto* row : rows)
        QTRY_COMPARE(row->isVisible(), row->property("conversationId") == "one");
    filter->setCurrentIndex(0);
    for (auto* row : rows)
        QVERIFY(row->isVisible());
    auto* warning = bar.findChild<QWidget*>("sidebarStorageWarning");
    QVERIFY(warning && !warning->isVisible());
    bar.setStorageStatus({{"ok", true},
                          {"status", QVariantMap{{"warning", true},
                                                 {"over_limit", true},
                                                 {"database_page_bytes", 9'000'000'000LL},
                                                 {"limit_bytes", 5'000'000'000LL}}}});
    QVERIFY(warning->isVisible());
    // The mock's banner text; the banner itself opens Settings → Storage.
    auto* banner = warning->findChild<ket::Banner*>();
    QCOMPARE(banner->title(), QString("Journal is 9.0 GB"));
    QCOMPARE(banner->message(), QString("Over its 5 GB limit"));
    QSignalSpy opened(&bar, &ketplus::ActivityBar::settingsSectionRequested);
    QTest::mouseClick(warning->findChild<QAbstractButton*>("sidebarStorageSettings"),
                      Qt::LeftButton);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().first().toString(), QString("storage"));
    const auto capture = qEnvironmentVariable("KETPLUS_STORAGE_SIDEBAR_CAPTURE");
    if (!capture.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(bar.grab().save(capture));
    }
    bar.setStorageStatus({{"ok", false}, {"status", QVariantMap{{"warning", true}}}});
    QVERIFY(!warning->isVisible());
    bar.setStorageStatus({{"ok", true}, {"status", QVariantMap{{"warning", false}}}});
    QVERIFY(!warning->isVisible());
    // Under 5 GB free on the disk warns on its own.
    bar.setStorageStatus(
        {{"ok", true},
         {"status", QVariantMap{{"warning", false},
                                {"low_disk", true},
                                {"filesystem_available_bytes", 3'200'000'000LL}}}});
    QVERIFY(warning->isVisible());
    QCOMPARE(banner->title(), QString("Disk space is low"));
    QCOMPARE(banner->message(), QString("3.2 GB free · journal writes may fail"));
    bar.setStorageStatus({{"ok", true},
                          {"status", QVariantMap{{"warning", true},
                                                 {"database_page_bytes", 4'100'000'000LL},
                                                 {"limit_bytes", 5'000'000'000LL}}}});
    QCOMPARE(banner->message(), QString("Near its 5 GB limit"));
}

void ExplorerPanelTest::scalesSidebarTypeWithInterfaceZoom() {
    ketplus::ThemePalette palette;
    palette.border = QStringLiteral("#2B3037");
    palette.sidebarBackground = QStringLiteral("#1B1F24");
    palette.textMain = QStringLiteral("#F2F4F7");
    palette.panelBackground = QStringLiteral("#1D2228");
    palette.textMuted = QStringLiteral("#858D99");
    palette.textSecondary = QStringLiteral("#B4BAC4");
    palette.surfaceHover = QStringLiteral("#292F37");
    palette.accentSubtle = QStringLiteral("#252B42");
    palette.textDisabled = QStringLiteral("#505862");
    palette.borderStrong = QStringLiteral("#3B424C");
    palette.accent = QStringLiteral("#5968DF");
    palette.positive = QStringLiteral("#40C97B");
    palette.warning = QStringLiteral("#E5A93C");
    palette.danger = QStringLiteral("#EF665C");
    palette.dark = true;

    const auto& profile = ket::Profile::current();
    QCOMPARE(ketplus::TypeScale::pixels(ketplus::TypeStep::Body, 100),
             qRound(profile.buttonFontPixels));
    QCOMPARE(ketplus::TypeScale::pixels(ketplus::TypeStep::Body, 125),
             ketplus::InterfaceZoomMetrics::scale(qRound(profile.buttonFontPixels), 125));
    QCOMPARE(ketplus::TypeScale::pixels(ketplus::TypeStep::Reading, 125),
             ketplus::InterfaceZoomMetrics::scale(qRound(profile.readingPixels), 125));

    ketplus::ActivityBar bar;
    bar.applyPalette(palette, ketplus::InterfaceZoomMetrics::defaultPercent);
    const QString medium = bar.styleSheet();
    QVERIFY(
        medium.contains(QStringLiteral("font-size: %1px").arg(qRound(profile.buttonFontPixels))));
    QVERIFY(!medium.contains(QStringLiteral("{{")));
    bar.applyPalette(palette, 125);
    const QString large = bar.styleSheet();
    QVERIFY(large.contains(QStringLiteral("font-size: %1px")
                               .arg(ketplus::TypeScale::pixels(ketplus::TypeStep::Body, 125))));
    bar.applyPalette(palette, 100);
    QCOMPARE(bar.styleSheet(), medium);
}

QTEST_MAIN(ExplorerPanelTest)
#include "ExplorerPanelTest.moc"
