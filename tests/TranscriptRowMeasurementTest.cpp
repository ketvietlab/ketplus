#include "agent/AgentMessageModel.h"
#include "agent/ChatMarkdown.h"
#include "agent/TranscriptRowGeometry.h"
#include "agent/TranscriptRowMeasurer.h"

#include <QSignalSpy>
#include <QTextDocument>
#include <QtTest>

using ketplus::TranscriptRowGeometry;
using ketplus::TranscriptRowMeasurer;

namespace {

QString paragraphs(const int count) {
    QString body;
    for (int index = 0; index < count; ++index)
        body += QStringLiteral("Paragraph %1 with enough words in it to wrap across more than a "
                               "single line at a narrow width.\n\n")
                    .arg(index);
    return body;
}

// What the delegate would arrive at for the same body, so the worker's answer
// can be held to the same standard rather than to itself.
qreal onThisThread(const QString& body, const bool markdown, const qreal width, const QFont& font) {
    QTextDocument document;
    document.setDefaultFont(font);
    if (markdown)
        document.setMarkdown(ketplus::chat_markdown::escapeRawHtml(body));
    else
        document.setPlainText(body);
    document.setTextWidth(width);
    return document.size().height();
}

} // namespace

class TranscriptRowMeasurementTest final : public QObject {
    Q_OBJECT
  private slots:
    void workerAgreesWithTheGuiThread();
    void bodiesNeedingGuiResourcesAreNotMeasured();
    void resultsFromAnOldGeometryAreDropped();
    void aMeasuredBodyIsNotARowHeightUntilChromeIsKnown();
    void chromeLearnedFromOneRowSizesItsWholeShape();
    void realizedHeightOutranksADerivedOne();
    void unmeasuredKeysReportsNewestFirst();
    void idleMeasurementExhaustsTextAndSkipsResourceBodies();
    void premeasuredPageMatchesTheGuiThread();
    void cachedPageTracksBodyFormatAndGeometryChanges();
};

// The worker is only useful if it is not a second opinion. A height that merely
// resembles the delegate's would put the geometry model confidently out of step
// with what the reader sees.
void TranscriptRowMeasurementTest::workerAgreesWithTheGuiThread() {
    TranscriptRowMeasurer measurer;
    QSignalSpy measured(&measurer, &TranscriptRowMeasurer::bodyMeasured);
    const QFont font;
    const QList<TranscriptRowMeasurer::Request> requests{
        {QStringLiteral("plain"), paragraphs(4), false},
        {QStringLiteral("markdown"), QStringLiteral("# Heading\n\n") + paragraphs(6), true},
        {QStringLiteral("short"), QStringLiteral("ok"), false},
        // An unclosed tag would hide the text after it if the worker parsed raw HTML.
        {QStringLiteral("html"), QStringLiteral("A \"<title>\" placeholder. ") + paragraphs(4),
         true},
    };
    measurer.measure(requests, 620, font);
    QTRY_COMPARE(measured.size(), 4);

    for (const QList<QVariant>& result : measured) {
        const QString key = result.at(0).toString();
        const auto request = std::find_if(
            requests.cbegin(), requests.cend(),
            [&key](const TranscriptRowMeasurer::Request& entry) { return entry.key == key; });
        QVERIFY(request != requests.cend());
        QCOMPARE(result.at(1).toDouble(),
                 onThisThread(request->body, request->markdown, 620, font));
        // As tall as the same answer without the tag, not just its first line.
        if (key == QStringLiteral("html"))
            QCOMPARE(result.at(1).toDouble(),
                     onThisThread(QStringLiteral("A \"[title]\" placeholder. ") + paragraphs(4),
                                  true, 620, font));
    }
}

void TranscriptRowMeasurementTest::bodiesNeedingGuiResourcesAreNotMeasured() {
    // Images resolve through the document's resource handler, which belongs to
    // the GUI thread. Measuring them without their picture would return a
    // confident, wrong number. A Mermaid fence is source until the reader asks
    // for the diagram, so it measures like any other code block.
    QVERIFY(!TranscriptRowMeasurer::measurable(QStringLiteral("see ![shot](a.png)")));
    QVERIFY(TranscriptRowMeasurer::measurable(QStringLiteral("```mermaid\ngraph TD\n```")));
    QVERIFY(!TranscriptRowMeasurer::measurable(QStringLiteral("<img src=\"a.png\">")));
    QVERIFY(!TranscriptRowMeasurer::measurable(QString{}));
    QVERIFY(TranscriptRowMeasurer::measurable(QStringLiteral("plain enough")));

    TranscriptRowMeasurer measurer;
    QSignalSpy measured(&measurer, &TranscriptRowMeasurer::bodyMeasured);
    measurer.measure({{QStringLiteral("img"), QStringLiteral("![a](a.png)"), true}}, 620, QFont{});
    QTest::qWait(50);
    QCOMPARE(measured.size(), 0);
    QCOMPARE(measurer.outstanding(), 0);
}

void TranscriptRowMeasurementTest::resultsFromAnOldGeometryAreDropped() {
    TranscriptRowMeasurer measurer;
    QSignalSpy measured(&measurer, &TranscriptRowMeasurer::bodyMeasured);
    measurer.measure({{QStringLiteral("a"), paragraphs(40), false}}, 900, QFont{});
    // The reader resizes while that is still in flight. A height for 900 px is
    // not an approximation of one for 400 px, so it must not arrive as if it
    // were.
    measurer.setGeometry(400, QFont{});
    QTest::qWait(80);
    QCOMPARE(measured.size(), 0);
}

void TranscriptRowMeasurementTest::aMeasuredBodyIsNotARowHeightUntilChromeIsKnown() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset({QStringLiteral("a")}, {QStringLiteral("assistant")});

    geometry.setBodyHeight(QStringLiteral("a"), 800);
    // Nothing has told us what surrounds a body of this shape yet, and inventing
    // the difference is how an estimate creeps back in.
    QCOMPARE(geometry.height(0), 100.0);
    QVERIFY(!geometry.measured(0));
}

void TranscriptRowMeasurementTest::chromeLearnedFromOneRowSizesItsWholeShape() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset({QStringLiteral("a"), QStringLiteral("b")},
                   {QStringLiteral("assistant"), QStringLiteral("assistant")});

    // One realized row: 520 px of which 480 was body, so this shape carries 40 px
    // of header, padding and spacing.
    geometry.setRowHeight(QStringLiteral("a"), 520, 480);
    QCOMPARE(geometry.height(0), 520.0);

    // b was never realized, but its body was measured and it is the same shape.
    geometry.setBodyHeight(QStringLiteral("b"), 1200);
    QCOMPARE(geometry.height(1), 1240.0);
    QVERIFY(geometry.measured(1));

    // A different shape does not borrow it.
    geometry.reset({QStringLiteral("c")}, {QStringLiteral("activity")});
    geometry.setBodyHeight(QStringLiteral("c"), 1200);
    QCOMPARE(geometry.height(0), 100.0);
}

void TranscriptRowMeasurementTest::realizedHeightOutranksADerivedOne() {
    TranscriptRowGeometry geometry;
    geometry.setProvisionalHeight(100);
    geometry.reset({QStringLiteral("a"), QStringLiteral("b")},
                   {QStringLiteral("assistant"), QStringLiteral("assistant")});
    geometry.setRowHeight(QStringLiteral("a"), 520, 480);
    geometry.setBodyHeight(QStringLiteral("b"), 1200);
    QCOMPARE(geometry.height(1), 1240.0);

    // The row lays itself out and disagrees with what we derived. The row is
    // right: chrome was learned from a different row and only ever approximated
    // this one.
    geometry.setRowHeight(QStringLiteral("b"), 1263, 1200);
    QCOMPARE(geometry.height(1), 1263.0);

    // And a later body measurement does not undo it.
    geometry.setBodyHeight(QStringLiteral("b"), 1200);
    QCOMPARE(geometry.height(1), 1263.0);
}

void TranscriptRowMeasurementTest::unmeasuredKeysReportsNewestFirst() {
    TranscriptRowGeometry geometry;
    geometry.reset({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")},
                   {QStringLiteral("s"), QStringLiteral("s"), QStringLiteral("s")});
    // Newest first: the reader is at the tail, so those rows are worth knowing
    // about before the ones they would have to scroll to reach.
    QCOMPARE(geometry.unmeasuredKeys(2), QStringList({QStringLiteral("c"), QStringLiteral("b")}));

    geometry.setRowHeight(QStringLiteral("c"), 300, 280);
    QCOMPARE(geometry.unmeasuredKeys(3), QStringList({QStringLiteral("b"), QStringLiteral("a")}));

    geometry.setBodyHeight(QStringLiteral("b"), 500);
    QCOMPARE(geometry.unmeasuredKeys(3), QStringList({QStringLiteral("a")}));
}

void TranscriptRowMeasurementTest::idleMeasurementExhaustsTextAndSkipsResourceBodies() {
    ketplus::AgentMessageModel model;
    model.reset(
        {QVariantMap{{"kind", "assistant"}, {"messageKey", "text"}, {"body", paragraphs(3)}},
         QVariantMap{
             {"kind", "assistant"}, {"messageKey", "image"}, {"body", "![preview](missing.png)"}}});
    // An unmeasurable newest row must not starve an older text row at a one-row limit.
    QVERIFY(model.measureUnmeasuredRows(1, 400, QFont{}));
    QTRY_VERIFY(!model.measureUnmeasuredRows(1, 400, QFont{}));
    // A new layout invalidates the previous measurements and makes work available again.
    model.geometry()->setLayoutFingerprint(QStringLiteral("narrow"));
    QVERIFY(model.measureUnmeasuredRows(1, 240, QFont{}));
    QTRY_VERIFY(!model.measureUnmeasuredRows(1, 240, QFont{}));
    model.append({{"kind", "assistant"}, {"messageKey", "new"}, {"body", paragraphs(2)}});
    QVERIFY(model.measureUnmeasuredRows(1, 240, QFont{}));
    QTRY_VERIFY(!model.measureUnmeasuredRows(1, 240, QFont{}));
}

// A page measured before it reaches the model is only worth having if each row
// comes back with the height its delegate would find, and a body that has
// changed since is not given the old one.
void TranscriptRowMeasurementTest::premeasuredPageMatchesTheGuiThread() {
    TranscriptRowMeasurer measurer;
    const QFont font;
    // The view asking for measurements is what tells the worker the geometry.
    measurer.measure({}, 540, font);
    const QString assistant = QStringLiteral("## Result\n\n") + paragraphs(5);
    const QString user = paragraphs(2);
    const QVariantList page{
        QVariantMap{{"messageKey", "a1"}, {"kind", "assistant"}, {"body", assistant}},
        QVariantMap{{"messageKey", "u1"}, {"kind", "user"}, {"body", user}},
        QVariantMap{{"messageKey", "img"}, {"kind", "assistant"}, {"body", "![shot](a.png)"}},
    };
    QObject context;
    bool done = false;
    TranscriptRowMeasurer::premeasure(page, &context, [&done] { done = true; });
    QTRY_VERIFY(done);

    qreal height = -1;
    QVERIFY(TranscriptRowMeasurer::cachedBodyHeight("a1", assistant, true, &height));
    QCOMPARE(height, onThisThread(assistant, true, 540, font));
    QVERIFY(TranscriptRowMeasurer::cachedBodyHeight("u1", user, false, &height));
    QCOMPARE(height, onThisThread(user, false, 540, font));
    QVERIFY(!TranscriptRowMeasurer::cachedBodyHeight("u1", user + "more", false, &height));
    QVERIFY(!TranscriptRowMeasurer::cachedBodyHeight("img", "![shot](a.png)", true, &height));

    // A new width is a different question.
    measurer.measure({}, 700, font);
    QVERIFY(!TranscriptRowMeasurer::cachedBodyHeight("a1", assistant, true, &height));
}

void TranscriptRowMeasurementTest::cachedPageTracksBodyFormatAndGeometryChanges() {
    TranscriptRowMeasurer measurer;
    QFont font;
    font.setPixelSize(16);
    QObject context;
    QVariantMap row{{"messageKey", "cached"},
                    {"kind", "assistant"},
                    {"body", QStringLiteral("## Title\n\n") + paragraphs(3)}};
    qreal width = 540;
    const auto check = [&] {
        measurer.setGeometry(width, font);
        bool done = false;
        TranscriptRowMeasurer::premeasure({row}, &context, [&] { done = true; });
        QTRY_VERIFY(done);
        const QString body = row.value("body").toString();
        const bool markdown = !row.value("partialBody").toBool();
        qreal height = -1;
        QVERIFY(TranscriptRowMeasurer::cachedBodyHeight("cached", body, markdown, &height));
        QCOMPARE(height, onThisThread(body, markdown, width, font));
    };
    check();
    check(); // Unchanged cached page.
    row["body"] = row.value("body").toString() + paragraphs(5);
    check();
    row["partialBody"] = true;
    check();
    width = 360;
    check();
    font.setPixelSize(20);
    check();
}

QTEST_MAIN(TranscriptRowMeasurementTest)
#include "TranscriptRowMeasurementTest.moc"
