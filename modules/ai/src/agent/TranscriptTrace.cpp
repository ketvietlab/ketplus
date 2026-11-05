#include "agent/TranscriptTrace.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

namespace ketplus::transcript_trace {
namespace {

struct Sink final {
    Sink() {
        const auto path = qEnvironmentVariable("KETPLUS_CHAT_LAYOUT_TRACE");
        if (path.isEmpty())
            return;
        file.setFileName(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            qWarning("Cannot open chat layout trace: %s", qPrintable(file.errorString()));
            return;
        }
        clock.start();
        // Per-frame records fill the default budget in seconds, and a capture
        // that stops early reads as a scroll that settled. Raise it for a
        // measurement session rather than editing this line.
        limit =
            qint64(
                qEnvironmentVariableIntegerValue("KETPLUS_CHAT_LAYOUT_TRACE_MAX_MB").value_or(8)) *
            1024 * 1024;
    }
    QMutex mutex;
    QFile file;
    QElapsedTimer clock;
    quint64 sequence{0};
    qint64 limit{8 * 1024 * 1024};
    bool truncated{false};
};

Sink& sink() {
    static Sink instance;
    return instance;
}

} // namespace

bool enabled() { return sink().file.isOpen(); }

void write(const QString& event, QVariantMap values) {
    auto& trace = sink();
    if (!trace.file.isOpen())
        return;
    QMutexLocker lock(&trace.mutex);
    if (trace.file.size() >= trace.limit) {
        // Silence here would be indistinguishable from a transcript that stopped
        // moving, which is precisely the thing being measured.
        if (!trace.truncated) {
            trace.truncated = true;
            qWarning("Chat layout trace reached %lld MB and stopped; raise "
                     "KETPLUS_CHAT_LAYOUT_TRACE_MAX_MB to capture a longer run",
                     trace.limit / (1024 * 1024));
        }
        return;
    }
    values.insert(QStringLiteral("event"), event);
    values.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    values.insert(QStringLiteral("sequence"), ++trace.sequence);
    values.insert(QStringLiteral("elapsedUs"), trace.clock.nsecsElapsed() / 1000);
    if (QCoreApplication::instance() && QThread::currentThread() != qApp->thread())
        values.insert(QStringLiteral("worker"), true);
    trace.file.write(QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact) + '\n');
    // Flush on the last event a presented frame can emit, and on every data
    // event, so a run that is cut short (crash, kill) keeps what led up to it.
    if (event != QStringLiteral("gesture-frame") && event != QStringLiteral("text-height"))
        trace.file.flush();
}

} // namespace ketplus::transcript_trace
