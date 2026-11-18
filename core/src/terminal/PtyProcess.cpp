#include "terminal/PtyProcess.h"

#include "terminal/PtyBackend.h"

namespace ketplus {

PtyProcess::PtyProcess(QObject* parent) : QObject(parent), backend_(PtyBackend::create()) {
    connect(backend_.get(), &PtyBackend::outputReceived, this, &PtyProcess::outputReceived);
    connect(backend_.get(), &PtyBackend::exited, this, &PtyProcess::exited);
}

PtyProcess::~PtyProcess() { stop(); }

bool PtyProcess::isRunning() const noexcept { return backend_->isRunning(); }

QString PtyProcess::shellPath() const { return backend_->shellPath(); }

bool PtyProcess::start(const QString& workingDirectory, const int rows, const int columns) {
    if (isRunning()) {
        return true;
    }
    QString error;
    if (!backend_->start(workingDirectory, rows, columns, error)) {
        emit errorOccurred(error);
        return false;
    }
    emit started();
    return true;
}

void PtyProcess::send(const QByteArray& bytes) {
    if (!isRunning() || bytes.isEmpty()) {
        return;
    }
    backend_->write(bytes);
}

void PtyProcess::resizeTerminal(const int rows, const int columns) {
    backend_->resize(rows, columns);
}

void PtyProcess::stop() { backend_->stop(); }

} // namespace ketplus
