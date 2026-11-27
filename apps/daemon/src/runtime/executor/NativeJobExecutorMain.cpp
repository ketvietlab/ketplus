#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobSupervisor.h"
#include "service/DaemonApplication.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QTextStream>

int main(int argc, char** argv) {
    ketplus::daemon::DaemonApplication application(argc, argv);
    // The supervisor opens its output spool with QSQLITE before it writes any receipt.
    ketplus::daemon::DaemonApplication::addBundledPluginPath();
    const auto arguments = application.arguments();
#ifdef Q_OS_LINUX
    if (arguments.size() == 4 && arguments[1] == "--linux-probe")
        return ketplus::daemon::executor::linuxContainmentProbe(arguments[2], arguments[3]);
    if (arguments.size() == 5 && arguments[1] == "--linux-probe" && arguments[4] == "--mounts")
        return ketplus::daemon::executor::linuxContainmentProbe(arguments[2], arguments[3], true);
    if (arguments.size() == 2 && arguments[1] == "--linux-launch") {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly))
            return 125;
        const auto bytes = input.read(256 * 1024 + 1);
        if (bytes.size() > 256 * 1024)
            return 125;
        const auto spec = QJsonDocument::fromJson(bytes).object();
        return spec.isEmpty() ? 125 : ketplus::daemon::executor::linuxIsolationLaunch(spec);
    }
#endif
    // Packaging runs this against the installed bundle: the build tree finds Qt's plugins on its
    // own, so only the deployed executor shows whether it can open its output spool.
    if (arguments.size() == 2 && arguments[1] == "--check-runtime") {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
            QTextStream(stderr) << "KetPlus job executor runtime check failed: QSQLITE is "
                                   "unavailable.\n";
            return 1;
        }
        QTextStream(stdout) << "ketplus-job-executor runtime ok\n";
        return 0;
    }
    if (arguments.size() != 4 || arguments[2].isEmpty() || arguments[2].size() > 128 ||
        arguments[3].isEmpty() || arguments[3].size() > 128)
        return 64;
    ketplus::daemon::executor::NativeJobSupervisor supervisor(arguments[1], arguments[2],
                                                              arguments[3]);
    QString error;
    if (!supervisor.initialize(&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 125;
    }
    return application.exec();
}
