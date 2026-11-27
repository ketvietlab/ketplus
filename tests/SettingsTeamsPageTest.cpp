// ADR 0114, open point 2: Settings → Teams, the automatic turns a new team starts with. The
// page reads and saves the daemon's value, refuses one out of range in words before anything
// is sent and shows the daemon's own refusal. Run with QT_QPA_PLATFORM=offscreen.
#include "app/SettingsTeamsPage.h"
#include "KetTextField.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"

#include <QJsonObject>
#include <QLabel>
#include <QtTest>

#include <functional>
#include <utility>

using namespace ketplus;

class SettingsTeamsPageTest final : public QObject {
    Q_OBJECT

    struct FakeDaemon {
        QList<QPair<QString, QJsonObject>> sent;
        QJsonObject settings{{"turn_budget", 40}};
        QJsonObject refusal;
        // With `hold`, answers wait in `held` until answer().
        bool hold = false;
        QList<std::function<void()>> held;
        void answer() {
            const auto waiting = std::exchange(held, {});
            for (const auto& one : waiting)
                one();
        }
        SettingsTeamsPage::Requester requester() {
            return [this](const QString& type, const QJsonObject& payload,
                          SettingsTeamsPage::Reply reply) {
                if (hold) {
                    held.append([this, type, payload, reply] {
                        const bool was = std::exchange(hold, false);
                        requester()(type, payload, reply);
                        hold = was;
                    });
                    return;
                }
                sent.append({type, payload});
                if (!refusal.isEmpty()) {
                    reply(refusal, refusal.value("error_message").toString());
                    return;
                }
                if (type == "team.settings.update")
                    settings.insert("turn_budget", payload.value("turn_budget"));
                reply({{"ok", true}, {"settings", settings}}, {});
            };
        }
    };

    static void leave(SettingsTeamsPage& page, const QString& text) {
        page.budgetField()->setText(text);
        emit page.budgetField()->editingFinished();
    }

  private slots:
    void theGlobalValueIsReadAndSaved() {
        FakeDaemon daemon;
        daemon.settings.insert("turn_budget", 70);
        SettingsTeamsPage page(daemon.requester());
        page.show();
        QCOMPARE(daemon.sent.first().first, QStringLiteral("team.settings.get"));
        QCOMPARE(page.budgetField()->text(), QStringLiteral("70"));
        leave(page, "25");
        QCOMPARE(daemon.sent.last().first, QStringLiteral("team.settings.update"));
        QCOMPARE(daemon.sent.last().second, (QJsonObject{{"turn_budget", 25}}));
        QCOMPARE(page.budgetField()->text(), QStringLiteral("25"));
        QVERIFY(page.error().isEmpty());
        // Leaving it unchanged sends nothing.
        const auto sent = daemon.sent.size();
        leave(page, "25");
        QCOMPARE(daemon.sent.size(), sent);
    }

    // Review round 1: a value left while the page was still reading is saved once the read is
    // answered, and the read does not put the old value back.
    void aValueLeftWhileReadingIsSaved() {
        FakeDaemon daemon;
        daemon.hold = true;
        SettingsTeamsPage page(daemon.requester());
        page.show();
        QCOMPARE(daemon.held.size(), 1);
        leave(page, "25");
        QCOMPARE(daemon.held.size(), 1);
        daemon.hold = false;
        daemon.answer();
        QCOMPARE(daemon.sent.size(), 2);
        QCOMPARE(daemon.sent.first().first, QStringLiteral("team.settings.get"));
        QCOMPARE(daemon.sent.last().first, QStringLiteral("team.settings.update"));
        QCOMPARE(daemon.sent.last().second, (QJsonObject{{"turn_budget", 25}}));
        QCOMPARE(page.budgetField()->text(), QStringLiteral("25"));
        QCOMPARE(daemon.settings.value("turn_budget").toInt(), 25);
    }

    void anInvalidValueIsRefusedInWords() {
        FakeDaemon daemon;
        SettingsTeamsPage page(daemon.requester());
        page.show();
        const auto sent = daemon.sent.size();
        for (const QString& bad : {"4", "201", "7.5", "", "forty", "-5"}) {
            leave(page, bad);
            QCOMPARE(page.error(),
                     QStringLiteral("Automatic turns are a whole number from 5 to 200."));
            QVERIFY(page.budgetField()->isInvalid());
            QVERIFY(page.findChild<QLabel*>("teamsTurnBudgetError")->isVisible());
        }
        QCOMPARE(daemon.sent.size(), sent);
        // Typing a valid value clears the refusal.
        page.budgetField()->setText("60");
        emit page.budgetField()->textEdited("60");
        QVERIFY(page.error().isEmpty());
        QVERIFY(!page.budgetField()->isInvalid());
    }

    // In Settings after Agent models, in both themes, inside the window; with a refusal too.
    // KETPLUS_TEAM_BUDGET_CAPTURE (a folder) saves them beside the mock's.
    void theSectionFitsInBothThemes_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void theSectionFitsInBothThemes() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        FakeDaemon daemon;
        auto* dialog = new SettingsDialog(AppearanceSettings{}, theme);
        SettingsWorkspacePage shell(dialog, {}, nullptr, {}, {}, daemon.requester());
        shell.resize(1200, 760);
        shell.show();
        shell.showSection("teams");
        auto* page = shell.findChild<SettingsTeamsPage*>();
        QVERIFY(page);
        QTRY_VERIFY(page->isVisible());
        QTRY_COMPARE(page->budgetField()->text(), QStringLiteral("40"));
        auto* field = page->budgetField();
        QVERIFY(shell.rect().contains(QRect(field->mapTo(&shell, QPoint()), field->size())));
        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_BUDGET_CAPTURE");
        const QString name = dark ? QStringLiteral("dark") : QStringLiteral("light");
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + "/app-settings-" + name + ".png"));
        leave(*page, "4");
        auto* error = page->findChild<QLabel*>("teamsTurnBudgetError");
        QTRY_VERIFY(error->isVisible());
        QVERIFY(shell.rect().contains(QRect(error->mapTo(&shell, QPoint()), error->size())));
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + "/app-settings-refused-" + name + ".png"));
    }

    void theDaemonsRefusalIsShown() {
        FakeDaemon daemon;
        SettingsTeamsPage page(daemon.requester());
        page.show();
        daemon.refusal = {{"ok", false},
                          {"error_code", "journal_error"},
                          {"error_message", "The journal refused the change."}};
        leave(page, "90");
        QCOMPARE(page.error(), QStringLiteral("The journal refused the change."));
        // Without a daemon the page says so instead of a number it cannot keep.
        SettingsTeamsPage alone({});
        alone.show();
        QCOMPARE(alone.budgetField()->text(), QStringLiteral("40"));
    }
};

QTEST_MAIN(SettingsTeamsPageTest)
#include "SettingsTeamsPageTest.moc"
