#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/TranscriptAtlasLinks.h"
#include <QScopeGuard>

#include <QDir>
#include <QFile>
#include <QQuickView>
#include <QTemporaryDir>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::atlasResultsUseExplicitCompletedLinks() {
    const QString href = "sample.ketatlas/atlas.json";
    const QString markdown = "[Preview](" + href + ")\n\n[Again](" + href +
                             ")\n\n"
                             "`[Inline](inline.ketatlas/atlas.json)`\n\n"
                             "```md\n[Code](code.ketatlas/atlas.json)\n```\n\n"
                             "<a href=\"raw.ketatlas/atlas.json\">Raw HTML</a>\n\n"
                             "[Web](https://example.test/remote.ketatlas/atlas.json)\n\n"
                             "[Host](file://example.test/remote.ketatlas/atlas.json)";
    QCOMPARE(ketplus::transcriptAtlasLinks(markdown), QStringList{href});
    QVERIFY(ketplus::transcriptAtlasLinks(QString(128 * 1024, 'x') + markdown).isEmpty());
    QString many;
    for (int i = 0; i < 12; ++i)
        many += QString("[Bundle](bundle%1.ketatlas/atlas.json)\n\n").arg(i);
    QCOMPARE(ketplus::transcriptAtlasLinks(many).size(), 8);
    QVariantMap source{
        {"kind", "assistant"}, {"body", markdown}, {"messageKey", "result"}, {"running", true}};
    QVERIFY(ketplus::AgentMessageModel::renderedItems(source, 0)
                .first()
                .value("atlasLinks")
                .toStringList()
                .isEmpty());
    source["running"] = false;
    QCOMPARE(ketplus::AgentMessageModel::renderedItems(source, 0)
                 .first()
                 .value("atlasLinks")
                 .toStringList(),
             QStringList{href});
    // Long answers publish one receipt at the end, parsing the complete Markdown.
    source["body"] = QString("Paragraph.\n\n").repeated(4000) + markdown;
    const auto chunks = ketplus::AgentMessageModel::renderedItems(source, 0);
    QVERIFY(chunks.size() > 1);
    for (int i = 0; i < chunks.size() - 1; ++i)
        QVERIFY(chunks[i].value("atlasLinks").toStringList().isEmpty());
    QCOMPARE(chunks.last().value("atlasLinks").toStringList(), QStringList{href});
    source["kind"] = "user";
    QVERIFY(ketplus::AgentMessageModel::renderedItems(source, 0)
                .first()
                .value("atlasLinks")
                .toStringList()
                .isEmpty());
}

void AgentWorkspaceViewTest::atlasResultCardFencesChangedWorktrees() {
    const auto previousMode = ket::Appearance::instance().mode();
    const auto restoreMode =
        qScopeGuard([previousMode] { ket::Appearance::instance().setMode(previousMode); });
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    QTemporaryDir workspace;
    QTemporaryDir outside;
    QVERIFY(QDir(workspace.path()).mkpath("sample.ketatlas"));
    const QString path = workspace.filePath("sample.ketatlas/atlas.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(workspace.path());
    view.setExecutionBranch("main", workspace.path(), 1);
    view.setAtlasResultInspector([](const QString&, const QString&) {
        return QVariantMap{{"available", true},
                           {"title", "Checkout"},
                           {"screens", 4},
                           {"flows", 2},
                           {"relativePath", "sample.ketatlas/atlas.json"}};
    });
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::atlasResultOpenRequested);
    QSignalSpy errors(&view, &ketplus::AgentWorkspaceView::composerErrorOccurred);
    view.resize(900, 680);
    view.show();
    view.resetMessages({QVariantMap{{"kind", "assistant"},
                                    {"messageKey", "receipt"},
                                    {"body", "[Checkout](sample.ketatlas/atlas.json)"}}},
                       "conversation-a");
    auto* root = view.quickView()->rootObject();
    QTRY_VERIFY(findVisualItem(root, "atlasResultCard"));
    auto* card = findVisualItem(root, "atlasResultCard");
    // The mock's card: a validated preview, named by its bundle and size.
    QStringList texts;
    for (auto* item : card->findChildren<QQuickItem*>())
        if (item->isVisible() && item->property("text").isValid())
            texts.append(item->property("text").toString());
    QVERIFY(texts.contains("Validated preview available"));
    QVERIFY(texts.contains(QString::fromUtf8("sample.ketatlas · 4 screens · 2 flows")));
    auto* edit = findVisualItem(card, "editAtlasResultButton");
    QVERIFY(edit && edit->isVisible());
    QCOMPARE(edit->property("iconName").toString(), "edit");
    QVERIFY(edit->width() <= card->width());
    QVERIFY(QMetaObject::invokeMethod(edit, "clicked"));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().at(0).toString(), QFileInfo(path).canonicalFilePath());
    QCOMPARE(opened.first().at(2).toBool(), false);
    auto* browser = findVisualItem(card, "openAtlasResultBrowserButton");
    QVERIFY(browser && QMetaObject::invokeMethod(browser, "clicked"));
    QCOMPARE(opened.size(), 2);
    QCOMPARE(opened.last().at(2).toBool(), true);
    if (const auto capture = qEnvironmentVariable("KETPLUS_ATLAS_RESULT_CAPTURE");
        !capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    view.setExecutionBranch("other", outside.path(), 2);
    QTRY_VERIFY(!card->property("result").toMap().value("available").toBool());
    view.openAtlasResult("sample.ketatlas/atlas.json", false);
    QCOMPARE(opened.size(), 2);
    QCOMPARE(errors.size(), 1);
    view.setExecutionBranch("main", workspace.path(), 1);
    QTRY_VERIFY(card->property("result").toMap().value("available").toBool());
    QVERIFY(QFile::remove(path));
    view.openAtlasResult("sample.ketatlas/atlas.json", false);
    QCOMPARE(opened.size(), 2);
    QCOMPARE(errors.size(), 2);
    view.resetMessages({}, "conversation-b");
    QTRY_VERIFY(!findVisualItem(root, "atlasResultCard"));
}
