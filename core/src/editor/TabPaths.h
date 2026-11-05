#pragma once

#include <QList>
#include <QString>

class QTabWidget;

namespace ketplus {

class EditorWidget;

// Keeps editor tabs in step with files that moved or went away, shared by the KétPlus and
// KétPlus Pro windows.
namespace tab_paths {

// Whether `path` is `root` or lies under it.
[[nodiscard]] bool isWithin(const QString& path, const QString& root);
// Editor tabs whose file is `path` or lies under it.
[[nodiscard]] QList<EditorWidget*> editorsWithin(QTabWidget* tabs, const QString& path);
// Points the tabs at or under `from` at the same place under `to`. Returns those editors.
QList<EditorWidget*> retarget(QTabWidget* tabs, const QString& from, const QString& to);

} // namespace tab_paths
} // namespace ketplus
