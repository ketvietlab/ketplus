#include "Environment.h"
#include "commands/CommandSupport.h"
#include "commands/Commands.h"
#include "run/RunLoop.h"
#include "state/RunnerState.h"

#include <QSet>

namespace ketplus::runner::app {
namespace {

struct CommandSpec {
    QLatin1StringView name;
    ExitCode (*run)(const CommandOptions&, const Environment&);
    QSet<QString> options; // without the leading "--"
};

const QList<CommandSpec>& commands() {
    static const QList<CommandSpec> list{
        {QLatin1StringView("register"),
         registerRunner,
         {QStringLiteral("token-file"), QStringLiteral("name"), QStringLiteral("label"),
          QStringLiteral("state-dir")}},
        {QLatin1StringView("pair"),
         pairDesktop,
         {QStringLiteral("token-file"), QStringLiteral("state-dir")}},
        {QLatin1StringView("unpair"), unpairDesktop, {QStringLiteral("state-dir")}},
        {QLatin1StringView("deregister"),
         deregisterRunner,
         {QStringLiteral("local-only"), QStringLiteral("state-dir")}},
        {QLatin1StringView("status"), showStatus, {QStringLiteral("state-dir")}},
        {QLatin1StringView("run"), runRunner, {QStringLiteral("state-dir")}},
    };
    return list;
}

// Options take their value as the next argument or after "="; "-" alone is a positional
// (standard input).
std::optional<CommandOptions> parse(const QStringList& arguments, const CommandSpec& spec,
                                    const Environment& environment) {
    CommandOptions options;
    options.command = spec.name;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString& argument = arguments.at(i);
        if (!argument.startsWith(QStringLiteral("--"))) {
            options.positional.append(argument);
            continue;
        }
        QString name = argument.mid(2);
        std::optional<QString> value;
        if (const qsizetype equals = name.indexOf(QLatin1Char('=')); equals >= 0) {
            value = name.mid(equals + 1);
            name.truncate(equals);
        }
        if (!spec.options.contains(name)) {
            environment.err(
                QStringLiteral("`%1` does not take --%2; see --help.").arg(spec.name, name));
            return std::nullopt;
        }
        if (name == QStringLiteral("local-only")) {
            if (value) {
                environment.err(QStringLiteral("--local-only takes no value."));
                return std::nullopt;
            }
            options.localOnly = true;
            continue;
        }
        if (!value) {
            if (i + 1 >= arguments.size()) {
                environment.err(QStringLiteral("--%1 needs a value.").arg(name));
                return std::nullopt;
            }
            value = arguments.at(++i);
        }
        if (name == QStringLiteral("state-dir"))
            options.stateDir = *value;
        else if (name == QStringLiteral("token-file"))
            options.tokenFile = *value;
        else if (name == QStringLiteral("name"))
            options.name = *value;
        else if (name == QStringLiteral("label"))
            options.labels.append(*value);
    }
    return options;
}

} // namespace

QString helpText() {
    return QStringLiteral(
        "Usage: ketplus-runner COMMAND [OPTIONS]\n"
        "\n"
        "Commands:\n"
        "  register TOKEN|- [--token-file FILE] [--name NAME] [--label LABEL]...\n"
        "                      Register this node with a pairing token for a new runner\n"
        "  pair TOKEN|- [--token-file FILE]\n"
        "                      Pair this runner with another desktop\n"
        "  unpair MACHINE_ID   Stop trusting a desktop and end the pair\n"
        "  deregister [--local-only]\n"
        "                      Remove this runner from the Relay and delete its state\n"
        "  status              Show the runner and its desktops\n"
        "  run                 Keep the runner online\n"
        "\n"
        "Every command takes --state-dir DIR (default: $KETPLUS_RUNNER_STATE_DIR, else\n"
        "$XDG_STATE_HOME/ketplus-runner, else ~/.local/state/ketplus-runner).\n"
        "A token given as \"-\" is read from standard input.\n"
        "\n"
        "Exit codes:\n"
        "  0  success\n"
        "  1  failure: the Relay could not be reached, or refused without a code below\n"
        "  2  usage: bad arguments, or a token, name or label refused before any call\n"
        "  3  state directory: permissions, owner, corrupt file, or in use by another runner\n"
        "  4  desktop_key_mismatch: the Relay reported a desktop the token does not name\n"
        "  5  the desktop's approval does not verify\n"
        "  6  the pairing was rejected, expired, revoked or is unknown\n"
        "  7  the Relay no longer accepts this runner's credential: register again\n"
        "  8  this runner was removed from the Relay\n"
        "  9  this platform is not supported (Linux on x86_64 or arm64 only)");
}

int runCli(const QStringList& arguments, const Environment& environment) {
    if (arguments.isEmpty() || arguments.front() == QStringLiteral("--help") ||
        arguments.front() == QStringLiteral("-h") || arguments.front() == QStringLiteral("help")) {
        (arguments.isEmpty() ? environment.err : environment.out)(helpText());
        return toInt(arguments.isEmpty() ? ExitCode::Usage : ExitCode::Ok);
    }
    if (arguments.front() == QStringLiteral("--version")) {
        environment.out(QStringLiteral("ketplus-runner %1").arg(environment.runnerVersion));
        return toInt(ExitCode::Ok);
    }
    for (const CommandSpec& spec : commands()) {
        if (arguments.front() != spec.name)
            continue;
        if (arguments.contains(QStringLiteral("--help"))) {
            environment.out(helpText());
            return toInt(ExitCode::Ok);
        }
        const std::optional<CommandOptions> options = parse(arguments, spec, environment);
        return toInt(options ? spec.run(*options, environment) : ExitCode::Usage);
    }
    environment.err(QStringLiteral("Unknown command `%1`; see --help.").arg(arguments.front()));
    return toInt(ExitCode::Usage);
}

ExitCode runRunner(const CommandOptions& options, const Environment& environment) {
    if (!options.positional.isEmpty()) {
        environment.err(QStringLiteral("Usage: ketplus-runner run"));
        return ExitCode::Usage;
    }
    if (!supportedPlatform(environment))
        return ExitCode::UnsupportedPlatform;
    if (!relayConfigured(environment))
        return ExitCode::Failure;
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    if (!state.hasIdentity()) {
        environment.err(QStringLiteral("This runner is not registered; run `ketplus-runner "
                                       "register` first."));
        return ExitCode::Usage;
    }
    QString error;
    std::optional<RunnerIdentity> identity = state.loadIdentity(&error);
    if (!identity) {
        environment.err(error);
        return ExitCode::State;
    }
    RelayClient relay(environment.relayUrl, environment.requestTimeoutMs);
    AccessTokens tokens(relay, state, *identity, environment);
    PairingContext context{environment, relay, state, tokens};
    return runHeartbeatLoop(context, *identity);
}

} // namespace ketplus::runner::app
