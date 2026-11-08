#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace ketplus::daemon {

// A provider message too large to parse. `fields` holds the scalar values of the requested
// paths (for example "id", "method" or "params.threadId") that appeared in the message.
struct OversizedProviderLine final {
    qsizetype bytes{0};
    QHash<QString, QJsonValue> fields;
};

// Splits a provider's newline-delimited JSON output into lines. A line longer than the limit is
// discarded as it streams in instead of being buffered, and reported once its newline arrives,
// so one oversized message costs only itself: the shared provider process and every other
// conversation on it keep running.
class ProviderLineReader final {
  public:
    using LineHandler = std::function<void(const QByteArray&)>;
    using OversizedHandler = std::function<void(const OversizedProviderLine&)>;

    ProviderLineReader(qsizetype maximumLineBytes, QStringList fieldPaths);

    // Handlers may call clear(); the remaining input is then dropped.
    void append(const QByteArray& data, const LineHandler& line, const OversizedHandler& oversized);
    void clear();

  private:
    struct Frame final {
        bool object{false};
        bool expectKey{false};
        QString key;
    };

    void resetScan();
    void scan(const QByteArray& data);
    void finishScalar();
    void finishValue(const QJsonValue& value);
    [[nodiscard]] QString valuePath() const;

    qsizetype maximumLineBytes_;
    QStringList fieldPaths_;
    QByteArray buffer_;
    quint64 generation_{0};

    // Streaming scan of the line being discarded.
    bool discarding_{false};
    bool routingValid_{true};
    OversizedProviderLine discarded_;
    QList<Frame> frames_;
    quint64 skippedDepth_{0};
    bool inString_{false};
    bool escaped_{false};
    bool stringIsKey_{false};
    bool capturing_{false};
    QByteArray token_;
    QByteArray scalar_;
};

} // namespace ketplus::daemon
