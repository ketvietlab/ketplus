// T-56: the team page in its team's language. A Vietnamese team shows no English fixed text on
// the page or in the task dialog, the team menu changes the language, and New Team starts
// from the goal's language. An English team reads as before. Run with QT_QPA_PLATFORM=offscreen.
// KETPLUS_T56_CAPTURE=<dir> saves the page in each language.
#include "KetDialog.h"
#include "KetExpandableText.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextArea.h"
#include "TeamLanguage.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableWidget>
#include <QXmlStreamReader>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject member(const QString& session, const QString& role, const QString& name,
                   const QString& lane = {}) {
    QJsonObject json{{"session_id", session}, {"role", role}, {"name", name}, {"number", "1"}};
    if (role == "developer")
        json.insert("lane", lane), json.insert("paths", QJsonArray{"src/" + lane.toLower()});
    return json;
}

QJsonObject task(const QString& id, const QString& state, const QString& owner) {
    return {{"id", id},
            {"title", "Trang " + id},
            {"state", state},
            {"owner", owner},
            {"lane", "Web"},
            {"criteria", QJsonArray{QJsonObject{{"text", "Chạy được"}, {"met", true}},
                                    QJsonObject{{"text", "Có test"}, {"met", false}}}},
            {"criteria_met", 1},
            {"outside_lane", QJsonArray{}},
            {"depends_on", QJsonArray{}},
            {"history", QJsonArray{QJsonObject{{"at", "2026-11-10T10:00:00.000Z"},
                                               {"actor", "lead"},
                                               {"kind", "created"},
                                               {"text", "Được tạo bởi Lead."}}}}};
}

// What a team writes is content: in Vietnamese here, so only fixed text can be English.
QJsonObject fixture(const QString& language) {
    return {{"id", "7"},
            {"name", "Shop"},
            {"goal", "Thanh toán trong một trang"},
            {"language", language},
            {"repository", "/work/shop"},
            {"integration_branch", "team/shop"},
            {"merge_policy", "leader"},
            {"paused", true},
            {"paused_reason", "budget"},
            {"turn_budget", 60},
            {"automatic_turns", 23},
            {"members", QJsonArray{member("lead", "leader", "Lead"),
                                   member("dev1", "developer", "Dev 1", "Web"),
                                   member("dev2", "developer", "Dev 2", "Api"),
                                   member("rev", "reviewer", "Review")}},
            {"tasks", QJsonArray{task("T-1", "in_progress", "dev1"), task("T-2", "blocked", "dev2"),
                                 task("T-3", "ready", "dev1"), task("T-4", "done", "dev2")}},
            {"decisions", QJsonArray{QJsonObject{{"id", "D-1"},
                                                 {"kind", "question"},
                                                 {"question", "Dùng Stripe?"},
                                                 {"context", "Cần một cổng thanh toán."},
                                                 {"choices", QJsonArray{"PayPal", "Stripe"}},
                                                 {"recommended", "Stripe"},
                                                 {"task_id", "T-2"},
                                                 {"raised_by", "lead"},
                                                 {"raised_at", "2026-11-10T10:00:00.000Z"},
                                                 {"state", "open"}}}},
            {"open_decisions", 1},
            {"revision", 1}};
}

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, {});
        };
    }
    QJsonObject last(const QString& type) const {
        for (auto it = sent.crbegin(); it != sent.crend(); ++it)
            if (it->first == type)
                return it->second;
        return {};
    }
};

// Every text a person sees in `root`: labels, buttons, fields, the table and its header.
QStringList shown(QWidget* root) {
    QStringList texts;
    for (auto* label : root->findChildren<QLabel*>())
        if (label->isVisibleTo(root) && !label->text().isEmpty())
            texts << label->text();
    for (auto* button : root->findChildren<QAbstractButton*>())
        if (button->isVisibleTo(root) && !button->text().isEmpty())
            texts << button->text();
    for (auto* text : root->findChildren<ket::ExpandableText*>())
        if (text->isVisibleTo(root))
            texts << text->text();
    for (auto* field : root->findChildren<QLineEdit*>())
        if (field->isVisibleTo(root) && !field->placeholderText().isEmpty())
            texts << field->placeholderText();
    for (auto* table : root->findChildren<QTableWidget*>()) {
        for (int column = 0; column < table->columnCount(); ++column)
            if (auto* header = table->horizontalHeaderItem(column))
                texts << header->text();
        for (int row = 0; row < table->rowCount(); ++row)
            for (int column = 0; column < table->columnCount(); ++column)
                if (auto* item = table->item(row, column))
                    texts << item->text();
    }
    return texts;
}

struct Text {
    QString source;
    QString translation;
};

// The team texts with a Vietnamese translation that differs from the English.
QList<Text> catalog() {
    QFile file(QStringLiteral(KETPLUS_TEAM_TS));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<Text> texts;
    QXmlStreamReader xml(&file);
    Text current;
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement())
            continue;
        if (xml.name() == QLatin1String("source")) {
            current.source = xml.readElementText();
        } else if (xml.name() == QLatin1String("translation")) {
            current.translation = xml.readElementText();
            if (current.translation != current.source)
                texts.append(current);
        }
    }
    return texts;
}

// A text with %1 placeholders as a whole-text pattern; a text without as itself.
QRegularExpression pattern(const QString& text) {
    QString expression = QRegularExpression::escape(text);
    expression.replace(QRegularExpression(QStringLiteral("%L?\\d")), QStringLiteral(".+"));
    return QRegularExpression(QLatin1Char('^') + expression + QLatin1Char('$'),
                              QRegularExpression::DotMatchesEverythingOption);
}

// The English fixed texts among `texts`: shown as English, not as their translation.
QStringList english(const QStringList& texts) {
    QStringList found;
    for (const Text& entry : catalog()) {
        const QRegularExpression source = pattern(entry.source);
        const QRegularExpression translated = pattern(entry.translation);
        for (const QString& text : texts) {
            // A fixed text is a line of what is shown (a label joins a few with " · ").
            for (const QString& part : text.split(QRegularExpression(QStringLiteral(" · |\\n"))))
                if (source.match(part.trimmed()).hasMatch() &&
                    !translated.match(part.trimmed()).hasMatch())
                    found << QStringLiteral("%1 (\"%2\")").arg(part.trimmed(), entry.source);
        }
    }
    found.removeDuplicates();
    return found;
}

void capture(QWidget* widget, const QString& name) {
    const QString folder = qEnvironmentVariable("KETPLUS_T56_CAPTURE");
    if (folder.isEmpty())
        return;
    QTest::qWait(200);
    QVERIFY(widget->grab().save(folder + QLatin1Char('/') + name + QStringLiteral(".png")));
}

} // namespace

class TeamPageLanguageTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

    void start(const QJsonObject& team, const QString& update) {
        daemon_ = {};
        daemon_.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team}}});
        daemon_.answers.insert(
            "team.get",
            {{"ok", true},
             {"team", team},
             {"updates", QJsonArray{QJsonObject{
                             {"id", 1}, {"at", "2026-11-10T10:00:00.000Z"}, {"text", update}}}}});
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        QVERIFY2(catalog().size() > 300, "the Vietnamese team catalog is read");
    }

    void aVietnameseTeamShowsNoEnglishFixedText() {
        start(fixture("vi"), "Đã giao T-1 cho Dev 1.");
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 1500);
        page.show();
        QTRY_COMPARE(page.updates().size(), 1);
        const QStringList texts = shown(&page);
        const QStringList left = english(texts);
        QVERIFY2(left.isEmpty(), qPrintable(left.join(QStringLiteral("\n"))));
        // What the page says, in Vietnamese.
        const QString all = texts.join(QLatin1Char('\n'));
        for (const char* expected :
             {"Đang chờ bạn", "Xong 1 trên 4 task", "Đã dùng 23 trên 60 lượt tự động",
              "Leader merge task", "Lượt tự động đang tạm dừng", "Cập nhật", "Thành viên"})
            QVERIFY2(all.contains(QString::fromUtf8(expected)), expected);
        capture(&page, QStringLiteral("team-page-vi"));

        // The task dialog speaks it too.
        auto* dialog = page.openTask("T-1");
        QVERIFY(dialog);
        const QStringList inDialog = english(shown(dialog));
        QVERIFY2(inDialog.isEmpty(), qPrintable(inDialog.join(QStringLiteral("\n"))));
        QVERIFY(shown(dialog).join(QLatin1Char('\n')).contains(QStringLiteral("Lịch sử")));
        capture(dialog, QStringLiteral("team-task-vi"));
        dialog->close();
    }

    // English is the source: the same page reads as before T-56.
    void anEnglishTeamReadsAsBefore() {
        start(fixture("en"), "T-1 assigned to Dev 1.");
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 1500);
        page.show();
        QTRY_COMPARE(page.updates().size(), 1);
        const QString all = shown(&page).join(QLatin1Char('\n'));
        for (const char* expected : {"Waiting for you", "1 of 4 tasks done",
                                     "23 of 60 automatic turns used", "The leader merges tasks"})
            QVERIFY2(all.contains(QLatin1String(expected)), expected);
        QVERIFY(!all.contains(QStringLiteral("Đang chờ bạn")));
        capture(&page, QStringLiteral("team-page-en"));
        // Without a team page in front, nothing else is translated.
        QCOMPARE(remote::team_language::current(), QStringLiteral("en"));
    }

    // The team menu offers the languages and sends the one chosen.
    void theTeamMenuChangesTheLanguage() {
        start(fixture("en"), "T-1 assigned to Dev 1.");
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 1500);
        page.show();
        QTRY_COMPARE(page.updates().size(), 1);
        page.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&page));
        QTimer::singleShot(0, &page, [] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            auto* languages = menu->findChild<QMenu*>("teamPageLanguage");
            QVERIFY(languages);
            auto* vietnamese = languages->findChild<QAction*>("teamPageLanguage_vi");
            QVERIFY(vietnamese);
            QVERIFY(!vietnamese->isChecked());
            QVERIFY(languages->findChild<QAction*>("teamPageLanguage_en")->isChecked());
            QCOMPARE(vietnamese->text(), QStringLiteral("Tiếng Việt"));
            menu->setActiveAction(languages->menuAction());
            languages->popup(menu->geometry().topRight());
            languages->setActiveAction(vietnamese);
            QTest::keyClick(languages, Qt::Key_Return);
        });
        page.findChild<QAbstractButton*>("teamPageMenu")->click();
        QTRY_COMPARE(daemon_.last("team.update").value("language").toString(),
                     QStringLiteral("vi"));
        QCOMPARE(daemon_.last("team.update").value("team_id").toString(), QStringLiteral("7"));
    }

    // New Team takes the goal's language until the user picks one, and sends it.
    void newTeamFollowsTheGoalUntilALanguageIsPicked() {
        start(fixture("en"), "T-1 assigned to Dev 1.");
        NewTeamForm form(teams_.get(), "/work/shop", {});
        auto* language = form.languageField();
        QVERIFY(language);
        QCOMPARE(language->count(), 2);
        form.goalField()->setText(QStringLiteral("Thanh toán trong một trang"));
        QCOMPARE(language->currentData().toString(), QStringLiteral("vi"));
        QCOMPARE(form.payload().value("language").toString(), QStringLiteral("vi"));
        // A language the user picks stays, whatever the goal says next.
        language->setCurrentIndex(language->findData(QStringLiteral("en")));
        emit language->activated(language->currentIndex());
        form.goalField()->setText(QStringLiteral("Thêm trang giỏ hàng"));
        QCOMPARE(form.payload().value("language").toString(), QStringLiteral("en"));
    }
};

QTEST_MAIN(TeamPageLanguageTest)
#include "TeamPageLanguageTest.moc"
