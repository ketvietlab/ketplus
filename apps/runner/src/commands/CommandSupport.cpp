#include "commands/CommandSupport.h"

#include "Environment.h"

#include <QFile>

namespace ketplus::runner::app {

bool supportedPlatform(const Environment& environment) {
    const Platform& platform = environment.platform;
    if (platform.os == QStringLiteral("linux") &&
        (platform.arch == QStringLiteral("x86_64") || platform.arch == QStringLiteral("arm64")))
        return true;
    environment.err(QStringLiteral("ketplus-runner runs on Linux on x86_64 or arm64; this is %1 "
                                   "on %2.")
                        .arg(platform.os, platform.arch));
    return false;
}

std::optional<StateDirectory> openState(const CommandOptions& options,
                                        const Environment& environment) {
    QString error;
    std::optional<StateDirectory> directory =
        StateDirectory::open(StateDirectory::resolve(options.stateDir, environment), &error);
    if (!directory)
        environment.err(error);
    return directory;
}

std::optional<pairing::PairingToken> readToken(const CommandOptions& options,
                                               const Environment& environment) {
    const bool fromArgument = !options.positional.isEmpty();
    const bool fromFile = !options.tokenFile.isEmpty();
    if (fromArgument == fromFile || options.positional.size() > 1) {
        environment.err(QStringLiteral("Give the pairing token once: as an argument, \"-\" for "
                                       "standard input, or --token-file FILE."));
        return std::nullopt;
    }
    QByteArray text;
    constexpr qint64 limit = pairing::maxPairingTokenBytes + 64;
    if (fromFile) {
        QFile file(options.tokenFile);
        if (!file.open(QIODevice::ReadOnly)) {
            environment.err(QStringLiteral("Cannot read %1").arg(options.tokenFile));
            return std::nullopt;
        }
        text = file.read(limit);
    } else if (options.positional.front() == QStringLiteral("-")) {
        text = environment.readStdin().left(limit);
    } else {
        text = options.positional.front().toUtf8();
    }
    pairing::PairingResult<pairing::PairingToken> token = pairing::PairingToken::parse(text);
    text.fill('\0');
    if (!token) {
        environment.err(QStringLiteral("The pairing token is not valid (%1).")
                            .arg(pairing::errorCode(token.error())));
        return std::nullopt;
    }
    return token.value();
}

bool relayConfigured(const Environment& environment) {
    if (environment.relayUrl.isValid())
        return true;
    environment.err(QStringLiteral("This build of ketplus-runner names no Relay."));
    return false;
}

} // namespace ketplus::runner::app
