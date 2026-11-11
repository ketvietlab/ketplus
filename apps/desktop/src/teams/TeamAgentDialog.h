#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QWidget>
#include <functional>

namespace ket {
class Dialog;
class Select;
} // namespace ket
class QLabel;

namespace ketplus {

// The agent a team member runs: a provider, a model of it and an effort (#465).
struct TeamAgent final {
    QString provider;
    QString model;
    QString effort;

    [[nodiscard]] bool isEmpty() const { return provider.isEmpty(); }
    [[nodiscard]] QJsonObject json() const;
    friend bool operator==(const TeamAgent&, const TeamAgent&) = default;
};

// Choices over the app's model list (model/list entries tagged with their provider).
namespace team_agents {
[[nodiscard]] QStringList readyProviders(const QVariantList& models);
[[nodiscard]] QString providerLabel(const QString& provider);
// The provider's default model and its default effort; empty when the provider has none.
[[nodiscard]] TeamAgent defaultFor(const QVariantList& models, const QString& provider);
// "Claude · Opus · high".
[[nodiscard]] QString label(const QVariantList& models, const TeamAgent& agent);

// Claude leads and reviews; another ready provider develops. With one provider, all of it.
struct Defaults final {
    TeamAgent leader;
    TeamAgent developer;
    TeamAgent reviewer;
    bool singleProvider{false};
};
[[nodiscard]] Defaults defaults(const QVariantList& models);
// The reviewer reviews read-only, which only Claude keeps (ADR 0067).
inline constexpr char reviewerProvider[] = "claude";
} // namespace team_agents

// The Model & context choice of one role, as the conversation's dialog offers it: a provider,
// its models and their efforts. `lockedProvider` keeps the reviewer on Claude and says why.
class TeamAgentChooser final : public QWidget {
    Q_OBJECT

  public:
    TeamAgentChooser(const QVariantList& models, const TeamAgent& current,
                     const QString& lockedProvider = {}, QWidget* parent = nullptr);

    [[nodiscard]] TeamAgent agent() const;
    [[nodiscard]] ket::Select* providers() const { return providers_; }
    [[nodiscard]] ket::Select* modelSelect() const { return models_; }

    // Opens the chooser in a dialog over `anchor`; `chosen` runs on Use.
    static ket::Dialog* present(QWidget* anchor, const QString& role, const QVariantList& models,
                                const TeamAgent& current, const QString& lockedProvider,
                                std::function<void(const TeamAgent&)> chosen);

  private:
    void fillModels(const QString& model);
    void fillEfforts(const QString& effort);

    QVariantList entries_;
    ket::Select* providers_;
    ket::Select* models_;
    ket::Select* efforts_;
};

} // namespace ketplus
