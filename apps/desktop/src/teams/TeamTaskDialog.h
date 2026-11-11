#pragma once

#include <QJsonArray>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QScrollArea;

namespace ket {
class Dialog;
class Select;
class TextField;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// #466: one task, laid out as mockups/qt's task dialog: its state and lane, the facts (owner,
// reviewer, pull request, branch, worktree, dependencies), the description as Markdown (a long
// one is cut, with Show all), the acceptance criteria and the history, read a page at a time
// with team.task.get (ADR 0115). It scrolls inside the dialog and follows the team while open.
// Hand over… opens a second dialog that needs a new owner and a reason (#457).
class TeamTaskDialog final : public QWidget {
    Q_OBJECT

  public:
    TeamTaskDialog(TeamDirectory* teams, const QString& teamId, const QString& taskId,
                   QWidget* parent = nullptr);

    [[nodiscard]] ket::Select* handoverOwner() const { return owner_; }
    [[nodiscard]] ket::TextField* handoverReason() const { return reason_; }
    [[nodiscard]] QString error() const;
    // Opens the handover dialog over this one; null when the task cannot change hands.
    ket::Dialog* openHandover();
    void handOver();

    // Wider than a form's dialog, for a description to read.
    static constexpr int kCardWidth = 640;

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                const QString& taskId);

  signals:
    // Open the owner: the page opens that conversation.
    void ownerRequested(const QString& sessionId);
    // Whether the task has an owner to open and can change hands; the footer follows.
    void actionsChanged(bool hasOwner, bool canHandOver);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

  private:
    void rebuild();
    void fitHeight();
    // The newest page of history, or with `earlier` the page before the first entry shown.
    void readHistory(bool earlier = false);

    TeamDirectory* teams_;
    QString teamId_;
    QString taskId_;
    QString ownerSession_;
    QScrollArea* scroll_;
    QWidget* details_{nullptr};
    // The handover's fields live here between two openings of its dialog.
    QWidget* form_;
    ket::Select* owner_;
    ket::TextField* reason_;
    QLabel* error_;
    QPointer<ket::Dialog> handover_;
    // History entries [historyStart_, historyStart_ + history_.size()) of historyTotal_. The
    // history only grows, so a newer page joins the entries read before.
    QJsonArray history_;
    qsizetype historyStart_{0};
    qsizetype historyTotal_{0};
    bool historyRead_{false};
    bool historyPending_{false};
    bool historyAgain_{false};
    bool wholeDescription_{false};
    bool canHandOver_{false};
};

} // namespace ketplus
