#pragma once

#include "runners/RunnerListModel.h"

#include <QDateTime>
#include <QWidget>

namespace ket {
class Button;
} // namespace ket

namespace ketplus::runners {

enum class RowAction { Pair, Disable, Enable, Unpair, Remove };

// One runner on Settings → Runners: name, relation and state, platform, version and last
// contact, labels, a warning when this computer and the Relay disagree, and the actions the
// runner allows. Every string from the Relay is shown as plain text.
class RunnerRowWidget final : public QWidget {
    Q_OBJECT

  public:
    RunnerRowWidget(const RunnerRow& row, const QDateTime& now, QWidget* parent = nullptr);

    [[nodiscard]] QString runnerId() const { return id_; }
    // The name as shown: plain, cut, or the id when the Relay sent none.
    [[nodiscard]] QString displayName() const { return name_; }
    // While a request for this runner is out, its actions cannot be used.
    void setBusy(bool busy);
    [[nodiscard]] bool isBusy() const { return busy_; }
    // The button for `action`, or nullptr when the runner does not allow it.
    [[nodiscard]] ket::Button* button(RowAction action) const;

  signals:
    void actionRequested(ketplus::runners::RowAction action, const QString& runnerId);

  private:
    void addAction(RowAction action, ket::Button* button);

    QString id_;
    QString name_;
    QList<QPair<RowAction, ket::Button*>> buttons_;
    bool busy_{false};
};

} // namespace ketplus::runners
