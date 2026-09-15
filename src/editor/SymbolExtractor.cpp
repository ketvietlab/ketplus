#include "editor/SymbolExtractor.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace ketplus {
namespace {

struct SymbolRule final {
    QRegularExpression pattern;
    int nameGroup;
    int kindGroup;
    QString kind;
};

QRegularExpression pattern(const char* expression, const bool caseInsensitive = false) {
    return QRegularExpression(
        QString::fromLatin1(expression),
        caseInsensitive ? QRegularExpression::CaseInsensitiveOption
                        : QRegularExpression::NoPatternOption);
}

QString normalizedKind(const QString& kind) {
    const QString word = kind.section(QLatin1Char(' '), 0, 0).toLower();
    static const QSet<QString> functionWords{QStringLiteral("def"), QStringLiteral("fn"),
                                             QStringLiteral("func"), QStringLiteral("fun"),
                                             QStringLiteral("function")};
    return functionWords.contains(word) ? QStringLiteral("function") : word;
}

const QList<SymbolRule>& cLikeRules() {
    static const QList<SymbolRule> rules{
        {pattern(R"(^\s*(?:(?:export|default|public|private|protected|internal|static|abstract|final|sealed|open|data|partial|unsafe|pub(?:\([^)]*\))?)\s+)*(class|struct|enum(?:\s+class|\s+struct)?|interface|trait|protocol|namespace|impl|object|union|record|extension)\s+([A-Za-z_][\w:.]*))"),
         2, 1, {}},
        {pattern(R"(^\s*func\s*\([^)]*\)\s*([A-Za-z_]\w*))"), 1, 0, QStringLiteral("method")},
        {pattern(R"(^\s*(?:(?:export|default|public|private|protected|internal|static|async|unsafe|const|override|open|suspend|inline|pub(?:\([^)]*\))?|extern(?:\s+"[^"]*")?)\s+)*(function|func|fun|fn)\b\s*\*?\s*([A-Za-z_$][\w$]*))"),
         2, 1, {}},
        {pattern(R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s+)?(?:function\b|\([^)]*\)\s*=>|[A-Za-z_$][\w$]*\s*=>))"),
         1, 0, QStringLiteral("function")},
    };
    return rules;
}

// C, C++, Java, C# and Dart definitions have no keyword, so match
// "type name(params) {" on shallowly indented lines without a semicolon.
const QRegularExpression& typedFunctionPattern() {
    static const QRegularExpression expression = pattern(
        R"(^\s*(?:[\w:<>,\[\]*&~]+\s+)*?[*&]*((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\b\s*)?(?:noexcept\b\s*)?(?:override\b\s*)?(?:final\b\s*)?(?:->\s*[\w:<>]+\s*)?\{?\s*$)");
    return expression;
}

QList<SymbolRule> rulesForSyntax(const QString& syntax) {
    if (syntax == QStringLiteral("python")) {
        return {{pattern(R"(^\s*(?:async\s+)?(def|class)\s+([A-Za-z_]\w*))"), 2, 1, {}}};
    }
    if (syntax == QStringLiteral("ruby")) {
        return {{pattern(R"(^\s*(def|class|module)\s+((?:self\.)?[\w:.?!=]+))"), 2, 1, {}}};
    }
    if (syntax == QStringLiteral("lua")) {
        return {{pattern(R"(^\s*(?:local\s+)?function\s+([\w.:]+))"), 1, 0,
                 QStringLiteral("function")}};
    }
    if (syntax == QStringLiteral("shell")) {
        return {{pattern(R"(^\s*function\s+([A-Za-z_][\w-]*))"), 1, 0, QStringLiteral("function")},
                {pattern(R"(^\s*([A-Za-z_][\w-]*)\s*\(\)\s*\{?)"), 1, 0,
                 QStringLiteral("function")}};
    }
    if (syntax == QStringLiteral("markdown")) {
        return {{pattern(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)"), 2, 0, QStringLiteral("heading")}};
    }
    if (syntax == QStringLiteral("css")) {
        return {{pattern(R"(^\s*(@(?:media|keyframes|supports|font-face|layer)[^{]*?)\s*\{)"), 1,
                 0, QStringLiteral("at-rule")},
                {pattern(R"(^\s*([^\s{}@/][^{};]*?)\s*\{)"), 1, 0, QStringLiteral("rule")}};
    }
    if (syntax == QStringLiteral("yaml")) {
        return {{pattern(R"(^([A-Za-z_][\w.-]*)\s*:)"), 1, 0, QStringLiteral("key")}};
    }
    if (syntax == QStringLiteral("toml")) {
        return {{pattern(R"(^\s*\[\[?\s*([^\]]+?)\s*\]\]?)"), 1, 0, QStringLiteral("table")}};
    }
    if (syntax == QStringLiteral("properties")) {
        return {{pattern(R"(^\s*\[([^\]]+)\])"), 1, 0, QStringLiteral("section")}};
    }
    if (syntax == QStringLiteral("sql")) {
        return {{pattern(
                     R"(^\s*create\s+(?:or\s+replace\s+)?(table|view|function|procedure|index|trigger|type|schema)\s+(?:if\s+not\s+exists\s+)?([\w."\[\]]+))",
                     true),
                 2, 1, {}}};
    }
    if (syntax == QStringLiteral("makefile")) {
        return {{pattern(R"(^([A-Za-z0-9_./-]+)\s*:(?!=))"), 1, 0, QStringLiteral("target")}};
    }
    if (syntax == QStringLiteral("cmake")) {
        return {{pattern(R"(^\s*(function|macro)\s*\(\s*([A-Za-z_]\w*))", true), 2, 1, {}}};
    }
    if (syntax == QStringLiteral("dockerfile")) {
        return {{pattern(R"(^\s*FROM\s+\S+\s+AS\s+(\S+))", true), 1, 0, QStringLiteral("stage")}};
    }
    if (syntax == QStringLiteral("html") || syntax == QStringLiteral("xml")) {
        return {{pattern(R"(<h([1-6])[^>]*>([^<]+))", true), 2, 0, QStringLiteral("heading")},
                {pattern(R"(<([\w-]+)[^>]*\sid\s*=\s*["']([^"']+))", true), 2, 0,
                 QStringLiteral("element")}};
    }
    return cLikeRules();
}

bool usesTypedFunctionPattern(const QString& syntax) {
    static const QSet<QString> syntaxes{QStringLiteral("generic"), QStringLiteral("c-cpp"),
                                        QStringLiteral("java"), QStringLiteral("csharp"),
                                        QStringLiteral("dart"), QStringLiteral("php")};
    return syntaxes.contains(syntax);
}

bool hasNoSymbols(const QString& syntax) {
    static const QSet<QString> syntaxes{QString(), QStringLiteral("plain"), QStringLiteral("json"),
                                        QStringLiteral("diff"), QStringLiteral("large-file")};
    return syntaxes.contains(syntax);
}

int indentationColumns(const QString& line) {
    int columns = 0;
    for (const QChar character : line) {
        if (character == u' ') {
            ++columns;
        } else if (character == u'\t') {
            columns += 4;
        } else {
            break;
        }
    }
    return columns;
}

} // namespace

QList<DocumentSymbol> extractDocumentSymbols(const QByteArray& text, const QString& syntaxName,
                                             const int maximumSymbols) {
    QList<DocumentSymbol> symbols;
    if (hasNoSymbols(syntaxName)) {
        return symbols;
    }

    static const QSet<QString> controlKeywords{
        QStringLiteral("if"),     QStringLiteral("for"),    QStringLiteral("while"),
        QStringLiteral("switch"), QStringLiteral("return"), QStringLiteral("catch"),
        QStringLiteral("else"),   QStringLiteral("sizeof"), QStringLiteral("new"),
        QStringLiteral("delete"), QStringLiteral("do"),     QStringLiteral("foreach"),
        QStringLiteral("using"),  QStringLiteral("lock"),   QStringLiteral("emit")};
    const QList<SymbolRule> rules = rulesForSyntax(syntaxName);
    const bool typedFunctions = usesTypedFunctionPattern(syntaxName);
    const bool markdown = syntaxName == QStringLiteral("markdown");
    constexpr int maximumTypedFunctionIndent = 4;

    int lineNumber = 0;
    for (const QByteArray& rawLine : text.split('\n')) {
        ++lineNumber;
        if (symbols.size() >= maximumSymbols) {
            break;
        }
        QString line = QString::fromUtf8(rawLine);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.trimmed().isEmpty()) {
            continue;
        }

        const int indent = indentationColumns(line);
        bool matched = false;
        for (const SymbolRule& rule : rules) {
            const auto match = rule.pattern.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            const QString name = match.captured(rule.nameGroup).trimmed();
            if (name.isEmpty() || controlKeywords.contains(name)) {
                continue;
            }
            const QString kind =
                rule.kindGroup > 0 ? normalizedKind(match.captured(rule.kindGroup)) : rule.kind;
            const int depth = markdown ? static_cast<int>(match.capturedLength(1)) - 1 : indent / 4;
            symbols.append({name, kind, lineNumber, depth});
            matched = true;
            break;
        }
        if (matched || !typedFunctions || indent > maximumTypedFunctionIndent) {
            continue;
        }

        const auto match = typedFunctionPattern().match(line);
        if (match.hasMatch()) {
            const QString name = match.captured(1);
            const QString bareName = name.section(QStringLiteral("::"), -1);
            if (!controlKeywords.contains(bareName)) {
                symbols.append({name, QStringLiteral("function"), lineNumber, indent / 4});
            }
        }
    }
    return symbols;
}

} // namespace ketplus
