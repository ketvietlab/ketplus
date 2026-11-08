#include "agent/TranscriptAtlasLinks.h"
#include "agent/ChatMarkdown.h"

#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>

namespace ketplus {
namespace {
class LinkDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
} // namespace
QStringList transcriptAtlasLinks(const QString& markdown) {
    // This is a convenience receipt, not a reason to parse unbounded transcripts.
    if (markdown.size() > 128 * 1024 || !markdown.contains(".ketatlas/atlas.json"))
        return {};
    LinkDocument document;
    document.setMarkdown(chat_markdown::escapeRawHtml(markdown));
    QStringList result;
    QSet<QString> seen;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (!format.isAnchor() || format.isImageFormat())
                continue;
            const auto href = format.anchorHref();
            const QUrl url(href);
            if (href.size() > 4096 || !url.isValid() || !url.host().isEmpty() ||
                (!url.scheme().isEmpty() && !url.isLocalFile()) || url.hasQuery() ||
                url.hasFragment() || !url.path().endsWith(".ketatlas/atlas.json") ||
                seen.contains(href))
                continue;
            seen.insert(href);
            result.append(href);
            if (result.size() == 8)
                return result;
        }
    }
    return result;
}
} // namespace ketplus
