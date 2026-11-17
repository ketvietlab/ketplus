#pragma once
#include <QKeySequence>
#include <QWidget>
class QLabel;
namespace ket {
class TextField;
}
namespace ketplus {
// A real kit text field. The shortcut hint yields its space while editing.
class SidebarFilterField final : public QWidget {
    Q_OBJECT
  public:
    explicit SidebarFilterField(QWidget* parent = nullptr);
    static QKeySequence shortcut();
    void focusFilter();
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
  signals:
    void filterTextChanged(const QString& text);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void refresh();
    ket::TextField* field_{nullptr};
    QLabel* hint_{nullptr};
};
} // namespace ketplus
