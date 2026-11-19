#include "AutoreleaseProbe.h"
#include "service/DaemonApplication.h"

#include <QEvent>
#include <QEventLoop>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <functional>

using namespace ketplus::daemon;
using namespace ketplus::test;

namespace {

const auto probeEventType = static_cast<QEvent::Type>(QEvent::registerEventType());

// Runs its handler for each probe event it receives, on the thread it lives in.
class ProbeReceiver final : public QObject {
  public:
    explicit ProbeReceiver(std::function<void()> handler = autoreleaseProbe)
        : handler_(std::move(handler)) {}

    bool event(QEvent* event) override {
        if (event->type() != probeEventType)
            return QObject::event(event);
        handler_();
        return true;
    }

  private:
    std::function<void()> handler_;
};

void postProbes(QObject* receiver, int count) {
    for (int index = 0; index < count; ++index)
        QCoreApplication::postEvent(receiver, new QEvent(probeEventType));
}

} // namespace

// QCoreApplication's event loop on macOS never drains an autorelease pool, so an object that Qt
// or a framework autoreleases while the daemon handles an event must be released by the pool
// DaemonApplication puts around that delivery, not when the process exits.
class DaemonAutoreleasePoolTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { resetReleasedProbes(); }

    void postedEventsReleaseWhatTheyAutorelease() {
        ProbeReceiver receiver;
        postProbes(&receiver, 50);
        QCoreApplication::sendPostedEvents(&receiver, probeEventType);
        QCOMPARE(releasedProbes(), 50);
    }

    void timerCallbacksReleaseWhatTheyAutorelease() {
        QTimer::singleShot(0, this, [] { autoreleaseProbe(); });
        QTRY_COMPARE(releasedProbes(), 1);
    }

    void workerThreadEventsReleaseWhatTheyAutorelease() {
        QThread thread;
        ProbeReceiver receiver;
        receiver.moveToThread(&thread);
        thread.start();
        const auto stop = qScopeGuard([&thread] {
            thread.quit();
            thread.wait();
        });
        postProbes(&receiver, 50);
        // Delivered after the probes, so they have all been handled when it returns.
        QMetaObject::invokeMethod(&receiver, [] {}, Qt::BlockingQueuedConnection);
        QCOMPARE(releasedProbes(), 50);
    }

    void aNestedEventLoopKeepsThePoolsBalanced() {
        int releasedInside = -1;
        ProbeReceiver receiver([&releasedInside] {
            autoreleaseProbe();
            QEventLoop loop;
            QTimer::singleShot(0, &loop, [&loop] {
                autoreleaseProbe();
                loop.quit();
            });
            loop.exec();
            // The inner delivery has its own pool; the outer one is still open.
            releasedInside = releasedProbes();
        });
        postProbes(&receiver, 1);
        QCoreApplication::sendPostedEvents(&receiver, probeEventType);
        QCOMPARE(releasedInside, 1);
        QCOMPARE(releasedProbes(), 2);
    }
};

int main(int argc, char* argv[]) {
    DaemonApplication application(argc, argv);
    DaemonAutoreleasePoolTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "DaemonAutoreleasePoolTest.moc"
