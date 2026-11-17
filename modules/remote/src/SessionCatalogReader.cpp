#include "SessionCatalogReader.h"

#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

namespace ketplus::remote {
namespace {
class Reader final : public QObject {
  public:
    Reader(DaemonClient* client, QObject* context, std::function<bool()> current,
           std::function<void(const QJsonArray&, bool)> page,
           DaemonClient::ResponseCallback finished)
        : QObject(context), client_(client), current_(std::move(current)), page_(std::move(page)),
          finished_(std::move(finished)) {}

    // Reads the live catalog only, without the deletion walk.
    void startLiveOnly() {
        liveOnly_ = true;
        live_ = true;
    }

    void request() {
        if (!client_ || !current_()) {
            deleteLater();
            return;
        }
        QJsonObject payload{{"deleted_sessions_page_version", 1}};
        if (liveOnly_ || !liveAfter_.isEmpty()) {
            payload.insert("session_catalog_live_only", true);
        } else {
            payload.insert("deleted_sessions_only", !live_);
            if (!after_.isEmpty())
                payload.insert("deleted_sessions_after", after_);
            if (!snapshot_.isEmpty())
                payload.insert("deleted_sessions_snapshot", snapshot_);
        }
        if (live_) {
            payload.insert("live_sessions_page_version", 1);
            if (!liveAfter_.isEmpty())
                payload.insert("live_sessions_after", liveAfter_);
        }
        const QPointer<Reader> self(this);
        client_->call("session.list", payload, {}, -1,
                      [self](const QJsonObject& result, const QString& error) {
                          if (self)
                              self->received(result, error);
                      });
    }

  private:
    void finish(const QJsonObject& result, const QString& error) {
        auto callback = std::move(finished_);
        deleteLater();
        callback(result, error);
    }

    void received(const QJsonObject& result, const QString& error) {
        if (!current_()) {
            deleteLater();
            return;
        }
        if (!error.isEmpty()) {
            if (result.value("error_code") == "session_catalog_changed" && restarts_++ < 3) {
                after_ = {};
                snapshot_.clear();
                live_ = liveOnly_;
                liveAfter_.clear();
                liveRows_ = {};
                QTimer::singleShot(0, this, &Reader::request);
            } else
                finish({}, error);
            return;
        }
        if (liveOnly_ || !liveAfter_.isEmpty()) {
            if (!result.value("sessions").isArray()) {
                finish({}, "Invalid daemon session page.");
                return;
            }
            livePage(result);
            return;
        }
        if (snapshot_.isEmpty() && !result.contains("deleted_sessions_page_version")) {
            finish(result, {}); // Old daemons ignore the opt-in and return a complete catalog.
            return;
        }
        const auto fail = [&] { finish({}, "Invalid daemon tombstone page."); };
        const auto rows = result.value("deleted_sessions").toArray();
        const auto token = result.value("deleted_sessions_snapshot").toString();
        if (result.value("deleted_sessions_page_version").toInteger(-1) != 1 || token.isEmpty() ||
            token.size() > 128 || (!snapshot_.isEmpty() && token != snapshot_) ||
            !result.value("deleted_sessions").isArray() || rows.size() > 128 ||
            QJsonDocument(rows).toJson(QJsonDocument::Compact).size() > 64 * 1024 ||
            !result.value("deleted_sessions_next").isObject()) {
            fail();
            return;
        }
        auto previous = after_;
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const auto id = row.value("session_id").toString().toUtf8();
            const auto generation = row.value("session_generation").toInteger(-1);
            const auto previousId = previous.value("session_id").toString().toUtf8();
            if (row.size() != 2 || id.isEmpty() || id.size() > 1024 || generation < 1 ||
                (!previous.isEmpty() &&
                 (id < previousId ||
                  (id == previousId &&
                   generation <= previous.value("session_generation").toInteger())))) {
                fail();
                return;
            }
            previous = row;
        }
        const auto next = result.value("deleted_sessions_next").toObject();
        if (!next.isEmpty() && (rows.isEmpty() || next != previous)) {
            fail();
            return;
        }
        if (live_) {
            if (!result.value("sessions").isArray()) {
                fail();
                return;
            }
            livePage(result);
            return;
        }
        const QPointer<Reader> self(this);
        page_(rows, snapshot_.isEmpty());
        if (!self)
            return;
        snapshot_ = token;
        after_ = next;
        live_ = next.isEmpty();
        QTimer::singleShot(0, this, &Reader::request);
    }

    // One page of live sessions: keeps its rows and asks for the next page, or returns the
    // first page's catalog with every row. A daemon without pages returns them all at once.
    void livePage(const QJsonObject& result) {
        const auto rows = result.value("sessions").toArray();
        if (!result.contains("live_sessions_page_version")) {
            if (liveAfter_.isEmpty()) {
                finish(result, {});
                return;
            }
            finish({}, "Invalid daemon session page.");
            return;
        }
        QByteArray previous = liveAfter_.toUtf8();
        for (const auto& value : rows) {
            const auto id = value.toObject().value("session_id").toString().toUtf8();
            if (id.isEmpty() || id <= previous) {
                finish({}, "Invalid daemon session page.");
                return;
            }
            previous = id;
        }
        const auto next = result.value("live_sessions_next");
        if (result.value("live_sessions_page_version").toInteger(-1) != 1 || !next.isString() ||
            (!next.toString().isEmpty() &&
             (rows.isEmpty() || next.toString().toUtf8() != previous))) {
            finish({}, "Invalid daemon session page.");
            return;
        }
        if (liveAfter_.isEmpty())
            first_ = result;
        for (const auto& value : rows)
            liveRows_.append(value);
        if (next.toString().isEmpty()) {
            auto complete = first_;
            complete.insert("sessions", liveRows_);
            complete.remove("live_sessions_next");
            finish(complete, {});
            return;
        }
        liveAfter_ = next.toString();
        QTimer::singleShot(0, this, &Reader::request);
    }

    QPointer<DaemonClient> client_;
    std::function<bool()> current_;
    std::function<void(const QJsonArray&, bool)> page_;
    DaemonClient::ResponseCallback finished_;
    QJsonObject after_;
    QString snapshot_;
    int restarts_{0};
    bool live_{false};
    bool liveOnly_{false};
    QString liveAfter_;
    QJsonArray liveRows_;
    QJsonObject first_;
};
} // namespace

void readSessionCatalog(DaemonClient* client, QObject* context, std::function<bool()> current,
                        std::function<void(const QJsonArray&, bool)> page,
                        DaemonClient::ResponseCallback finished) {
    auto* reader =
        new Reader(client, context, std::move(current), std::move(page), std::move(finished));
    reader->request();
}

void readLiveSessions(DaemonClient* client, QObject* context, std::function<bool()> current,
                      DaemonClient::ResponseCallback finished) {
    auto* reader = new Reader(
        client, context, std::move(current), [](const QJsonArray&, bool) {}, std::move(finished));
    reader->startLiveOnly();
    reader->request();
}

} // namespace ketplus::remote
