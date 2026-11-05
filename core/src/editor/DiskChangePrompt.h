#pragma once

class QTabWidget;
class QWidget;

namespace ketplus {

class EditorWidget;

// Disk-change handling shared by the KétPlus and KétPlus Pro windows.
namespace disk_change {

// A hibernated tab holds an empty buffer, so it is read again before Save or Save As writes it.
// Returns false, after telling the user, when the file cannot be read.
[[nodiscard]] bool restoreBeforeSave(QWidget* parent, EditorWidget* editor);

// Asks before a save would replace a file that changed on disk since it was loaded or saved
// (an agent's edit, a branch switch, another editor). Returns whether saving may go ahead.
// Choosing Reload replaces the buffer with the file and returns false.
[[nodiscard]] bool confirmSave(QWidget* parent, EditorWidget* editor);

// Brings clean, loaded tabs up to date with their files. Modified tabs are left alone and are
// caught by confirmSave; hibernated tabs read the file when they are next shown.
void refreshCleanTabs(QTabWidget* tabs);

} // namespace disk_change
} // namespace ketplus
