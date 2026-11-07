#pragma once

#include <QWidget>

namespace ket {
class Select;
}

namespace ketplus {

// Preference range/value adapter; the Két select owns rendering and interaction.
class PixelSelect final : public QWidget {
    Q_OBJECT
  public:
    explicit PixelSelect(QWidget* parent = nullptr);
    [[nodiscard]] int value() const;
    [[nodiscard]] int minimum() const { return minimum_; }
    [[nodiscard]] int maximum() const { return maximum_; }
    void setRange(int minimum, int maximum);
    void setMinimum(int minimum);
    void setValue(int value);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  signals:
    void valueChanged(int value);

  private:
    ket::Select* select_;
    int minimum_{0};
    int maximum_{99};
};

} // namespace ketplus
