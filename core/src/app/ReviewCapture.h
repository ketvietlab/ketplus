#pragma once

#include <QString>

class QMainWindow;

namespace ketplus {

class ThemeManager;

// Review evidence without a screen (docs/design-system.md). Writes the capture's interface
// zoom into the temporary preference store before the window reads it.
void prepareReviewCapture();

// Runs the capture steps from the environment against the shown window, saves the top-most
// window to `file`, then quits the application.
void runReviewCapture(ThemeManager& theme, QMainWindow& window, const QString& file);

} // namespace ketplus
