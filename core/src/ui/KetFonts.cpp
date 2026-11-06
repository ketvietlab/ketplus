#include "ui/KetFonts.h"

#include <QDirIterator>
#include <QFontDatabase>

namespace ketplus {

QStringList registerKetFonts() {
    QStringList families;
    QDirIterator fonts(QStringLiteral(":/ketplus/fonts"), {QStringLiteral("*.ttf")}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (fonts.hasNext()) {
        const QString path = fonts.next();
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            qWarning("Unable to register the embedded font %s", qPrintable(path));
            continue;
        }
        for (const QString& family : QFontDatabase::applicationFontFamilies(id)) {
            if (!families.contains(family)) {
                families.append(family);
            }
        }
    }
    return families;
}

} // namespace ketplus
