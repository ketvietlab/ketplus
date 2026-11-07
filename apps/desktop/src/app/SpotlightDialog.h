#pragma once

#include <QVector>
#include <QWidget>

class QLineEdit;
namespace ket {
class CommandPalette;
}

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

class SpotlightDialog final : public QWidget {
    Q_OBJECT
  public:
    explicit SpotlightDialog(QWidget* parent = nullptr);
    void setEntries(const QVector<SpotlightEntry>& entries);
    void setQuery(const QString& query);
    [[nodiscard]] ket::CommandPalette* commandPalette() const { return palette_; }
  signals:
    void entryActivated(const QString& id);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    void filter();
    void reposition();
    QVector<SpotlightEntry> entries_;
    QLineEdit* query_;
    ket::CommandPalette* palette_;
};

} // namespace ketplus
