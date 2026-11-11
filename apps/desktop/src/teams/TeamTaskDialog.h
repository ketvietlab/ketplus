#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace ket {
class Dialog;
class Select;
class TextField;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// #466: one task — description, criteria, dependencies, its whole history (criteria and lane
// changes included) and Hand over…, which needs a new owner and a reason (#457). It follows
// the team while open.
class TeamTaskDialog final : public QWidget {
    Q_OBJECT

  public:
    TeamTaskDialog(TeamDirectory* teams, const QString& teamId, const QString& taskId,
                   QWidget* parent = nullptr);

    [[nodiscard]] ket::Select* handoverOwner() const { return owner_; }
    [[nodiscard]] ket::TextField* handoverReason() const { return reason_; }
    [[nodiscard]] QString error() const;
    void handOver();

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                const QString& taskId);

  private:
    void rebuild();

    TeamDirectory* teams_;
    QString teamId_;
    QString taskId_;
    QWidget* details_;
    QVBoxLayout* detailsLayout_;
    ket::Select* owner_;
    ket::TextField* reason_;
    QLabel* error_;
};

} // namespace ketplus
