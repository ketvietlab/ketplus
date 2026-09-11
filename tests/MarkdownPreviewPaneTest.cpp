#include "preview/MarkdownPreviewPane.h"
#include "preview/MermaidDiagramDialog.h"
#include "preview/MermaidRenderer.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFile>
#include <QGraphicsView>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextTable>
#include <QUuid>
#include <QtTest>

class MarkdownPreviewPaneTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersMarkdownFromTheLiveBuffer();
    void appliesReadableDocumentTypography();
    void appliesCustomDocumentTypography();
    void explainsHowToInstallMissingMmdc();
    void discoversMmdcInstalledByNvm();
    void rendersMermaidAndOpensThePopup();
    void opensVectorDiagramWithPanControls();
};

ketplus::ThemePalette previewTestPalette() {
    ketplus::ThemePalette palette;
    palette.panelBackground = QStringLiteral("#FFFFFF");
    palette.panelSubtle = QStringLiteral("#F7F5F5");
    palette.surfaceRaised = QStringLiteral("#FBFAFA");
    palette.textMain = QStringLiteral("#24262A");
    palette.textSecondary = QStringLiteral("#5A5C5E");
    palette.border = QStringLiteral("#E9E7E8");
    palette.borderStrong = QStringLiteral("#CCCCCD");
    palette.accent = QStringLiteral("#5167C4");
    palette.accentSubtle = QStringLiteral("#EEF0FB");
    palette.accentMuted = QStringLiteral("#DDE2F7");
    return palette;
}

void MarkdownPreviewPaneTest::rendersMarkdownFromTheLiveBuffer() {
    ketplus::MarkdownPreviewPane preview;
    preview.setSource(QStringLiteral("# KetPlus\n\n- live preview"),
                      QStringLiteral("/tmp/README.md"), ketplus::ThemePalette{});

    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
    QVERIFY(browser != nullptr);
    QTRY_VERIFY(browser->toPlainText().contains(QStringLiteral("KetPlus")));
    QVERIFY(browser->toPlainText().contains(QStringLiteral("live preview")));
}

void MarkdownPreviewPaneTest::appliesReadableDocumentTypography() {
    ketplus::MarkdownPreviewPane preview;
    preview.setSource(QStringLiteral(R"(# Heading

A paragraph with **strong text**, *emphasis*, `inline code`, and a [link](https://example.com).

## Section

> A concise quotation that should remain visually separate from the body.

- First item
- Second item

```cpp
EditorSettings settings = EditorSettings::load();
preview.applyTheme(theme.palette());
```

| Area | Status |
| --- | --- |
| Typography | Ready |
)"),
                      QStringLiteral("/tmp/README.md"), previewTestPalette());

    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
    QVERIFY(browser != nullptr);
    QTRY_VERIFY(browser->toPlainText().contains(QStringLiteral("A paragraph with")));
    QCOMPARE(browser->document()->documentMargin(), 28.0);
    const QString style = browser->document()->defaultStyleSheet();
    QVERIFY(style.contains(QStringLiteral("font-size: 14px")));
    QVERIFY(style.contains(QStringLiteral("line-height: 22px")));
    QVERIFY(style.contains(QStringLiteral("font-size: 26px")));
    QVERIFY(style.contains(QStringLiteral("font-size: 20px")));
    QVERIFY(style.contains(QStringLiteral("border-bottom: 1px solid")));
    QVERIFY(style.contains(QStringLiteral("font-family: \"")));
    QVERIFY(style.contains(QStringLiteral("margin: 0")));

    bool sawCodeBlock = false;
    bool sawInlineCode = false;
    bool sawQuote = false;
    QTextTable* table = nullptr;
    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
        const bool isHeading = block.blockFormat().headingLevel() > 0;
        const bool isCodeBlock = block.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage) ||
                                 block.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
        QTextTable* currentTable = QTextCursor(block).currentTable();
        const bool isEmptySpacer = block.text().isEmpty() && currentTable == nullptr;
        table = table != nullptr ? table : currentTable;
        const qreal expectedLineHeight =
            isHeading ? 24.0 : (isCodeBlock ? 21.0 : (isEmptySpacer ? 60.0 : 22.0));
        QCOMPARE(block.blockFormat().lineHeight(), expectedLineHeight);
        const auto expectedLineHeightType =
            isEmptySpacer ? QTextBlockFormat::ProportionalHeight : QTextBlockFormat::FixedHeight;
        QCOMPARE(block.blockFormat().lineHeightType(), static_cast<int>(expectedLineHeightType));
        if (isCodeBlock) {
            sawCodeBlock = true;
            QCOMPARE(block.blockFormat().background().style(), Qt::NoBrush);
            const bool previousIsCode =
                block.previous().isValid() &&
                block.previous().blockFormat().hasProperty(QTextFormat::BlockCodeFence);
            const bool nextIsCode =
                block.next().isValid() &&
                block.next().blockFormat().hasProperty(QTextFormat::BlockCodeFence);
            QCOMPARE(block.blockFormat().topMargin(), previousIsCode ? 0.0 : 20.0);
            QCOMPARE(block.blockFormat().bottomMargin(), nextIsCode ? 0.0 : 20.0);
        }
        if (block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            sawQuote = true;
            QCOMPARE(block.blockFormat().background().color(), QColor(QStringLiteral("#F7F5F5")));
        }
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            if (fragment.fragment().text() == QStringLiteral("inline code")) {
                sawInlineCode = true;
                QVERIFY(fragment.fragment().charFormat().fontFixedPitch());
                QCOMPARE(fragment.fragment().charFormat().background().style(), Qt::NoBrush);
            }
        }
    }
    QVERIFY(sawCodeBlock);
    QVERIFY(sawInlineCode);
    QVERIFY(sawQuote);
    QVERIFY(table != nullptr);
    QCOMPARE(table->format().width().type(), QTextLength::PercentageLength);
    QCOMPARE(table->format().width().rawValue(), 100.0);
}

void MarkdownPreviewPaneTest::appliesCustomDocumentTypography() {
    ketplus::MarkdownPreviewPane preview;
    preview.setTypography(17, 29);
    preview.setSource(QStringLiteral("# Heading\n\nParagraph\n\n```cpp\nreturn 0;\n```"),
                      QStringLiteral("/tmp/README.md"), previewTestPalette());

    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
    QVERIFY(browser != nullptr);
    QTRY_VERIFY(browser->toPlainText().contains(QStringLiteral("Paragraph")));
    const QString style = browser->document()->defaultStyleSheet();
    QVERIFY(style.contains(QStringLiteral("font-size: 17px")));
    QVERIFY(style.contains(QStringLiteral("line-height: 29px")));
    QVERIFY(style.contains(QStringLiteral("font-size: 29px")));
    QVERIFY(style.contains(QStringLiteral("font-size: 23px")));

    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
        const bool isCodeBlock = block.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage) ||
                                 block.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
        const bool isEmptySpacer =
            block.text().isEmpty() && QTextCursor(block).currentTable() == nullptr;
        const qreal expectedLineHeight = block.blockFormat().headingLevel() > 0
                                             ? 31.0
                                             : (isCodeBlock ? 28.0 : (isEmptySpacer ? 60.0 : 29.0));
        QCOMPARE(block.blockFormat().lineHeight(), expectedLineHeight);
        const auto expectedLineHeightType =
            isEmptySpacer ? QTextBlockFormat::ProportionalHeight : QTextBlockFormat::FixedHeight;
        QCOMPARE(block.blockFormat().lineHeightType(), static_cast<int>(expectedLineHeightType));
    }
}

void MarkdownPreviewPaneTest::explainsHowToInstallMissingMmdc() {
    const bool overrideWasSet = qEnvironmentVariableIsSet("KETPLUS_MMDC");
    const QByteArray previousOverride = qgetenv("KETPLUS_MMDC");
    qputenv("KETPLUS_MMDC", QByteArray("/definitely/missing/mmdc"));
    {
        ketplus::MarkdownPreviewPane preview;
        preview.setSource(QStringLiteral("```mermaid\nflowchart LR\nA --> B\n```"),
                          QStringLiteral("/tmp/diagram.md"), ketplus::ThemePalette{});

        auto* notice = preview.findChild<QLabel*>(QStringLiteral("markdownPreviewNotice"));
        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
        QVERIFY(notice != nullptr);
        QVERIFY(browser != nullptr);
        QTRY_VERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains(QStringLiteral("npm install -g")));
        QVERIFY(browser->toPlainText().contains(QStringLiteral("flowchart LR")));
    }
    if (overrideWasSet) {
        qputenv("KETPLUS_MMDC", previousOverride);
    } else {
        qunsetenv("KETPLUS_MMDC");
    }
}

void MarkdownPreviewPaneTest::discoversMmdcInstalledByNvm() {
#ifndef Q_OS_UNIX
    QSKIP("The NVM layout is only used on POSIX platforms.");
#else
    QTemporaryDir temporaryHome;
    QVERIFY(temporaryHome.isValid());
    const QString binDirectory =
        temporaryHome.filePath(QStringLiteral(".nvm/versions/node/v24.0.0/bin"));
    QVERIFY(QDir().mkpath(binDirectory));
    const QString executablePath = QDir(binDirectory).filePath(QStringLiteral("mmdc"));
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    const QByteArray script = "#!/bin/sh\nexit 0\n";
    QCOMPARE(executable.write(script), script.size());
    executable.close();
    QVERIFY(executable.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));

    const QByteArray previousHome = qgetenv("HOME");
    const QByteArray previousPath = qgetenv("PATH");
    const bool overrideWasSet = qEnvironmentVariableIsSet("KETPLUS_MMDC");
    const QByteArray previousOverride = qgetenv("KETPLUS_MMDC");
    qputenv("HOME", temporaryHome.path().toUtf8());
    qputenv("PATH", QByteArray("/usr/bin:/bin"));
    qunsetenv("KETPLUS_MMDC");
    QString discoveredPath;
    {
        ketplus::MermaidRenderer renderer;
        discoveredPath = renderer.executablePath();
    }
    qputenv("HOME", previousHome);
    qputenv("PATH", previousPath);
    if (overrideWasSet) {
        qputenv("KETPLUS_MMDC", previousOverride);
    } else {
        qunsetenv("KETPLUS_MMDC");
    }

    QCOMPARE(discoveredPath, executablePath);
#endif
}

void MarkdownPreviewPaneTest::rendersMermaidAndOpensThePopup() {
#ifndef Q_OS_UNIX
    QSKIP("The lightweight fake mmdc used by this test requires a POSIX shell.");
#else
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString executablePath = temporaryDirectory.filePath(QStringLiteral("mmdc"));
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    const QByteArray script = R"(#!/bin/sh
output=""
config=""
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-o" ]; then output="$2"; shift 2
  elif [ "$1" = "-c" ]; then config="$2"; shift 2
  else shift
  fi
done
if [ -z "$config" ] || ! grep -q '"stateLabelColor":"#CDD2D8"' "$config"; then
  exit 12
fi
printf '%s' '<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160"><rect x="10" y="10" width="300" height="140" fill="#8b5cf6"/></svg>' > "$output"
)";
    QCOMPARE(executable.write(script), script.size());
    executable.close();
    QVERIFY(executable.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));

    const bool overrideWasSet = qEnvironmentVariableIsSet("KETPLUS_MMDC");
    const QByteArray previousOverride = qgetenv("KETPLUS_MMDC");
    qputenv("KETPLUS_MMDC", executablePath.toUtf8());
    {
        ketplus::MarkdownPreviewPane preview;
        ketplus::ThemePalette darkPalette;
        darkPalette.dark = true;
        const QString uniqueNode = QUuid::createUuid().toString(QUuid::WithoutBraces);
        preview.setSource(
            QStringLiteral("```mermaid\nflowchart LR\nA[%1] --> B\n```").arg(uniqueNode),
            temporaryDirectory.filePath(QStringLiteral("diagram.md")), darkPalette);

        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
        QVERIFY(browser != nullptr);
        QString diagramLink;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (QTextBlock block = browser->document()->begin(); block.isValid();
                     block = block.next()) {
                    for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                        const QTextCharFormat format = fragment.fragment().charFormat();
                        if (format.isImageFormat() && format.isAnchor()) {
                            diagramLink = format.anchorHref();
                            return diagramLink.startsWith(QStringLiteral("ketplus-mermaid-open:"));
                        }
                    }
                }
                return false;
            }(),
            5000);

        QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection,
                                          Q_ARG(QUrl, QUrl(diagramLink))));
        QTRY_VERIFY(preview.findChild<ketplus::MermaidDiagramDialog*>() != nullptr);
    }
    if (overrideWasSet) {
        qputenv("KETPLUS_MMDC", previousOverride);
    } else {
        qunsetenv("KETPLUS_MMDC");
    }
#endif
}

void MarkdownPreviewPaneTest::opensVectorDiagramWithPanControls() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString svgPath = temporaryDirectory.filePath(QStringLiteral("diagram.svg"));
    QFile svg(svgPath);
    QVERIFY(svg.open(QIODevice::WriteOnly));
    const QByteArray svgContents =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160">
        <rect x="10" y="10" width="300" height="140" fill="#8b5cf6"/>
    </svg>)";
    QVERIFY(svg.write(svgContents) > 0);
    svg.close();

    ketplus::MermaidDiagramDialog dialog(svgPath, ketplus::ThemePalette{});
    auto* view = dialog.findChild<QGraphicsView*>(QStringLiteral("mermaidDiagramView"));
    auto* zoomIn = dialog.findChild<QPushButton*>(QStringLiteral("mermaidZoomIn"));
    QVERIFY(view != nullptr);
    QVERIFY(zoomIn != nullptr);
    QCOMPARE(view->dragMode(), QGraphicsView::ScrollHandDrag);
    QVERIFY(!view->scene()->items().isEmpty());
    const qreal initialScale = view->transform().m11();
    zoomIn->click();
    QVERIFY(view->transform().m11() > initialScale);
}

QTEST_MAIN(MarkdownPreviewPaneTest)
#include "MarkdownPreviewPaneTest.moc"
