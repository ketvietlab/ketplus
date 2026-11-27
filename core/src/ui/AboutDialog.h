#pragma once

#include "ui/BrandLockup.h"

class QWidget;

namespace ketplus {

// About ketPlus / About ketPlus Pro: the approved lockup and the application version, in the
// kit dialog. One per parent window, raised when reopened.
void showAboutDialog(QWidget* parent, BrandLockup::Edition edition);

} // namespace ketplus
