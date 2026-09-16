#include "editor/DefinitionPattern.h"

#include <QRegularExpression>

namespace ketplus {
namespace {

bool isSymbolName(const QString& symbol) {
    if (symbol.isEmpty() || symbol.size() > 200) {
        return false;
    }
    if (symbol.at(0).isDigit()) {
        return false;
    }
    for (const QChar character : symbol) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('_') &&
            character != QLatin1Char('$')) {
            return false;
        }
    }
    return true;
}

// Keywords that introduce a declaration, by syntax family.
QString declarationKeywords(const QString& syntax) {
    if (syntax == QStringLiteral("python")) {
        return QStringLiteral("def|class");
    }
    if (syntax == QStringLiteral("ruby")) {
        return QStringLiteral("def|class|module");
    }
    if (syntax == QStringLiteral("go")) {
        return QStringLiteral("func|type|var|const");
    }
    if (syntax == QStringLiteral("rust")) {
        return QStringLiteral("fn|struct|enum|trait|impl|type|const|static|mod|macro_rules!");
    }
    if (syntax == QStringLiteral("javascript-typescript")) {
        return QStringLiteral(
            "function|class|interface|type|enum|const|let|var|namespace|abstract\\s+class");
    }
    if (syntax == QStringLiteral("php")) {
        return QStringLiteral("function|class|interface|trait|enum|const");
    }
    if (syntax == QStringLiteral("java") || syntax == QStringLiteral("kotlin") ||
        syntax == QStringLiteral("scala") || syntax == QStringLiteral("groovy")) {
        return QStringLiteral("class|interface|enum|record|object|trait|fun|def|val|var");
    }
    if (syntax == QStringLiteral("csharp")) {
        return QStringLiteral("class|interface|enum|struct|record|delegate|namespace");
    }
    if (syntax == QStringLiteral("swift")) {
        return QStringLiteral("func|class|struct|enum|protocol|extension|typealias|let|var");
    }
    if (syntax == QStringLiteral("lua")) {
        return QStringLiteral("function|local\\s+function");
    }
    if (syntax == QStringLiteral("shell")) {
        return QStringLiteral("function");
    }
    if (syntax == QStringLiteral("cmake")) {
        return QStringLiteral("function|macro");
    }
    // C, C++, Objective-C and anything else that declares types with these words.
    return QStringLiteral("class|struct|enum|union|namespace|typedef|using|interface|protocol");
}

} // namespace

QString definitionExpression(const QString& symbol, const QString& syntaxName) {
    if (!isSymbolName(symbol)) {
        return {};
    }
    const QString name = QRegularExpression::escape(symbol);

    if (syntaxName == QStringLiteral("css") || syntaxName == QStringLiteral("scss") ||
        syntaxName == QStringLiteral("less")) {
        // A rule, a custom property or a variable.
        return QStringLiteral("(?:^|[\\s,>+~])[.#]?%1\\s*[\\{,:]|--%1\\s*:|\\$%1\\s*:").arg(name);
    }
    if (syntaxName == QStringLiteral("yaml") || syntaxName == QStringLiteral("toml") ||
        syntaxName == QStringLiteral("json") || syntaxName == QStringLiteral("properties")) {
        return QStringLiteral("^\\s*\\\"?%1\\\"?\\s*[:=]").arg(name);
    }
    if (syntaxName == QStringLiteral("markdown")) {
        return QStringLiteral("^#{1,6}\\s+.*%1").arg(name);
    }
    if (syntaxName == QStringLiteral("makefile")) {
        return QStringLiteral("^%1\\s*:").arg(name);
    }

    const QString keywords = declarationKeywords(syntaxName);
    return QStringLiteral(
               // `keyword Name`, a typed declaration such as `int Name(`, or `Name = function`
               "(?:\\b(?:%1)\\s+[\\w:<>,\\*&\\[\\]\\s]*?\\b%2\\b)"
               "|(?:^\\s*[\\w:<>,\\*&\\[\\]]+[\\*&\\s]+%2\\s*\\()"
               "|(?:\\b%2\\s*(?:=|:=)\\s*(?:async\\s+)?(?:function\\b|\\(|\\[|new\\b))"
               "|(?:^\\s*(?:def|fn|func|function|sub)\\s+%2\\b)")
        .arg(keywords, name);
}

bool looksLikeFileReference(const QString& token) {
    if (token.isEmpty() || token.size() > 4096 || token.contains(QLatin1Char('\n'))) {
        return false;
    }
    if (token.startsWith(QStringLiteral("./")) || token.startsWith(QStringLiteral("../")) ||
        token.contains(QLatin1Char('/'))) {
        return true;
    }
    static const QRegularExpression suffix(QStringLiteral("\\.[A-Za-z0-9]{1,8}$"));
    return suffix.match(token).hasMatch();
}

} // namespace ketplus
