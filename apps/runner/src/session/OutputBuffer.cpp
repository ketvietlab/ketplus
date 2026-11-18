#include "session/OutputBuffer.h"

#include <algorithm>

namespace ketplus::runner::session {
namespace {

// Small writes are joined so a read walks few pieces.
constexpr qsizetype pieceBytes = 64 * 1024;

} // namespace

void OutputBuffer::append(const QByteArray& bytes) {
    if (bytes.isEmpty())
        return;
    if (!m_pieces.empty() && m_pieces.back().size() + bytes.size() <= pieceBytes)
        m_pieces.back().append(bytes);
    else
        m_pieces.push_back(bytes);
    m_end += bytes.size();
    if (m_end - m_start > m_capacity)
        dropTo(m_end - m_capacity);
}

void OutputBuffer::acknowledge(const qint64 offset) { dropTo(std::min(offset, m_end)); }

void OutputBuffer::clear() {
    m_pieces.clear();
    m_start = 0;
    m_end = 0;
}

void OutputBuffer::dropTo(const qint64 offset) {
    while (m_start < offset && !m_pieces.empty()) {
        QByteArray& front = m_pieces.front();
        const qint64 drop = std::min<qint64>(offset - m_start, front.size());
        if (drop == front.size())
            m_pieces.pop_front();
        else
            front.remove(0, drop);
        m_start += drop;
    }
}

QByteArray OutputBuffer::read(const qint64 from, const qint64 maxBytes) const {
    QByteArray out;
    if (from < m_start || from >= m_end || maxBytes <= 0)
        return out;
    qint64 position = m_start;
    for (const QByteArray& piece : m_pieces) {
        const qint64 pieceEnd = position + piece.size();
        if (pieceEnd > from) {
            const qint64 skip = std::max<qint64>(0, from - position);
            const qint64 take = std::min<qint64>(piece.size() - skip, maxBytes - out.size());
            out.append(piece.constData() + skip, take);
            if (out.size() >= maxBytes)
                break;
        }
        position = pieceEnd;
    }
    return out;
}

} // namespace ketplus::runner::session
