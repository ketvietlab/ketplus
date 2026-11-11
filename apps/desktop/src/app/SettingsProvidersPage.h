#pragma once

#include "app/SettingsClaudeCredentials.h"
#include <QJsonArray>
#include <QSet>
#include <QWidget>

#include <functional>

class QLabel;
class QVBoxLayout;

namespace ketplus {

// Settings → Providers: the agent CLI versions the daemon found and whether KetPlus supports
// them (ADR 0015). Read-only; the daemon owns detection and the supported ranges.
class SettingsProvidersPage final : public QWidget {
    Q_OBJECT

  public:
    // Answers with the daemon's `provider.runtime_status` providers, or an error. `recheck` is
    // Check again: the daemon forgets reported failures and probes again (ADR 0110).
    using Reply = std::function<void(const QJsonArray& providers, const QString& error)>;
    using Fetcher = std::function<void(bool recheck, Reply)>;

    explicit SettingsProvidersPage(Fetcher fetcher, QWidget* parent = nullptr,
                                   SettingsClaudeCredentials::Requester credentials = {});

    void refresh(bool recheck = false);
    [[nodiscard]] const QJsonArray& providers() const noexcept { return providers_; }

    // Short user-facing label for a support status ("supported", "outdated", ...).
    [[nodiscard]] static QString statusText(const QString& status);

  signals:
    void refreshed();
    // From a setup guide (#170): a command to type into a terminal, unrun; another Settings page.
    void typeInTerminalRequested(const QString& command);
    void sectionRequested(const QString& id);

  private:
    void apply(const QJsonArray& providers, const QString& error);

    Fetcher fetcher_;
    QLabel* status_{nullptr};
    QWidget* list_{nullptr};
    QVBoxLayout* listLayout_{nullptr};
    QJsonArray providers_;
    quint64 generation_{0};
    // Providers whose guide asked to check again; a guide that stays says so.
    QSet<QString> checking_;
};

} // namespace ketplus
