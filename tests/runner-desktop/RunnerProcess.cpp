#include "RunnerProcess.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTest>

#include <signal.h>

namespace runner_desktop_test {

RunnerProcess::RunnerProcess(QString program, const QString& root, QUrl relay)
    : m_program(std::move(program)), m_state(QDir(root).filePath(QStringLiteral("runner-state"))),
      m_work(QDir(root).filePath(QStringLiteral("runner-work"))), m_relay(std::move(relay)) {
    // The runner refuses a work directory other users can reach.
    QDir().mkpath(m_work);
    QFile::setPermissions(m_work, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

RunnerProcess::~RunnerProcess() { stop(10'000); }

bool RunnerProcess::start(const QStringList& arguments) {
    if (running())
        return false;
    m_process = std::make_unique<QProcess>();
    // The session's runtime directory and bus stay: the executor runs jobs as user units.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("KETPLUS_CONTROL_URL"), m_relay.toString());
    environment.insert(QStringLiteral("KETPLUS_RUNNER_STATE_DIR"), m_state);
    environment.remove(QStringLiteral("KETPLUS_RUN_RUNNER_DESKTOP_E2E"));
    m_process->setProcessEnvironment(environment);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(m_process.get(), &QProcess::readyRead, m_process.get(), [this] { read(); });
    m_process->start(m_program, arguments);
    return m_process->waitForStarted(10'000);
}

void RunnerProcess::read() {
    if (m_process)
        m_log += m_process->readAll();
}

bool RunnerProcess::startRegister(const QByteArray& token, const QStringList& labels) {
    QStringList arguments{QStringLiteral("register"), QStringLiteral("-"), QStringLiteral("--name"),
                          QStringLiteral("e2e-runner")};
    for (const QString& label : labels)
        arguments << QStringLiteral("--label") << label;
    if (!start(arguments))
        return false;
    m_process->write(token + '\n');
    m_process->closeWriteChannel();
    return true;
}

QString RunnerProcess::waitCode(const int timeoutMs) {
    static const QRegularExpression line(QStringLiteral("Confirmation code: (\\S+)"));
    QString code;
    (void)QTest::qWaitFor(
        [&] {
            code = line.match(QString::fromUtf8(m_log)).captured(1);
            return !code.isEmpty() || !running();
        },
        timeoutMs);
    return code;
}

bool RunnerProcess::startRun() {
    return start({QStringLiteral("run"), QStringLiteral("--work-dir"), m_work});
}

bool RunnerProcess::running() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

qint64 RunnerProcess::pid() const { return m_process ? m_process->processId() : 0; }

bool RunnerProcess::waitExit(const int timeoutMs, int* code) {
    if (!m_process)
        return false;
    if (!QTest::qWaitFor([this] { return !running(); }, timeoutMs))
        return false;
    read();
    if (code)
        *code = m_process->exitStatus() == QProcess::NormalExit ? m_process->exitCode() : -1;
    return true;
}

bool RunnerProcess::stop(const int timeoutMs) {
    if (!running())
        return true;
    ::kill(pid_t(pid()), SIGTERM);
    if (waitExit(timeoutMs))
        return true;
    kill();
    return false;
}

void RunnerProcess::kill() {
    if (!running())
        return;
    ::kill(pid_t(pid()), SIGKILL);
    (void)waitExit(10'000);
}

} // namespace runner_desktop_test
