// A long-lived install lists thousands of conversations. The sidebar builds rows only for what it
// shows and rebuilds only the repository that changed, so one new title does not rebuild every
// row; the Tab order still follows the groups as shown. Offscreen, synthetic rows only.
#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "KetStyle.h"
#include "workspace/SidebarRepositoryList.h"

#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtTest>

using ketplus::SidebarRepository;
using ketplus::SidebarRepositoryList;

namespace {

QVariantMap conversation(const QString& id, const QString& path, const qint64 startedAt) {
    return {{"id", id},     {"title", QStringLiteral("Title ") + id},
            {"path", path}, {"started", true},
            {"seen", true}, {"lastTurnStartedAt", startedAt}};
}

// `count` conversations, newest first by their number: c0 is the newest.
SidebarRepository repository(const QString& name, const int count) {
    SidebarRepository result{.path = QStringLiteral("/work/") + name, .name = name};
    for (int index = 0; index < count; ++index)
        result.conversations.append(
            conversation(name + QString::number(index), result.path, 1'000'000 - index));
    return result;
}

QStringList rowTitles(QWidget* parent) {
    QStringList titles;
    for (auto* row : parent->findChildren<ket::SidebarRow*>("productConversationButton"))
        titles << row->text();
    return titles;
}

QWidget* groupOf(SidebarRepositoryList& list, const QString& name) {
    for (auto* group : list.findChildren<QWidget*>("productRepositoryGroup"))
        if (group->property("repositoryPath") == QStringLiteral("/work/") + name)
            return group;
    return nullptr;
}

} // namespace

class SidebarScaleTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    void init() {
        QSettings().remove(QStringLiteral("sidebar"));
        ket::Appearance::instance().setZoom(1.0);
    }

    // Rows past the preview are not built until "Show N more", which adds them in order.
    void onlyPreviewedRowsAreBuiltUntilShowMore() {
        SidebarRepositoryList list;
        list.setRepositories({repository("alpha", 400)});
        list.show();
        QWidget* group = groupOf(list, "alpha");
        QVERIFY(group != nullptr);
        QCOMPARE(rowTitles(group), (QStringList{"Title alpha0", "Title alpha1", "Title alpha2",
                                                "Title alpha3", "Title alpha4"}));

        auto* more = group->findChild<QPushButton*>("productShowMoreConversations");
        QVERIFY(more != nullptr);
        QCOMPARE(more->text(), QStringLiteral("Show 395 more"));
        more->click();
        QCOMPARE(groupOf(list, "alpha"), group);
        QStringList expected;
        for (int index = 0; index < 400; ++index)
            expected << QStringLiteral("Title alpha%1").arg(index);
        QStringList shown;
        auto* held = group->findChild<ket::SidebarTreeGuide*>("productRepositoryHeld");
        for (int part = 0; part < held->partsLayout()->count(); ++part)
            if (auto* row =
                    qobject_cast<ket::SidebarRow*>(held->partsLayout()->itemAt(part)->widget()))
                shown << row->text();
        QCOMPARE(shown, expected);
        for (auto* row : group->findChildren<ket::SidebarRow*>())
            QVERIFY(row->isVisibleTo(&list));
        QVERIFY(!more->isVisibleTo(&list));
    }

    // A running conversation far down the history keeps one of the five slots, so its row is
    // built.
    void aRunningConversationOutsideThePreviewIsBuilt() {
        SidebarRepositoryList list;
        SidebarRepository alpha = repository("alpha", 50);
        list.setRepositories({alpha});
        QVariantMap old = alpha.conversations.at(30).toMap();
        old.insert("running", true);
        alpha.conversations[30] = old;
        list.setRepositories({alpha});
        QCOMPARE(rowTitles(groupOf(list, "alpha")).size(), 5);
        QVERIFY(rowTitles(groupOf(list, "alpha")).contains("Title alpha30"));
    }

    // A title changing in one repository rebuilds that group only, and a title further down the
    // history than the preview rebuilds nothing.
    void anUpdateRebuildsOnlyTheGroupThatChanged() {
        SidebarRepositoryList list;
        QVector<SidebarRepository> repositories{repository("alpha", 20), repository("beta", 20)};
        list.setRepositories(repositories);
        const QPointer<QWidget> alphaGroup = groupOf(list, "alpha");
        const QPointer<QWidget> betaGroup = groupOf(list, "beta");

        QVariantMap hidden = repositories[0].conversations.at(15).toMap();
        hidden.insert("title", QStringLiteral("Renamed out of sight"));
        repositories[0].conversations[15] = hidden;
        list.setRepositories(repositories);
        QCOMPARE(groupOf(list, "alpha"), alphaGroup.data());

        QVariantMap shown = repositories[0].conversations.at(0).toMap();
        shown.insert("title", QStringLiteral("Renamed"));
        repositories[0].conversations[0] = shown;
        list.setRepositories(repositories);
        QVERIFY(alphaGroup.isNull());
        QCOMPARE(groupOf(list, "beta"), betaGroup.data());
        QVERIFY(rowTitles(groupOf(list, "alpha")).contains("Renamed"));
    }

    // Children and the Tab order follow the groups as shown, also when a group in the middle is
    // the only one built again.
    void theTabOrderFollowsTheGroupsAsShown() {
        QWidget window;
        auto* list = new SidebarRepositoryList(&window);
        QVector<SidebarRepository> repositories{repository("alpha", 3), repository("beta", 3),
                                                repository("gamma", 3)};
        list->setRepositories(repositories);
        QVariantMap renamed = repositories[1].conversations.at(0).toMap();
        renamed.insert("title", QStringLiteral("Renamed"));
        repositories[1].conversations[0] = renamed;
        list->setRepositories(repositories);

        QStringList children;
        for (auto* group :
             list->findChildren<QWidget*>("productRepositoryGroup", Qt::FindDirectChildrenOnly))
            children << group->property("repositoryPath").toString();
        QCOMPARE(children, (QStringList{"/work/alpha", "/work/beta", "/work/gamma"}));

        QStringList tabbed;
        QWidget* first = list->findChildren<ket::SidebarRow*>().first();
        QWidget* widget = first;
        do {
            if (auto* row = qobject_cast<ket::SidebarRow*>(widget))
                tabbed << row->text();
            widget = widget->nextInFocusChain();
        } while (widget != first);
        QCOMPARE(tabbed, (QStringList{"Title alpha0", "Title alpha1", "Title alpha2", "Renamed",
                                      "Title beta1", "Title beta2", "Title gamma0", "Title gamma1",
                                      "Title gamma2"}));
    }
};

QTEST_MAIN(SidebarScaleTest)
#include "SidebarScaleTest.moc"
