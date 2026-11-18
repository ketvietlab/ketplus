#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>

// What the command runner needs from the job executor and the platform. The real ones wrap the
// executor's client and output store; tests replace them.
namespace ketplus::runner::exec {

// One job of the executor: its supervisor process and the command it runs.
class ExecutorJob {
  public:
    // A message from the supervisor: "ready", "running", "stopping", "update" or "done", with
    // the supervisor's record of the job.
    using Event = std::function<void(const QJsonObject& event)>;
    virtual ~ExecutorJob() = default;
    // Starts the supervisor, which runs `execution` and stops it after `timeoutMs`. False when no
    // supervisor was started.
    [[nodiscard]] virtual bool start(const QJsonObject& execution, qint64 timeoutMs, Event event,
                                     QString* error) = 0;
    // For a job a previous runner process left: watches what its supervisor or the job's
    // directory still says, without running anything.
    [[nodiscard]] virtual bool reconnect(Event event, QString* error) = 0;
    virtual void stop(const QString& reason) = 0;
    // The final result is recorded: the supervisor may exit.
    virtual void acknowledge() = 0;
};

// The job's output store, read from the first byte not read yet.
class OutputSpool {
  public:
    struct Page {
        QByteArray bytes;
        qint64 lost = 0; // bytes the store dropped before they were read, ahead of `bytes`
        bool failed = false;
    };
    virtual ~OutputSpool() = default;
    [[nodiscard]] virtual Page read(int maxBytes) = 0;
};

class Executor {
  public:
    virtual ~Executor() = default;
    [[nodiscard]] virtual std::unique_ptr<ExecutorJob>
    job(const QString& directory, const QString& taskId, const QString& lease) = 0;
    [[nodiscard]] virtual std::unique_ptr<OutputSpool> output(const QString& directory,
                                                              const QString& taskId) = 0;
};

// Whether this machine can run job commands. Answers once, possibly later.
class PlatformPolicy {
  public:
    virtual ~PlatformPolicy() = default;
    virtual void probe(std::function<void(bool available, const QString& code)> done) = 0;
};

// The policy of this version on every platform: no job command runs.
class ClosedPolicy final : public PlatformPolicy {
  public:
    void probe(std::function<void(bool, const QString&)> done) override {
        done(false, QStringLiteral("containment_unavailable"));
    }
};

} // namespace ketplus::runner::exec
