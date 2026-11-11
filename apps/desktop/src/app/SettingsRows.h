#pragma once

class QFrame;
class QString;
class QWidget;

namespace ketplus::settings_rows {

// The mock's setting row: the title and its help on the left, the control at the row's end
// at the mock's 200 px.
[[nodiscard]] QWidget* field(const QString& title, const QString& help, QWidget* control,
                             QWidget* parent);
// The mock's status line: a 180 px label column, then the value (which the row adopts).
[[nodiscard]] QWidget* info(const QString& title, QWidget* value, QWidget* parent);
// The hairline between a page's sections.
[[nodiscard]] QFrame* divider(QWidget* parent);

} // namespace ketplus::settings_rows
