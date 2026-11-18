#pragma once
#include <QString>

namespace ketplus::remote {
// Windows: replaces the object's DACL with one entry granting full control to the current
// user only, and blocks inheritance from the parent (so a shared parent such as a profile
// folder cannot add entries). On a directory the entry is inherited by everything created
// inside afterwards. Other platforms keep their POSIX mode-bit handling and return true.
bool restrictToCurrentUser(const QString& path, bool directory, QString* error = nullptr);
} // namespace ketplus::remote
