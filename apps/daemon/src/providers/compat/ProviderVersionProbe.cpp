#include "providers/compat/ProviderVersionProbe.h"

#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <memory>
#include <utility>

namespace ketplus::daemon::compat {
namespace {

// Version banners are one short line; anything longer is not a version banner.
constexpr qsizetype maximumOutputBytes = 4096;

struct ProbeOutput final {
    QByteArray bytes;
    bool oversized{false};
};

} // namespace

ProviderVersionProbe::ProviderVersionProbe(Policy policy, InvocationBuilder invocation,
                                           QObject* parent)
    : QObject(parent), policy_(std::move(policy)), invocation_(std::move(invocation)),
      timeout_(new QTimer(this)) {
    timeout_->setSingleShot(true);
    timeout_->setInterval(timeoutMs);
    connect(timeout_, &QTimer::timeout, this, [this] {
        if (process_ != nullptr)
            process_->kill();
    });
}

ProviderVersionProbe::~ProviderVersionProbe() {
    if (process_ != nullptr) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(500);
    }
}

ProviderVersionProbe::Fingerprint ProviderVersionProbe::fingerprintOf(const QString& executable) {
    const QFileInfo info(executable);
    const QString resolved = info.canonicalFilePath();
    const QFileInfo target(resolved.isEmpty() ? executable : resolved);
    return {target.absoluteFilePath(), target.size(), target.lastModified()};
}

std::optional<Runtime> ProviderVersionProbe::cached(const QString& executable) const {
    if (cachedFingerprint_ && *cachedFingerprint_ == fingerprintOf(executable) &&
        cachedRuntime_.executable == executable)
        return cachedRuntime_;
    return std::nullopt;
}

void ProviderVersionProbe::check(const QString& executable, Callback callback) {
    if (executable.isEmpty()) {
        callback(missing(policy_));
        return;
    }
    if (const auto runtime = cached(executable)) {
        callback(*runtime);
        return;
    }
    if (process_ != nullptr &&
        (probing_ != executable || probingFingerprint_ != fingerprintOf(executable))) {
        // Only identical executable generations share a result. Re-check this request
        // after the current probe, including cache/fingerprint validation at that point.
        waiters_.append([guard = QPointer<ProviderVersionProbe>(this), executable,
                         callback = std::move(callback)](const Runtime&) mutable {
            if (guard)
                guard->check(executable, std::move(callback));
        });
        return;
    }
    waiters_.append(std::move(callback));
    if (process_ != nullptr)
        return;
    probing_ = executable;
    probingFingerprint_ = fingerprintOf(executable);
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process_->setProcessEnvironment(environment);
    const Invocation invocation = invocation_
                                      ? invocation_(executable)
                                      : Invocation{executable, {QStringLiteral("--version")}};
    process_->setProgram(invocation.program);
    process_->setArguments(invocation.arguments);
    // A CLI that ignores --version and reads stdin must still exit.
    connect(process_, &QProcess::started, process_, &QProcess::closeWriteChannel);
    const auto output = std::make_shared<ProbeOutput>();
    const auto readOutput = [this, output] {
        if (process_ == nullptr || output->oversized)
            return;
        const qsizetype remaining = maximumOutputBytes - output->bytes.size();
        const QByteArray chunk = process_->read(remaining + 1);
        if (chunk.size() > remaining) {
            output->oversized = true;
            // No further data from this one-shot probe can become a version result.
            process_->closeReadChannel(QProcess::StandardOutput);
            process_->kill();
            return;
        }
        output->bytes.append(chunk);
    };
    connect(process_, &QProcess::readyRead, this, readOutput);
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, output, readOutput](int exitCode, QProcess::ExitStatus status) {
                readOutput();
                finish(status == QProcess::NormalExit && exitCode == 0 && !output->oversized
                           ? QString::fromUtf8(output->bytes).trimmed()
                           : QString());
            });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({});
    });
    timeout_->start();
    process_->start();
}

void ProviderVersionProbe::finish(const QString& output) {
    timeout_->stop();
    if (process_ != nullptr) {
        process_->disconnect(this);
        process_->deleteLater();
        process_ = nullptr;
    }
    // A vanished executable is missing, not an unreadable version; the launch reports it.
    cachedRuntime_ = output.isEmpty() && !QFileInfo::exists(probing_)
                         ? missing(policy_)
                         : evaluate(policy_, probing_, output.section(QLatin1Char('\n'), 0, 0));
    // A probe that could not run is retried at the next launch rather than cached.
    if (!output.isEmpty())
        cachedFingerprint_ = probingFingerprint_;
    else
        cachedFingerprint_.reset();
    const Runtime runtime = cachedRuntime_;
    const auto waiters = std::exchange(waiters_, {});
    for (const Callback& waiter : waiters)
        waiter(runtime);
}

} // namespace ketplus::daemon::compat
