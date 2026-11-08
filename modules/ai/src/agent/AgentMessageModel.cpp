#include "agent/AgentMessageModel.h"
#include "agent/TranscriptAtlasLinks.h"

#include <utility>

#include "agent/AgentMessageFormatter.h"
#include "agent/TranscriptActivityGroups.h"
#include "agent/TranscriptTrace.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace ketplus {
namespace {
bool presentationHidden(const QVariantMap& item) {
    // Preserve canonical history/source indexes, but allocate no delegate or geometry
    // to expired unanswered questions, including rows restored from an older cache.
    return item.value("presentationHidden").toBool() ||
           (item.value("subtype") == "question" && item.value("status") == "expired" &&
            item.value("questionAnswers").toMap().isEmpty());
}
} // namespace

AgentMessageModel::AgentMessageModel(QObject* parent) : QAbstractListModel(parent) {
    geometry_ = new TranscriptRowGeometry(this);
    measurer_ = new TranscriptRowMeasurer(this);
    // Off-thread results arrive one queued call each. They are collected and
    // handed to the geometry together, so a burst is one layout pass rather
    // than one per row.
    connect(measurer_, &TranscriptRowMeasurer::bodyMeasured, this,
            [this](const QString& key, const qreal bodyHeight) {
                pendingBodyHeights_.insert(key, bodyHeight);
                if (pendingBodyHeights_.size() > 1)
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        const auto heights = std::exchange(pendingBodyHeights_, {});
                        geometry_->setBodyHeights(heights);
                    },
                    Qt::QueuedConnection);
            });
    int role = Qt::UserRole;
    for (const auto* name : {"kind",
                             "body",
                             "meta",
                             "timestamp",
                             "filePath",
                             "subtype",
                             "command",
                             "running",
                             "imageSources",
                             "fileStats",
                             "questionData",
                             "questionAnswerData",
                             "messageKey",
                             "category",
                             "eventType",
                             "phase",
                             "title",
                             "summary",
                             "target",
                             "status",
                             "cwd",
                             "commandDescription",
                             "fileLocation",
                             "exitCode",
                             "durationMs",
                             "detailsData",
                             "payloadData",
                             "atlasLinks",
                             "detailsRef",
                             "payloadRef",
                             "partialBody",
                             "bodyPrefixCharacters",
                             "chunkContinuation",
                             "estimatedBodyLines"})
        roles_.insert(++role, name);
    for (const QString& name : agent_message::activityGroupRoleNames()) {
        roles_.insert(++role, name.toUtf8());
        groupRoleIds_.append(role);
    }
    // Geometry and rows reach the view as one state. Inside a row change the
    // geometry already knows the new rows while the window and the view still
    // hold the old ones, and anything reading positions then reads the wrong
    // rows. So the geometry is held until the change has been reported -- and
    // after the window has followed it, which is why these connect after it.
    const auto opened = [this] { ++rowTransactions_; };
    connect(this, &QAbstractItemModel::rowsAboutToBeInserted, this, opened);
    connect(this, &QAbstractItemModel::rowsAboutToBeRemoved, this, opened);
    connect(this, &QAbstractItemModel::rowsAboutToBeMoved, this, opened);
    connect(this, &QAbstractItemModel::modelAboutToBeReset, this, opened);
    // After the role table: the window reads it to find each row's key.
    window_ = new TranscriptWindowModel(this, this);
    const auto closed = [this] {
        rowTransactions_ = std::max(0, rowTransactions_ - 1);
        publishGeometry();
    };
    connect(this, &QAbstractItemModel::rowsInserted, this, closed);
    connect(this, &QAbstractItemModel::rowsRemoved, this, closed);
    connect(this, &QAbstractItemModel::rowsMoved, this, closed);
    connect(this, &QAbstractItemModel::modelReset, this, closed);
}

int AgentMessageModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}
QHash<int, QByteArray> AgentMessageModel::roleNames() const { return roles_; }
QVariant AgentMessageModel::data(const QModelIndex& index, int role) const {
    return index.isValid() && index.row() >= 0 && index.row() < count()
               ? items_[visible_[index.row()]].value(QString::fromUtf8(roles_.value(role)))
               : QVariant{};
}
QVariantMap AgentMessageModel::get(int index) const {
    return index >= 0 && index < count() ? items_[visible_[index]] : QVariantMap{};
}
QVariantMap AgentMessageModel::source(int index) const {
    return index >= 0 && index < sourceItems_.size() ? sourceItems_.at(index).toMap()
                                                     : QVariantMap{};
}
QVariantMap AgentMessageModel::sourceForRow(const int row) const {
    if (row < 0 || row >= count())
        return {};
    const int item = visible_.at(row);
    QString key = items_.at(item).value(QStringLiteral("messageKey")).toString();
    if (const qsizetype block = key.indexOf(QStringLiteral(":block:")); block > 0)
        key.truncate(block);
    const int sourceRow = sourceIndexes_.value(key, -1);
    if (sourceRow >= 0 && sourceRow < sourceItems_.size())
        return sourceItems_.at(sourceRow).toMap();
    const auto next = std::upper_bound(renderedStart_.cbegin(), renderedStart_.cend(), item);
    const auto fallback = int(next - renderedStart_.cbegin()) - 1;
    return fallback >= 0 && fallback < sourceItems_.size() ? sourceItems_.at(fallback).toMap()
                                                           : items_.at(item);
}
QString AgentMessageModel::searchableText(const int row) const {
    if (row < 0 || row >= count())
        return {};
    const QVariantMap& item = items_.at(visible_.at(row));
    const QString category = item.value(QStringLiteral("category")).toString();
    if (category == QStringLiteral("activity"))
        return agent_message::entryCopyText(item);
    return item.value(QStringLiteral("body")).toString();
}
QVariantList AgentMessageModel::findMatches(const QString& query, const int limit) const {
    QVariantList matches;
    if (query.isEmpty())
        return matches;
    // Search every rendered row, not only the visible ones: text inside a
    // collapsed group is still in the conversation, and find is how the reader
    // asks to be taken to it. A hidden hit reports its group so the caller can
    // open it before navigating.
    for (int row = 0; row < itemCount() && matches.size() < limit; ++row) {
        const QVariantMap& item = items_.at(row);
        if (item.value(QStringLiteral("presentationHidden")).toBool())
            continue;
        const QString category = item.value(QStringLiteral("category")).toString();
        const QString text = category == QStringLiteral("activity")
                                 ? agent_message::entryCopyText(item)
                                 : item.value(QStringLiteral("body")).toString();
        const QString key = item.value(QStringLiteral("messageKey")).toString();
        const int visibleRow = visibleRowOf(row);
        for (qsizetype start = text.indexOf(query, 0, Qt::CaseInsensitive);
             start >= 0 && matches.size() < limit;
             start = text.indexOf(query, start + query.size(), Qt::CaseInsensitive))
            matches.append(QVariantMap{
                {QStringLiteral("row"), visibleRow},
                {QStringLiteral("key"), key},
                {QStringLiteral("groupKey"),
                 visibleRow >= 0 ? QString{} : item.value(QStringLiteral("groupKey")).toString()},
                {QStringLiteral("start"), int(start)},
                {QStringLiteral("length"), int(query.size())}});
    }
    return matches;
}
int AgentMessageModel::indexOfKey(const QString& key) const { return indexes_.value(key, -1); }
int AgentMessageModel::groupLeaderIndex(const int row) const {
    if (row < 0 || row >= count())
        return -1;
    const QVariantMap& item = items_.at(visible_.at(row));
    if (item.value(QStringLiteral("groupRole")).toString().isEmpty())
        return -1;
    return indexes_.value(item.value(agent_message::activityGroupLeaderKeyName()).toString(), -1);
}
QStringList AgentMessageModel::activityGroupKeys() const {
    QStringList keys;
    for (const QVariantMap& item : items_)
        if (item.value(QStringLiteral("groupRole")).toString() == QStringLiteral("leader"))
            keys.append(item.value(QStringLiteral("groupKey")).toString());
    return keys;
}

qsizetype AgentMessageModel::estimatedTextBytes() const {
    if (textBytes_ >= 0)
        return textBytes_;
    qsizetype bytes = 0;
    for (const auto& value : sourceItems_) {
        const auto entry = value.toMap();
        for (auto it = entry.cbegin(); it != entry.cend(); ++it)
            if (it->metaType().id() == QMetaType::QString)
                bytes += it->toString().size() * 2;
    }
    textBytes_ = bytes;
    return bytes;
}

void AgentMessageModel::clearPayloads() {
    payloads_.clear();
    retainedPayloadBytes_ = 0;
    payloadAccess_ = 0;
}

void AgentMessageModel::evictPayloads(const qsizetype requiredBytes) {
    while (!payloads_.isEmpty() && (payloads_.size() >= kMaximumPayloadCount ||
                                    retainedPayloadBytes_ + requiredBytes > kMaximumPayloadBytes)) {
        auto oldest = payloads_.begin();
        for (auto it = payloads_.begin(); it != payloads_.end(); ++it) {
            if (it->access < oldest->access)
                oldest = it;
        }
        retainedPayloadBytes_ -= oldest->json.size();
        payloads_.erase(oldest);
    }
}

QString AgentMessageModel::retainPayload(const QVariant& value, const QString& kind,
                                         QString* inlineJson) {
    const QByteArray json = QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact);
    if (json.size() <= kInlinePayloadBytes) {
        *inlineJson = QString::fromUtf8(json);
        return {};
    }
    const QString reference =
        QStringLiteral("transcript:%1:%2")
            .arg(kind, QString::fromLatin1(
                           QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex()));
    // Materialization also runs off the GUI thread. Publish references here;
    // only resolvePayload() owns and mutates the bounded GUI-thread cache.
    *inlineJson = QStringLiteral("{\"deferred\":true,\"byteSize\":%1}").arg(json.size());
    return reference;
}

QString AgentMessageModel::resolvePayload(const QString& reference) {
    auto found = payloads_.find(reference);
    if (found != payloads_.end()) {
        found->access = ++payloadAccess_;
        return QString::fromUtf8(found->json);
    }
    // The session owns the canonical payload. Rehydrate only a reference still
    // present in this model, and verify its content hash before exposing it.
    {
        for (const auto& item : items_) {
            for (const auto* kind : {"details", "payload"}) {
                if (item.value(QString::fromLatin1(kind) + "Ref").toString() != reference)
                    continue;
                QVariant value;
                const auto key = item.value("messageKey").toString();
                if (payloadSource_)
                    value = payloadSource_(key, kind);
                // Standalone previews and retained windows can have canonical source
                // rows even when the external owner no longer supplies this detail.
                // The content hash below still fences the fallback to this exact row.
                if (!value.isValid()) {
                    const int sourceRow = sourceIndexes_.value(key, -1);
                    if (sourceRow >= 0 && sourceRow < sourceItems_.size())
                        value = sourceItems_.at(sourceRow).toMap().value(
                            QString::fromLatin1(kind) == "payload" ? "sanitizedPayload"
                                                                   : "details");
                }
                if (!value.isValid())
                    continue;
                const auto json = QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact);
                const auto expected =
                    QStringLiteral("transcript:%1:%2")
                        .arg(kind, QString::fromLatin1(
                                       QCryptographicHash::hash(json, QCryptographicHash::Sha256)
                                           .toHex()));
                if (reference != expected)
                    continue;
                if (json.size() <= kMaximumPayloadBytes) {
                    evictPayloads(json.size());
                    payloads_.insert(reference, RetainedPayload{json, ++payloadAccess_});
                    retainedPayloadBytes_ += json.size();
                }
                return QString::fromUtf8(json);
            }
        }
    }
    return QStringLiteral("{\"deferred\":true,\"available\":false}");
}

QVariantMap AgentMessageModel::normalized(QVariantMap item) {
    for (const auto* key : {"kind", "body", "meta", "timestamp", "filePath", "subtype", "command",
                            "category", "eventType", "phase", "title", "summary", "target",
                            "status", "cwd", "commandDescription", "fileLocation"})
        if (!item.contains(key))
            item.insert(key, QString{});
    if (item.value("category").toString().isEmpty()) {
        const QString kind = item.value("kind").toString();
        item.insert("category", kind == QStringLiteral("user")        ? QStringLiteral("user")
                                : kind == QStringLiteral("assistant") ? QStringLiteral("final")
                                                                      : QStringLiteral("activity"));
    }
    if (item.value("eventType").toString().isEmpty()) {
        const QString category = item.value("category").toString();
        item.insert("eventType", category == QStringLiteral("user") ? QStringLiteral("user.message")
                                 : category == QStringLiteral("final")
                                     ? QStringLiteral("assistant.final")
                                     : QStringLiteral("activity.generic"));
    }
    // Keep every tool event in sourceItems_ for copy/export, while the transcript
    // presents only browser visits and visual evidence.
    if (item.value("subtype").toString() == QStringLiteral("ketplusBrowser")) {
        const QString event = item.value("eventType").toString();
        item.insert("presentationHidden", event != QStringLiteral("browser.open") &&
                                              event != QStringLiteral("browser.screenshot") &&
                                              event != QStringLiteral("browser.compare"));
    }
    if (item.value("subtype").toString().isEmpty() &&
        item.value("kind").toString() == QStringLiteral("file")) {
        item.insert("subtype", QStringLiteral("fileChange"));
        item.insert("eventType", QStringLiteral("file.change"));
    }
    if (item.value("title").toString().isEmpty())
        item.insert("title", item.value("meta").toString());
    if (item.value("summary").toString().isEmpty())
        item.insert("summary", item.value("body").toString());
    item.insert("atlasLinks", QStringList{});
    if (!item.contains("running"))
        item.insert("running", false);
    if (!item.contains("partialBody"))
        item.insert("partialBody", false);
    if (!item.contains("bodyPrefixCharacters"))
        item.insert("bodyPrefixCharacters", 0);
    if (!item.contains("chunkContinuation"))
        item.insert("chunkContinuation", false);
    if (!item.contains("estimatedBodyLines")) {
        const QString body = item.value("body").toString();
        item.insert("estimatedBodyLines", std::max<qsizetype>(1, body.count(u'\n') + 1));
    }
    if (!item.contains("exitCode"))
        item.insert("exitCode", QVariant{});
    if (!item.contains("durationMs"))
        item.insert("durationMs", QVariant{});
    if (item.value("messageKey").toString().isEmpty())
        item.insert("messageKey", QUuid::createUuid().toString());
    // Ungrouped defaults, so group recomputation only reports rows whose grouping changed.
    for (const auto* key : {"groupKey", "groupRole", "groupSummary"})
        if (!item.contains(key))
            item.insert(key, QString{});
    for (const auto* key : {"groupSize", "groupFailed"})
        if (!item.contains(key))
            item.insert(key, 0);
    if (!item.contains("groupRunning"))
        item.insert("groupRunning", false);
    QVariantList imageSources;
    for (const QVariant& image : item.value("images", QVariantList{}).toList()) {
        if (!image.toString().trimmed().isEmpty())
            imageSources.append(image);
    }
    // Structured roles are exposed as QVariantList/QVariantMap; delegates use
    // them as JS arrays/objects without a JSON round trip.
    item.insert("imageSources", imageSources);
    item.insert("fileStats", item.value("fileChanges", QVariantList{}).toList());
    item.insert("questionData", item.value("questions", QVariantList{}).toList());
    item.insert("questionAnswerData", item.value("questionAnswers", QVariantMap{}).toMap());
    QString detailsData;
    const QString detailsRef = retainPayload(item.value("details", QVariantMap{}),
                                             QStringLiteral("details"), &detailsData);
    QString payloadData;
    const QString payloadRef = retainPayload(item.value("sanitizedPayload", QVariantMap{}),
                                             QStringLiteral("payload"), &payloadData);
    item.remove("details");
    item.remove("sanitizedPayload");
    item.insert("detailsData", detailsData);
    item.insert("payloadData", payloadData);
    item.insert("detailsRef", detailsRef);
    item.insert("payloadRef", payloadRef);
    return item;
}
namespace {
constexpr qsizetype kTargetChunkCharacters = 12 * 1024;
constexpr qsizetype kMaximumChunkCharacters = 16 * 1024;
constexpr qsizetype kDeferredMaterializationCharacters = 512 * 1024;

struct MarkdownBoundary final {
    qsizetype end;
    bool safe;
    bool fencedAfter;
};

MarkdownBoundary safeMarkdownBoundary(const QString& body, qsizetype start, bool fencedBefore) {
    const qsizetype target = std::min(start + kTargetChunkCharacters, body.size());
    const qsizetype maximum = std::min(start + kMaximumChunkCharacters, body.size());
    const bool finalChunk = maximum >= body.size();

    // Prefer a complete top-level Markdown block. A blank line inside a fenced
    // block is not a safe boundary because it would reinterpret the following
    // text. The bounded fallback is a line boundary and is rendered as plain
    // text by renderedItems(), retaining exact source for copy/export.
    bool fenced = fencedBefore;
    qsizetype lineStart = start;
    while (lineStart < maximum) {
        const QStringView remaining(body.constData() + lineStart, maximum - lineStart);
        const qsizetype relativeLineEnd = remaining.indexOf(u'\n');
        const qsizetype end = relativeLineEnd < 0 ? maximum : lineStart + relativeLineEnd + 1;
        const QStringView line(body.constData() + lineStart, end - lineStart);
        const auto trimmed = line.trimmed();
        if ((relativeLineEnd >= 0 || finalChunk) &&
            (trimmed.startsWith(u"```", Qt::CaseSensitive) ||
             trimmed.startsWith(u"~~~", Qt::CaseSensitive)))
            fenced = !fenced;
        if (!fenced && end >= target && end < body.size() && body.at(end) == u'\n') {
            return {end + 1, true, false};
        }
        if (end >= maximum || end >= body.size())
            break;
        lineStart = end;
    }
    if (finalChunk)
        return {body.size(), !fenced, fenced};
    const qsizetype newline = body.lastIndexOf(u'\n', maximum - 1);
    return {newline > start ? newline + 1 : maximum, false, fenced};
}
} // namespace

QList<QVariantMap> AgentMessageModel::renderedItems(const QVariantMap& source, int sourceIndex) {
    QVariantMap base = source;
    if (base.value("messageKey").toString().isEmpty())
        base.insert("messageKey", QStringLiteral("snapshot:%1").arg(sourceIndex));
    const QString body = base.value("body").toString();
    QList<QVariantMap> result;
    if (base.value("kind") != QStringLiteral("assistant") || body.size() <= kMaximumChunkCharacters)
        result.append(normalized(base));
    else
        appendChunks(&result, base, body, base.value("messageKey").toString(), 0, false, false);
    if (!result.isEmpty() && base.value("kind") == QStringLiteral("assistant") &&
        !base.value("running").toBool() && !base.value("partialBody").toBool())
        result.last().insert("atlasLinks", transcriptAtlasLinks(body));
    return result;
}

void AgentMessageModel::appendChunks(QList<QVariantMap>* result, const QVariantMap& base,
                                     const QString& body, const QString& originalKey,
                                     qsizetype offset, bool fenced, bool forcedPlainContinuation) {
    while (offset < body.size()) {
        const auto boundary = safeMarkdownBoundary(body, offset, fenced);
        const qsizetype end = boundary.end;
        QVariantMap block = base;
        block.insert("body", body.mid(offset, end - offset));
        block.insert("bodyPrefixCharacters",
                     std::max<qint64>(0, base.value("bodyPrefixCharacters").toLongLong() - offset));
        block.insert("messageKey",
                     offset == 0 ? originalKey
                                 : QStringLiteral("%1:block:%2").arg(originalKey).arg(offset));
        block.insert("chunkContinuation", offset != 0);
        // Chunker state after this block; lets a streamed append resume
        // chunking at the last block without rescanning committed blocks.
        block.insert("chunkSafe", boundary.safe);
        block.insert("chunkFencedAfter", boundary.fencedAfter);
        // When a pathological fenced block has no safe boundary, render only
        // that bounded fragment as plain text. Complete Markdown blocks retain
        // rich semantics, links, images and Mermaid handling.
        if (forcedPlainContinuation || !boundary.safe || boundary.fencedAfter)
            block.insert("partialBody", true);
        forcedPlainContinuation = !boundary.safe;
        fenced = boundary.fencedAfter;
        if (offset != 0) {
            block.insert("meta", QString{});
            block.insert("images", QVariantList{});
        }
        result->append(normalized(block));
        offset = end;
    }
}

QList<QVariantMap> AgentMessageModel::renderedItems(const QVariantList& items) {
    QList<QVariantMap> result;
    result.reserve(items.size() + 8);
    for (int i = 0; i < items.size(); ++i) {
        result.append(renderedItems(items[i].toMap(), i));
    }
    return result;
}
bool AgentMessageModel::needsDeferredMaterialization(const QVariantList& items) {
    return std::any_of(items.cbegin(), items.cend(), [](const QVariant& value) {
        const auto item = value.toMap();
        return item.value("kind") == QStringLiteral("assistant") &&
               item.value("body").toString().size() >= kDeferredMaterializationCharacters;
    });
}
void AgentMessageModel::publishDeferredMaterialization(const QVariantList& source,
                                                       QList<QVariantMap> rendered,
                                                       const quint64 generation) {
    if (generation != materializationGeneration_ || sourceItems_ != source) {
        if (generation == deferredResetGeneration_)
            deferredResetActive_ = false;
        return;
    }
    publishDeferredBatch(source, std::make_shared<QList<QVariantMap>>(std::move(rendered)), 0,
                         generation);
}
void AgentMessageModel::publishDeferredBatch(const QVariantList& source,
                                             const std::shared_ptr<QList<QVariantMap>>& rendered,
                                             const int offset, const quint64 generation) {
    if (generation != materializationGeneration_ || sourceItems_ != source) {
        if (generation == deferredResetGeneration_)
            deferredResetActive_ = false;
        return;
    }
    const int batchSize = offset == 0 ? 1 : 32;
    const int remainingEnd = offset == 0 ? int(rendered->size()) : offset;
    const int start = std::max(0, remainingEnd - batchSize);
    if (remainingEnd > start) {
        for (int index = remainingEnd - 1; index >= start; --index)
            items_.prepend(rendered->at(index));
        regroup(0, remainingEnd - start);
        emit countChanged();
    }
    if (start > 0) {
        QTimer::singleShot(1, this, [this, source, rendered, start, generation] {
            publishDeferredBatch(source, rendered, start, generation);
        });
        return;
    }
    deferredResetActive_ = false;
    materializing_ = false;
    emit materializingChanged();
}

bool AgentMessageModel::memberOfCollapsedGroup(const QVariantMap& item) const {
    if (item.value(QStringLiteral("groupRole")).toString() != QStringLiteral("member"))
        return false;
    return !groupExpanded(item.value(QStringLiteral("groupKey")).toString());
}

QList<int> AgentMessageModel::computeVisible() const {
    QList<int> visible;
    visible.reserve(items_.size());
    for (int row = 0; row < items_.size(); ++row) {
        const auto& item = items_.at(row);
        if (!presentationHidden(item) && !memberOfCollapsedGroup(item))
            visible.append(row);
    }
    return visible;
}

QStringList AgentMessageModel::keysOf(const QList<int>& rows) const {
    QStringList keys;
    keys.reserve(rows.size());
    for (const int row : rows)
        keys.append(items_.at(row).value(QStringLiteral("messageKey")).toString());
    return keys;
}

// Explicit expand/collapse choices outlive a window change only while their group
// is still loaded; otherwise the map would grow for the life of the session.
void AgentMessageModel::pruneGroupExpansion() {
    if (groupExpansion_.isEmpty())
        return;
    QSet<QString> loaded;
    for (const QVariantMap& item : items_)
        if (item.value(QStringLiteral("groupRole")).toString() == QStringLiteral("leader"))
            loaded.insert(item.value(QStringLiteral("groupKey")).toString());
    for (auto it = groupExpansion_.begin(); it != groupExpansion_.end();)
        it = loaded.contains(it.key()) ? std::next(it) : groupExpansion_.erase(it);
}

void AgentMessageModel::resetVisible() {
    visible_ = computeVisible();
    publishedKeys_ = keysOf(visible_);
    rebuildKeys();
}

// Model row showing items_ row `row`, or -1 while its group is collapsed.
int AgentMessageModel::visibleRowOf(const int row) const {
    const auto at = std::lower_bound(visible_.cbegin(), visible_.cend(), row);
    return at != visible_.cend() && *at == row ? int(at - visible_.cbegin()) : -1;
}

// Model row of the first visible items_ row at or after `row`, which is where an
// insertion at `row` lands. Equals count() when nothing visible follows.
int AgentMessageModel::visibleRowAtOrAfter(const int row) const {
    return int(std::lower_bound(visible_.cbegin(), visible_.cend(), row) - visible_.cbegin());
}

// Re-derive the visible set without reporting anything, for callers that have
// already told the view what changed.
void AgentMessageModel::republishVisible() {
    visible_ = computeVisible();
    publishedKeys_ = keysOf(visible_);
    rebuildKeys();
}

// The single place that turns a change in items_ into model signals. Callers
// mutate items_ freely and then call this; the diff below is by messageKey,
// because items_ row numbers shift under the very edits being reported.
//
// Rows are only ever added or removed as one contiguous run here -- a group's
// members are adjacent, and so are a page of history or a chunked message -- so
// one run is expressed exactly and anything stranger falls back to a reset
// rather than being approximated into a wrong row number.
void AgentMessageModel::syncVisible() {
    // publishedKeys_ is what the view currently believes it has. It cannot be
    // recomputed from visible_ here: callers mutate items_ before calling, so
    // visible_ already points at rows that have moved or ceased to exist.
    const QStringList& before = publishedKeys_;
    QList<int> next = computeVisible();
    const QStringList after = keysOf(next);
    const auto commit = [this, &after](QList<int> rows) {
        visible_ = std::move(rows);
        publishedKeys_ = after;
        rebuildKeys();
    };
    if (before == after) {
        commit(std::move(next));
        return;
    }
    int prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before.at(prefix) == after.at(prefix))
        ++prefix;
    int suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before.at(before.size() - 1 - suffix) == after.at(after.size() - 1 - suffix))
        ++suffix;
    const int removed = int(before.size()) - prefix - suffix;
    const int inserted = int(after.size()) - prefix - suffix;
    if (removed > 0 && inserted > 0) {
        // Rows left and arrived at the same place -- an older page whose last
        // turn takes over a group, so the previous leader is drawn into it while
        // the page's own rows appear in front. Report it as the removal it is
        // followed by the insertion it is, never as a reset: every key outside
        // [prefix, suffix) survives, and a reset would make the view rebuild
        // and rehome every delegate it already has.
        QList<int> retained = next.mid(0, prefix);
        retained.append(next.mid(int(next.size()) - suffix, suffix));
        QStringList retainedKeys = after.mid(0, prefix);
        retainedKeys.append(after.mid(int(after.size()) - suffix, suffix));
        beginRemoveRows({}, prefix, prefix + removed - 1);
        visible_ = std::move(retained);
        publishedKeys_ = std::move(retainedKeys);
        rebuildKeys();
        endRemoveRows();
        beginInsertRows({}, prefix, prefix + inserted - 1);
        commit(std::move(next));
        endInsertRows();
        return;
    }
    if (removed > 0) {
        beginRemoveRows({}, prefix, prefix + removed - 1);
        commit(std::move(next));
        endRemoveRows();
        return;
    }
    beginInsertRows({}, prefix, prefix + inserted - 1);
    commit(std::move(next));
    endInsertRows();
}

bool AgentMessageModel::groupExpanded(const QString& key) const {
    return groupExpansion_.value(key, false);
}

void AgentMessageModel::setGroupExpanded(const QString& key, const bool expanded) {
    if (key.isEmpty() || groupExpansion_.value(key, !expanded) == expanded)
        return;
    groupExpansion_.insert(key, expanded);
    syncVisible();
    emit countChanged();
    emit groupExpansionChanged();
    emit remoteGroupRequested(key, expanded);
}

void AgentMessageModel::toggleGroup(const QString& key) {
    setGroupExpanded(key, !groupExpanded(key));
}

// Rows sharing this signature are laid out the same way around their body, so
// chrome learned from one applies to the others. Anything that changes the
// surrounding furniture belongs here; anything that only changes the body does
// not.
QString AgentMessageModel::rowShape(const QVariantMap& item) {
    return QStringLiteral("%1/%2/%3/%4/%5")
        .arg(item.value(QStringLiteral("kind")).toString(),
             item.value(QStringLiteral("category")).toString(),
             item.value(QStringLiteral("subtype")).toString(),
             item.value(QStringLiteral("groupRole")).toString(),
             item.value(QStringLiteral("chunkContinuation")).toBool() ? QStringLiteral("cont")
                                                                      : QString{});
}

void AgentMessageModel::measureUnmeasuredRows(const int limit, const qreal width,
                                              const QFont& font) {
    const QStringList keys = geometry_->unmeasuredKeys(limit);
    if (keys.isEmpty())
        return;
    QList<TranscriptRowMeasurer::Request> requests;
    requests.reserve(keys.size());
    for (const QString& key : keys) {
        const QVariantMap item = rowForKey(key);
        if (item.isEmpty())
            continue;
        // The delegate renders a complete assistant body as Markdown and
        // everything else literally; measuring the other one would answer a
        // different question than the reader will see.
        const bool markdown = TranscriptRowMeasurer::markdownBody(item);
        requests.append({key, item.value(QStringLiteral("body")).toString(), markdown});
    }
    measurer_->measure(requests, width, font);
}

void AgentMessageModel::rebuildKeys() {
    indexes_.clear();
    renderedIndexes_.clear();
    renderedStart_.clear();
    sourceIndexes_.clear();
    renderedStart_.reserve(sourceItems_.size());
    // renderedStart_ addresses items_ (streaming arrives by source row and must
    // reach a row even while its group is collapsed); indexes_ addresses the
    // rows the view sees, because QML looks keys up to get model rows.
    int visibleRow = 0;
    for (int i = 0; i < itemCount(); ++i) {
        if (!items_[i].value("chunkContinuation").toBool())
            renderedStart_.append(i);
        renderedIndexes_.insert(items_[i].value("messageKey").toString(), i);
        if (presentationHidden(items_[i]) || memberOfCollapsedGroup(items_[i]))
            continue;
        indexes_.insert(items_[i].value("messageKey").toString(), visibleRow++);
    }
    for (int i = 0; i < sourceItems_.size(); ++i) {
        const auto key = sourceItems_.at(i).toMap().value("messageKey").toString();
        if (!key.isEmpty() && !sourceIndexes_.contains(key))
            sourceIndexes_.insert(key, i);
    }
    textBytes_ = -1;
    // Every path that republishes rows ends here, so this is the one place that
    // keeps the geometry's row set identical to the one the view was told about.
    geometryPending_ = true;
    publishGeometry();
}

void AgentMessageModel::publishGeometry() {
    if (rowTransactions_ > 0 || !geometryPending_)
        return;
    geometryPending_ = false;
    QStringList shapes;
    shapes.reserve(visible_.size());
    for (const int item : visible_)
        shapes.append(rowShape(items_.at(item)));
    geometry_->reset(publishedKeys_, shapes);
    // Rows whose page was measured on a worker before it arrived take those
    // heights in the same step as they appear, rather than waiting for the idle
    // measurement pass once they may already be on screen.
    QHash<QString, qreal> premeasured;
    for (const QString& key : geometry_->unmeasuredKeys(std::numeric_limits<int>::max())) {
        const QVariantMap item = rowForKey(key);
        qreal height = 0;
        if (!item.isEmpty() && TranscriptRowMeasurer::cachedBodyHeight(
                                   key, item.value(QStringLiteral("body")).toString(),
                                   TranscriptRowMeasurer::markdownBody(item), &height))
            premeasured.insert(key, height);
    }
    if (!premeasured.isEmpty())
        geometry_->setBodyHeights(premeasured);
}
void AgentMessageModel::regroup(const int first, const int last) {
    if (items_.isEmpty()) {
        syncVisible();
        return;
    }
    const QList<int> changed = agent_message::assignActivityGroups(items_, first, last);
    // Group roles decide which rows exist, so settle the row set before
    // reporting role changes against it.
    syncVisible();
    // Only unloaded groups are dropped, so no loaded row changes visibility here.
    pruneGroupExpansion();
    for (const int row : changed) {
        const int visibleRow = visibleRowOf(row);
        if (visibleRow >= 0)
            emit dataChanged(index(visibleRow), index(visibleRow), groupRoleIds_);
    }
}
void AgentMessageModel::reset(const QVariantList& items) {
    clearPayloads();
    const quint64 generation = ++materializationGeneration_;
    const bool deferred = needsDeferredMaterialization(items);
    beginResetModel();
    sourceItems_ = items;
    items_ = deferred ? QList<QVariantMap>{} : renderedItems(items);
    agent_message::assignActivityGroups(items_);
    pruneGroupExpansion();
    resetVisible();
    endResetModel();
    if (materializing_ != deferred) {
        materializing_ = deferred;
        emit materializingChanged();
    }
    emit countChanged();
    for (const auto& row : items_)
        if (row.value("subtype") == "daemonGroup" &&
            groupExpanded(row.value("groupKey").toString()))
            emit remoteGroupRequested(row.value("groupKey").toString(), true);
    if (!deferred) {
        deferredPending_ = false;
        deferredPendingReset_ = false;
        deferredPendingSource_.clear();
        deferredPendingUpdates_.clear();
        return;
    }
    scheduleDeferredReset(items, generation);
}
bool AgentMessageModel::resetIfChanged(const QVariantList& items) {
    if (sourceItems_ == items)
        return false;
    if (transcript_trace::enabled() && !sourceItems_.isEmpty()) {
        // A warm conversation rebuilt from rows that barely differ is a stall
        // on every switch back; name the first differing row and fields (never
        // their text) to find what drifted.
        const auto shared = std::min(sourceItems_.size(), items.size());
        qsizetype row = 0;
        while (row < shared && sourceItems_.at(row) == items.at(row))
            ++row;
        QStringList fields;
        if (row < shared) {
            const auto before = sourceItems_.at(row).toMap();
            const auto after = items.at(row).toMap();
            for (auto it = after.cbegin(); it != after.cend(); ++it)
                if (before.value(it.key()) != it.value())
                    fields.append(it.key());
            for (auto it = before.cbegin(); it != before.cend(); ++it)
                if (!after.contains(it.key()))
                    fields.append(it.key());
        }
        transcript_trace::write(
            QStringLiteral("model-diff"),
            {{QStringLiteral("model"), QString::number(quintptr(this), 16)},
             {QStringLiteral("row"), int(row)},
             {QStringLiteral("key"), row < shared
                                         ? items.at(row).toMap().value(QStringLiteral("messageKey"))
                                         : QVariant{}},
             {QStringLiteral("fields"), fields}});
    }
    resetFor("session", items);
    return true;
}
void AgentMessageModel::resetFor(const char* reason, const QVariantList& items) {
    // A reset recreates every delegate and forgets the reader's row unless its
    // key survives; the trace names which path fell back to one.
    if (transcript_trace::enabled())
        transcript_trace::write(QStringLiteral("model-reset"),
                                {{QStringLiteral("reason"), QString::fromLatin1(reason)},
                                 {QStringLiteral("model"), QString::number(quintptr(this), 16)},
                                 {QStringLiteral("before"), int(sourceItems_.size())},
                                 {QStringLiteral("after"), int(items.size())},
                                 {QStringLiteral("materializing"), materializing_}});
    reset(items);
}
void AgentMessageModel::append(const QVariantMap& item) {
    const auto key = item.value("messageKey").toString();
    if (!key.isEmpty()) {
        if (const int index = sourceIndexes_.value(key, -1); index >= 0) {
            set(index, item);
            return;
        }
    }
    // A giant row re-rendering on a worker leaves every other row in place, and
    // an appended row shifts no source index. Only a reset still being built or
    // published has no row set to append to.
    if (deferredResetInFlight()) {
        auto source = sourceItems_;
        source.append(item);
        resetFor("append-materializing", source);
        return;
    }
    const auto rendered = renderedItems(item, sourceItems_.size());
    const int firstInserted = itemCount();
    sourceItems_.append(item);
    items_.append(rendered);
    regroup(firstInserted, itemCount() - 1);
    emit countChanged();
}
void AgentMessageModel::prepend(const QVariantList& incoming) {
    QVariantList items;
    QSet<QString> keys;
    for (const auto& value : incoming) {
        const auto key = value.toMap().value("messageKey").toString();
        if (!key.isEmpty() && (keys.contains(key) || sourceIndexes_.contains(key)))
            continue;
        keys.insert(key);
        items.append(value);
    }
    if (items.isEmpty())
        return;
    if (deferredResetInFlight()) {
        resetFor("prepend-materializing", items + sourceItems_);
        return;
    }
    // Pending worker renders address source rows by index, which the prefix
    // shifts. Take them by key and schedule them again at their new rows.
    const QStringList deferredKeys = takeDeferredUpdateKeys();
    QList<QVariantMap> prefix = renderedItems(items);
    sourceItems_ = items + sourceItems_;
    items_ = prefix + items_;
    regroup(0, int(prefix.size()));
    emit countChanged();
    rescheduleDeferredUpdates(deferredKeys);
}

QStringList AgentMessageModel::takeDeferredUpdateKeys() {
    QStringList keys;
    for (const auto* updates : {&deferredPendingUpdates_, &deferredActiveUpdates_})
        for (const QVariantMap& update : *updates)
            keys.append(update.value(QStringLiteral("messageKey")).toString());
    deferredPendingUpdates_.clear();
    deferredActiveUpdates_.clear();
    if (!deferredPendingReset_)
        deferredPending_ = false;
    return keys;
}

void AgentMessageModel::rescheduleDeferredUpdates(const QStringList& keys) {
    if (keys.isEmpty())
        return;
    // A new generation discards the running worker's result, which was
    // rendered for the old row numbers.
    const quint64 generation = ++materializationGeneration_;
    for (const QString& key : keys)
        if (const int row = sourceIndexes_.value(key, -1); row >= 0)
            scheduleDeferredUpdate(row, sourceItems_.at(row).toMap(), generation);
    settleMaterializing();
}

QString AgentMessageModel::visibleKeyFor(const QString& key) const {
    if (key.isEmpty())
        return {};
    if (indexes_.contains(key))
        return key;
    static const QString blockMarker = QStringLiteral(":block:");
    int row = renderedIndexes_.value(key, -1);
    if (row < 0) {
        // A block key whose offset moved when its body gained a prefix, or that
        // was re-chunked: the block of the same message nearest the old offset.
        const qsizetype marker = key.indexOf(blockMarker);
        const QString base = marker > 0 ? key.left(marker) : key;
        const qint64 offset = marker > 0 ? key.mid(marker + blockMarker.size()).toLongLong() : 0;
        row = renderedIndexes_.value(base, -1);
        if (row < 0)
            return {};
        for (int next = row + 1; next < itemCount(); ++next) {
            const QVariantMap& block = items_.at(next);
            if (!block.value(QStringLiteral("chunkContinuation")).toBool())
                break;
            const QString nextKey = block.value(QStringLiteral("messageKey")).toString();
            const qsizetype at = nextKey.indexOf(blockMarker);
            if (at < 0 || nextKey.mid(at + blockMarker.size()).toLongLong() > offset)
                break;
            row = next;
        }
    }
    if (visibleRowOf(row) >= 0)
        return items_.at(row).value(QStringLiteral("messageKey")).toString();
    // Withheld inside a collapsed group: the group's row is where it is shown.
    const QString leader =
        items_.at(row).value(agent_message::activityGroupLeaderKeyName()).toString();
    if (indexes_.contains(leader))
        return leader;
    const int visible = visibleRowAtOrAfter(row);
    return visible < count()
               ? items_.at(visible_.at(visible)).value(QStringLiteral("messageKey")).toString()
               : QString{};
}

void AgentMessageModel::replaceWindow(const QVariantList& items) {
    if (sourceItems_ == items)
        return;
    if (deferredResetInFlight()) {
        resetFor("window-materializing", items);
        return;
    }
    // As in prepend(): edge removals shift the rows pending worker renders.
    const QStringList deferredKeys = takeDeferredUpdateKeys();
    replaceWindowRows(items);
    rescheduleDeferredUpdates(deferredKeys);
}

void AgentMessageModel::replaceWindowRows(const QVariantList& items) {
    if (sourceItems_.isEmpty() || items.isEmpty()) {
        resetFor("window-empty", items);
        return;
    }
    // Newer-page shape: [evicted oldest rows] + retained rows + [appended newer rows]. Remove
    // and insert at the edges so rows around the reader keep their delegates.
    const int evicted =
        sourceIndexes_.value(items.constFirst().toMap().value("messageKey").toString(), -1);
    if (evicted >= 0 && evicted + items.size() < sourceItems_.size()) {
        // Idle-trim shape: rows cut from both edges around the reader, nothing new. Remove
        // both edges in place; a reset would rebuild every retained row.
        bool inside = true;
        for (int index = 0; inside && index < items.size(); ++index)
            inside = sourceItems_.at(evicted + index).toMap().value("messageKey") ==
                     items.at(index).toMap().value("messageKey");
        const int retainedEnd = evicted + int(items.size());
        const int firstNewerRemoval = renderedStart_.value(retainedEnd, -1);
        const int firstRetainedRendered = evicted > 0 ? renderedStart_.value(evicted, -1) : 0;
        if (inside && firstNewerRemoval > 0 && firstRetainedRendered >= 0 &&
            firstRetainedRendered < firstNewerRemoval) {
            sourceItems_.erase(sourceItems_.begin() + retainedEnd, sourceItems_.end());
            items_.erase(items_.begin() + firstNewerRemoval, items_.end());
            if (evicted > 0) {
                sourceItems_.erase(sourceItems_.begin(), sourceItems_.begin() + evicted);
                items_.erase(items_.begin(), items_.begin() + firstRetainedRendered);
            }
            regroup(0, 0);
            regroup(itemCount() - 1, itemCount() - 1);
            emit countChanged();
            return;
        }
    }
    if (evicted >= 0) {
        const int kept = int(sourceItems_.size()) - evicted;
        bool matches = kept <= items.size() && (evicted > 0 || kept < items.size());
        for (int index = 0; matches && index < kept; ++index)
            matches = sourceItems_.at(evicted + index).toMap().value("messageKey") ==
                      items.at(index).toMap().value("messageKey");
        if (matches) {
            if (evicted > 0) {
                const int firstRetainedRendered = renderedStart_.value(evicted, -1);
                if (firstRetainedRendered <= 0) {
                    resetFor("window-evict-unmapped", items);
                    return;
                }
                sourceItems_.erase(sourceItems_.begin(), sourceItems_.begin() + evicted);
                items_.erase(items_.begin(), items_.begin() + firstRetainedRendered);
                regroup(0, 0);
                emit countChanged();
            }
            // Append the newer rows in one insertion (one countChanged) when none of them
            // duplicates a retained key; otherwise append() merges them one by one.
            QList<QVariantMap> rendered;
            QVariantList appended;
            QSet<QString> seen;
            bool duplicate = false;
            for (int index = kept; index < items.size() && !duplicate; ++index) {
                const auto item = items.at(index).toMap();
                const auto key = item.value("messageKey").toString();
                duplicate = !key.isEmpty() && (sourceIndexes_.contains(key) || seen.contains(key));
                seen.insert(key);
                rendered += renderedItems(item, int(sourceItems_.size() + appended.size()));
                appended.append(item);
            }
            if (duplicate) {
                for (int index = kept; index < items.size(); ++index)
                    append(items.at(index).toMap());
            } else if (!rendered.isEmpty()) {
                const int firstInserted = itemCount();
                sourceItems_.append(appended);
                items_.append(rendered);
                regroup(firstInserted, itemCount() - 1);
                emit countChanged();
            }
            return;
        }
    }
    const QString firstKey = sourceItems_.constFirst().toMap().value("messageKey").toString();
    int prefixCount = -1;
    for (int index = 0; index < items.size(); ++index) {
        if (!firstKey.isEmpty() &&
            items.at(index).toMap().value("messageKey").toString() == firstKey) {
            prefixCount = index;
            break;
        }
    }
    int retainedSources = 0;
    if (prefixCount >= 0) {
        while (retainedSources < sourceItems_.size() &&
               prefixCount + retainedSources < items.size() &&
               sourceItems_.at(retainedSources).toMap().value("messageKey") ==
                   items.at(prefixCount + retainedSources).toMap().value("messageKey"))
            ++retainedSources;
    }
    if (prefixCount < 0 || retainedSources == 0 || prefixCount + retainedSources != items.size()) {
        resetFor("window-unmatched", items);
        return;
    }

    if (retainedSources < sourceItems_.size()) {
        const int firstRenderedRemoval = renderedStart_.value(retainedSources, -1);
        if (firstRenderedRemoval < 0) {
            resetFor("window-trim-unmapped", items);
            return;
        }
        sourceItems_.erase(sourceItems_.begin() + retainedSources, sourceItems_.end());
        items_.erase(items_.begin() + firstRenderedRemoval, items_.end());
        regroup(itemCount() - 1, itemCount() - 1);
        emit countChanged();
    }
    if (prefixCount > 0)
        prepend(items.mid(0, prefixCount));
}
void AgentMessageModel::set(int row, const QVariantMap& item) {
    if (row < 0 || row >= sourceItems_.size())
        return;
    auto updated = item;
    const auto old = sourceItems_.at(row).toMap();
    // A deferred reset can expose no complete source mapping yet. Replace that
    // reset with the newest authoritative snapshot. Once rows exist, keep them
    // stable while a single worker coalesces updates below.
    if (materializing_ && items_.isEmpty()) {
        updated.insert("messageKey", old.value("messageKey"));
        auto source = sourceItems_;
        source[row] = updated;
        resetFor("set-before-materialized", source);
        return;
    }
    const int renderedRow = renderedStart_.value(row, -1);
    const QString oldKey = renderedRow >= 0 ? items_.at(renderedRow).value("messageKey").toString()
                                            : old.value("messageKey").toString();
    updated.insert("messageKey", oldKey);
    // A keyless source row takes its synthesized rendered key; keep key lookups in sync.
    if (old.value("messageKey").toString() != oldKey)
        sourceIndexes_.insert(oldKey, row);
    sourceItems_[row] = updated;
    textBytes_ = -1;
    const quint64 generation = ++materializationGeneration_;
    retainInvalidatedDeferredUpdates(generation);
    if (updated.value("kind") == QStringLiteral("assistant") &&
        updated.value("body").toString().size() >= kDeferredMaterializationCharacters) {
        if (!materializing_) {
            materializing_ = true;
            emit materializingChanged();
        }
        scheduleDeferredUpdate(row, updated, generation);
        return;
    }
    QList<QVariantMap> tail;
    int firstBlock = 0;
    if (renderIncrementally(row, old, updated, renderedRow, &tail, &firstBlock)) {
        applyRenderedUpdate(row, updated, std::move(tail), generation, firstBlock);
        return;
    }
    applyRenderedUpdate(row, updated, renderedItems(updated, row), generation);
}

bool AgentMessageModel::renderIncrementally(const int row, const QVariantMap& old,
                                            const QVariantMap& updated, const int renderedRow,
                                            QList<QVariantMap>* tail, int* firstBlock) const {
    // A streamed append leaves every block but the last byte-identical: a
    // non-final block's boundary depends only on text before it. Resume the
    // chunker at the last block instead of re-chunking the whole body.
    Q_UNUSED(row)
    if (!updated.value("running").toBool() || materializing_ || renderedRow < 0 ||
        old.size() != updated.size() || updated.value("kind") != QStringLiteral("assistant"))
        return false;
    const QString oldBody = old.value("body").toString();
    const QString body = updated.value("body").toString();
    if (body.size() <= kMaximumChunkCharacters || body.size() < oldBody.size() ||
        !body.startsWith(oldBody))
        return false;
    for (auto it = updated.cbegin(); it != updated.cend(); ++it) {
        if (it.key() == QStringLiteral("body"))
            continue;
        const auto found = old.constFind(it.key());
        if (found == old.cend() || *found != *it)
            return false;
    }
    int blocks = 1;
    while (renderedRow + blocks < items_.size() &&
           items_.at(renderedRow + blocks).value("chunkContinuation").toBool())
        ++blocks;
    if (blocks < 2)
        return false;
    const int last = renderedRow + blocks - 1;
    const auto& previous = items_.at(last - 1);
    if (!previous.contains("chunkSafe"))
        return false;
    qsizetype offset = 0;
    for (int index = renderedRow; index < last; ++index)
        offset += items_.at(index).value("body").toString().size();
    if (offset <= 0 || offset >= body.size() ||
        QStringView(oldBody).first(offset) != QStringView(body).first(offset))
        return false;
    const QString originalKey = items_.at(renderedRow).value("messageKey").toString();
    appendChunks(tail, updated, body, originalKey, offset,
                 previous.value("chunkFencedAfter").toBool(),
                 !previous.value("chunkSafe").toBool());
    *firstBlock = blocks - 1;
    return true;
}

void AgentMessageModel::scheduleDeferredReset(const QVariantList& source,
                                              const quint64 generation) {
    deferredPending_ = true;
    deferredPendingReset_ = true;
    deferredPendingSource_ = source;
    deferredPendingUpdates_.clear();
    deferredPendingGeneration_ = generation;
    maximumDeferredOutstanding_ = std::max(maximumDeferredOutstanding_, deferredOutstanding());
    startDeferredWork();
}

void AgentMessageModel::scheduleDeferredUpdate(const int sourceRow, const QVariantMap& updated,
                                               const quint64 generation) {
    // If a running batch is invalidated by this generation, retain all its
    // rows in the one pending batch. Otherwise an interleaved A/B stream could
    // publish B while leaving A at an older rendered revision forever.
    retainInvalidatedDeferredUpdates(generation);
    deferredPending_ = true;
    if (deferredPendingReset_) {
        if (sourceRow >= 0 && sourceRow < deferredPendingSource_.size())
            deferredPendingSource_[sourceRow] = updated;
    } else {
        deferredPendingSource_.clear();
        deferredPendingUpdates_.insert(sourceRow, updated);
        deferredPendingReset_ = false;
    }
    deferredPendingGeneration_ = generation;
    maximumDeferredOutstanding_ = std::max(maximumDeferredOutstanding_, deferredOutstanding());
    startDeferredWork();
}

void AgentMessageModel::retainInvalidatedDeferredUpdates(const quint64 generation) {
    if (!deferredWorkerActive_ || deferredActiveUpdates_.isEmpty())
        return;
    for (auto it = deferredActiveUpdates_.cbegin(); it != deferredActiveUpdates_.cend(); ++it) {
        if (it.key() < 0 || it.key() >= sourceItems_.size())
            continue;
        const auto current = sourceItems_.at(it.key()).toMap();
        if (current.value("messageKey") != it.value().value("messageKey"))
            continue;
        if (deferredPendingReset_ && it.key() < deferredPendingSource_.size())
            deferredPendingSource_[it.key()] = current;
        else
            deferredPendingUpdates_.insert(it.key(), current);
    }
    deferredPending_ = true;
    deferredPendingGeneration_ = generation;
    maximumDeferredOutstanding_ = std::max(maximumDeferredOutstanding_, deferredOutstanding());
}

void AgentMessageModel::startDeferredWork() {
    if (deferredWorkerActive_ || !deferredPending_)
        return;
    const bool reset = deferredPendingReset_;
    const QVariantList source = deferredPendingSource_;
    const QMap<int, QVariantMap> updates = deferredPendingUpdates_;
    const quint64 generation = deferredPendingGeneration_;
    deferredPending_ = false;
    deferredPendingReset_ = false;
    deferredPendingSource_.clear();
    deferredPendingUpdates_.clear();
    deferredWorkerActive_ = true;
    deferredActiveUpdates_ = updates;
    if (reset) {
        deferredResetActive_ = true;
        deferredResetGeneration_ = generation;
    }
    if (!materializing_) {
        materializing_ = true;
        emit materializingChanged();
    }
    maximumDeferredOutstanding_ = std::max(maximumDeferredOutstanding_, deferredOutstanding());

    QPointer<AgentMessageModel> guard(this);
    QThreadPool::globalInstance()->start([guard, reset, source, updates, generation] {
        QList<QVariantMap> renderedReset;
        QMap<int, QList<QVariantMap>> renderedUpdates;
        if (reset) {
            renderedReset = renderedItems(source);
        } else {
            for (auto it = updates.cbegin(); it != updates.cend(); ++it)
                renderedUpdates.insert(it.key(), renderedItems(it.value(), it.key()));
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, reset, source, updates, renderedReset = std::move(renderedReset),
             renderedUpdates = std::move(renderedUpdates), generation]() mutable {
                if (!guard)
                    return;
                if (reset)
                    guard->finishDeferredReset(source, std::move(renderedReset), generation);
                else
                    guard->finishDeferredUpdates(updates, std::move(renderedUpdates), generation);
            },
            Qt::QueuedConnection);
    });
}

void AgentMessageModel::finishDeferredReset(const QVariantList& source, QList<QVariantMap> rendered,
                                            const quint64 generation) {
    deferredWorkerActive_ = false;
    deferredActiveUpdates_.clear();
    publishDeferredMaterialization(source, std::move(rendered), generation);
    startDeferredWork();
}

void AgentMessageModel::finishDeferredUpdates(const QMap<int, QVariantMap>& updates,
                                              QMap<int, QList<QVariantMap>> rendered,
                                              const quint64 generation) {
    deferredActiveUpdates_.clear();
    if (generation == materializationGeneration_) {
        for (auto it = updates.cbegin(); it != updates.cend(); ++it)
            applyRenderedUpdate(it.key(), it.value(), std::move(rendered[it.key()]), generation);
    }
    // Keep the batch logically active while its rows are applied on the GUI
    // thread, otherwise the first row can publish a false "settled" state
    // before the remaining rows or a replacement batch are ready.
    deferredWorkerActive_ = false;
    startDeferredWork();
    settleMaterializing();
}

void AgentMessageModel::settleMaterializing() {
    if (materializing_ && !deferredWorkerActive_ && !deferredPending_) {
        materializing_ = false;
        emit materializingChanged();
    }
}

void AgentMessageModel::applyRenderedUpdate(const int sourceRow, const QVariantMap& updated,
                                            QList<QVariantMap> renderedUpdate,
                                            const quint64 generation, const int firstBlock) {
    if (generation != materializationGeneration_ || sourceRow < 0 ||
        sourceRow >= sourceItems_.size() || sourceItems_.at(sourceRow).toMap() != updated)
        return;
    const int renderedRow = renderedStart_.value(sourceRow, -1);
    if (renderedRow < 0) {
        resetFor("update-unmapped", sourceItems_);
        return;
    }
    int oldRenderedCount = 1;
    while (renderedRow + oldRenderedCount < items_.size() &&
           items_.at(renderedRow + oldRenderedCount).value("chunkContinuation").toBool())
        ++oldRenderedCount;
    // Diff this source row's rendered blocks by position. Block keys encode
    // their source offset, so a streamed body crossing a chunk boundary may
    // change continuation keys; that is still a local row change (dataChanged
    // plus tail insert/remove), never a whole-model reset.
    // renderedUpdate holds blocks [firstBlock, end); earlier blocks are
    // unchanged committed blocks and are neither compared nor re-emitted.
    if (firstBlock > oldRenderedCount) {
        resetFor("update-block-gap", sourceItems_);
        return;
    }
    const int newRenderedCount = firstBlock + int(renderedUpdate.size());
    const int overlap = std::min(oldRenderedCount, newRenderedCount);
    bool keysChanged = false;
    for (int block = firstBlock; block < overlap; ++block) {
        auto& normalizedUpdate = renderedUpdate[block - firstBlock];
        const int targetRow = renderedRow + block;
        // Group roles are derived; carry them so only real changes are compared and published.
        for (const QString& name : agent_message::activityGroupRoleNames())
            if (const auto found = items_[targetRow].constFind(name);
                found != items_[targetRow].cend())
                normalizedUpdate.insert(name, *found);
        if (const auto found =
                items_[targetRow].constFind(agent_message::activityGroupLeaderKeyName());
            found != items_[targetRow].cend())
            normalizedUpdate.insert(agent_message::activityGroupLeaderKeyName(), *found);
        QList<int> changed;
        for (auto it = roles_.cbegin(); it != roles_.cend(); ++it)
            if (items_[targetRow].value(it.value()) != normalizedUpdate.value(it.value()))
                changed.append(it.key());
        keysChanged = keysChanged ||
                      items_[targetRow].value("messageKey") != normalizedUpdate.value("messageKey");
        items_[targetRow] = normalizedUpdate;
        // A row inside a collapsed group has no model row of its own. Its delta
        // still lands in items_, and the regroup below republishes the leader's
        // summary, which is where the reader sees it.
        const int visibleRow = visibleRowOf(targetRow);
        if (!changed.isEmpty() && visibleRow >= 0)
            emit dataChanged(index(visibleRow), index(visibleRow), changed);
    }
    // Re-chunking renames the blocks it keeps as well as adding or dropping one,
    // so syncVisible()'s key diff would read a rename next to an insertion as a
    // removal and an insertion at once, and fall back to a reset -- on every
    // chunk boundary a streamed body crosses. Here the moved rows are known
    // exactly, and a chunk row is never a collapsed group member, so map the
    // rendered range straight into visible rows instead of inferring it.
    if (oldRenderedCount > newRenderedCount) {
        const int first = renderedRow + newRenderedCount;
        const int last = renderedRow + oldRenderedCount - 1;
        const int firstVisible = visibleRowAtOrAfter(first);
        const int lastVisible = visibleRowAtOrAfter(last + 1) - 1;
        const bool visibleRun = lastVisible >= firstVisible;
        if (visibleRun)
            beginRemoveRows({}, firstVisible, lastVisible);
        items_.erase(items_.begin() + first, items_.begin() + last + 1);
        republishVisible();
        if (visibleRun)
            endRemoveRows();
        emit countChanged();
    } else if (newRenderedCount > oldRenderedCount) {
        const int first = renderedRow + oldRenderedCount;
        const int firstVisible = visibleRowAtOrAfter(first);
        for (int block = oldRenderedCount; block < newRenderedCount; ++block)
            items_.insert(first + block - oldRenderedCount, renderedUpdate.at(block - firstBlock));
        beginInsertRows({}, firstVisible, firstVisible + newRenderedCount - oldRenderedCount - 1);
        republishVisible();
        endInsertRows();
        emit countChanged();
    } else if (keysChanged) {
        // The row set is unchanged, only a key's text is. Republish the key list
        // with it: syncVisible() diffs against publishedKeys_, and a key left
        // stale there reads as one row removed and another inserted, which is a
        // full reset -- during streaming, on every key change.
        republishVisible();
    }
    regroup(renderedRow, renderedRow + newRenderedCount - 1);
    settleMaterializing();
}

} // namespace ketplus
