#include "runtime/executor/NativeJobSupervisor.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
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
