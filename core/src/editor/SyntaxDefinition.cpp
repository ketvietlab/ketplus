#include "editor/SyntaxDefinition.h"

#include <QByteArray>
#include <QString>

#include <string_view>

namespace ketplus {
namespace {

std::string_view view(const QByteArray& bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

const SyntaxDefinition& syntaxDefinitionForPath(const QString& filePath) {
    return syntax::definitionForPath(view(filePath.toUtf8()));
}

const SyntaxDefinition* syntaxDefinitionByName(const QString& name) {
    return syntax::definitionByName(view(name.toUtf8()));
}

const std::vector<SyntaxChoice>& syntaxChoices() {
    return syntax::choices();
}

QString syntaxDisplayName(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    const std::string_view displayName = syntax::displayName(view(bytes));
    return QString::fromUtf8(displayName.data(), static_cast<qsizetype>(displayName.size()));
}

} // namespace ketplus
