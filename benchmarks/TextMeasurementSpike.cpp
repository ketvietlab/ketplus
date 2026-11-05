// Does shaping once and re-wrapping many times actually pay, or would we be
// rewriting QTextDocument by hand?
//
// Stage 2 of the owned-layout plan rests on one claim borrowed from Telegram's
// text engine: parse and shape a message once, then answer "how tall at width
// W" with a line-breaking pass over pre-shaped runs. Telegram's Ui::Text::String
// builds _blocks/_words once and countHeight(width) only re-walks them
// (scratchpad/refs/lib_ui/ui/text/text.h:288-309,519-526). If that separation
// does not hold for us, the 1,400 lines Stage 2 would add buy nothing, and the
// honest move is to find out before writing them.
//
// Measured here, on synthetic bodies sized like the real corpus (p99 5,947
// characters, max 12,780):
//   A. QTextDocument: build + measure at one width, then measure at a new width.
//   B. QTextLayout: shape once, then wrap at each width.
// The number that decides Stage 2 is B's re-wrap, which must be far cheaper than
// its own shape and than A's rebuild. The plan's gate is under 5 ms for a full
// 300-row window at a new width.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QtGlobal>

#include <cstdio>
#include <vector>

namespace {

// Bodies shaped like the measured corpus rather than uniform filler: mostly
// short, a long tail, with the paragraph structure real messages have.
QStringList corpus(const int bodies) {
    QStringList result;
    result.reserve(bodies);
    quint32 seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    const QString sentence =
        QStringLiteral("The transcript window keeps a bounded slice of the conversation so every "
                       "row in it can afford to be measured exactly rather than estimated. ");
    for (int index = 0; index < bodies; ++index) {
        const int characters = index % 20 == 0 ? 12000 : 400 + int(next() % 5600);
        QString body;
        body.reserve(characters + 64);
        while (body.size() < characters) {
            body += sentence;
            if (next() % 5 == 0)
                body += QStringLiteral("\n\n");
        }
        result.append(body);
    }
    return result;
}

QList<QStringList> paragraphsOf(const QStringList& bodies) {
    QList<QStringList> result;
    result.reserve(bodies.size());
    for (const QString& body : bodies)
        result.append(body.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts));
    return result;
}

qreal wrap(QTextLayout& layout, const qreal width, const qreal leading) {
    layout.beginLayout();
    qreal height = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        line.setPosition(QPointF(0, height));
        height += line.height() + leading;
    }
    layout.endLayout();
    return height;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const int bodies = argc > 1 ? QByteArray(argv[1]).toInt() : 45;
    const qreal firstWidth = 800;
    const qreal secondWidth = 620;

    const QStringList texts = corpus(bodies);
    const QList<QStringList> paragraphs = paragraphsOf(texts);
    const QFont font;
    const qreal leading = QFontMetricsF(font).leading();

    QElapsedTimer timer;

    // A. QTextDocument, which is what the delegate uses today.
    timer.start();
    std::vector<std::unique_ptr<QTextDocument>> documents;
    documents.reserve(texts.size());
    qreal documentTotal = 0;
    for (const QString& text : texts) {
        auto document = std::make_unique<QTextDocument>();
        document->setDefaultFont(font);
        document->setMarkdown(text);
        document->setTextWidth(firstWidth);
        documentTotal += document->size().height();
        documents.push_back(std::move(document));
    }
    const qint64 documentBuild = timer.elapsed();

    timer.restart();
    qreal documentRewrapTotal = 0;
    for (auto& document : documents) {
        document->setTextWidth(secondWidth);
        documentRewrapTotal += document->size().height();
    }
    const qint64 documentRewrap = timer.elapsed();

    // B. QTextLayout, shaped once and wrapped per width.
    timer.restart();
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    for (const QStringList& body : paragraphs)
        for (const QString& paragraph : body) {
            auto layout = std::make_unique<QTextLayout>(paragraph, font);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout->setTextOption(option);
            layouts.push_back(std::move(layout));
        }
    qreal layoutTotal = 0;
    for (auto& layout : layouts)
        layoutTotal += wrap(*layout, firstWidth, leading);
    const qint64 layoutShapeAndWrap = timer.elapsed();

    timer.restart();
    qreal layoutRewrapTotal = 0;
    for (auto& layout : layouts)
        layoutRewrapTotal += wrap(*layout, secondWidth, leading);
    const qint64 layoutRewrap = timer.elapsed();

    // And the same wrap again, to separate one-off cost from steady state.
    timer.restart();
    for (auto& layout : layouts)
        wrap(*layout, firstWidth, leading);
    const qint64 layoutRewrapAgain = timer.elapsed();

    std::printf("bodies=%d paragraphs=%zu\n", bodies, layouts.size());
    std::printf("QTextDocument  build+measure : %lld ms   (height %.0f)\n", documentBuild,
                documentTotal);
    std::printf("QTextDocument  re-measure    : %lld ms   (height %.0f)\n", documentRewrap,
                documentRewrapTotal);
    std::printf("QTextLayout    shape+wrap    : %lld ms   (height %.0f)\n", layoutShapeAndWrap,
                layoutTotal);
    std::printf("QTextLayout    re-wrap       : %lld ms   (height %.0f)\n", layoutRewrap,
                layoutRewrapTotal);
    std::printf("QTextLayout    re-wrap again : %lld ms\n", layoutRewrapAgain);
    return 0;
}
