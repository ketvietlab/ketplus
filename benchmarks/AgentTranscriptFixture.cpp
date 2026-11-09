#include "AgentTranscriptFixture.h"
#include "agent/DaemonTranscriptProjection.h"
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace ketplus::benchmark {
namespace {

// The mock's Claude answer (TranscriptFixtures.qml) with its shared-clock diagram.
const char* const claudeAnswer =
    "The sidebar now steps every working row on **one shared clock**: 20 steps a second in "
    "front, 4 behind another app.\n\n"
    "| Build | Idle CPU |\n|---|---|\n| Before | 41–47% |\n| After | 0–2% |\n\n"
    "```qml\nrotation: 360 * (KetTheme.animationClock % spinMs) / spinMs\n```\n\n"
    "Every indicator holds the same clock:\n\n"
    "```mermaid\nflowchart LR\n    subgraph Indicators\n        rows[\"Working rows\"]\n"
    "        bars[\"Activity bars\"]\n    end\n    clock((\"animationClock\"))\n"
    "    redraw[\"One window redraw\"]\n    rows -->|hold| clock\n    bars -->|hold| clock\n"
    "    clock -->|\"20 Hz in front\"| redraw\n    clock -.->|\"4 Hz behind\"| redraw\n```";

class Journal {
  public:
    explicit Journal(QString provider) : provider_(std::move(provider)) {}
    void user(const QDateTime& at, const QString& text, const QJsonObject& extra = {}) {
        QJsonObject payload{{"text", text}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            payload.insert(it.key(), it.value());
        append("user.message", payload, at, ++turn_);
        turnStart_ = at;
    }
    void agent(const QString& phase, const QString& text) {
        const QString id = phase + QString::number(cursor_ + 1);
        append(
            "item.completed",
            {{"provider", provider_},
             {"item_id", id},
             {"item",
              QJsonObject{{"type", "agentMessage"}, {"id", id}, {"phase", phase}, {"text", text}}}},
            {}, turn_);
    }
    void steer(const QString& text) {
        append("turn.steered", {{"text", text}, {"provider", provider_}}, {}, turn_);
    }
    // The usage the provider reported during the turn: its context and the session estimate.
    void usage(qint64 contextTokens, double sessionUsd) {
        append("usage.context",
               {{"tokenUsage", QJsonObject{{"last", QJsonObject{{"totalTokens", contextTokens}}},
                                           {"modelContextWindow", 200000}}}},
               {}, turn_);
        if (sessionUsd >= 0)
            append("usage.session", {{"mode", "api_key"}, {"estimatedCostUsd", sessionUsd}}, {},
                   turn_);
    }
    // Ends the turn `seconds` after its prompt, as the mock's turn line reads.
    void finish(const QString& status, int seconds) {
        append("turn.completed", {{"status", status}}, turnStart_.addSecs(seconds), turn_);
    }
    QVariantList rows() const {
        QVariantList rows = agent_message::daemonWindowMessages({{"events", events_}});
        for (auto& row : rows) {
            auto entry = row.toMap();
            agent_message::labelProvider(entry, provider_);
            row = entry;
        }
        return rows;
    }

  private:
    void append(const QString& type, const QJsonObject& payload, const QDateTime& at, qint64 turn) {
        QJsonObject event{{"event_type", type},
                          {"cursor", ++cursor_},
                          {"turn_sequence", turn},
                          {"payload", payload}};
        if (at.isValid())
            event.insert("sent_at", at.toUTC().toString(Qt::ISODateWithMs));
        events_.append(event);
    }
    QString provider_;
    QJsonArray events_;
    qint64 cursor_ = 0;
    qint64 turn_ = 0;
    QDateTime turnStart_;
};

QJsonObject fromAgent(const QString& number, const QString& title, const QString& provider,
                      const QString& text) {
    return {{"origin", "agent"},
            {"agent_messages", QJsonArray{QJsonObject{{"message_id", "m" + number},
                                                      {"kind", "reply"},
                                                      {"from_number", number},
                                                      {"from_title", title},
                                                      {"from_provider", provider},
                                                      {"body", text}}}}};
}

} // namespace

QVariantList transcriptFixture(const QString& provider, const QString& assets) {
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime today = now.addSecs(-20 * 60);
    Journal journal(provider);
    if (provider == "claude") {
        journal.user(QDateTime(QDate(now.date().year(), 9, 24), QTime(9, 12)),
                     "Profile why the sidebar keeps the CPU busy when nothing happens.");
        journal.usage(141000, 2.08);
        journal.finish("completed", 380);
        journal.user(today,
                     "The sidebar spinner still looks choppy. Check it against this capture and "
                     "fix it.");
        journal.agent("commentary",
                      "I'll read how the sidebar draws its working rows, then measure the idle "
                      "CPU.");
        journal.user(today.addSecs(120),
                     "20 steps a second reads as smooth at 14 px. Keep 4 behind another app.",
                     fromAgent("8941", "DS Icon", "claude",
                               "20 steps a second reads as smooth at 14 px. Keep 4 behind "
                               "another app."));
        journal.steer("Also check dark mode before you finish.");
        journal.agent("final_answer", QString::fromUtf8(claudeAnswer));
        journal.usage(182000, 3.32);
        journal.finish("completed", 252);
    } else if (provider == "cursor") {
        journal.user(today, "Find where the staging tenant gets created and summarise the flow.");
        journal.agent("final_answer",
                      "A staging tenant is created in **two steps**:\n\n1. `POST /tenants` in the "
                      "gateway validates the slug.\n2. `createTenant()` provisions the schema and "
                      "seeds the admin user.");
        journal.finish("failed", 151);
    } else {
        journal.user(today, "Add a journal size warning to the daemon status and the sidebar.");
        journal.agent("commentary",
                      "Retention never reclaims free pages. I'll measure the journal before "
                      "changing it.");
        journal.agent("final_answer",
                      "The daemon now reports `journal.size` in `session.list` and warns past "
                      "**80%** of the limit. Still to do:");
        journal.usage(96000, -1);
        journal.finish("interrupted", 68);
    }
    QVariantList rows = journal.rows();
    // The mock's first Claude prompt today carries a capture; journal media needs a store, so
    // the row takes the screenshot's file URL directly.
    if (provider == "claude") {
        const QString shot =
            QUrl::fromLocalFile(QDir(assets).filePath("screenshot-sidebar.png")).toString();
        for (auto& row : rows) {
            auto entry = row.toMap();
            if (entry.value("kind") == "user" &&
                entry.value("body").toString().contains("capture")) {
                entry.insert("images", QVariantList{shot});
                row = entry;
                break;
            }
        }
    }
    return rows;
}

} // namespace ketplus::benchmark
