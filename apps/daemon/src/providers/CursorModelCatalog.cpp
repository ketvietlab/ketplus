#include "providers/CursorAdapter.h"
#include "providers/CursorModelVariant.h"
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
#include <algorithm>
#include <memory>
#include <utility>

namespace ketplus::daemon {
namespace {
using namespace cursor_model;
constexpr qsizetype maximumCatalogOutputBytes = 1024 * 1024;
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

void configureHiddenProcess(QProcess* process) {
#if defined(Q_OS_WIN)
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(process);
#endif
}

} // namespace
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
    QProcess* process = new QProcess(this);
    catalogProcess_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    configureHiddenProcess(process);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process->setProcessEnvironment(environment);
    const ProcessInvocation invocation = listModelsInvocation(executable_.path());
    process->setProgram(invocation.program);
    process->setArguments(invocation.arguments);
    connect(process, &QProcess::started, process, &QProcess::closeWriteChannel);
    struct Output final {
        QByteArray stdoutBytes;
        qsizetype totalBytes{0};
        bool rejected{false};
    };
    const auto output = std::make_shared<Output>();
    const auto readOutput = [this, process, output](QProcess::ProcessChannel channel) {
        if (catalogProcess_ != process || output->rejected)
            return;
        process->setReadChannel(channel);
        while (process->bytesAvailable() > 0) {
            const qsizetype remaining = maximumCatalogOutputBytes - output->totalBytes;
            const QByteArray chunk = process->read(qMin<qsizetype>(64 * 1024, remaining + 1));
            if (chunk.isEmpty())
                break;
            if (chunk.size() > remaining) {
                output->rejected = true;
                output->stdoutBytes.clear();
                process->closeReadChannel(QProcess::StandardOutput);
                process->closeReadChannel(QProcess::StandardError);
                process->kill();
                return;
            }
            output->totalBytes += chunk.size();
            if (channel == QProcess::StandardOutput)
                output->stdoutBytes.append(chunk);
        }
    };
    const auto complete = [this, process, output](bool successful) {
        if (catalogProcess_ != process)
            return;
        const QJsonArray models = successful && !output->rejected
                                      ? parseModelList(QString::fromUtf8(output->stdoutBytes))
                                      : QJsonArray{};
        catalogTimer_->stop();
        process->disconnect(this);
        process->deleteLater();
        catalogProcess_ = nullptr;
        finishCatalog(models.isEmpty() ? modelCatalog() : models);
    };
    connect(process, &QProcess::readyReadStandardOutput, this,
            [readOutput] { readOutput(QProcess::StandardOutput); });
    connect(process, &QProcess::readyReadStandardError, this,
            [readOutput] { readOutput(QProcess::StandardError); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [readOutput, complete](int exitCode, QProcess::ExitStatus status) {
                readOutput(QProcess::StandardOutput);
                readOutput(QProcess::StandardError);
                complete(status == QProcess::NormalExit && exitCode == 0);
            });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false);
    });
    catalogTimer_->start(15 * 1000);
    process->start();
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
} // namespace ketplus::daemon
