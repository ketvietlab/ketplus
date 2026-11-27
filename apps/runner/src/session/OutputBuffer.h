#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <deque>

// The job's output from the first byte the desktop has not acknowledged to the last byte the
// command wrote, at most `capacity` bytes: beyond it the oldest bytes are dropped. Offsets count
// every byte the command wrote, dropped ones included. Bytes lost before they reached the buffer
// leave a range that is never held.
namespace ketplus::runner::session {

inline constexpr qint64 outputRingBytes = 16LL * 1024 * 1024;

class OutputBuffer {
  public:
    explicit OutputBuffer(qint64 capacity = outputRingBytes) : m_capacity(capacity) {}

    void append(const QByteArray& bytes);
    // `bytes` more bytes of output that will never be held.
    void skip(qint64 bytes);
    // The desktop holds every byte before `offset`.
    void acknowledge(qint64 offset);
    void clear();

    // The first byte still held (end() when none is), and the end of the output (its total size).
    [[nodiscard]] qint64 start() const;
    [[nodiscard]] qint64 end() const { return m_end; }
    // `from` when that byte is held, otherwise the next byte held after it, or end().
    [[nodiscard]] qint64 heldFrom(qint64 from) const;
    // Up to `maxBytes` held bytes from `from`, stopping at a range that is not held.
    [[nodiscard]] QByteArray read(qint64 from, qint64 maxBytes) const;

  private:
    struct Piece {
        qint64 offset = 0;
        QByteArray bytes;
    };
    void dropTo(qint64 offset);
    void dropFront(qint64 bytes);

    qint64 m_capacity;
    qint64 m_end = 0;
    qint64 m_held = 0;
    std::deque<Piece> m_pieces; // the held bytes, in offset order
};

} // namespace ketplus::runner::session
