#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ketplus {

// The readable rows of one remote conversation, built from the daemon's journal events
// (`event_type`, `payload`, `cursor`) as a transcript window, a page or a live event delivers
// them. It keeps what the machine sent, so the transcript stays readable from this cache while
// the machine is Reconnecting or Offline.
class MachineTranscript final {
  public:
    enum class Kind { User, Assistant, Activity, Error };
    struct Item {
        Kind kind{Kind::Activity};
        QString text;
        QString itemId;
        friend bool operator==(const Item&, const Item&) = default;
    };

    // Replaces the cache with a window's events (newest window on open or after a reconnect).
    void reset(const QJsonArray& events);
    // Applies one event; an event at or before the last applied cursor is ignored, so a reload
    // that overlaps live delivery adds nothing twice. Returns whether the rows changed.
    bool apply(const QJsonObject& delivered);
    // Adds an older page's events (oldest first) in front of the cache. Events at or after the
    // first cached cursor are ignored, so overlapping pages add nothing twice; the rows are
    // rebuilt from every kept event, so a turn split across pages reads as one.
    void prepend(const QJsonArray& older);

    [[nodiscard]] const QVector<Item>& items() const { return items_; }
    // The events kept, oldest first, in the transcript-window shape.
    [[nodiscard]] const QJsonArray& events() const { return events_; }
    [[nodiscard]] qint64 firstCursor() const;
    [[nodiscard]] qint64 lastCursor() const { return lastCursor_; }

  private:
    void rebuild();
    bool applyRow(const QJsonObject& event);

    QJsonArray events_;
    QVector<Item> items_;
    qint64 lastCursor_{0};
};

} // namespace ketplus
