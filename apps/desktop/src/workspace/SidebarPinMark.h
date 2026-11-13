#pragma once

#include <QWidget>

namespace ket {
class SidebarRepositoryHeader;
}

namespace ketplus {

// T-92: the small pin a pinned repository shows at the end of its name row. Két Symbol has no
// pin glyph yet, so it is drawn here, in the muted text colour of the theme; it follows the
// header's size, zoom and hover, and lets clicks through to the header.
//
// The pin sits right after the drawn name, inside the header's name area, so it never meets the
// count or the hover action. A name too long for the name area and the pin together is
// shortened in the middle here, the way the header shortens it; the header's accessible name
// stays the full name.
class SidebarPinMark final : public QWidget {
    Q_OBJECT

  public:
    explicit SidebarPinMark(ket::SidebarRepositoryHeader* header);

    // Puts the mark just after the drawn name, shortening the name when both do not fit.
    void place();
    // The header's name as it was before any shortening.
    [[nodiscard]] const QString& fullName() const { return fullName_; }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void placeLater();

    ket::SidebarRepositoryHeader* header_;
    QString fullName_;
    bool placing_ = false;
};

} // namespace ketplus
