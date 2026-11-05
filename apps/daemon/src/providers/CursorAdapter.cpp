#include "providers/CursorAdapter.h"
#include "BrowserAgentInstructions.h"
#include "BrowserToolLaunch.h"
#include "providers/CursorToolItem.h"
#include "providers/compat/ProviderVersionProbe.h"
#include "runtime/ResponseGuidance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumProviderMessageBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumStderrBytes = 1024 * 1024;
constexpr qsizetype maximumImageBytes = 4 * 1024 * 1024;

QString errorText(const QJsonObject& error) {
    const QString message = error.value(QStringLiteral("message")).toString();
    if (!message.isEmpty())
        return message;
    const QJsonObject data = error.value(QStringLiteral("data")).toObject();
    const QString nested = data.value(QStringLiteral("message")).toString();
    if (!nested.isEmpty())
        return nested;
    return QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact));
}

QString requestKey(const QJsonValue& id) {
    return id.isString() ? id.toString() : QString::number(id.toVariant().toLongLong());
}

QString pinnedSpawnModel(const QString& model) {
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral("auto") ||
        trimmed == QStringLiteral("default") || trimmed == QStringLiteral("auto-smart"))
        return {};
    return trimmed;
}

QJsonObject occupancyTokenUsage(const QJsonObject& source) {
    QJsonObject usage = source.value(QStringLiteral("tokenUsage")).toObject();
    if (usage.isEmpty())
        usage = source.value(QStringLiteral("usage")).toObject();
    const QJsonObject last = usage.value(QStringLiteral("last")).toObject();
    qint64 used = last.value(QStringLiteral("totalTokens"))
                      .toInteger(last.value(QStringLiteral("total_tokens")).toInteger(-1));
    qint64 limit = usage.value(QStringLiteral("modelContextWindow"))
                       .toInteger(usage.value(QStringLiteral("model_context_window")).toInteger(0));
    if (used < 0) {
        used = source.value(QStringLiteral("used"))
                   .toInteger(usage.value(QStringLiteral("used")).toInteger(-1));
    }
    if (limit <= 0) {
        limit = source.value(QStringLiteral("size"))
                    .toInteger(usage.value(QStringLiteral("size")).toInteger(0));
    }
    if (used < 0 || limit <= 0)
        return {};
    return {{QStringLiteral("last"), QJsonObject{{QStringLiteral("totalTokens"), used}}},
            {QStringLiteral("modelContextWindow"), limit}};
}

bool isCursorAutoModel(const QString& model) {
    const QString id = model.trimmed().toLower();
    return id.isEmpty() || id == QStringLiteral("auto") || id == QStringLiteral("default") ||
           id == QStringLiteral("auto-smart");
}

bool isPlanLimitPrefix(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (CursorAdapter::looksLikePlanLimit(trimmed))
        return true;
    return QStringLiteral("upgrade your plan to continue").startsWith(trimmed, Qt::CaseInsensitive);
}

struct AcpSelectValue final {
    QString raw;
    QString base;
    QHash<QString, QString> params;
    bool parameterized{false};
};

AcpSelectValue parseAcpSelectValue(const QString& raw) {
    AcpSelectValue parsed;
    parsed.raw = raw;
    const int bracket = raw.indexOf(QLatin1Char('['));
    if (bracket < 0) {
        parsed.base = raw;
        return parsed;
    }
    parsed.parameterized = true;
    parsed.base = raw.left(bracket);
    QString inner = raw.mid(bracket + 1);
    if (inner.endsWith(QLatin1Char(']')))
        inner.chop(1);
    const QStringList parts = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        parsed.params.insert(part.left(eq).trimmed(), part.mid(eq + 1).trimmed());
    }
    return parsed;
}

QString normalizedAcpBase(QString base) {
    if (base.startsWith(QStringLiteral("cursor-")))
        base = base.mid(7);
    return base;
}

QJsonArray modelChoices(const QJsonArray& configOptions) {
    for (const QJsonValue& entry : configOptions) {
        const QJsonObject option = entry.toObject();
        const QString id = option.value(QStringLiteral("id")).toString();
        const QString category = option.value(QStringLiteral("category")).toString();
        if (id != QStringLiteral("model") && category != QStringLiteral("model"))
            continue;
        const QJsonArray choices = option.value(QStringLiteral("options")).toArray();
        if (!choices.isEmpty())
            return choices;
    }
    return {};
}

bool configHasChoice(const QJsonArray& configOptions, const QString& configId,
                     const QString& value = {}) {
    for (const QJsonValue& entry : configOptions) {
        const QJsonObject option = entry.toObject();
        if (option.value(QStringLiteral("id")).toString() != configId)
            continue;
        if (value.isEmpty())
            return true;
        for (const QJsonValue& choice : option.value(QStringLiteral("options")).toArray()) {
            if (choice.toObject().value(QStringLiteral("value")).toString() == value)
                return true;
        }
        return false;
    }
    return false;
}

QJsonObject cursorModelEntry(const QString& id, const QString& label, bool isDefault,
                             const QJsonArray& efforts, const QString& defaultEffort) {
    const QString display = label.isEmpty() ? id : label;
    QJsonArray reasoning;
    for (const QJsonValue& value : efforts) {
        const QString effortId = value.toObject().value(QStringLiteral("id")).toString();
        if (!effortId.isEmpty())
            reasoning.append(QJsonObject{{QStringLiteral("reasoningEffort"), effortId}});
    }
    const bool slider = efforts.size() > 1;
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("label"), display},
        {QStringLiteral("efforts"), efforts},
        {QStringLiteral("default_effort"),
         defaultEffort.isEmpty() ? QJsonValue(QString()) : QJsonValue(defaultEffort)},
        {QStringLiteral("model"), id},
        {QStringLiteral("displayName"), display},
        {QStringLiteral("hidden"), false},
        {QStringLiteral("provider"), QStringLiteral("cursor")},
        {QStringLiteral("description"),
         QStringLiteral("Listed from the Cursor CLI. Sign in with `agent login` if "
                        "the list is incomplete.")},
        {QStringLiteral("defaultReasoningEffort"),
         slider ? QJsonValue(defaultEffort) : QJsonValue(QString())},
        {QStringLiteral("supportedReasoningEfforts"), slider ? reasoning : QJsonArray{}},
        {QStringLiteral("permissions"), CursorAdapter::permissionProfiles()},
        {QStringLiteral("defaultPermission"), QStringLiteral("workspace-write")},
        {QStringLiteral("isDefault"), isDefault}};
}

QJsonObject cursorModelEntry(const QString& id, const QString& label, bool isDefault = false) {
    return cursorModelEntry(id, label, isDefault, {}, {});
}

struct CursorVariant final {
    QString id;
    QString label;
    QString family;
    QString effort;
    bool fast{false};
};

const QStringList& cursorEffortSuffixes() {
    static const QStringList suffixes{QStringLiteral("extra-high"), QStringLiteral("minimal"),
                                      QStringLiteral("medium"),     QStringLiteral("xhigh"),
                                      QStringLiteral("none"),       QStringLiteral("high"),
                                      QStringLiteral("low"),        QStringLiteral("max")};
    return suffixes;
}

CursorVariant parseCursorVariant(const QString& id, const QString& label) {
    CursorVariant variant;
    variant.id = id;
    variant.label = label;
    QString rest = id;
    if (rest.endsWith(QStringLiteral("-fast"))) {
        variant.fast = true;
        rest.chop(5);
    }
    for (const QString& suffix : cursorEffortSuffixes()) {
        const QString token = QLatin1Char('-') + suffix;
        if (rest.size() > token.size() && rest.endsWith(token)) {
            rest.chop(token.size());
            variant.family = rest;
            variant.effort = suffix;
            return variant;
        }
    }
    variant.family = rest;
    variant.effort = variant.fast ? QStringLiteral("fast") : QStringLiteral("medium");
    return variant;
}

QList<QPair<QString, QString>> extraModelOptions(const QString& requested, const QString& chosen,
                                                 const QJsonArray& options) {
    if (isCursorAutoModel(requested))
        return {};
    const CursorVariant variant = parseCursorVariant(requested, {});
    const AcpSelectValue parsed = parseAcpSelectValue(chosen);
    QList<QPair<QString, QString>> rest;
    if (parsed.parameterized) {
        const QString advertisedFast = parsed.params.value(QStringLiteral("fast"));
        if (variant.fast && advertisedFast == QStringLiteral("false") &&
            configHasChoice(options, QStringLiteral("fast")))
            rest.append({QStringLiteral("fast"), QStringLiteral("true")});
        else if (!variant.fast && advertisedFast == QStringLiteral("true") &&
                 configHasChoice(options, QStringLiteral("fast")))
            rest.append({QStringLiteral("fast"), QStringLiteral("false")});
        if (!variant.effort.isEmpty() && variant.effort != QStringLiteral("fast") &&
            parsed.params.value(QStringLiteral("effort")) != variant.effort &&
            configHasChoice(options, QStringLiteral("effort"), variant.effort))
            rest.append({QStringLiteral("effort"), variant.effort});
        return rest;
    }
    if (configHasChoice(options, QStringLiteral("effort"), variant.effort) &&
        variant.effort != QStringLiteral("fast"))
        rest.append({QStringLiteral("effort"), variant.effort});
    if (configHasChoice(options, QStringLiteral("fast"))) {
        rest.append({QStringLiteral("fast"),
                     variant.fast ? QStringLiteral("true") : QStringLiteral("false")});
    }
    return rest;
}

QString cursorFamilyLabel(QString label) {
    label.replace(QRegularExpression(QStringLiteral(R"(\s*\(default\))"),
                                     QRegularExpression::CaseInsensitiveOption),
                  QString());
    const QRegularExpression trailing(
        QStringLiteral(R"((?:\s+(?:None|Minimal|Low|Medium|High|Extra High|Max|Fast))+$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < 4; ++i) {
        const QString next = QString(label).remove(trailing).trimmed();
        if (next == label)
            break;
        label = next;
    }
    return label.trimmed();
}

QString cursorEffortId(const CursorVariant& variant) {
    if (variant.fast && variant.effort != QStringLiteral("fast"))
        return variant.effort + QStringLiteral("-fast");
    return variant.effort;
}

int cursorEffortRank(const QString& effort) {
    QString base = effort;
    int fast = 0;
    if (effort == QStringLiteral("fast")) {
        base = QStringLiteral("medium");
        fast = 1;
    } else if (effort.endsWith(QStringLiteral("-fast"))) {
        base = effort.left(effort.size() - 5);
        fast = 1;
    }
    static const QStringList order{QStringLiteral("none"),       QStringLiteral("minimal"),
                                   QStringLiteral("low"),        QStringLiteral("medium"),
                                   QStringLiteral("high"),       QStringLiteral("xhigh"),
                                   QStringLiteral("extra-high"), QStringLiteral("max")};
    const int index = order.indexOf(base);
    return (index < 0 ? 80 : index) * 2 + fast;
}

QString defaultCursorEffort(const QJsonArray& efforts) {
    QStringList ids;
    for (const QJsonValue& value : efforts)
        ids.append(value.toObject().value(QStringLiteral("id")).toString());
    for (const QString& preferred :
         {QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("low")}) {
        if (ids.contains(preferred))
            return preferred;
    }
    return ids.isEmpty() ? QString() : ids.first();
}

CursorAdapter::ProcessInvocation wrapExecutable(const QString& executable,
                                                const QStringList& arguments) {
#if defined(Q_OS_WIN)
    const QString suffix = QFileInfo(executable).suffix().toLower();
    if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")) {
        QStringList wrapped{QStringLiteral("/c"), QDir::toNativeSeparators(executable)};
        wrapped.append(arguments);
        return {QStringLiteral("cmd.exe"), wrapped};
    }
    if (suffix == QStringLiteral("ps1")) {
        QStringList wrapped{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                            QStringLiteral("Bypass"), QStringLiteral("-File"),
                            QDir::toNativeSeparators(executable)};
        wrapped.append(arguments);
        return {QStringLiteral("powershell.exe"), wrapped};
    }
#endif
    return {executable, arguments};
}

void configureHiddenProcess(QProcess* process) {
#if defined(Q_OS_WIN)
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(process);
#endif
}

QString mimeForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
        return QStringLiteral("image/jpeg");
    return QStringLiteral("image/png");
}

bool looksRunnable(const QFileInfo& info) {
    if (!info.isFile())
        return false;
#if defined(Q_OS_WIN)
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("exe") || suffix == QStringLiteral("cmd") ||
        suffix == QStringLiteral("bat") || suffix == QStringLiteral("ps1") || suffix.isEmpty())
        return true;
#endif
    return info.isExecutable();
}

} // namespace

CursorAdapter::CursorAdapter(const QString& executable, QObject* parent)
    : ProviderAdapter(parent), executable_(executable.isEmpty() ? findExecutable() : executable) {
    process_ = new QProcess(this);
    versionProbe_ = new compat::ProviderVersionProbe(
        compat::policy(QStringLiteral("cursor")),
        [](const QString& executable) {
            const ProcessInvocation invocation = versionInvocation(executable);
            return compat::ProviderVersionProbe::Invocation{invocation.program,
                                                            invocation.arguments};
        },
        this);
    catalogTimer_ = new QTimer(this);
    restartTimer_ = new QTimer(this);
    restartTimer_->setSingleShot(true);
    catalogTimer_->setSingleShot(true);
    connect(restartTimer_, &QTimer::timeout, this, [this] {
        if (!queued_.isEmpty())
            ensureStarted();
    });
    connect(catalogTimer_, &QTimer::timeout, this, [this] {
        if (catalogProcess_ == nullptr)
            return;
        catalogProcess_->kill();
    });
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    configureHiddenProcess(process_);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::started, this, &CursorAdapter::initialize);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CursorAdapter::consumeStdout);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        stderrBuffer_.append(process_->readAllStandardError());
        if (stderrBuffer_.size() > maximumStderrBytes)
            stderrBuffer_.remove(0, stderrBuffer_.size() - maximumStderrBytes);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        starting_ = false;
        setReady(false);
        noteUnexpectedStop();
        const QString reason = QStringLiteral("Unable to start Cursor ACP: %1%2")
                                   .arg(process_->errorString(), stderrTail());
        failPending(reason);
        emit stopped(reason);
    });
    connect(
        process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](const int, const QProcess::ExitStatus) {
            consumeStdout();
            starting_ = false;
            setReady(false);
            if (stopping_) {
                stopping_ = false;
                failPending(QStringLiteral("The Cursor ACP process was stopped."));
                return;
            }
            noteUnexpectedStop();
            const QString reason =
                QStringLiteral("The Cursor ACP process exited unexpectedly.%1").arg(stderrTail());
            failPending(reason);
            emit stopped(reason);
        });
}

CursorAdapter::~CursorAdapter() {
    // Owners destroy adapters at shutdown, after the SessionHost whose callbacks are still
    // pending; calling them here would use the freed host. Drop them instead of failing them.
    pending_.clear();
    queued_.clear();
    stop();
}

bool CursorAdapter::isAvailable() const { return !executable_.isEmpty(); }

QString CursorAdapter::missingMessage() const { return missingExecutableMessage(); }

bool CursorAdapter::isReady() const { return ready_; }

QString CursorAdapter::executable() const { return executable_; }

QString CursorAdapter::installCommand() {
#if defined(Q_OS_WIN)
    return QStringLiteral("irm 'https://cursor.com/install?win32=true' | iex");
#else
    return QStringLiteral("curl https://cursor.com/install -fsS | bash");
#endif
}

QString CursorAdapter::loginCommand() { return QStringLiteral("agent login"); }

QString CursorAdapter::missingExecutableMessage() {
    return QStringLiteral("Cursor CLI was not found. Install it in a terminal, then send again:\n\n"
                          "%1")
        .arg(installCommand());
}

QString CursorAdapter::notAuthenticatedMessage() {
#if defined(Q_OS_WIN)
    return QStringLiteral(
               "Cursor CLI is not signed in. In PowerShell, run:\n\n%1\n\nIf that command is not "
               "found:\n\n& \"$env:LOCALAPPDATA\\cursor-agent\\agent.cmd\" login\n\nThen send "
               "again. KetPlus does not store Cursor credentials.")
        .arg(loginCommand());
#else
    return QStringLiteral("Cursor CLI is not signed in. In a terminal, run:\n\n%1\n\nThen send "
                          "again. KetPlus does not store Cursor credentials.")
        .arg(loginCommand());
#endif
}

QString CursorAdapter::planLimitMessage() {
    const QString email = cliAuthEmail();
    const QString account = email.isEmpty()
                                ? QStringLiteral("the Cursor Agent CLI account")
                                : QStringLiteral("the Cursor Agent CLI account (%1)").arg(email);
    return QStringLiteral(
               "Cursor blocked this model for %1. Cursor desktop and Agent CLI use separate "
               "logins — if the desktop app is signed into a different email that has this model, "
               "sign the CLI into that account in a terminal:\n\n%2\n\nThen send again, or choose "
               "Auto / another model available on this CLI plan. "
               "https://cursor.com/dashboard/usage")
        .arg(account, loginCommand());
}

bool CursorAdapter::looksLikePlanLimit(const QString& message) {
    const QString lower = message.trimmed().toLower();
    return lower.contains(QStringLiteral("upgrade your plan")) ||
           lower.contains(QStringLiteral("named models unavailable")) ||
           lower.contains(QStringLiteral("free plans can only use auto")) ||
           lower.contains(QStringLiteral("switch to auto or upgrade"));
}

QString CursorAdapter::cliAuthEmail() {
    const QString path = QDir::home().filePath(QStringLiteral(".cursor/cli-config.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(QStringLiteral("authInfo"))
        .toObject()
        .value(QStringLiteral("email"))
        .toString()
        .trimmed();
}

QStringList CursorAdapter::wellKnownExecutableCandidates() {
    QStringList candidates;
#if defined(Q_OS_WIN)
    const QString localAppData = qEnvironmentVariable(
        "LOCALAPPDATA", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const QDir cursorAgent(QDir(localAppData).filePath(QStringLiteral("cursor-agent")));
    for (const QString& name : {QStringLiteral("agent.cmd"), QStringLiteral("cursor-agent.cmd"),
                                QStringLiteral("agent.exe"), QStringLiteral("cursor-agent.exe")})
        candidates.append(cursorAgent.filePath(name));
#else
    candidates.append(QDir::home().filePath(QStringLiteral(".local/bin/agent")));
    candidates.append(QDir::home().filePath(QStringLiteral(".local/bin/cursor-agent")));
    candidates.append(QStringLiteral("/opt/homebrew/bin/agent"));
    candidates.append(QStringLiteral("/usr/local/bin/agent"));
#endif
    return candidates;
}

CursorAdapter::ProcessInvocation CursorAdapter::processInvocation(const QString& executable,
                                                                  const QString& model) {
    QStringList arguments;
    const QString pinned = pinnedSpawnModel(model);
    if (!pinned.isEmpty())
        arguments << QStringLiteral("--model") << pinned;
    arguments << QStringLiteral("acp");
    return wrapExecutable(executable, arguments);
}

CursorAdapter::ProcessInvocation CursorAdapter::listModelsInvocation(const QString& executable) {
    return wrapExecutable(executable, {QStringLiteral("models")});
}

CursorAdapter::ProcessInvocation CursorAdapter::versionInvocation(const QString& executable) {
    return wrapExecutable(executable, {QStringLiteral("--version")});
}

QJsonArray CursorAdapter::modelCatalog() {
    return {cursorModelEntry(QStringLiteral("auto"), QStringLiteral("Cursor Auto"), true)};
}

QJsonArray CursorAdapter::parseModelList(const QString& output) {
    struct FamilyBuild final {
        QString display;
        bool isDefault{false};
        QHash<QString, QString> variantByEffort;
    };
    QStringList familyOrder;
    QHash<QString, FamilyBuild> families;
    QSet<QString> seenIds;
    QSet<QString> familiesWithBase;
    QList<CursorVariant> variants;

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QStringLiteral("Available models")) ||
            line.startsWith(QStringLiteral("Tip:"), Qt::CaseInsensitive))
            continue;
        const int separator = line.indexOf(QStringLiteral(" - "));
        if (separator <= 0)
            continue;
        const QString id = line.left(separator).trimmed();
        const QString label = line.mid(separator + 3).trimmed();
        if (id.isEmpty() || id.contains(QLatin1Char(' ')) || seenIds.contains(id))
            continue;
        seenIds.insert(id);
        const CursorVariant variant = parseCursorVariant(id, label);
        variants.append(variant);
        if (!variant.fast)
            familiesWithBase.insert(variant.family);
    }

    for (const CursorVariant& variant : variants) {
        if (variant.fast && familiesWithBase.contains(variant.family))
            continue;
        if (!families.contains(variant.family))
            familyOrder.append(variant.family);
        FamilyBuild& family = families[variant.family];
        const QString display = cursorFamilyLabel(variant.label);
        if (family.display.isEmpty() || display.size() < family.display.size())
            family.display = display;
        family.isDefault = family.isDefault || variant.id == QStringLiteral("auto") ||
                           variant.label.contains(QStringLiteral("(default)"), Qt::CaseInsensitive);
        family.variantByEffort.insert(cursorEffortId(variant), variant.id);
    }

    QJsonArray models;
    for (const QString& familyId : familyOrder) {
        const FamilyBuild family = families.value(familyId);
        QStringList effortIds = family.variantByEffort.keys();
        std::sort(effortIds.begin(), effortIds.end(),
                  [](const QString& left, const QString& right) {
                      return cursorEffortRank(left) < cursorEffortRank(right);
                  });
        QJsonArray efforts;
        for (const QString& effortId : effortIds) {
            efforts.append(
                QJsonObject{{QStringLiteral("id"), effortId},
                            {QStringLiteral("label"), effortId},
                            {QStringLiteral("model"), family.variantByEffort.value(effortId)}});
        }
        models.append(cursorModelEntry(familyId,
                                       family.display.isEmpty() ? familyId : family.display,
                                       family.isDefault, efforts, defaultCursorEffort(efforts)));
    }
    return models;
}

QString CursorAdapter::cliModelId(const QJsonArray& catalog, const QString& model,
                                  const QString& effort) {
    const QString trimmedModel = model.trimmed();
    const QString trimmedEffort = effort.trimmed();
    if (trimmedModel.isEmpty() || trimmedModel == QStringLiteral("auto"))
        return trimmedModel;
    for (const QJsonValue& value : catalog) {
        const QJsonObject entry = value.toObject();
        const QJsonArray efforts = entry.value(QStringLiteral("efforts")).toArray();
        bool matchedFamily = entry.value(QStringLiteral("id")).toString() == trimmedModel ||
                             entry.value(QStringLiteral("model")).toString() == trimmedModel;
        QString fallback = entry.value(QStringLiteral("model")).toString(trimmedModel);
        QString exactVariant;
        QString defaultVariant;
        const QString defaultId = entry.value(QStringLiteral("default_effort")).toString();
        for (const QJsonValue& effortValue : efforts) {
            const QJsonObject item = effortValue.toObject();
            const QString variant = item.value(QStringLiteral("model")).toString();
            const QString effortId = item.value(QStringLiteral("id")).toString();
            if (variant == trimmedModel) {
                matchedFamily = true;
                exactVariant = variant;
            }
            if (fallback.isEmpty() || fallback == trimmedModel)
                fallback = variant.isEmpty() ? fallback : variant;
            if (effortId == defaultId && !variant.isEmpty())
                defaultVariant = variant;
        }
        if (!matchedFamily)
            continue;
        if (!trimmedEffort.isEmpty()) {
            for (const QJsonValue& effortValue : efforts) {
                const QJsonObject item = effortValue.toObject();
                if (item.value(QStringLiteral("id")).toString() == trimmedEffort) {
                    const QString variant = item.value(QStringLiteral("model")).toString();
                    if (!variant.isEmpty())
                        return variant;
                }
            }
        }
        if (!exactVariant.isEmpty())
            return exactVariant;
        if (!defaultVariant.isEmpty())
            return defaultVariant;
        return fallback;
    }
    if (!trimmedEffort.isEmpty() && trimmedEffort != QStringLiteral("medium") &&
        !trimmedModel.endsWith(QLatin1Char('-') + trimmedEffort))
        return trimmedModel + QLatin1Char('-') + trimmedEffort;
    return trimmedModel;
}

QString CursorAdapter::acpModelId(const QString& model) {
    if (isCursorAutoModel(model))
        return QStringLiteral("default");
    const CursorVariant variant = parseCursorVariant(model.trimmed(), {});
    QString id = variant.family;
    if (id.startsWith(QStringLiteral("cursor-")))
        id = id.mid(7);
    return id.isEmpty() ? model.trimmed() : id;
}

QString CursorAdapter::acpModelConfigValue(const QString& model, const QJsonArray& configOptions) {
    if (isCursorAutoModel(model))
        return QStringLiteral("default");
    const QString want = acpModelId(model);
    const CursorVariant variant = parseCursorVariant(model.trimmed(), {});
    const QJsonArray choices = modelChoices(configOptions);
    if (choices.isEmpty())
        return want;
    int bestScore = -1;
    QString best;
    for (const QJsonValue& choice : choices) {
        const QString raw = choice.toObject().value(QStringLiteral("value")).toString();
        if (raw.isEmpty())
            continue;
        const AcpSelectValue parsed = parseAcpSelectValue(raw);
        if (normalizedAcpBase(parsed.base) != want)
            continue;
        int score = 1;
        const QString advertisedFast = parsed.params.value(QStringLiteral("fast"));
        const QString advertisedEffort = parsed.params.value(QStringLiteral("effort"));
        if (variant.fast) {
            if (advertisedFast == QStringLiteral("true"))
                score += 40;
            else if (advertisedFast == QStringLiteral("false"))
                score -= 20;
        } else if (advertisedFast == QStringLiteral("false")) {
            score += 40;
        } else if (advertisedFast == QStringLiteral("true")) {
            score -= 20;
        } else {
            score += 8;
        }
        if (!variant.effort.isEmpty() && variant.effort != QStringLiteral("fast")) {
            if (advertisedEffort == variant.effort)
                score += 30;
            else if (!advertisedEffort.isEmpty() && variant.effort != QStringLiteral("medium"))
                score -= 5;
        }
        if (parsed.parameterized)
            score += 3;
        if (score > bestScore) {
            bestScore = score;
            best = raw;
        }
    }
    return best.isEmpty() ? want : best;
}

QJsonArray CursorAdapter::permissionProfiles() {
    return {QJsonObject{{QStringLiteral("id"), QStringLiteral("read-only")},
                        {QStringLiteral("label"), QStringLiteral("Read only")},
                        {QStringLiteral("description"),
                         QStringLiteral("Ask mode: inspect files without edits")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-write")},
                        {QStringLiteral("label"), QStringLiteral("Workspace")},
                        {QStringLiteral("description"),
                         QStringLiteral("Agent mode; Cursor asks before tools")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("danger-full-access")},
                {QStringLiteral("label"), QStringLiteral("Full access")},
                {QStringLiteral("description"), QStringLiteral("Skip Cursor permission prompts")},
                {QStringLiteral("elevated"), true}}};
}

QString CursorAdapter::permissionMode(const QString& profile) {
    if (profile == QStringLiteral("read-only"))
        return QStringLiteral("ask");
    return QStringLiteral("agent");
}

void CursorAdapter::runtimeStatus(std::function<void(const compat::Runtime&)> callback) {
    versionProbe_->check(executable_,
                         [this, callback = std::move(callback)](const compat::Runtime& runtime) {
                             callback(narrowing_.apply(runtime));
                         });
}

bool CursorAdapter::supports(const QString& capability) const {
    if (narrowing_.removed(capability))
        return false;
    const auto runtime = versionProbe_->cached(executable_);
    return runtime ? runtime->supports(capability)
                   : versionProbe_->policy().baselineSupports(capability);
}

void CursorAdapter::call(const QString& method, const QJsonObject& params, Callback callback) {
    if (!isAvailable()) {
        callback({}, missingExecutableMessage());
        return;
    }
    if (method == QStringLiteral("model/list")) {
        listCliModels(std::move(callback));
        return;
    }
    if (method == QStringLiteral("turn/start")) {
        const auto launch = [this](QJsonObject params, Callback callback) {
            const QString cliModel =
                cliModelId(cachedCatalog_, params.value(QStringLiteral("model")).toString(),
                           params.value(QStringLiteral("effort")).toString());
            params.insert(QStringLiteral("model"), cliModel);
            if (!ready_ && process_->state() == QProcess::NotRunning &&
                pendingSpawnModel_.isEmpty())
                pendingSpawnModel_ = pinnedSpawnModel(cliModel);
            if (!ready_) {
                queued_.append({QStringLiteral("turn/start"), params, std::move(callback)});
                ensureStarted();
                return;
            }
            startTurn(params, std::move(callback));
        };
        if (cachedCatalog_.isEmpty()) {
            listCliModels([launch, params, callback = std::move(callback)](const QJsonObject&,
                                                                           const QString&) mutable {
                launch(params, std::move(callback));
            });
            return;
        }
        launch(params, std::move(callback));
        return;
    }
    if (method == QStringLiteral("turn/interrupt")) {
        if (!ready_) {
            queued_.append({method, params, std::move(callback)});
            ensureStarted();
            return;
        }
        interruptTurn(params, std::move(callback));
        return;
    }
    if (!ready_) {
        queued_.append({method, params, std::move(callback)});
        ensureStarted();
        return;
    }
    sendCall(method, params, std::move(callback));
}

void CursorAdapter::configurationCatalog(Callback callback) {
    listCliModels([callback = std::move(callback)](const QJsonObject& result,
                                                   const QString& providerError) {
        if (!providerError.isEmpty()) {
            callback({}, providerError);
            return;
        }
        QJsonArray catalog;
        for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
            const QJsonObject source = value.toObject();
            catalog.append(
                QJsonObject{{QStringLiteral("id"), source.value(QStringLiteral("id"))},
                            {QStringLiteral("label"), source.value(QStringLiteral("label"))},
                            {QStringLiteral("efforts"), source.value(QStringLiteral("efforts"))},
                            {QStringLiteral("default_effort"),
                             source.value(QStringLiteral("default_effort"))}});
        }
        callback(QJsonObject{{QStringLiteral("catalog"), catalog}}, {});
    });
}

void CursorAdapter::respond(const QJsonValue& requestId, const QJsonObject& result) {
    if (planRequests_.remove(requestKey(requestId))) {
        const bool allow = result.value("allow").toBool() || result.value("decision") == "accept";
        sendObject(
            {{"jsonrpc", "2.0"},
             {"id", requestId},
             {"result", QJsonObject{{"outcome",
                                     QJsonObject{{"outcome", allow ? "accepted" : "rejected"}}}}}});
        return;
    }
    const auto asked = questionOptionsByRequest_.take(requestKey(requestId));
    if (!asked.isEmpty() && result.value("answers").toObject().isEmpty()) {
        sendObject({{"jsonrpc", "2.0"},
                    {"id", requestId},
                    {"result", QJsonObject{{"outcome", QJsonObject{{"outcome", "cancelled"}}}}}});
        return;
    }
    if (!asked.isEmpty()) {
        QJsonArray answers;
        const QJsonObject submitted = result.value(QStringLiteral("answers")).toObject();
        for (const QJsonValue& questionValue : asked) {
            const QJsonObject question = questionValue.toObject();
            const QString questionId = question.value(QStringLiteral("id")).toString();
            const QJsonArray values =
                submitted.value(questionId).toObject().value(QStringLiteral("answers")).toArray();
            QJsonArray selected;
            for (const QJsonValue& value : values) {
                const QString label = value.toString();
                for (const QJsonValue& optionValue :
                     question.value(QStringLiteral("options")).toArray()) {
                    const QJsonObject option = optionValue.toObject();
                    if (option.value(QStringLiteral("label")).toString() == label) {
                        selected.append(option.value(QStringLiteral("id")));
                        break;
                    }
                }
            }
            answers.append(QJsonObject{{QStringLiteral("questionId"), questionId},
                                       {QStringLiteral("selectedOptionIds"), selected}});
        }
        sendObject(
            {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
             {QStringLiteral("id"), requestId},
             {QStringLiteral("result"),
              QJsonObject{{QStringLiteral("outcome"),
                           QJsonObject{{QStringLiteral("outcome"), QStringLiteral("answered")},
                                       {QStringLiteral("answers"), answers}}}}}});
        return;
    }
    if (result.contains(QStringLiteral("outcome"))) {
        sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), requestId},
                    {QStringLiteral("result"), result}});
        return;
    }
    const auto permissionOptions = permissionOptionsByRequest_.take(requestKey(requestId));
    const bool allow =
        result.value(QStringLiteral("allow")).toBool() ||
        result.value(QStringLiteral("decision")).toString() == QStringLiteral("accept");
    QString optionId;
    for (const auto& value : permissionOptions) {
        const auto option = value.toObject();
        if (option.value("kind") == (allow ? "allow_once" : "reject_once") ||
            (option.value("kind").toString().isEmpty() &&
             option.value("optionId") == (allow ? "allow-once" : "reject-once"))) {
            optionId = option.value("optionId").toString();
            break;
        }
    }
    // Older agents omit options; retain their documented ids for compatibility.
    if (permissionOptions.isEmpty())
        optionId = allow ? "allow-once" : "reject-once";
    QJsonObject outcome{{"outcome", "cancelled"}};
    if (!optionId.isEmpty())
        outcome = {{"outcome", "selected"}, {"optionId", optionId}};
    sendObject(
        {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", QJsonObject{{"outcome", outcome}}}});
}

void CursorAdapter::respondError(const QJsonValue& requestId, const QString& code,
                                 const QString& message) {
    questionOptionsByRequest_.remove(requestKey(requestId));
    permissionOptionsByRequest_.remove(requestKey(requestId));
    planRequests_.remove(requestKey(requestId));
    sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), requestId},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                      {QStringLiteral("message"), message}}}});
}

void CursorAdapter::stop() {
    stopping_ = true;
    restartTimer_->stop();
    catalogTimer_->stop();
    failPending(QStringLiteral("The Cursor ACP process was stopped."));
    finishCatalog(modelCatalog());
    if (catalogProcess_ != nullptr && catalogProcess_->state() != QProcess::NotRunning) {
        catalogProcess_->kill();
        catalogProcess_->waitForFinished(500);
    }
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(500)) {
            process_->kill();
            process_->waitForFinished(500);
        }
    }
    starting_ = false;
    setReady(false);
}

void CursorAdapter::ensureStarted() {
    if (starting_ || process_->state() != QProcess::NotRunning)
        return;
    if (!restartNotBefore_.hasExpired()) {
        if (!restartTimer_->isActive())
            restartTimer_->start(std::max<qint64>(1, restartNotBefore_.remainingTime()));
        return;
    }
    // Every launch re-checks the CLI version (cached until the executable changes on disk);
    // `agent` updates itself, so the version can change between launches (ADR 0015).
    if (!versionCleared_) {
        if (checkingVersion_)
            return;
        checkingVersion_ = true;
        versionProbe_->check(executable_, [this](const compat::Runtime& runtime) {
            checkingVersion_ = false;
            if (runtime.blocksLaunch()) {
                failPending(runtime.message);
                return;
            }
            // stop() during the check already failed the queued calls; nothing to launch for.
            if (queued_.isEmpty())
                return;
            versionCleared_ = true;
            ensureStarted();
        });
        return;
    }
    versionCleared_ = false;
    stopping_ = false;
    starting_ = true;
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    configureProcess();
    process_->start();
}

void CursorAdapter::configureProcess() {
    const ProcessInvocation invocation = processInvocation(executable_, pendingSpawnModel_);
    spawnModel_ = pendingSpawnModel_;
    pendingSpawnModel_.clear();
    process_->setProgram(invocation.program);
    process_->setArguments(invocation.arguments);
}

void CursorAdapter::initialize() {
    narrowing_.reset();
    sendCall(
        QStringLiteral("initialize"),
        {{QStringLiteral("protocolVersion"), 1},
         {QStringLiteral("clientCapabilities"),
          QJsonObject{{QStringLiteral("fs"), QJsonObject{{QStringLiteral("readTextFile"), false},
                                                         {QStringLiteral("writeTextFile"), false}}},
                      {QStringLiteral("terminal"), false},
                      {QStringLiteral("session"),
                       QJsonObject{{QStringLiteral("configOptions"),
                                    QJsonObject{{QStringLiteral("boolean"), QJsonObject{}}}}}},
                      {QStringLiteral("_meta"),
                       QJsonObject{{QStringLiteral("parameterizedModelPicker"), true}}}}},
         {QStringLiteral("clientInfo"),
          QJsonObject{{QStringLiteral("name"), QStringLiteral("ketplus-daemon")},
                      {QStringLiteral("title"), QStringLiteral("KetPlus Daemon")},
                      {QStringLiteral("version"), QStringLiteral("0.1.0")}}}},
        [this](const QJsonObject& result, const QString& error) {
            starting_ = false;
            narrowing_.acpInitialized(result);
            if (!error.isEmpty()) {
                const QString reason = QStringLiteral("Cursor ACP initialization failed: %1%2")
                                           .arg(error, stderrTail());
                noteUnexpectedStop();
                failPending(reason);
                emit stopped(reason);
                stopping_ = true;
                process_->terminate();
                return;
            }
            setReady(true);
            const QList<QueuedCall> calls = std::exchange(queued_, {});
            for (const QueuedCall& call : calls)
                call.method == QStringLiteral("turn/start") ? startTurn(call.params, call.callback)
                : call.method == QStringLiteral("turn/interrupt")
                    ? interruptTurn(call.params, call.callback)
                    : sendCall(call.method, call.params, call.callback);
        });
}

void CursorAdapter::startTurn(const QJsonObject& params, Callback callback) {
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();
    const QString cwd = params.value(QStringLiteral("cwd")).toString();
    if (sessionId.isEmpty() || cwd.isEmpty()) {
        callback({}, QStringLiteral("Cursor turn is missing a session or worktree."));
        return;
    }
    // One turn at a time per session: a second start would drop the running turn's state, and its
    // still-pending prompt reply would then complete the new turn.
    if (turns_.contains(sessionId) || startingSessions_.contains(sessionId)) {
        callback({}, QStringLiteral("A Cursor turn is already running for this session."));
        return;
    }
    startingSessions_.insert(sessionId);
    permissionBySession_.insert(sessionId, params.value(QStringLiteral("permission")).toString());
    if (const QJsonObject server = params.value(QStringLiteral("agentMcpServer")).toObject();
        server.isEmpty()) {
        agentServers_.remove(sessionId);
        agentInstructions_.remove(sessionId);
    } else {
        agentServers_.insert(sessionId, server);
        agentInstructions_.insert(sessionId,
                                  params.value(QStringLiteral("agentInstructions")).toString());
    }
    ensureAcpSession(
        sessionId, cwd, params.value(QStringLiteral("resume")).toString(),
        params.value(QStringLiteral("model")).toString(),
        [this, params, callback = std::move(callback), sessionId](const QJsonObject& result,
                                                                  const QString& error) {
            startingSessions_.remove(sessionId);
            if (!error.isEmpty()) {
                callback({}, looksLikeAuthError(error) ? notAuthenticatedMessage() : error);
                return;
            }
            TurnState turn;
            turn.ketplusSessionId = sessionId;
            turn.acpSessionId = result.value(QStringLiteral("sessionId")).toString();
            turn.turnId = QStringLiteral("cursor-turn-%1").arg(nextTurnNumber_++);
            turn.itemId = QStringLiteral("cursor-answer-%1").arg(turn.turnId);
            turn.permission = permissionBySession_.value(sessionId);
            turns_.insert(sessionId, turn);
            callback(QJsonObject{{QStringLiteral("turn"),
                                  QJsonObject{{QStringLiteral("id"), turn.turnId}}},
                                 {QStringLiteral("thread"),
                                  QJsonObject{{QStringLiteral("id"), turn.acpSessionId}}}},
                     {});
            sendPrompt(turns_[sessionId], params);
        });
}

void CursorAdapter::applySessionModel(const QString& acpSessionId, const QString& model,
                                      const QJsonArray& advertisedOptions,
                                      std::function<void()> done) {
    const QString requested = model;
    const QString chosen = acpModelConfigValue(requested, advertisedOptions);
    sendCall(QStringLiteral("session/set_config_option"),
             {{QStringLiteral("sessionId"), acpSessionId},
              {QStringLiteral("configId"), QStringLiteral("model")},
              {QStringLiteral("value"), chosen}},
             [this, acpSessionId, requested, chosen,
              done = std::move(done)](const QJsonObject& result, const QString&) {
                 const QJsonArray options = result.value(QStringLiteral("configOptions")).toArray();
                 const QString refined = acpModelConfigValue(requested, options);
                 const auto extras = [this, acpSessionId, requested,
                                      done](const QJsonArray& options, const QString& value) {
                     applyConfigOptions(acpSessionId, extraModelOptions(requested, value, options),
                                        done);
                 };
                 if (!refined.isEmpty() && refined != chosen) {
                     sendCall(QStringLiteral("session/set_config_option"),
                              {{QStringLiteral("sessionId"), acpSessionId},
                               {QStringLiteral("configId"), QStringLiteral("model")},
                               {QStringLiteral("value"), refined}},
                              [extras, refined](const QJsonObject& next, const QString&) {
                                  extras(next.value(QStringLiteral("configOptions")).toArray(),
                                         refined);
                              });
                     return;
                 }
                 extras(options, chosen);
             });
}

void CursorAdapter::applyConfigOptions(const QString& acpSessionId,
                                       QList<QPair<QString, QString>> options,
                                       std::function<void()> done) {
    if (options.isEmpty()) {
        done();
        return;
    }
    const QPair<QString, QString> next = options.takeFirst();
    sendCall(
        QStringLiteral("session/set_config_option"),
        {{QStringLiteral("sessionId"), acpSessionId},
         {QStringLiteral("configId"), next.first},
         {QStringLiteral("value"), next.second}},
        [this, acpSessionId, options, done = std::move(done)](const QJsonObject&, const QString&) {
            applyConfigOptions(acpSessionId, options, done);
        });
}

void CursorAdapter::listCliModels(Callback callback) {
    if (!cachedCatalog_.isEmpty() && catalogFetchedAt_.isValid() &&
        catalogFetchedAt_.elapsed() < 60 * 1000) {
        callback(QJsonObject{{QStringLiteral("data"), cachedCatalog_}}, {});
        return;
    }
    catalogWaiters_.append(std::move(callback));
    if (catalogProcess_ != nullptr)
        return;
    startCatalogProcess();
}

void CursorAdapter::startCatalogProcess() {
    if (!isAvailable()) {
        finishCatalog(modelCatalog());
        return;
    }
    catalogProcess_ = new QProcess(this);
    catalogProcess_->setProcessChannelMode(QProcess::SeparateChannels);
    configureHiddenProcess(catalogProcess_);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    catalogProcess_->setProcessEnvironment(environment);
    const ProcessInvocation invocation = listModelsInvocation(executable_);
    catalogProcess_->setProgram(invocation.program);
    catalogProcess_->setArguments(invocation.arguments);
    connect(catalogProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int, const QProcess::ExitStatus) {
                catalogTimer_->stop();
                QJsonArray models;
                if (catalogProcess_ != nullptr)
                    models =
                        parseModelList(QString::fromUtf8(catalogProcess_->readAllStandardOutput()));
                if (catalogProcess_ != nullptr)
                    catalogProcess_->deleteLater();
                catalogProcess_ = nullptr;
                finishCatalog(models.isEmpty() ? modelCatalog() : models);
            });
    connect(catalogProcess_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError) {
        if (catalogProcess_ == nullptr || catalogProcess_->state() != QProcess::NotRunning)
            return;
        catalogTimer_->stop();
        catalogProcess_->deleteLater();
        catalogProcess_ = nullptr;
        finishCatalog(modelCatalog());
    });
    catalogTimer_->start(15 * 1000);
    catalogProcess_->start();
}

void CursorAdapter::finishCatalog(const QJsonArray& models) {
    if (catalogWaiters_.isEmpty())
        return;
    cachedCatalog_ = models;
    catalogFetchedAt_.restart();
    const auto waiters = std::exchange(catalogWaiters_, {});
    const QJsonObject result{{QStringLiteral("data"), models}};
    for (const Callback& callback : waiters)
        callback(result, {});
}

void CursorAdapter::sendPrompt(TurnState& turn, const QJsonObject& params) {
    // ACP has no system-prompt field. Supply host context as a separate block;
    // the journal and user prompt remain unchanged, including image attachments.
    auto prompt = promptContent(params);
    QString hostText =
        browser_tools::hostAgentInstructions(browserSessions_.contains(turn.ketplusSessionId));
    if (agentSessions_.contains(turn.ketplusSessionId) &&
        !agentInstructions_.value(turn.ketplusSessionId).isEmpty())
        hostText += QStringLiteral("\n\n") + agentInstructions_.value(turn.ketplusSessionId);
    hostText = withResponseGuidance(hostText);
    prompt.prepend(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), hostText}});
    emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("turn.started"),
                        {{QStringLiteral("turn_id"), turn.turnId},
                         {QStringLiteral("thread_id"), turn.acpSessionId}});
    const quint64 id = nextRequestId_++;
    pending_.insert(id, [this, sessionId = turn.ketplusSessionId](const QJsonObject& result,
                                                                  const QString& error) {
        auto it = turns_.find(sessionId);
        if (it == turns_.end())
            return;
        if (!error.isEmpty()) {
            finishTurn(sessionId, QStringLiteral("failed"),
                       looksLikeAuthError(error)   ? notAuthenticatedMessage()
                       : looksLikePlanLimit(error) ? planLimitMessage()
                                                   : error);
            return;
        }
        if (looksLikePlanLimit(it->assistantText)) {
            finishTurn(sessionId, QStringLiteral("failed"), planLimitMessage());
            return;
        }
        emitContextUsage(sessionId, result);
        const QString stop = result.value(QStringLiteral("stopReason")).toString();
        finishTurn(sessionId,
                   stop == QStringLiteral("cancelled") || stop == QStringLiteral("interrupted")
                       ? QStringLiteral("interrupted")
                       : QStringLiteral("completed"));
    });
    sendObject(
        {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
         {QStringLiteral("id"), static_cast<qint64>(id)},
         {QStringLiteral("method"), QStringLiteral("session/prompt")},
         {QStringLiteral("params"), QJsonObject{{QStringLiteral("sessionId"), turn.acpSessionId},
                                                {QStringLiteral("prompt"), prompt}}}});
}

QJsonArray CursorAdapter::promptContent(const QJsonObject& params) const {
    QJsonArray prompt;
    const QString text = params.value(QStringLiteral("prompt")).toString();
    if (!text.isEmpty())
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), text}});
    for (const QJsonValue& value : params.value(QStringLiteral("images")).toArray()) {
        const QString path = value.toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = file.read(maximumImageBytes + 1);
        if (bytes.isEmpty() || bytes.size() > maximumImageBytes)
            continue;
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                  {QStringLiteral("mimeType"), mimeForPath(path)},
                                  {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}});
    }
    if (prompt.isEmpty())
        prompt.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), QStringLiteral(" ")}});
    return prompt;
}

void CursorAdapter::interruptTurn(const QJsonObject& params, Callback callback) {
    const QString sessionId = params.value(QStringLiteral("sessionId")).toString();
    const QString acpId = acpSessionByKetplus_.value(sessionId);
    if (acpId.isEmpty()) {
        callback({}, QStringLiteral("The Cursor session has no active ACP session to interrupt."));
        return;
    }
    auto turn = turns_.find(sessionId);
    if (turn != turns_.end())
        turn->interrupted = true;
    sendObject({{"jsonrpc", "2.0"},
                {"method", "session/cancel"},
                {"params", QJsonObject{{"sessionId", acpId}}}});
    callback({}, {});
}

void CursorAdapter::finishTurn(const QString& ketplusSessionId, const QString& status,
                               const QString& errorMessage) {
    const auto it = turns_.find(ketplusSessionId);
    if (it == turns_.end())
        return;
    const TurnState turn = it.value();
    turns_.erase(it);
    if (const QString signal = browserSessions_.value(ketplusSessionId);
        !signal.isEmpty() && !browser_tools::signalTurnEnd(signal))
        qWarning("Unable to signal the browser server that a turn of %s ended",
                 qUtf8Printable(ketplusSessionId));
    if (!turn.assistantText.isEmpty() && !looksLikePlanLimit(turn.assistantText)) {
        emit canonicalEvent(
            ketplusSessionId, QStringLiteral("item.completed"),
            {{QStringLiteral("item_id"), turn.itemId},
             {QStringLiteral("item_type"), QStringLiteral("agentMessage")},
             {QStringLiteral("text"), turn.assistantText},
             {QStringLiteral("item"),
              QJsonObject{{QStringLiteral("id"), turn.itemId},
                          {QStringLiteral("type"), QStringLiteral("agentMessage")},
                          {QStringLiteral("phase"), QStringLiteral("final_answer")},
                          {QStringLiteral("text"), turn.assistantText},
                          {QStringLiteral("status"), QStringLiteral("completed")}}}});
    }
    if (!turn.interrupted && !errorMessage.isEmpty()) {
        emit canonicalEvent(ketplusSessionId, QStringLiteral("runtime.error"),
                            {{QStringLiteral("message"), errorMessage}});
    }
    emit canonicalEvent(
        ketplusSessionId, QStringLiteral("turn.completed"),
        {{QStringLiteral("status"), turn.interrupted ? QStringLiteral("interrupted") : status},
         {QStringLiteral("thread_id"), turn.acpSessionId}});
}

void CursorAdapter::emitContextUsage(const QString& sessionId, const QJsonObject& source) {
    const QJsonObject usage = occupancyTokenUsage(source);
    if (sessionId.isEmpty() || usage.isEmpty())
        return;
    emit canonicalEvent(sessionId, QStringLiteral("usage.context"),
                        {{QStringLiteral("tokenUsage"), usage}});
}

void CursorAdapter::handleSessionUpdate(const QJsonObject& params) {
    QJsonObject update = params.value(QStringLiteral("update")).toObject();
    if (update.isEmpty() && params.contains(QStringLiteral("sessionUpdate")))
        update = params;
    const QString acpId = params.value(QStringLiteral("sessionId")).toString();
    const QString sessionId = ketplusByAcp_.value(acpId);
    if (sessionId.isEmpty())
        return;
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();
    if (kind == QStringLiteral("usage_update")) {
        emitContextUsage(sessionId, update);
        return;
    }
    auto it = turns_.find(sessionId);
    if (it == turns_.end())
        return;
    if (kind == QStringLiteral("agent_message_chunk")) {
        const QString delta = update.value(QStringLiteral("content"))
                                  .toObject()
                                  .value(QStringLiteral("text"))
                                  .toString();
        if (delta.isEmpty())
            return;
        const QString combined = it->assistantText + delta;
        if (isPlanLimitPrefix(combined)) {
            it->assistantText = combined;
            it->heldAssistant = true;
            return;
        }
        QString outgoing = delta;
        if (it->heldAssistant) {
            outgoing = combined;
            it->assistantText.clear();
            it->heldAssistant = false;
        }
        if (outgoing.isEmpty())
            return;
        if (it->assistantText.isEmpty()) {
            emit canonicalEvent(
                sessionId, QStringLiteral("item.started"),
                {{QStringLiteral("item_id"), it->itemId},
                 {QStringLiteral("item_type"), QStringLiteral("agentMessage")},
                 {QStringLiteral("item"),
                  QJsonObject{{QStringLiteral("id"), it->itemId},
                              {QStringLiteral("type"), QStringLiteral("agentMessage")},
                              {QStringLiteral("phase"), QStringLiteral("final_answer")},
                              {QStringLiteral("text"), QString()},
                              {QStringLiteral("status"), QStringLiteral("inProgress")}}}});
        }
        it->assistantText += outgoing;
        emit canonicalEvent(
            sessionId, QStringLiteral("assistant.delta"),
            {{QStringLiteral("item_id"), it->itemId}, {QStringLiteral("delta"), outgoing}});
        return;
    }
    if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
        const QString toolId = update.value("toolCallId").toString();
        if (toolId.isEmpty())
            return;
        auto& raw = it->toolItems[toolId];
        raw = cursorToolUpdate(raw, update);
        const auto item = cursorToolItem(raw);
        const auto status = item.value("status").toString();
        emit canonicalEvent(
            sessionId, status == "inProgress" ? "item.started" : "item.completed",
            {{"item_id", toolId}, {"item_type", item.value("type")}, {"item", item}});
        return;
    }
    if (kind == "plan") {
        const QJsonObject item{{"id", it->turnId + ":plan"},
                               {"type", "dynamicToolCall"},
                               {"tool", "Plan"},
                               {"status", "completed"},
                               {"entries", update.value("entries")}};
        emit canonicalEvent(sessionId, "item.completed",
                            {{"item_id", item.value("id")}, {"item", item}});
    }
}

void CursorAdapter::handlePermissionRequest(const QJsonValue& requestId,
                                            const QJsonObject& params) {
    const QString acpId = params.value(QStringLiteral("sessionId")).toString();
    const QString sessionId = ketplusByAcp_.value(acpId);
    if (sessionId.isEmpty()) {
        respond(requestId, {{QStringLiteral("allow"), false}});
        return;
    }
    permissionOptionsByRequest_.insert(requestKey(requestId), params.value("options").toArray());
    if (permissionBySession_.value(sessionId) == QStringLiteral("danger-full-access")) {
        respond(requestId, {{QStringLiteral("allow"), true}});
        return;
    }
    const QJsonObject toolCall = params.value(QStringLiteral("toolCall")).toObject();
    QJsonObject input = toolCall.value(QStringLiteral("rawInput")).toObject();
    // Cursor emits MCP identity in rawInput, independently of the display title.
    // Only the server injected for this session receives the browser exemption.
    if (browserSessions_.contains(sessionId) &&
        input.value(QStringLiteral("providerIdentifier")).toString() ==
            QStringLiteral("ketplus_browser") &&
        !input.value(QStringLiteral("toolName")).toString().isEmpty()) {
        respond(requestId, {{QStringLiteral("allow"), true}});
        return;
    }
    if (agentSessions_.contains(sessionId) &&
        input.value(QStringLiteral("providerIdentifier")).toString() ==
            QStringLiteral("ketplus_agents") &&
        !input.value(QStringLiteral("toolName")).toString().isEmpty()) {
        respond(requestId, {{QStringLiteral("allow"), true}});
        return;
    }
    if (input.isEmpty())
        input = toolCall;
    emit requestReceived(
        requestId, QStringLiteral("permission.request"),
        {{QStringLiteral("sessionId"), sessionId},
         {QStringLiteral("tool"), toolCall.value(QStringLiteral("title"))
                                      .toString(toolCall.value(QStringLiteral("kind")).toString())},
         {QStringLiteral("input"), input}});
}

void CursorAdapter::sendCall(const QString& method, const QJsonObject& params, Callback callback) {
    const quint64 id = nextRequestId_++;
    pending_.insert(id, std::move(callback));
    pendingMethods_.insert(id, method);
    sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), static_cast<qint64>(id)},
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), params}});
}

void CursorAdapter::sendObject(const QJsonObject& object) {
    if (process_->state() == QProcess::NotRunning)
        return;
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process_->write(payload);
}

void CursorAdapter::consumeStdout() {
    stdoutBuffer_.append(process_->readAllStandardOutput());
    if (stdoutBuffer_.size() > maximumProviderMessageBytes && !stdoutBuffer_.contains('\n')) {
        const QString reason =
            QStringLiteral("The Cursor ACP process emitted an oversized message.");
        stdoutBuffer_.clear();
        failPending(reason);
        emit stopped(reason);
        process_->kill();
        return;
    }
    qsizetype newline = -1;
    while ((newline = stdoutBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = stdoutBuffer_.left(newline).trimmed();
        stdoutBuffer_.remove(0, newline + 1);
        if (!line.isEmpty())
            consumeLine(line);
    }
}

void CursorAdapter::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return;
    const QJsonObject object = document.object();
    const QString method = object.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("session/update")) {
        handleSessionUpdate(object.value(QStringLiteral("params")).toObject());
        return;
    }
    if (!method.isEmpty() && object.contains(QStringLiteral("id"))) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (method == QStringLiteral("session/request_permission")) {
            handlePermissionRequest(object.value(QStringLiteral("id")), params);
            return;
        }
        if (method == QStringLiteral("cursor/ask_question")) {
            const QString acpSessionId = params.value(QStringLiteral("sessionId")).toString();
            const QString sessionId = ketplusByAcp_.value(acpSessionId);
            QJsonArray questions;
            for (const QJsonValue& questionValue :
                 params.value(QStringLiteral("questions")).toArray()) {
                const QJsonObject question = questionValue.toObject();
                questions.append(QJsonObject{
                    {QStringLiteral("id"), question.value(QStringLiteral("id"))},
                    {QStringLiteral("header"), params.value(QStringLiteral("title"))},
                    {QStringLiteral("question"), question.value(QStringLiteral("prompt"))},
                    {QStringLiteral("selection"),
                     question.value(QStringLiteral("allowMultiple")).toBool()
                         ? QStringLiteral("multiple")
                         : QStringLiteral("single")},
                    {QStringLiteral("options"), question.value(QStringLiteral("options"))}});
            }
            if (sessionId.isEmpty() || questions.isEmpty()) {
                respond(object.value(QStringLiteral("id")),
                        {{QStringLiteral("outcome"),
                          QJsonObject{{QStringLiteral("outcome"), QStringLiteral("skipped")}}}});
                return;
            }
            questionOptionsByRequest_.insert(requestKey(object.value(QStringLiteral("id"))),
                                             questions);
            QJsonObject normalized{
                {QStringLiteral("sessionId"), sessionId},
                {QStringLiteral("itemId"), params.value(QStringLiteral("toolCallId"))},
                {QStringLiteral("questions"), questions},
                {QStringLiteral("isBlocking"), true}};
            emit requestReceived(object.value(QStringLiteral("id")), method, normalized);
            return;
        }
        if (method == QStringLiteral("cursor/create_plan")) {
            QString sessionId = ketplusByAcp_.value(params.value("sessionId").toString());
            if (sessionId.isEmpty()) {
                for (auto turn = turns_.cbegin(); turn != turns_.cend(); ++turn)
                    if (turn->toolItems.contains(params.value("toolCallId").toString())) {
                        sessionId = turn.key();
                        break;
                    }
            }
            if (sessionId.isEmpty()) {
                respondError(object.value("id"), "unknown_session", "Plan has no active session.");
                return;
            }
            planRequests_.insert(requestKey(object.value("id")));
            const QJsonObject item{{"id", params.value("toolCallId")},
                                   {"type", "dynamicToolCall"},
                                   {"tool", "Plan"},
                                   {"arguments", params},
                                   {"status", "completed"}};
            emit canonicalEvent(sessionId, "item.completed",
                                {{"item_id", item.value("id")}, {"item", item}});
            emit requestReceived(
                object.value("id"), "permission.request",
                {{"sessionId", sessionId}, {"tool", "Accept plan"}, {"input", params}});
            return;
        }
        emit requestReceived(object.value(QStringLiteral("id")), method, params);
        return;
    }
    if (!object.contains(QStringLiteral("id")))
        return;
    const quint64 id = object.value(QStringLiteral("id")).toVariant().toULongLong();
    const auto callback = pending_.take(id);
    const QString requestMethod = pendingMethods_.take(id);
    if (!callback)
        return;
    if (object.contains(QStringLiteral("error"))) {
        const QJsonObject error = object.value(QStringLiteral("error")).toObject();
        if (error.value(QStringLiteral("code")).toInt() == -32601)
            (void)narrowing_.methodNotFound(requestMethod);
        callback({}, errorText(error));
    } else
        callback(object.value(QStringLiteral("result")).toObject(), {});
}

void CursorAdapter::failPending(const QString& reason) {
    pendingMethods_.clear();
    const auto pending = std::exchange(pending_, {});
    for (const Callback& callback : pending)
        callback({}, reason);
    const auto queued = std::exchange(queued_, {});
    for (const QueuedCall& call : queued)
        call.callback({}, reason);
    startingSessions_.clear();
    browserSessions_.clear();
    agentSessions_.clear();
    acpSessionByKetplus_.clear();
    ketplusByAcp_.clear();
    spawnModel_.clear();
    const auto turns = std::exchange(turns_, {});
    for (const TurnState& turn : turns) {
        emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("runtime.error"),
                            {{QStringLiteral("message"), reason}});
        emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("turn.completed"),
                            {{QStringLiteral("status"), QStringLiteral("failed")},
                             {QStringLiteral("thread_id"), turn.acpSessionId}});
    }
}

void CursorAdapter::setReady(const bool ready) { ready_ = ready; }

void CursorAdapter::noteUnexpectedStop() {
    restartDelayMs_ = restartDelayMs_ <= 0 ? 1000 : std::min(restartDelayMs_ * 2, 60 * 1000);
    restartNotBefore_.setRemainingTime(restartDelayMs_);
}

QString CursorAdapter::stderrTail() const {
    constexpr qsizetype maximumTailBytes = 2000;
    const QString text =
        QString::fromUtf8(stderrBuffer_.right(std::min(stderrBuffer_.size(), maximumTailBytes)))
            .trimmed();
    return text.isEmpty() ? QString() : QStringLiteral("\n\n%1").arg(text);
}

bool CursorAdapter::looksLikeAuthError(const QString& message) {
    const QString lower = message.toLower();
    return lower.contains(QStringLiteral("not authenticated")) ||
           lower.contains(QStringLiteral("unauthenticated")) ||
           lower.contains(QStringLiteral("sign in")) ||
           lower.contains(QStringLiteral("cursor_login")) ||
           (lower.contains(QStringLiteral("login")) && lower.contains(QStringLiteral("auth")));
}

QString CursorAdapter::findExecutable() {
    const QString fromEnv = qEnvironmentVariable("KETPLUS_CURSOR_AGENT");
    if (!fromEnv.isEmpty()) {
        const QFileInfo info(fromEnv);
        if (looksRunnable(info))
            return info.absoluteFilePath();
    }
    for (const QString& name : {QStringLiteral("agent"), QStringLiteral("cursor-agent")}) {
        const QString fromPath = QStandardPaths::findExecutable(name);
        if (!fromPath.isEmpty())
            return fromPath;
    }
    for (const QString& candidate : wellKnownExecutableCandidates()) {
        const QFileInfo info(candidate);
        if (looksRunnable(info))
            return info.absoluteFilePath();
    }
    return {};
}

} // namespace ketplus::daemon
