#include "editor/EditorViewOptions.h"

#include <QSettings>
#include <QtGlobal>

namespace ketplus {

EditorViewOptions EditorViewOptions::load() { return load(QSettings()); }

EditorViewOptions EditorViewOptions::load(const QSettings& settings) {
    const EditorViewOptions defaults;
    EditorViewOptions options;
    options.wordWrap =
        settings.value(QStringLiteral("editorView/wordWrap"), defaults.wordWrap).toBool();
    options.showWhitespace =
        settings.value(QStringLiteral("editorView/showWhitespace"), defaults.showWhitespace)
            .toBool();
    options.indentGuides =
        settings.value(QStringLiteral("editorView/indentGuides"), defaults.indentGuides).toBool();
    options.showRuler =
        settings.value(QStringLiteral("editorView/showRuler"), defaults.showRuler).toBool();
    options.rulerColumn =
        settings.value(QStringLiteral("editorView/rulerColumn"), defaults.rulerColumn).toInt();
    options.codeFolding =
        settings.value(QStringLiteral("editorView/codeFolding"), defaults.codeFolding).toBool();
    options.autoCloseBrackets =
        settings.value(QStringLiteral("editorView/autoCloseBrackets"), defaults.autoCloseBrackets)
            .toBool();
    options.autoIndent =
        settings.value(QStringLiteral("editorView/autoIndent"), defaults.autoIndent).toBool();
    options.wordCompletion =
        settings.value(QStringLiteral("editorView/wordCompletion"), defaults.wordCompletion)
            .toBool();
    options.useTabs =
        settings.value(QStringLiteral("editorView/useTabs"), defaults.useTabs).toBool();
    options.tabWidth =
        settings.value(QStringLiteral("editorView/tabWidth"), defaults.tabWidth).toInt();
    options.zoom = settings.value(QStringLiteral("editorView/zoom"), defaults.zoom).toInt();
    return options.normalized();
}

void EditorViewOptions::save() const {
    QSettings settings;
    save(settings);
}

void EditorViewOptions::save(QSettings& settings) const {
    const EditorViewOptions options = normalized();
    settings.setValue(QStringLiteral("editorView/wordWrap"), options.wordWrap);
    settings.setValue(QStringLiteral("editorView/showWhitespace"), options.showWhitespace);
    settings.setValue(QStringLiteral("editorView/indentGuides"), options.indentGuides);
    settings.setValue(QStringLiteral("editorView/showRuler"), options.showRuler);
    settings.setValue(QStringLiteral("editorView/rulerColumn"), options.rulerColumn);
    settings.setValue(QStringLiteral("editorView/codeFolding"), options.codeFolding);
    settings.setValue(QStringLiteral("editorView/autoCloseBrackets"), options.autoCloseBrackets);
    settings.setValue(QStringLiteral("editorView/autoIndent"), options.autoIndent);
    settings.setValue(QStringLiteral("editorView/wordCompletion"), options.wordCompletion);
    settings.setValue(QStringLiteral("editorView/useTabs"), options.useTabs);
    settings.setValue(QStringLiteral("editorView/tabWidth"), options.tabWidth);
    settings.setValue(QStringLiteral("editorView/zoom"), options.zoom);
}

EditorViewOptions EditorViewOptions::normalized() const {
    EditorViewOptions options = *this;
    options.tabWidth = qBound(minimumTabWidth, options.tabWidth, maximumTabWidth);
    options.rulerColumn = qBound(minimumRulerColumn, options.rulerColumn, maximumRulerColumn);
    options.zoom = qBound(minimumZoom, options.zoom, maximumZoom);
    return options;
}

} // namespace ketplus
