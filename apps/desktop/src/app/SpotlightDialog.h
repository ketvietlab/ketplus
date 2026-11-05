#pragma once

#include <QDialog>
#include <QVector>

class QLineEdit;
class QListWidget;
class QLabel;

namespace ketplus {

struct SpotlightEntry {
    QString id;
    QString title;
    QString subtitle;
    QString category;
    bool available{true};
    QString kind{QStringLiteral("file")};
    QString searchText{};
};

class SpotlightDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit SpotlightDialog(QWidget* parent = nullptr);
    void setEntries(const QVector<SpotlightEntry>& entries);
    void setQuery(const QString& query);
    void setAvailableHeight(int height);
  signals:
    void entryActivated(const QString& id);
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
  private:
    void filter();
    void activateCurrent();
    QVector<SpotlightEntry> entries_;
    QLineEdit* query_;
    QListWidget* results_;
    QLabel* empty_;
    int availableHeight_{680};
};

} // namespace ketplus
