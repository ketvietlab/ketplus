#pragma once

#include "KetAppearance.h"

#include <QSplitter>
#include <QWidget>
#include <QtMath>

namespace ketplus {

// One width for the Explorer and Source Control columns. They open at the default, or at
// wherever the user last dragged either of them, so switching between the two never moves the
// editor. It is kept in logical pixels, so it follows the interface zoom.
class NavigationPanelWidth final {
  public:
    // Logical pixels at 100% zoom.
    static constexpr int defaultWidth = 288;

    // The pixels to open a column at, at the interface zoom.
    [[nodiscard]] int shown() const { return qRound(ket::Appearance::instance().scaled(logical_)); }

    // The user dragged `panel`'s handle: its width, as the splitter now has it, is the one for
    // both columns. A hidden panel, or one the splitter does not hold, changes nothing.
    void remember(const QSplitter* splitter, QWidget* panel) {
        if (splitter == nullptr || panel == nullptr || !panel->isVisible())
            return;
        const int index = splitter->indexOf(panel);
        const qreal unit = ket::Appearance::instance().scaled(1.0);
        if (index < 0 || unit <= 0)
            return;
        const int width = splitter->sizes().value(index);
        if (width > 0)
            logical_ = width / unit;
    }

  private:
    qreal logical_ = defaultWidth;
};

} // namespace ketplus
