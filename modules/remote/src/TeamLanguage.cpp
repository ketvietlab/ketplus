#include "TeamLanguage.h"

#include <QRegularExpression>

namespace ketplus::remote::team_language {
QStringList supported() { return {automatic, english, vietnamese}; }

bool isSupported(const QString& language) { return supported().contains(language); }

QString orEnglish(const QString& language) { return isSupported(language) ? language : english; }

QString fromLocale(const QString& tag) {
    const QString primary =
        tag.trimmed().section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
    return isSupported(primary) ? primary : english;
}

QString detect(const QString& text) {
    static const QRegularExpression vietnamese(
        QStringLiteral("[ăâđêôơưạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịĩọỏốồổỗộớờởỡợụủũứừửữựỳỵỷỹ]"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return vietnamese.match(text).hasMatch() ? team_language::vietnamese : QString{};
}

QString nativeName(const QString& language) {
    if (language == automatic)
        return QStringLiteral("Follow prompt");
    return language == vietnamese ? QStringLiteral("Vietnamese") : QStringLiteral("English");
}

QString text(const QString&, const char*, const char* source, const int n) {
    // Fixed application/daemon text is English. Stored and agent-authored content never
    // passes through this formatter. Keep catalogs as historical resources, not UI state.
    return n < 0 ? QString::fromUtf8(source)
                 : QString::fromUtf8(source).replace(QStringLiteral("%n"), QString::number(n));
}

// Compatibility entry points for existing page/menu callers. Content preferences must
// never install a process-global QTranslator or change any UI's translation context.
void install() {}
Scope::Scope(const QString&) {}
Scope::~Scope() = default;
void clearAmbient(QObject*) {}
void setAmbient(const QString&, QObject*) {}
QString current() { return english; }

} // namespace ketplus::remote::team_language
