#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <functional>

namespace ketplus {

// Details are transient. Closing a group releases them and fences late replies; summaries
// remain canonical transcript rows. Only one page per group may be in flight.
class ActivityGroupLoader final : public QObject {
    Q_OBJECT
  public:
    using Reply = std::function<void(const QJsonObject&, const QString&)>;
    using Reader = std::function<void(const QString&, const QJsonObject&, Reply)>;
    explicit ActivityGroupLoader(Reader reader, QObject* parent = nullptr);
    // `membership` is the daemon's cursor of the last step that left the group; when it
    // advances, details already loaded are dropped and read again without that step.
    void setOpen(const QString& session, const QString& group, qint64 watermark, bool open,
                 qint64 membership = 0);
    void updateSummary(const QString& session, const QString& group, qint64 watermark,
                       qint64 membership = 0);
    void closeExcept(const QString& session);
    QVariantMap details(const QString& session, const QString& group) const;

  signals:
    void changed(const QString& session, const QString& group);

  private:
    struct State {
        QString session, group;
        quint64 epoch{0};
        bool open{false}, inFlight{false};
        qint64 applied{0}, desired{0}, snapshot{-1}, revision{-1}, membership{0};
        QVariantList rows;
        QString error;
    };
    static QString key(const QString& session, const QString& group);
    void request(const QString& key);
    Reader reader_;
    QHash<QString, State> states_;
};
} // namespace ketplus
