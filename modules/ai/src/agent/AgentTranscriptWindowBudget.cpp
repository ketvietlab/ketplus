#include "agent/AgentTranscriptWindowBudget.h"

#include <algorithm>
#include <cmath>

namespace ketplus::transcript_window {

namespace {

// UTF-8 length without encoding: this runs over every retained row on every page.
qsizetype utf8Bytes(const QStringView text) {
    qsizetype bytes = text.size();
    for (qsizetype index = 0; index < text.size(); ++index) {
        const char16_t unit = text[index].unicode();
        if (unit < 0x80)
            continue;
        if (QChar::isHighSurrogate(unit) && index + 1 < text.size() &&
            QChar::isLowSurrogate(text[index + 1].unicode())) {
            bytes += 2; // Four bytes for the two units of a pair.
            ++index;
        } else {
            bytes += unit < 0x800 ? 1 : 2;
        }
    }
    return bytes;
}

// Approximately the compact JSON size of a nested value, without serialising it.
qsizetype nestedBytes(const QVariant& value) {
    switch (value.metaType().id()) {
    case QMetaType::QString:
        return utf8Bytes(get<QString>(value)) + 2;
    case QMetaType::QVariantMap: {
        const auto& map = get<QVariantMap>(value);
        qsizetype bytes = 2 + std::max<qsizetype>(0, map.size() - 1); // Braces and commas.
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            bytes += utf8Bytes(it.key()) + 3 + nestedBytes(it.value()); // Quotes and colon.
        return bytes;
    }
    case QMetaType::QVariantList: {
        const auto& list = get<QVariantList>(value);
        qsizetype bytes = 2 + std::max<qsizetype>(0, list.size() - 1);
        for (const auto& item : list)
            bytes += nestedBytes(item);
        return bytes;
    }
    default:
        return 8;
    }
}

} // namespace

qsizetype sourceBytes(const QVariantMap& message) {
    qsizetype bytes = 0;
    for (auto it = message.cbegin(); it != message.cend(); ++it) {
        if (it->metaType().id() == QMetaType::QString)
            bytes += utf8Bytes(get<QString>(*it));
        else if (it->metaType().id() == QMetaType::QVariantMap ||
                 it->metaType().id() == QMetaType::QVariantList)
            bytes += nestedBytes(*it);
    }
    return bytes;
}

int messageBudget(const double renderedHeight, const int loadedMessages,
                  const double viewportHeight) {
    if (!(renderedHeight > 0) || loadedMessages <= 0 || !(viewportHeight > 0))
        return maximumMessages;
    const double perMessage = renderedHeight / loadedMessages;
    const double wanted = std::ceil(viewportsPerWindow * viewportHeight / perMessage);
    if (!std::isfinite(wanted))
        return largestMessageBudget;
    return int(std::clamp(wanted, double(maximumMessages), double(largestMessageBudget)));
}

ByteCut byteCut(const QVector<QVariantMap>& messages, const int protectFrom, const int protectTo,
                const bool evictNewer, const qsizetype utf8ByteLimit) {
    const int count = int(messages.size());
    const int first = std::clamp(protectFrom, 0, count);
    const int last = std::clamp(protectTo, first, count);
    QVector<qsizetype> sizes;
    sizes.reserve(count);
    qsizetype total = 0;
    qsizetype older = 0;
    qsizetype newer = 0;
    for (int index = 0; index < count; ++index) {
        sizes.append(sourceBytes(messages.at(index)));
        total += sizes.constLast();
        if (index < first)
            older += sizes.constLast();
        else if (index >= last)
            newer += sizes.constLast();
    }
    if (!evictNewer)
        newer = 0;
    ByteCut cut;
    while (total > utf8ByteLimit && (older > 0 || newer > 0)) {
        if (older >= newer) {
            const qsizetype size = sizes.at(cut.fromOldest++);
            older -= size;
            total -= size;
        } else {
            const qsizetype size = sizes.at(count - 1 - cut.fromNewest++);
            newer -= size;
            total -= size;
        }
    }
    return cut;
}

TrimResult trim(QVector<QVariantMap>& messages, const RetainedEdge retainedEdge,
                const int messageLimit, const qsizetype utf8ByteLimit, const QString& readingKey) {
    TrimResult result;
    if (messages.isEmpty())
        return result;
    bool removeBack = retainedEdge == RetainedEdge::Oldest;
    int protectedRow = -1;

    // Compute sizes and turn keys once, against the original indices. A
    // turnless row is its own unit; its key must never be re-derived from a
    // shifted index after a removal (that made consecutive turnless rows alias).
    // Sizing walks every character of the window, so it is skipped when the
    // caller bounds the count only.
    const bool sized = utf8ByteLimit < unboundedUtf8Bytes;
    QVector<qsizetype> sizes;
    QVector<QString> keys;
    if (sized)
        sizes.reserve(messages.size());
    keys.reserve(messages.size());
    qsizetype bytes = 0;
    for (int index = 0; index < messages.size(); ++index) {
        if (!readingKey.isEmpty() &&
            messages.at(index).value("messageKey").toString() == readingKey)
            protectedRow = index;
        if (sized) {
            sizes.append(sourceBytes(messages.at(index)));
            bytes += sizes.constLast();
        }
        const QString turn = messages.at(index).value(QStringLiteral("turnId")).toString();
        keys.append(turn.isEmpty() ? QStringLiteral("\x01message:%1").arg(index) : turn);
    }

    // Removal happens on the [begin, end) range; the retained window is erased
    // from `messages` once at the end.
    int begin = 0;
    int end = int(messages.size());
    const auto over = [&] { return end - begin > messageLimit || bytes > utf8ByteLimit; };
    const auto removeOne = [&] {
        if (protectedRow >= begin && protectedRow < end) {
            // Evict the farther edge, leaving context on both sides of the reader.
            removeBack = end - 1 - protectedRow > protectedRow - begin;
        }
        const int index = removeBack ? end - 1 : begin;
        if (sized)
            bytes -= sizes.at(index);
        removeBack ? --end : ++begin;
    };

    // Prefer complete-turn eviction so ordinary pages never leave an orphan
    // tool/result row. Nothing is removed from the durable journal.
    while (protectedRow < 0 && end - begin > 1 && over()) {
        const int boundary = removeBack ? end - 1 : begin;
        const QString& key = keys.at(boundary);
        int turnSize = 1;
        while (turnSize < end - begin &&
               keys.at(removeBack ? end - 1 - turnSize : begin + turnSize) == key)
            ++turnSize;
        if (turnSize == end - begin)
            break;
        for (int removed = 0; removed < turnSize; ++removed)
            removeOne();
    }
    // Hard bound: a single remaining turn may still exceed either cap. Trim
    // individual rows from the evicted edge, always keeping at least the one
    // row at the retained edge. If that row alone exceeds the byte cap it is
    // the only row allowed to: the other rows are trimmed until they fit in
    // oversizedCompanionBytes. That small allowance keeps nearby context such
    // as the prompt (dropping it could never bring the window under the cap)
    // while bounding the total to the oversized row plus 1 MiB.
    const qsizetype retainedEdgeBytes =
        sized ? sizes.at(protectedRow >= 0 ? protectedRow : (removeBack ? 0 : messages.size() - 1))
              : 0;
    const qsizetype byteLimit = retainedEdgeBytes > utf8ByteLimit
                                    ? retainedEdgeBytes + oversizedCompanionBytes
                                    : utf8ByteLimit;
    while (end - begin > 1 && (end - begin > messageLimit || bytes > byteLimit))
        removeOne();

    if (begin == 0 && end == messages.size())
        return result;
    result.changed = true;
    result.removedNewer = end < messages.size();
    result.removedOlder = begin > 0;
    messages.remove(end, messages.size() - end);
    messages.remove(0, begin);
    return result;
}

} // namespace ketplus::transcript_window
