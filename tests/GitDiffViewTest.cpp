#include "git/GitDiffView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QTableView>
#include <QtTest>

class GitDiffViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersStructuredInlineDiff();
};

void GitDiffViewTest::rendersStructuredInlineDiff() {
    qApp->setStyleSheet(
        QStringLiteral("QWidget { font-family: Helvetica; font-size: 20px; }"));

    ketplus::ThemePalette palette;
    palette.dark = true;
    palette.panelBackground = QStringLiteral("#1d2228");
    palette.panelSubtle = QStringLiteral("#171b20");
    palette.textMain = QStringLiteral("#cdd2d8");
    palette.textSecondary = QStringLiteral("#b4bac4");
    palette.textMuted = QStringLiteral("#858d99");
    palette.accent = QStringLiteral("#5968df");
    palette.accentActive = QStringLiteral("#4c59c9");
    palette.accentSubtle = QStringLiteral("#252b42");
    palette.positive = QStringLiteral("#40c97b");
    palette.danger = QStringLiteral("#ef665c");

    const QString diff = QString::fromUtf8(
        "diff --git a/example.cpp b/example.cpp\n"
        "index 1111111..2222222 100644\n"
        "--- a/example.cpp\n"
        "+++ b/example.cpp\n"
        "@@ -1,2 +1,2 @@\n"
        " int unchanged = 0;\n"
        "-int before = 1;\n"
        "+int after = 2;\n");

    ketplus::GitDiffView view;
    view.setDiff(QStringLiteral("/repo/example.cpp"), ketplus::GitDiffMode::Unstaged,
                 diff, palette);

    QCOMPARE(view.filePath(), QStringLiteral("/repo/example.cpp"));
    QCOMPARE(view.mode(), ketplus::GitDiffMode::Unstaged);

    auto* table = view.findChild<QTableView*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->font().family(),
             QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    QCOMPARE(table->font().pixelSize(), 13);
    const QFont renderedCodeFont = table->model()->index(1, 3).data(Qt::FontRole).value<QFont>();
    QCOMPARE(renderedCodeFont.family(),
             QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    QCOMPARE(renderedCodeFont.pixelSize(), 13);
    QVERIFY(renderedCodeFont.family() != QStringLiteral("Helvetica"));
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 24);
    QCOMPARE(table->model()->rowCount(), 4);
    QCOMPARE(table->columnSpan(0, 0), 4);
    QCOMPARE(table->model()->index(0, 0).data().toString(),
             QStringLiteral("@@ -1,2 +1,2 @@"));
    QCOMPARE(table->model()->index(1, 0).data().toString(), QStringLiteral("1"));
    QCOMPARE(table->model()->index(1, 1).data().toString(), QStringLiteral("1"));
    QCOMPARE(table->model()->index(2, 0).data().toString(), QStringLiteral("2"));
    QVERIFY(table->model()->index(2, 1).data().toString().isEmpty());
    QCOMPARE(table->model()->index(2, 2).data().toString(), QString::fromUtf8("−"));
    QCOMPARE(table->model()->index(2, 3).data().toString(), QStringLiteral("int before = 1;"));
    QVERIFY(table->model()->index(2, 3).data(Qt::BackgroundRole).isValid());
    QVERIFY(table->model()->index(2, 3).data(Qt::BackgroundRole).value<QColor>() !=
            QColor(palette.panelBackground));
    QCOMPARE(table->model()->index(2, 3).data(Qt::ForegroundRole).value<QColor>(),
             QColor(palette.textMain));
    QVERIFY(table->model()->index(2, 3).data(Qt::ForegroundRole).value<QColor>() !=
            QColor(palette.danger));
    QVERIFY(table->model()->index(3, 0).data().toString().isEmpty());
    QCOMPARE(table->model()->index(3, 1).data().toString(), QStringLiteral("2"));
    QCOMPARE(table->model()->index(3, 2).data().toString(), QStringLiteral("+"));
    QCOMPARE(table->model()->index(3, 3).data().toString(), QStringLiteral("int after = 2;"));
    QVERIFY(table->model()->index(3, 3).data(Qt::BackgroundRole).isValid());
    QVERIFY(table->model()->index(3, 3).data(Qt::BackgroundRole).value<QColor>() !=
            QColor(palette.panelBackground));
    QCOMPARE(table->model()->index(3, 3).data(Qt::ForegroundRole).value<QColor>(),
             QColor(palette.textMain));
    QVERIFY(table->model()->index(3, 3).data(Qt::ForegroundRole).value<QColor>() !=
            QColor(palette.positive));

    const QVariantList syntaxColors =
        table->model()->index(3, 3).data(Qt::UserRole + 1).toList();
    QVERIFY(syntaxColors.contains(QColor(palette.accent)));

    auto customTypography = ketplus::EditorSettings::defaults();
    customTypography.fontSizePixels = 15;
    customTypography.lineHeightPixels = 28;
    view.applyEditorSettings(customTypography);
    const QFont customDiffFont = table->model()->index(1, 3).data(Qt::FontRole).value<QFont>();
    QCOMPARE(customDiffFont.pixelSize(), 15);
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 28);

    view.resize(800, 320);
    view.show();
    qApp->processEvents();
    QImage rendered(table->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    table->viewport()->render(&rendered);
    const int backgroundX = table->columnViewportPosition(3) + table->columnWidth(3) - 12;
    const int deletionY = table->rowViewportPosition(2) + table->rowHeight(2) / 2;
    QCOMPARE(rendered.pixelColor(backgroundX, deletionY).rgba(),
             table->model()->index(2, 3).data(Qt::BackgroundRole).value<QColor>().rgba());

    bool foundStats = false;
    for (const auto* label : view.findChildren<QLabel*>()) {
        if (label->property("kvRole") == QStringLiteral("diffStats")) {
            QCOMPARE(label->text(), QString::fromUtf8("+1  −1"));
            foundStats = true;
        }
    }
    QVERIFY(foundStats);
}

QTEST_MAIN(GitDiffViewTest)
#include "GitDiffViewTest.moc"
