#include "editor/TabPaths.h"

#include "editor/EditorWidget.h"

#include <QDir>
#include <QTabWidget>

namespace ketplus::tab_paths {

bool isWithin(const QString& path, const QString& root) {
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);
    return !cleanRoot.isEmpty() &&
           (cleanPath == cleanRoot || cleanPath.startsWith(cleanRoot + QLatin1Char('/')));
}

QList<EditorWidget*> editorsWithin(QTabWidget* tabs, const QString& path) {
    QList<EditorWidget*> editors;
    for (int index = 0; index < tabs->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs->widget(index));
        if (editor != nullptr && !editor->document().isUntitled() &&
            isWithin(editor->document().filePath(), path)) {
            editors.append(editor);
        }
    }
    return editors;
}

QList<EditorWidget*> retarget(QTabWidget* tabs, const QString& from, const QString& to) {
    const QString source = QDir::cleanPath(from);
    const QList<EditorWidget*> editors = editorsWithin(tabs, source);
    for (EditorWidget* editor : editors) {
        const QString path = QDir::cleanPath(editor->document().filePath());
        editor->document().setFilePath(QDir::cleanPath(to) + path.mid(source.size()));
    }
    return editors;
}

} // namespace ketplus::tab_paths
