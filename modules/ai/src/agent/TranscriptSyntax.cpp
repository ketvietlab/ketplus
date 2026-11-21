#include "agent/TranscriptSyntax.h"
#include "ui/theme/SchemeDocumentSyntax.h"
namespace ketplus {
void applyTranscriptSyntax(QTextDocument* document, ket::Theme theme, const ColorScheme* resolved) {
    if (resolved) {
        applySchemeDocumentSyntax(document, *resolved);
        return;
    }
    static const auto light = resolveColorScheme(builtinColorScheme(SchemeKind::Light));
    static const auto dark = resolveColorScheme(builtinColorScheme(SchemeKind::Dark));
    applySchemeDocumentSyntax(document, theme == ket::Theme::Dark ? dark : light);
}
} // namespace ketplus
