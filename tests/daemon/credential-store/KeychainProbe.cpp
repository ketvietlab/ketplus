#include "cloud/KeychainCredentialBackend.h"
#include <QCoreApplication>
#include <QFile>
using namespace ketplus::daemon;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2)
        return 2;
    SecKeychainRef keychain = nullptr;
    const auto path = QFile::encodeName(app.arguments().at(1));
    if (SecKeychainOpen(path.constData(), &keychain) != errSecSuccess)
        return 3;
    const auto backend = keychainCredentialBackend(keychain);
    QByteArray value;
    QString error;
    const auto result = backend->read("native-fixture", &value, &error);
    CFRelease(keychain);
    // This helper never requests interactive approval or prints the returned credential.
    return result == CredentialBackend::Result::Error && !error.isEmpty() && value.isEmpty() ? 0
                                                                                             : 1;
}
