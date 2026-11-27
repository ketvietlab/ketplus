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
    if (!m_pieces.empty() && m_pieces.back().offset + m_pieces.back().bytes.size() == m_end &&
        m_pieces.back().bytes.size() + bytes.size() <= pieceBytes)
        m_pieces.back().bytes.append(bytes);
    else
        m_pieces.push_back({m_end, bytes});
    m_end += bytes.size();
    m_held += bytes.size();
    if (m_held > m_capacity)
        dropFront(m_held - m_capacity);
}

void OutputBuffer::skip(const qint64 bytes) {
    if (bytes > 0)
        m_end += bytes;
}

void OutputBuffer::acknowledge(const qint64 offset) { dropTo(std::min(offset, m_end)); }

void OutputBuffer::clear() {
    m_pieces.clear();
    m_end = 0;
    m_held = 0;
}

qint64 OutputBuffer::start() const { return m_pieces.empty() ? m_end : m_pieces.front().offset; }

qint64 OutputBuffer::heldFrom(const qint64 from) const {
    for (const Piece& piece : m_pieces) {
        if (piece.offset + piece.bytes.size() > from)
            return std::max(from, piece.offset);
    }
    return m_end;
}

void OutputBuffer::dropTo(const qint64 offset) {
    while (!m_pieces.empty() && m_pieces.front().offset < offset) {
        Piece& front = m_pieces.front();
        const qint64 drop = std::min<qint64>(offset - front.offset, front.bytes.size());
        if (drop == front.bytes.size()) {
            m_pieces.pop_front();
        } else {
            front.bytes.remove(0, drop);
            front.offset += drop;
        }
        m_held -= drop;
    }
}

// Drops the oldest `bytes` held bytes.
void OutputBuffer::dropFront(qint64 bytes) {
    while (bytes > 0 && !m_pieces.empty()) {
        Piece& front = m_pieces.front();
        const qint64 drop = std::min<qint64>(bytes, front.bytes.size());
        if (drop == front.bytes.size()) {
            m_pieces.pop_front();
        } else {
            front.bytes.remove(0, drop);
            front.offset += drop;
        }
        m_held -= drop;
        bytes -= drop;
    }
}

QByteArray OutputBuffer::read(const qint64 from, const qint64 maxBytes) const {
    QByteArray out;
    if (from >= m_end || maxBytes <= 0)
        return out;
    qint64 next = from;
    for (const Piece& piece : m_pieces) {
        const qint64 pieceEnd = piece.offset + piece.bytes.size();
        if (pieceEnd <= next)
            continue;
        if (piece.offset > next)
            break; // a range that is not held
        const qint64 skip = next - piece.offset;
        const qint64 take = std::min<qint64>(piece.bytes.size() - skip, maxBytes - out.size());
        out.append(piece.bytes.constData() + skip, take);
        next += take;
        if (out.size() >= maxBytes)
            break;
    }
    return out;
}

} // namespace ketplus::runner::session
