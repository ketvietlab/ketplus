#pragma once

#include <QFont>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>

#include <functional>

namespace ketplus {

// Measures a transcript row's body without realizing its delegate.
//
// The geometry model can only be exact about rows it has heard a height for,
// and it only hears from rows the view has realized -- so a window the reader
// has not scrolled through is mostly one constant repeated. This closes that
// gap by measuring bodies off the GUI thread.
//
// Two measurements decided the shape of this class:
//
// Shaping once and re-wrapping many times, which is how Telegram's text engine
// answers "how tall at width W" cheaply, does not transfer: QTextLayout
// re-shapes on every layout pass, so a re-wrap at a new width costs the same as
// the first one (76 ms against 77 ms over 45 bodies) and the same as
// QTextDocument (76 ms). There is no cheap re-wrap to build on, so we use
// QTextDocument and keep the code that already agrees with the delegate.
//
// Measuring on a worker thread gives identical answers -- 45 of 45 bodies to
// 0.0000 px -- at 75 ms against 140 ms on the GUI thread. That is what makes
// measuring affordable at all: an earlier attempt to force layout on the GUI
// thread pushed p99 flick frames from 16 ms to 138 ms. Nothing here runs on the
// GUI thread except delivering the result.
//
// A body carrying an image is not measured. Images resolve through the
// document's resource handler, which is GUI-thread state, and a height that
// silently omitted them would be worse than no height at all: the geometry
// model would believe it. A Mermaid block the reader has switched to its diagram
// is measured as source until its delegate reports the real height.
class TranscriptRowMeasurer final : public QObject {
    Q_OBJECT
  public:
    explicit TranscriptRowMeasurer(QObject* parent = nullptr);

    struct Request {
        QString key;
        QString body;
        bool markdown{false};
    };

    // Measures what it can and drops the rest. Requests for a key already in
    // flight at the same geometry are ignored rather than queued twice.
    void measure(const QList<Request>& requests, qreal width, const QFont& font);

    // Anything measured at a different width or font is not an approximation of
    // this one, so in-flight results for the previous geometry are discarded
    // when they arrive rather than being applied.
    void setGeometry(qreal width, const QFont& font);

    // True when this body would resolve a GUI-thread resource, so it can only be
    // measured by its own delegate.
    [[nodiscard]] static bool measurable(const QString& body);

    [[nodiscard]] int outstanding() const { return int(inFlight_.size()); }

    // The delegate renders a complete assistant body as Markdown and everything
    // else literally.
    [[nodiscard]] static bool markdownBody(const QVariantMap& item);

    // Measures a page of history before it reaches the model, so its rows are
    // published with their heights instead of acquiring them after they are on
    // screen. Source entries are rendered into rows the way the model renders
    // them, measured across worker threads at the geometry the view last asked
    // for, and cached by row key; `done` then runs on `context`'s thread. With no
    // geometry known yet it runs `done` straight away -- a page is never held
    // back waiting for a view that does not exist.
    static void premeasure(const QVariantList& sourceEntries, QObject* context,
                           std::function<void()> done);

    // A pre-measured body height, if one was taken for exactly this body at the
    // current geometry.
    [[nodiscard]] static bool cachedBodyHeight(const QString& key, const QString& body,
                                               bool markdown, qreal* height);

  signals:
    // Body height only. The chrome around it -- header, padding, row spacing --
    // is the delegate's, and is learned from realized rows rather than
    // reconstructed here, because reconstructing it would be a second copy of
    // the QML layout to drift out of step with the first.
    void bodyMeasured(const QString& key, qreal bodyHeight);

  private:
    void deliver(const QString& key, qreal height, quint64 generation);

    QSet<QString> inFlight_;
    qreal width_{0};
    QFont font_;
    // Bumped on every geometry change so a result computed against the old one
    // can be recognized and dropped.
    quint64 generation_{0};
};

} // namespace ketplus
