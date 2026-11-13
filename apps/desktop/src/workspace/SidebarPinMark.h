#pragma once

#include <QWidget>

namespace ket {
class SidebarRepositoryHeader;
}

namespace ketplus {

// T-92: the small pin a pinned repository shows at the end of its name row. Két Symbol has no
// pin glyph yet, so it is drawn here, in the muted text colour of the theme; it follows the
// header's size and zoom, and lets clicks through to the header.
class SidebarPinMark final : public QWidget {
    Q_OBJECT

  public:
    explicit SidebarPinMark(ket::SidebarRepositoryHeader* header);

    // Puts the mark just after the header's name area, before its count.
    void place();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    ket::SidebarRepositoryHeader* header_;
};

} // namespace ketplus
