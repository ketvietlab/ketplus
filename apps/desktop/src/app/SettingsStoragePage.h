#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
class QAbstractButton;
class QComboBox;
class QLabel;
class QTimer;
class QVBoxLayout;
namespace ket {
class Banner;
class Meter;
} // namespace ket
namespace ketplus {
// Settings → Storage as in the mock (ADR 0108): what the journal holds, the retention
// choices, the largest conversations, Compact now and Export. Every choice applies when it is
// made, with the daemon's revision as a fence; a failed or conflicting save requires a reload.
class SettingsStoragePage final : public QWidget {
    Q_OBJECT
  public:
    using Reply = std::function<void(const QJsonObject&, const QString&)>;
    // Operations: "read"/"apply" (detail policy), "size.*", "options.*", "compaction.*",
    // "breakdown.read" and "export.*".
    using Requester = std::function<void(const QString&, const QJsonObject&, Reply)>;
    // Asks for a folder to export into; empty when the user cancels.
    using FolderChooser = std::function<QString(QWidget*)>;
    explicit SettingsStoragePage(Requester requester = {}, QWidget* parent = nullptr);
    void setFolderChooser(FolderChooser chooser) { chooseFolder_ = std::move(chooser); }

  private:
    // Policies (SettingsStoragePage.cpp).
    void refresh();
    void saveDetail();
    void saveSize();
    void saveOptions();
    void request(const QString& operation, const QJsonObject& payload);
    void receive(const QString& operation, const QJsonObject& result, const QString& error);
    void receiveDetail(const QJsonObject& result, bool saving);
    void receiveSize(const QJsonObject& result, bool saving);
    void receiveOptions(const QJsonObject& result, bool saving);
    void setBusy(bool busy);
    void fail(const QString& message);
    void finish();
    // What the journal holds and the jobs (SettingsStorageOverview.cpp).
    void buildOverview(QVBoxLayout* layout);
    void buildJobs(QVBoxLayout* layout);
    void poll(const QString& operation, const QJsonObject& payload,
              const std::function<void(const QJsonObject&)>& done);
    void showMeasurements(const QJsonObject& result);
    void showBreakdown(const QJsonObject& result);
    void showCompaction(const QJsonObject& compaction);
    void showExport(const QJsonObject& status);
    void startCompaction();
    void startExport();

    Requester requester_;
    FolderChooser chooseFolder_;
    QTimer* timeout_{};
    QTimer* jobsTimer_{};
    // Overview.
    QLabel* fileSize_{};
    QLabel* available_{};
    ket::Banner* limitBanner_{};
    ket::Banner* diskBanner_{};
    ket::Meter* journal_{};
    QList<ket::Meter*> parts_;
    QWidget* largestSection_{};
    QVBoxLayout* largest_{};
    // Retention.
    QComboBox* days_{};
    QWidget* sizeRow_{};
    QComboBox* limit_{};
    QLabel* sizeStatus_{};
    QWidget* optionsGroup_{};
    QComboBox* images_{};
    QComboBox* archived_{};
    QAbstractButton* warn_{};
    QAbstractButton* automatic_{};
    // Jobs.
    QLabel* lastCompaction_{};
    ket::Meter* compacting_{};
    QAbstractButton* compact_{};
    QAbstractButton* export_{};
    QLabel* jobStatus_{};
    QLabel* status_{};
    QAbstractButton* refresh_{};

    qint64 revision_{-1};
    qint64 sizeRevision_{-1};
    qint64 optionsRevision_{-1};
    int warningPercent_{80};
    qint64 limitBytes_{0};
    qint64 databaseBytes_{-1};
    qint64 freeBytes_{-1};
    int detailDays_{-1};
    QString oldestEvent_;
    QString directory_;
    QString compactionState_;
    bool busy_{false};
    bool optionsSupported_{false};
    bool saved_{false}; // a save in this read chain succeeded
    bool compactionRunning_{false};
    bool exportRunning_{false};
    bool measuring_{false};
    quint64 generation_{0};
    quint64 jobsGeneration_{0};
};
} // namespace ketplus
