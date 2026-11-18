#pragma once

#include <QString>

class QLabel;
class QWidget;

// Text inside the runner dialogs. A dialog sits over the whole window, outside the Settings
// page, so its labels take their colours and sizes from the Két field-row recipe and follow
// theme and zoom changes themselves. Every label is plain text.
namespace ketplus::runners::dialog_text {

enum class Role { Label, Help, Error, Code };

[[nodiscard]] QLabel* make(const QString& text, Role role, QWidget* parent);
// Paints every label make() created under `root` for the current theme and zoom, now and on
// every later change while `root` lives.
void follow(QWidget* root);

} // namespace ketplus::runners::dialog_text
