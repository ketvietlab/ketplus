#pragma once

#include "MachineBackend.h"

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>

#include <functional>

namespace ketplus::test {

// A MachineBackend for Desktop composition tests: it records every request and answers it
// through `answer` (or holds it until release() when `hold` is set), with no transport.
class FakeMachineBackend final : public remote::MachineBackend {
    Q_OBJECT

  public:
    struct Call {
        QString operation;
        QString sessionId;
        qint64 generation{0};
        QJsonObject request;
        Callback callback;
    };
    using Answer = std::function<void(const Call& call, QJsonObject* result, QString* error)>;

    FakeMachineBackend(QString machineId, Route route, QObject* parent = nullptr)
        : MachineBackend(parent), machineId_(std::move(machineId)), route_(route) {}

    [[nodiscard]] QString machineId() const override { return machineId_; }
    [[nodiscard]] Route route() const override { return route_; }
    [[nodiscard]] ConnectionState connectionState() const override { return state_; }
    [[nodiscard]] QString connectionReason() const override { return reason_; }

    void setState(ConnectionState state, const QString& reason = {}) {
        state_ = state;
        reason_ = reason;
        emit connectionStateChanged(state, reason);
    }
    void announceCatalogChange() { emit catalogChanged(); }
    // Delivers a window-shaped event (`event_type`, `payload`) as every route's eventReceived()
    // does: the session.event shape, `payload {event_type, data}`.
    // An event already in the session.event shape is sent as it is.
    void deliver(const QJsonObject& event) {
        if (!event.contains("event_type")) {
            emit eventReceived(event);
            return;
        }
        QJsonObject live = event;
        live.insert("message_type", "session.event");
        live.insert("payload", QJsonObject{{"event_type", event.value("event_type")},
                                           {"data", event.value("payload")}});
        live.remove("event_type");
        emit eventReceived(live);
    }
    void complete(const QString& commandId, const QJsonObject& result, const QString& error = {}) {
        emit commandCompleted(commandId, result, error);
    }

    // Answers every held request in order.
    void release() {
        const QList<Call> held = std::exchange(held_, {});
        for (const Call& call : held)
            respond(call);
    }
    [[nodiscard]] QList<Call> calls(const QString& operation) const {
        QList<Call> matching;
        for (const Call& call : calls_)
            if (call.operation == operation)
                matching.append(call);
        return matching;
    }
    void clearCalls() { calls_.clear(); }

    Answer answer;
    bool hold{false};

    void listRepositories(Callback callback) override {
        record("listRepositories", {}, 0, {}, std::move(callback));
    }
    void listWorktrees(const QString& sessionId, qint64 generation, const QJsonObject& request,
                       Callback callback) override {
        record("listWorktrees", sessionId, generation, request, std::move(callback));
    }
    void listConversations(const QJsonObject& options, Callback callback) override {
        record("listConversations", {}, 0, options, std::move(callback));
    }
    void transcriptWindow(const QString& sessionId, qint64 generation, const QJsonObject& request,
                          Callback callback) override {
        record("transcriptWindow", sessionId, generation, request, std::move(callback));
    }
    void transcriptPage(const QString& sessionId, qint64 generation, const QJsonObject& request,
                        Callback callback) override {
        record("transcriptPage", sessionId, generation, request, std::move(callback));
    }
    void transcriptLargeEvent(const QString& sessionId, qint64 generation,
                              const QJsonObject& request, Callback callback) override {
        record("transcriptLargeEvent", sessionId, generation, request, std::move(callback));
    }
    void createSession(const QJsonObject& request, Callback callback) override {
        record("createSession", {}, 0, request, std::move(callback));
    }
    void prepareSession(const QJsonObject& request, Callback callback) override {
        record("prepareSession", {}, 0, request, std::move(callback));
    }
    void cancelPreparedSession(const QString& id, qint64 generation, Callback callback) override {
        record("cancelPreparedSession", id, generation, {}, std::move(callback));
    }
    void attachmentCommand(AttachmentOperation operation, const QString& id, qint64 generation,
                           const QJsonObject& request, Callback callback) override {
        record(attachmentCommandKind(operation), id, generation, request, std::move(callback));
    }
    void startTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                   Callback callback) override {
        record("startTurn", sessionId, generation, request, std::move(callback));
    }
    void steerTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                   Callback callback) override {
        record("steerTurn", sessionId, generation, request, std::move(callback));
    }
    void stopTurn(const QString& sessionId, qint64 generation, Callback callback) override {
        record("stopTurn", sessionId, generation, {}, std::move(callback));
    }
    void pendingApprovals(const QString& sessionId, qint64 generation, const QString& afterId,
                          Callback callback) override {
        record("pendingApprovals", sessionId, generation, {{"after_id", afterId}},
               std::move(callback));
    }
    void resolveApproval(const QString& sessionId, qint64 generation, const QString& approvalId,
                         const QJsonObject& resolution, Callback callback) override {
        record("resolveApproval", sessionId, generation,
               {{"approval_id", approvalId}, {"resolution", resolution}}, std::move(callback));
    }
    void answerQuestion(const QString& sessionId, qint64 generation, const QString& questionId,
                        const QJsonObject& resolution, Callback callback) override {
        record("answerQuestion", sessionId, generation,
               {{"approval_id", questionId}, {"resolution", resolution}}, std::move(callback));
    }

    void conversationClosed(const QString& sessionId) override { closed.append(sessionId); }
    QStringList closed;

    // The conversation operations this machine offers; the others use the base reason.
    QSet<Feature> features;
    [[nodiscard]] bool supports(Feature feature) const override {
        return state_ == ConnectionState::Connected && features.contains(feature);
    }
    void renameConversation(const QString& sessionId, qint64 generation, const QString& title,
                            Callback callback) override {
        record("renameConversation", sessionId, generation, {{"title", title}},
               std::move(callback));
    }
    void forkConversation(const QString& sessionId, qint64 generation, const QString& forkSessionId,
                          Callback callback) override {
        record("forkConversation", sessionId, generation, {{"fork_session_id", forkSessionId}},
               std::move(callback));
    }
    void setConversationArchived(const QString& sessionId, qint64 generation, bool archived,
                                 Callback callback) override {
        record("setConversationArchived", sessionId, generation, {{"archived", archived}},
               std::move(callback));
    }

  private:
    void record(const QString& operation, const QString& sessionId, qint64 generation,
                const QJsonObject& request, Callback callback) {
        Call call{operation, sessionId, generation, request, std::move(callback)};
        calls_.append(call);
        if (hold)
            held_.append(call);
        else
            respond(call);
    }
    void respond(const Call& call) {
        QJsonObject result;
        QString error;
        if (answer)
            answer(call, &result, &error);
        call.callback(result, error);
    }

    QString machineId_;
    Route route_;
    ConnectionState state_{ConnectionState::Connected};
    QString reason_;
    QList<Call> calls_;
    QList<Call> held_;
};

} // namespace ketplus::test
