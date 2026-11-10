#pragma once

#include <QString>
#include <QVariantMap>
#include <QWidget>

class QQuickView;

namespace ketplus {

// ADR 0106: the details pane's Report tab. It shows one conversation's whole work report with
// the transcript's card in ledger mode, and follows that conversation's later reports.
class WorkReportPane final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY reportChanged)
    Q_PROPERTY(QVariantMap report READ report NOTIFY reportChanged)
    Q_PROPERTY(QString message READ message NOTIFY reportChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY reportChanged)

  public:
    explicit WorkReportPane(QWidget* parent = nullptr);
    ~WorkReportPane() override;

    [[nodiscard]] QString conversationId() const { return conversationId_; }
    [[nodiscard]] QVariantMap report() const { return report_; }
    [[nodiscard]] QString message() const { return message_; }
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] QQuickView* quickView() const { return view_; }

    // Shows `conversationId`, keeping nothing of another conversation's report.
    void showConversation(const QString& conversationId);
    // A ledger, or the error that kept it from being read. An older version of the shown
    // conversation's ledger never replaces a newer one, so a slow read cannot undo a live update.
    void acceptReport(const QString& conversationId, const QVariantMap& report,
                      const QString& error);

  signals:
    void reportChanged();

  private:
    QQuickView* view_{nullptr};
    QString conversationId_;
    QVariantMap report_;
    QString message_;
    bool loading_{false};
};

} // namespace ketplus
