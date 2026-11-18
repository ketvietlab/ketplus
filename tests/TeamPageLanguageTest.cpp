// T30: English chrome coexists with multilingual stored content under a Vietnamese OS locale.
// Run offscreen; KETPLUS_T56_CAPTURE saves real Qt page/dialog captures for inspection.
#include "KetDialog.h"
#include "KetExpandableText.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "TeamLanguage.h"
#include "app/SettingsTeamsPage.h"
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
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
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

// Authored content is Vietnamese here; fixed application text stays English.
QJsonObject fixture(const QString& language) {
    return {{"id", "7"},
            {"name", "Shop"},
            {"goal", QStringLiteral("Thanh toán trong một trang; giữ nguyên nội dung tiếng Việt. ")
                         .repeated(8)},
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

// All preserved catalog entries, including English/Vietnamese homographs.
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

// Detect catalog translations leaking into fixed chrome; fixture content remains untouched.
QStringList translatedChrome(const QStringList& texts) {
    QStringList found;
    QSet<QString> englishLiterals;
    for (const Text& entry : catalog())
        if (!entry.source.contains(QLatin1Char('%')))
            englishLiterals.insert(entry.source);
    for (const Text& entry : catalog()) {
        const QRegularExpression source = pattern(entry.source);
        const QRegularExpression translated = pattern(entry.translation);
        for (const QString& text : texts) {
            // A fixed text is a line of what is shown (a label joins a few with " · ").
            for (const QString& part : text.split(QRegularExpression(QStringLiteral(" · |\\n"))))
                if (translated.match(part.trimmed()).hasMatch() &&
                    !source.match(part.trimmed()).hasMatch() &&
                    !englishLiterals.contains(part.trimmed()))
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
    QTemporaryDir settings_;
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
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QLocale::setDefault(QLocale("vi_VN"));
        ket::Style::install(*qApp);
        QVERIFY2(catalog().size() > 300, "the Vietnamese team catalog is read");
    }

    void hidingOneTeamPageKeepsTheOtherPagesLanguage() {
        namespace language = ketplus::remote::team_language;
        start(fixture("vi"), "Đã giao T-1 cho Dev 1.");
        TeamPage first(teams_.get(), "7", "/work/shop");
        TeamPage second(teams_.get(), "7", "/work/shop");
        first.show();
        second.show();
        QTRY_COMPARE(language::current(), QStringLiteral("en"));
        second.hide();
        QCOMPARE(language::current(), QStringLiteral("en"));
        QCOMPARE(QCoreApplication::translate("Team", "Members"), QStringLiteral("Members"));
        first.hide();
        QCOMPARE(language::current(), QStringLiteral("en"));
    }

    void aVietnameseTeamKeepsEnglishChromeAndStoredContent_data() {
        QTest::addColumn<QSize>("viewport");
        QTest::newRow("desktop") << QSize(1024, 900);
        QTest::newRow("narrow") << QSize(480, 640);
    }

    void aVietnameseTeamKeepsEnglishChromeAndStoredContent() {
        QFETCH(QSize, viewport);
        start(fixture("vi"), "Đã giao T-1 cho Dev 1.");
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(viewport);
        page.show();
        QTRY_COMPARE(page.updates().size(), 1);
        auto* scroll = page.findChild<QScrollArea*>("teamPageScroll");
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->widget()->width() <= scroll->viewport()->width());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        const QStringList texts = shown(&page);
        QStringList chrome = texts;
        chrome.removeAll(QStringLiteral("Đã giao T-1 cho Dev 1."));
        chrome.removeAll(QStringLiteral("Được tạo bởi Lead."));
        const QStringList left = translatedChrome(chrome);
        QVERIFY2(left.isEmpty(), qPrintable(left.join(QStringLiteral("\n"))));
        // Chrome is English; the historical update is still byte-for-byte Vietnamese.
        const QString all = texts.join(QLatin1Char('\n'));
        for (const char* expected :
             {"Waiting for you", "1 of 4 tasks done", "23 of 60 automatic turns used",
              "The leader merges tasks", "Updates", "Members", "Đã giao T-1 cho Dev 1."})
            QVERIFY2(all.contains(QString::fromUtf8(expected)), expected);
        QVERIFY(all.contains(QStringLiteral("Thanh toán trong một trang")));
        QVERIFY(all.contains(QStringLiteral("Dùng Stripe?")));
        capture(&page, QStringLiteral("team-page-vi-") + QLatin1String(QTest::currentDataTag()));

        // The task dialog also keeps English chrome.
        auto* dialog = page.openTask("T-1");
        QVERIFY(dialog);
        QStringList dialogChrome = shown(dialog);
        dialogChrome.removeAll(QStringLiteral("Được tạo bởi Lead."));
        dialogChrome.removeAll(QStringLiteral("Chạy được"));
        dialogChrome.removeAll(QStringLiteral("Có test"));
        const QStringList inDialog = translatedChrome(dialogChrome);
        QVERIFY2(inDialog.isEmpty(), qPrintable(inDialog.join(QStringLiteral("\n"))));
        QVERIFY(shown(dialog).join(QLatin1Char('\n')).contains(QStringLiteral("History")));
        QVERIFY(shown(dialog).join(QLatin1Char('\n')).contains(QStringLiteral("Chạy được")));
        capture(dialog, QStringLiteral("team-task-vi-") + QLatin1String(QTest::currentDataTag()));
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

    void teamSettingsStayEnglishUnderVietnameseLocaleAndTeamContext() {
        remote::team_language::Scope scope("vi");
        SettingsTeamsPage settings(
            [](const QString&, const QJsonObject&, SettingsTeamsPage::Reply reply) {
                reply({{"ok", true}, {"settings", QJsonObject{{"turn_budget", 40}}}}, {});
            });
        settings.resize(480, 640);
        settings.show();
        const QString all = shown(&settings).join(QLatin1Char('\n'));
        QVERIFY(all.contains(QStringLiteral("Teams")));
        QVERIFY(all.contains(QStringLiteral("Automatic turns per team")));
        QVERIFY(translatedChrome(shown(&settings)).isEmpty());
        capture(&settings, QStringLiteral("team-settings-vi-os"));
        // Match normal settings dismissal: deliver focus loss while the derived widget lives.
        settings.budgetField()->clearFocus();
        QApplication::processEvents();
        settings.close();
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
            QCOMPARE(vietnamese->text(), QStringLiteral("Vietnamese"));
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

    // New Team never infers content preferences from OS locale or edits to Goal.
    void newTeamFollowsPromptWithoutChangingItsContentOrChrome() {
        start(fixture("en"), "T-1 assigned to Dev 1.");
        NewTeamForm form(teams_.get(), "/work/shop", {});
        auto* language = form.languageField();
        QVERIFY(language);
        QCOMPARE(language->count(), 3);
        form.goalField()->setText(QStringLiteral("Thanh toán trong một trang"));
        QCOMPARE(language->currentData().toString(), QStringLiteral("auto"));
        QCOMPARE(form.payload().value("language").toString(), QStringLiteral("auto"));
        QCOMPARE(form.goalField()->text(), QStringLiteral("Thanh toán trong một trang"));
        // A language the user picks stays, whatever the goal says next.
        language->setCurrentIndex(language->findData(QStringLiteral("en")));
        emit language->activated(language->currentIndex());
        form.goalField()->setText(QStringLiteral("Thêm trang giỏ hàng"));
        QCOMPARE(form.payload().value("language").toString(), QStringLiteral("en"));
    }
};

QTEST_MAIN(TeamPageLanguageTest)
#include "TeamPageLanguageTest.moc"
