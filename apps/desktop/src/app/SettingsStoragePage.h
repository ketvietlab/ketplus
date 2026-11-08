#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
class QAbstractButton;
class QComboBox;
class QLabel;
class QTimer;
class QSpinBox;
namespace ketplus {
class SettingsStoragePage final : public QWidget {
    Q_OBJECT
  public:
    using Reply = std::function<void(const QJsonObject&, const QString&)>;
    using Requester = std::function<void(const QString&, const QJsonObject&, Reply)>;
    explicit SettingsStoragePage(Requester requester = {}, QWidget* parent = nullptr);

  private:
    void refresh();
    void save();
    void request(const QString& operation, const QJsonObject& payload);
    void receive(const QJsonObject& result, const QString& error, bool saving);
    void setBusy(bool busy);
    void saveSize();
    void receiveSize(const QJsonObject& result, const QString& error, bool saving);
    void showUsage(const QJsonObject& result);
    void fail(const QString& message);
    Requester requester_;
    QTimer* timeout_{};
    QComboBox* days_{};
    QAbstractButton* automatic_{};
    QLabel* status_{};
    QLabel* usage_{};
    QAbstractButton* refresh_{};
    QWidget* sizeGroup_{};
    QComboBox* limit_{};
    QSpinBox* warning_{};
    QAbstractButton* saveSize_{};
    QLabel* sizeStatus_{};
    QLabel* sizeDraftStatus_{};
    bool sizeDraftDirty_{false};
    qint64 sizeRevision_{-1};
    qint64 revision_{-1};
    bool busy_{false};
    quint64 generation_{0};
};
} // namespace ketplus
