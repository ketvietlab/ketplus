#include "app/ConversationMenu.h"

#include "KetStyle.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QStringList>
#include <QtTest>

class ConversationMenuTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { ket::Style::install(*qApp); }

    // The sidebar's conversation menu, item for item as the mock's (Sidebar.qml rowMenu).
    void readsAsTheMockMenu() {
        QMenu menu;
        const auto actions = ketplus::addConversationMenuActions(menu, true, false, "@agent/8941");
        QStringList texts;
        for (const auto* action : menu.actions())
            texts.append(action->isSeparator() ? QStringLiteral("—") : action->text());
        QCOMPARE(texts, (QStringList{"Rename…", "Fork conversation", "Change worktree…",
                                     "Copy reference\t@agent/8941", "—", "Archive", "Delete…"}));
        for (const auto* action : menu.actions())
            QVERIFY(action->isSeparator() || !action->icon().isNull());
        QVERIFY(actions.copyReference->isVisible());
        QVERIFY(actions.remove->property(ket::Style::kDangerProperty).toBool());
        QVERIFY(actions.remove->isEnabled());
        if (const auto capture = qEnvironmentVariable("KETPLUS_CONVERSATION_MENU_CAPTURE");
            !capture.isEmpty()) {
            menu.popup({40, 40});
            QVERIFY(QTest::qWaitForWindowExposed(&menu));
            QVERIFY(menu.grab().save(capture));
            menu.close();
        }
    }

    // A running conversation cannot be deleted, and says why; one without a number yet has
    // nothing to copy.
    void runningConversationExplainsDelete() {
        QMenu menu;
        const auto actions = ketplus::addConversationMenuActions(menu, false, true, {});
        QVERIFY(!actions.fork->isEnabled());
        QVERIFY(!actions.remove->isEnabled());
        QCOMPARE(actions.remove->property(ket::Style::kDescriptionProperty).toString(),
                 QStringLiteral("Stop the conversation before deleting it."));
        QVERIFY(!actions.copyReference->isVisible());
    }
};

QTEST_MAIN(ConversationMenuTest)
#include "ConversationMenuTest.moc"
