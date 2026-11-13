#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"
#include "ui/theme/ColorScheme.h"
#include <QDir>
#include <QQmlComponent>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>
class SchemeChatTest : public QObject {
    Q_OBJECT
  private slots:
    void nativeBuiltinComparisonOnDell() {
        if (!qEnvironmentVariableIsSet("KETPLUS_NATIVE_SCHEME_CAPTURE_DIR"))
            QSKIP("Explicit native DELL capture only");
        QScreen* dell = nullptr;
        for (auto* screen : qApp->screens())
            if ((screen->name() + screen->model())
                    .contains(QStringLiteral("DELL"), Qt::CaseInsensitive))
                dell = screen;
        QVERIFY2(dell, "A DELL display is required; no window was opened");
        QTemporaryDir directory;
        ketplus::ThemeManager theme(*qApp, nullptr, directory.path());
        ketplus::AgentWorkspaceView view;
        view.setScreen(dell);
        view.setGeometry(
            QRect(dell->availableGeometry().topLeft() + QPoint(80, 70), QSize(1100, 850)));
        view.resetMessages(
            {QVariantMap{
                {"kind", "assistant"},
                {"body",
                 "## Native scheme parity\n\nA paragraph with **bold**, `inline code`, and [a "
                 "link](https://example.com).\n\n```cpp\nint value = 42; // comment\n```"},
                {"messageKey", "native"}}},
            QStringLiteral("native-scheme-test"));
        auto* model = view.messageModel();
        auto* root = view.quickView()->rootObject();
        const auto output = qEnvironmentVariable("KETPLUS_NATIVE_SCHEME_CAPTURE_DIR");
        QVERIFY(QDir().mkpath(output));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QCOMPARE(view.screen(), dell);
        for (auto kind : {ketplus::SchemeKind::Light, ketplus::SchemeKind::Dark}) {
            const auto name = kind == ketplus::SchemeKind::Light ? QStringLiteral("light")
                                                                 : QStringLiteral("dark");
            theme.setMode(kind == ketplus::SchemeKind::Light ? ketplus::ThemeManager::Mode::Light
                                                             : ketplus::ThemeManager::Mode::Dark);
            view.applyTheme(theme.palette());
            view.quickView()->update();
            QTest::qWait(800);
            QVERIFY(view.quickView()->grabWindow().save(
                QDir(output).filePath(name + QStringLiteral("-native-chat-legacy.png"))));
            view.applyColorScheme(theme.colorScheme());
            view.quickView()->update();
            QTest::qWait(800);
            QVERIFY(view.quickView()->grabWindow().save(
                QDir(output).filePath(name + QStringLiteral("-native-chat-scheme.png"))));
            QCOMPARE(view.messageModel(), model);
            QCOMPARE(view.quickView()->rootObject(), root);
        }
        qInfo() << "Native display" << dell->name() << dell->model();
    }
    void schemeRefreshRetainsQuickTranscriptAndDocument() {
        QTemporaryDir directory;
        ketplus::ThemeManager theme(*qApp, nullptr, directory.path());
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        ketplus::AgentWorkspaceView view;
        view.applyColorScheme(theme.colorScheme());
        view.resetMessages(
            {QVariantMap{{"kind", "assistant"}, {"body", "streaming text"}, {"messageKey", "one"}}},
            QStringLiteral("scheme-test"));
        auto* model = view.messageModel();
        const auto rows = model->rowCount();
        auto* root = view.quickView()->rootObject();
        QVERIFY(root);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QQmlComponent component(view.quickView()->engine());
        component.setData(R"(import QtQuick
TextEdit { width: 400; textFormat: TextEdit.MarkdownText; text: "```cpp\nint n = 7;\n```" })",
                          QUrl(QStringLiteral("qrc:/tests/SchemeChat.qml")));
        QScopedPointer<QObject> item(component.create());
        QVERIFY2(item, qPrintable(component.errorString()));
        auto* quickDocument = item->property("textDocument").value<QQuickTextDocument*>();
        QVERIFY(quickDocument);
        auto* document = quickDocument->textDocument();
        QVERIFY(document);
        const auto text = document->toPlainText();
        view.formatMessageDocument(quickDocument, 14, 400);
        QVERIFY(QMetaObject::invokeMethod(item.data(), "select", Q_ARG(int, 0), Q_ARG(int, 3)));
        connect(&theme, &ketplus::ThemeManager::themeChanged, &view,
                [&] { view.applyColorScheme(theme.colorScheme()); });
        connect(&view, &ketplus::AgentWorkspaceView::documentStyleChanged, &view,
                [&] { view.formatMessageDocument(quickDocument, 14, 400); });
        QSignalSpy styles(&view, &ketplus::AgentWorkspaceView::documentStyleChanged);
        auto scheme = theme.colorScheme();
        scheme.id = QStringLiteral("preview");
        auto& keyword = scheme.syntax[QStringLiteral("keyword")];
        keyword.fg = QColor(123, 45, 67);
        keyword.bold = keyword.italic = keyword.underline = true;
        theme.previewColorScheme(scheme);
        QTRY_COMPARE(theme.colorScheme().id, QStringLiteral("preview"));
        QCOMPARE(styles.size(), 1);
        QCOMPARE(view.messageModel(), model);
        QCOMPARE(model->rowCount(), rows);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(view.quickView()->rootObject(), root);
        QCOMPARE(quickDocument->textDocument(), document);
        QCOMPARE(document->toPlainText(), text);
        QCOMPARE(item->property("selectionStart").toInt(), 0);
        QCOMPARE(item->property("selectionEnd").toInt(), 3);
        bool found = false;
        for (const auto& format : document->begin().layout()->formats()) {
            if (format.start != 0 || format.length != 3)
                continue;
            found = true;
            QCOMPARE(format.format.foreground().color(), QColor(123, 45, 67));
            QCOMPARE(format.format.fontWeight(), QFont::Bold);
            QVERIFY(format.format.fontItalic());
            QVERIFY(format.format.fontUnderline());
        }
        QVERIFY(found);
        // Ordinary repeated formatting retains the same highlighter and snapshot.
        view.formatMessageDocument(quickDocument, 14, 400);
        QCOMPARE(styles.size(), 1);
        view.applyTheme(theme.palette());
        QCOMPARE(styles.size(), 2);
        QCOMPARE(quickDocument->textDocument(), document);
        for (const auto& format : document->begin().layout()->formats())
            if (format.start == 0 && format.length == 3)
                QVERIFY(format.format.foreground().color() != QColor(123, 45, 67));
    }
};
QTEST_MAIN(SchemeChatTest)
#include "SchemeChatTest.moc"
