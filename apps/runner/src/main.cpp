#include "Environment.h"
#include "commands/Commands.h"

#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ketplus-runner"));
    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();
    return ketplus::runner::app::runCli(arguments, ketplus::runner::app::systemEnvironment());
}
