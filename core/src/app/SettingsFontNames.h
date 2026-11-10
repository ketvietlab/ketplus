#pragma once

#include <QString>

class QComboBox;

namespace ketplus::settings_fonts {

// The name a font select shows: the bundled "KET Sans Mono" family reads "KÉT Sans Mono",
// as the mock and the font's own name do. Other families show as they are.
QString label(const QString& family);
// The family the select holds: its item data, or its text for an item added without data.
QString selectedFamily(const QComboBox* select);
// Selects `family`, adding it under its label when the list does not have it.
void selectFamily(QComboBox* select, const QString& family);

} // namespace ketplus::settings_fonts
