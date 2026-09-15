#pragma once

#include <QFrame>
#include <QList>
#include <QString>
#include <QVariant>

class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;

namespace ketplus {

struct QuickOpenItem final {
    QString label;
    QString detail;
    QVariant data;
};

// A keyboard-driven picker overlay shared by the command palette, go to file and
// go to symbol. Items are filtered with fuzzy matching as the user types.
class QuickOpenPopup final : public QFrame {
    Q_OBJECT

  public:
    static constexpr int maximumVisibleItems = 200;
    static constexpr int maximumVisibleRows = 12;

    explicit QuickOpenPopup(QWidget* parent);

    void open(const QString& placeholder, const QList<QuickOpenItem>& items);
    void setItems(const QList<QuickOpenItem>& items);
    void setStatusText(const QString& text);
    [[nodiscard]] QString query() const;
    [[nodiscard]] QList<QuickOpenItem> visibleItems() const;

  signals:
    void itemActivated(const QVariant& data);
    void dismissed();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void refilter();
    void activateCurrent();
    void dismiss();
    void moveCurrentRow(int offset);
    void reposition();

    QLineEdit* queryEdit_{nullptr};
    QLabel* statusLabel_{nullptr};
    QListWidget* list_{nullptr};
    QList<QuickOpenItem> items_;
    QList<qsizetype> visibleIndexes_;
};

} // namespace ketplus
