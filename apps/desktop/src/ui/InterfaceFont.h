#pragma once

class QApplication;
class QString;

namespace ketplus {

// KÉT Sans, bundled Inter, then the platform UI font. Both editions use the
// same primary face; this adapter resolves the family for existing Pro consumers.
struct InterfaceUiFont final {
    [[nodiscard]] static QString resolvedFamily();
    [[nodiscard]] static QString styleSheetFamilyList();
    [[nodiscard]] static QString settingsLabel();
    static void apply(QApplication& application, int pixelSize);
};

} // namespace ketplus
