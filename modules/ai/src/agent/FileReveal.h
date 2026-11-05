#pragma once

#include <QString>

#include <functional>

namespace ketplus::file_reveal {

// Shows `absolutePath` selected in Finder, Explorer or the file manager without opening it.
void reveal(const QString& absolutePath);
// Replaces the platform reveal, the way QDesktopServices::setUrlHandler replaces opening a
// URL. An empty handler restores the platform behavior.
void setHandler(std::function<void(const QString& absolutePath)> handler);

} // namespace ketplus::file_reveal
