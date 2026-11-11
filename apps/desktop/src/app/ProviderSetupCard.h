#pragma once

#include <QFrame>
#include <QVariantMap>

class QLabel;

namespace ket {
class Button;
}

namespace ketplus {

// The provider setup guide in Settings → Providers (#170, mockups/qt ProviderSetupCard with
// inSettings): what happened, the provider's own words, the numbered steps with their commands,
// and Check again. It renders a provider_setup::guide map; steps that belong to a turn are left
// out, and the page heading carries the provider's name and badge.
//
// A command is copied, or typed into a terminal for the user to run with Return; the card never
// runs it.
class ProviderSetupCard final : public QFrame {
    Q_OBJECT

  public:
    explicit ProviderSetupCard(const QVariantMap& guide, QWidget* parent = nullptr);

    [[nodiscard]] QString guideId() const { return guide_.value(QStringLiteral("id")).toString(); }
    // While the daemon checks again: the primary button reads "Checking…" and is disabled.
    void setChecking(bool checking);
    // The check found the same problem again: the guide's `still` text, when it has one.
    void showStillFailing();

  signals:
    void checkAgainRequested();
    // A link to another page ("settings/updates").
    void actionRequested(const QString& action);
    void typeInTerminalRequested(const QString& command);
    void commandCopied(const QString& command);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void applyColors();

    QVariantMap guide_;
    QString primaryAction_;
    ket::Button* primary_{nullptr};
    QLabel* still_{nullptr};
    QString primaryText_;
};

} // namespace ketplus
