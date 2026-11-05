#include "agent/TranscriptRowMeasurer.h"

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentMessageModel.h"
#include "agent/ChatMarkdown.h"

#include <QMutex>
#include <QPointer>
#include <QRunnable>
#include <QTextDocument>
#include <QThreadPool>
#include <QTimer>

namespace ketplus {
namespace {

// A body that would pull in a picture cannot be measured away from the GUI
// thread, and measuring it without the picture would produce a confidently
// wrong number. Markdown images are the way a transcript body does that. A
// Mermaid fence is not one until the reader switches that block to its diagram;
// until then it is ordinary source, which measures like any other code block.
bool referencesResource(const QString& body) {
    return body.contains(QStringLiteral("!["), Qt::CaseSensitive) ||
           body.contains(QStringLiteral("<img"), Qt::CaseInsensitive);
}

qreal bodyHeight(const QString& body, const bool markdown, const qreal width, const QFont& font) {
    QTextDocument document;
    document.setDefaultFont(font);
    // Match the delegate's HTML escaping and bare-URL autolinks when measuring wrapped rows.
    if (markdown)
        document.setMarkdown(
            agent_message::qtMarkdownSource(chat_markdown::prepareForDisplay(body)));
    else
        document.setPlainText(body);
    document.setTextWidth(width);
    return document.size().height();
}

// The geometry the view last measured at, and what was pre-measured for it.
// GUI-thread state: workers receive copies and report back through a queued
// call.
struct SharedMeasurements final {
    qreal width{0};
    QFont font;
    quint64 generation{0};
    struct Entry final {
        size_t bodyHash;
        bool markdown;
        qreal height;
    };
    QHash<QString, Entry> heights;
};

SharedMeasurements& shared() {
    static SharedMeasurements measurements;
    return measurements;
}

constexpr qsizetype kMaximumCachedHeights = 16000;

void setSharedGeometry(const qreal width, const QFont& font) {
    auto& measurements = shared();
    if (qFuzzyCompare(measurements.width, width) && measurements.font == font)
        return;
    measurements.width = width;
    measurements.font = font;
    ++measurements.generation;
    measurements.heights.clear();
}

class MeasureTask final : public QRunnable {
  public:
    MeasureTask(QString key, QString body, bool markdown, qreal width, QFont font,
                quint64 generation, std::function<void(QString, qreal, quint64)> report)
        : key_(std::move(key)), body_(std::move(body)), markdown_(markdown), width_(width),
          font_(std::move(font)), generation_(generation), report_(std::move(report)) {
        setAutoDelete(true);
    }

    void run() override { report_(key_, bodyHeight(body_, markdown_, width_, font_), generation_); }

  private:
    QString key_;
    QString body_;
    bool markdown_;
    qreal width_;
    QFont font_;
    quint64 generation_;
    std::function<void(QString, qreal, quint64)> report_;
};

} // namespace

TranscriptRowMeasurer::TranscriptRowMeasurer(QObject* parent) : QObject(parent) {}

bool TranscriptRowMeasurer::measurable(const QString& body) {
    return !body.isEmpty() && !referencesResource(body);
}

bool TranscriptRowMeasurer::markdownBody(const QVariantMap& item) {
    return item.value(QStringLiteral("kind")).toString() == QStringLiteral("assistant") &&
           !item.value(QStringLiteral("partialBody")).toBool();
}

bool TranscriptRowMeasurer::cachedBodyHeight(const QString& key, const QString& body,
                                             const bool markdown, qreal* height) {
    const auto found = shared().heights.constFind(key);
    if (found == shared().heights.cend() || found->markdown != markdown ||
        found->bodyHash != qHash(body))
        return false;
    *height = found->height;
    return true;
}

void TranscriptRowMeasurer::premeasure(const QVariantList& sourceEntries, QObject* context,
                                       std::function<void()> done) {
    const auto& measurements = shared();
    if (!(measurements.width > 0) || sourceEntries.isEmpty()) {
        QMetaObject::invokeMethod(context, std::move(done), Qt::QueuedConnection);
        return;
    }
    struct Row final {
        QString key;
        size_t bodyHash;
        bool markdown;
        qreal height;
    };
    struct Batch final {
        QMutex mutex;
        QList<Row> rows;
        int outstanding{0};
    };
    auto batch = std::make_shared<Batch>();
    const qreal width = measurements.width;
    const QFont font = measurements.font;
    const quint64 generation = measurements.generation;
    // Implicitly shared, immutable worker snapshot. Pages already measured by
    // read-ahead must not parse and lay out every body again at publication.
    const auto cachedHeights = measurements.heights;
    QPointer<QObject> guard(context);
    const auto finish = [batch, guard, generation, done = std::move(done)] {
        QMetaObject::invokeMethod(
            guard,
            [batch, generation, done] {
                auto& target = shared();
                if (target.generation == generation) {
                    if (target.heights.size() + batch->rows.size() > kMaximumCachedHeights)
                        target.heights.clear();
                    for (const Row& row : std::as_const(batch->rows))
                        target.heights.insert(row.key, {row.bodyHash, row.markdown, row.height});
                }
                done();
            },
            Qt::QueuedConnection);
    };
    // A few entries a task: enough to share a page across the pool, few enough
    // that one long body does not hold the others back.
    constexpr qsizetype kEntriesPerTask = 4;
    const qsizetype tasks = (sourceEntries.size() + kEntriesPerTask - 1) / kEntriesPerTask;
    batch->outstanding = int(tasks);
    for (qsizetype task = 0; task < tasks; ++task) {
        const QVariantList entries = sourceEntries.mid(task * kEntriesPerTask, kEntriesPerTask);
        const int firstIndex = int(task * kEntriesPerTask);
        QThreadPool::globalInstance()->start([batch, entries, firstIndex, width, font, finish,
                                              cachedHeights] {
            QList<Row> rows;
            for (int index = 0; index < entries.size(); ++index) {
                const auto rendered =
                    AgentMessageModel::renderedItems(entries.at(index).toMap(), firstIndex + index);
                for (const QVariantMap& item : rendered) {
                    const QString body = item.value(QStringLiteral("body")).toString();
                    if (!measurable(body))
                        continue;
                    const bool markdown = markdownBody(item);
                    const QString key = item.value(QStringLiteral("messageKey")).toString();
                    const auto hash = qHash(body);
                    const auto cached = cachedHeights.constFind(key);
                    if (cached != cachedHeights.cend() && cached->bodyHash == hash &&
                        cached->markdown == markdown)
                        continue;
                    rows.append({key, hash, markdown, bodyHeight(body, markdown, width, font)});
                }
            }
            QMutexLocker lock(&batch->mutex);
            batch->rows += rows;
            if (--batch->outstanding == 0) {
                lock.unlock();
                finish();
            }
        });
    }
}

void TranscriptRowMeasurer::setGeometry(const qreal width, const QFont& font) {
    setSharedGeometry(width, font);
    if (qFuzzyCompare(width_, width) && font_ == font)
        return;
    width_ = width;
    font_ = font;
    ++generation_;
    // Results still being computed belong to the old geometry. They are left to
    // finish and dropped on arrival rather than cancelled, because a half-torn
    // document is worse than a wasted millisecond.
    inFlight_.clear();
}

void TranscriptRowMeasurer::deliver(const QString& key, const qreal height,
                                    const quint64 generation) {
    if (generation != generation_)
        return;
    inFlight_.remove(key);
    if (height >= 0)
        emit bodyMeasured(key, height);
}

void TranscriptRowMeasurer::measure(const QList<Request>& requests, const qreal width,
                                    const QFont& font) {
    setGeometry(width, font);
    if (!(width_ > 0))
        return;
    QPointer<TranscriptRowMeasurer> self(this);
    const quint64 generation = generation_;
    for (const Request& request : requests) {
        if (request.key.isEmpty() || !measurable(request.body) || inFlight_.contains(request.key))
            continue;
        inFlight_.insert(request.key);
        auto report = [self](QString key, qreal height, quint64 taskGeneration) {
            // Back to the thread that owns the geometry model before anything
            // reads the result.
            QMetaObject::invokeMethod(
                self,
                [self, key = std::move(key), height, taskGeneration] {
                    if (self)
                        self->deliver(key, height, taskGeneration);
                },
                Qt::QueuedConnection);
        };
        QThreadPool::globalInstance()->start(new MeasureTask(request.key, request.body,
                                                             request.markdown, width_, font_,
                                                             generation, std::move(report)));
    }
}

} // namespace ketplus
