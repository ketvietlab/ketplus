#pragma once

#include <QAbstractButton>
#include <QWidget>

namespace ketplus {

// The mock's Quick search under the brand: a 28 px field-styled button with the search
// glyph, "Quick search" and its shortcut. It opens the command palette; it takes no text.
class SidebarSearchButton final : public QAbstractButton {
    Q_OBJECT
  public:
    explicit SidebarSearchButton(const QString& text, const QString& shortcut,
                                 QWidget* parent = nullptr);

    [[nodiscard]] QString shortcutText() const { return shortcut_; }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

  private:
    QString shortcut_;
};

} // namespace ketplus
