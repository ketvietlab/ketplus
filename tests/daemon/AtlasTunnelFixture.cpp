#include "atlas/AtlasLiveTunnel.h"
#include <QCoreApplication>
#include <QTimer>
#include <QUrl>
using namespace ketplus::daemon;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4)
        return 2;
    const auto port = args[2].toUShort();
    const auto relay = QUrl(args[1]);
    if (!port || relay.scheme() != "ws" || relay.host() != "127.0.0.1")
        return 2;
    AtlasLiveTunnel tunnel(QByteArray(32, char(7)), args[3], {{"viewer", port}});
    QObject::connect(&tunnel, &AtlasLiveTunnel::closed, &app, &QCoreApplication::quit);
    tunnel.connectRelay(relay, "isolated-test-only");
    QTimer::singleShot(120000, &app, &QCoreApplication::quit);
    return app.exec();
}
