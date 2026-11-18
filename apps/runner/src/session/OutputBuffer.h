#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <deque>

// The job's output from the first byte the desktop has not acknowledged to the last byte the
// command wrote, at most `capacity` bytes: beyond it the oldest bytes are dropped. Offsets count
// every byte the command wrote, dropped ones included.
namespace ketplus::runner::session {

inline constexpr qint64 outputRingBytes = 16LL * 1024 * 1024;

class OutputBuffer {
  public:
    explicit OutputBuffer(qint64 capacity = outputRingBytes) : m_capacity(capacity) {}

    void append(const QByteArray& bytes);
    // The desktop holds every byte before `offset`.
    void acknowledge(qint64 offset);
    void clear();

    // The first byte still held, and the end of the output (its total size).
    [[nodiscard]] qint64 start() const { return m_start; }
    [[nodiscard]] qint64 end() const { return m_end; }
    // Up to `maxBytes` from `from`, which lies in [start(), end()].
    [[nodiscard]] QByteArray read(qint64 from, qint64 maxBytes) const;

  private:
    void dropTo(qint64 offset);

    qint64 m_capacity;
    qint64 m_start = 0;
    qint64 m_end = 0;
    std::deque<QByteArray> m_pieces; // the held bytes, m_start onwards
};

} // namespace ketplus::runner::session
