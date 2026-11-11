#pragma once

#include <QAbstractButton>
#include <QWidget>

class QLabel;

namespace ket {
class IconButton;
}

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

// The mock's active tag filter above the repositories: "Tagged <label>" on the accent tint,
// with a button that clears it. Hidden while no filter is set.
class SidebarTagFilterChip final : public QWidget {
    Q_OBJECT
  public:
    explicit SidebarTagFilterChip(QWidget* parent = nullptr);

    [[nodiscard]] QString tag() const { return tag_; }
    void setTag(const QString& tag);
    [[nodiscard]] QSize sizeHint() const override;

  signals:
    void cleared();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void restyle();

    QString tag_;
    QLabel* label_{nullptr};
    ket::IconButton* clear_{nullptr};
};

} // namespace ketplus
