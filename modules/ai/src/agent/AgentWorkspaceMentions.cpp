#include "agent/AgentWorkspaceView.h"
#include "agent/FileMentions.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSet>
#include <QThreadPool>

namespace ketplus {
namespace {

constexpr qint64 fileMentionCacheMs = 10 * 1000;
// Enough rows to fill the taller palette before it scrolls.
constexpr qsizetype kFileMentionSuggestions = 20;

} // namespace

// Listing runs `git ls-files` or walks the directory tree, which on a large worktree takes
// long enough to stall typing; it runs on the pool and replaces the entries when it lands.
void AgentWorkspaceView::refreshFileMentions() {
    const QString root = executionPath();
    if (root.isEmpty() || fileMentionLoadingRoot_ == root)
        return;
    fileMentionLoadingRoot_ = root;
    QThreadPool::globalInstance()->start([self = QPointer<AgentWorkspaceView>(this), root] {
        QStringList entries = file_mentions::listEntries(root);
        QStringList folded;
        folded.reserve(entries.size());
        for (const QString& entry : std::as_const(entries))
            folded.append(entry.toCaseFolded());
        QMetaObject::invokeMethod(
            qApp,
            [self, root, entries = std::move(entries), folded = std::move(folded)]() mutable {
                if (!self || self->fileMentionLoadingRoot_ != root)
                    return;
                self->fileMentionLoadingRoot_.clear();
                if (root != self->executionPath())
                    return;
                self->fileMentionRoot_ = root;
                self->fileMentionEntries_ = std::move(entries);
                self->fileMentionFoldedEntries_ = std::move(folded);
                self->fileMentionAge_.start();
                const bool linksChanged =
                    self->fileLinks_.setFiles(root, self->fileMentionEntries_);
                ++self->fileMentionRevision_;
                emit self->fileMentionsChanged();
                // Messages already shown were formatted without the list.
                if (linksChanged)
                    emit self->fileLinksChanged();
            },
            Qt::QueuedConnection);
    });
}

QVariantList AgentWorkspaceView::fileMentionSuggestions(const QString& query) {
    const bool current = fileMentionRoot_ == executionPath();
    if (!current || !fileMentionAge_.isValid() || fileMentionAge_.elapsed() > fileMentionCacheMs)
        refreshFileMentions();
    // Another worktree's entries would suggest paths that do not exist here.
    if (!current)
        return {};
    QVariantList suggestions;
    for (const QString& entry : file_mentions::rankEntries(
             fileMentionEntries_, fileMentionFoldedEntries_, query, kFileMentionSuggestions)) {
        const bool directory = entry.endsWith(QLatin1Char('/'));
        const QString trimmed = directory ? entry.chopped(1) : entry;
        const qsizetype slash = trimmed.lastIndexOf(QLatin1Char('/'));
        suggestions.append(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("file")},
            {QStringLiteral("path"), entry},
            {QStringLiteral("name"),
             trimmed.mid(slash + 1) + (directory ? QStringLiteral("/") : QString{})},
            {QStringLiteral("description"), slash < 0 ? QString{} : trimmed.left(slash)},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("mention"), file_mentions::insertionText(entry)},
        });
    }
    return suggestions;
}

QVariantList AgentWorkspaceView::conversationMentionSuggestions(const QString& query) const {
    if (!conversationDirectory_)
        return {};
    const QString needle = query.trimmed();
    QVariantList suggestions;
    for (const QVariant& value : conversationDirectory_()) {
        const QVariantMap conversation = value.toMap();
        const QString number = conversation.value(QStringLiteral("number")).toString();
        const QString title = conversation.value(QStringLiteral("title")).toString();
        if (number.isEmpty() || (!needle.isEmpty() && !number.startsWith(needle) &&
                                 !title.contains(needle, Qt::CaseInsensitive)))
            continue;
        const QString provider = conversation.value(QStringLiteral("provider")).toString();
        QStringList detail{provider.left(1).toUpper() + provider.mid(1)};
        if (conversation.value(QStringLiteral("running")).toBool())
            detail.append(tr("working"));
        if (const QString path = conversation.value(QStringLiteral("path")).toString();
            !path.isEmpty())
            detail.append(path.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty));
        suggestions.append(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("conversation")},
            {QStringLiteral("name"), QLatin1Char('#') + number},
            {QStringLiteral("mention"), QLatin1Char('#') + number},
            {QStringLiteral("description"), title},
            {QStringLiteral("detail"), detail.join(QStringLiteral(" · "))},
        });
        if (suggestions.size() >= kFileMentionSuggestions)
            break;
    }
    return suggestions;
}

bool AgentWorkspaceView::referencedPathsForPrompt(const QString& prompt, QVariantList& paths) {
    const QString root = executionPath();
    QSet<QString> seen;
    for (const file_mentions::Mention& mention : file_mentions::parseMentions(prompt)) {
        const file_mentions::ResolvedMention resolved = file_mentions::resolve(root, mention.path);
        if (resolved.resolution == file_mentions::Resolution::Outside) {
            emit composerErrorOccurred(
                tr("@%1 is outside this conversation's worktree. Mention only files inside it.")
                    .arg(mention.path));
            return false;
        }
        if (resolved.resolution == file_mentions::Resolution::Inside &&
            !seen.contains(resolved.relativePath)) {
            seen.insert(resolved.relativePath);
            paths.append(QVariantMap{
                {QStringLiteral("path"),
                 resolved.relativePath.isEmpty() ? QStringLiteral("./") : resolved.relativePath},
                {QStringLiteral("directory"), resolved.directory},
            });
        }
    }
    return true;
}

} // namespace ketplus
