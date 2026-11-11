#include "editor/DocumentTab.h"

#include "KetIcons.h"
#include "editor/EditorWidget.h"

#include <QFileInfo>

namespace ketplus {

QString documentTabTitle(const EditorWidget* editor) {
    auto title = editor->document().displayName();
    if (editor->document().isModified())
        title.append(QStringLiteral(" •"));
    return title;
}

QIcon documentTabIcon(const EditorWidget* editor) {
    if (editor->document().isUntitled())
        return ket::Icons::icon(ket::Icons::codeForLucide(u"file"));
    const auto suffix = QFileInfo(editor->document().filePath()).suffix().toLower();
    const bool text = suffix == QLatin1String("md") || suffix == QLatin1String("markdown") ||
                      suffix == QLatin1String("txt");
    return ket::Icons::icon(ket::Icons::codeForLucide(text ? u"file-text" : u"file-code"));
}

} // namespace ketplus
