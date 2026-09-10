#include "preview/MermaidRenderer.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace ketplus {
namespace {

constexpr auto cacheVersion = "ketplus-mermaid-v3";

bool isExecutableFile(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isExecutable();
}

QByteArray mermaidConfiguration(const bool darkTheme) {
    const QString background = darkTheme ? QStringLiteral("#1D2228")
                                         : QStringLiteral("#FFFFFF");
    const QString surface = darkTheme ? QStringLiteral("#23282F")
                                      : QStringLiteral("#F7F5F5");
    const QString text = darkTheme ? QStringLiteral("#CDD2D8")
                                   : QStringLiteral("#24262A");
    const QString border = darkTheme ? QStringLiteral("#858D99")
                                     : QStringLiteral("#717373");
    const QString line = darkTheme ? QStringLiteral("#B4BAC4")
                                   : QStringLiteral("#5A5C5E");

    const QJsonObject themeVariables{
        {QStringLiteral("darkMode"), darkTheme},
        {QStringLiteral("background"), background},
        {QStringLiteral("primaryColor"), surface},
        {QStringLiteral("primaryTextColor"), text},
        {QStringLiteral("primaryBorderColor"), border},
        {QStringLiteral("secondaryColor"), surface},
        {QStringLiteral("secondaryTextColor"), text},
        {QStringLiteral("secondaryBorderColor"), border},
        {QStringLiteral("tertiaryColor"), background},
        {QStringLiteral("tertiaryTextColor"), text},
        {QStringLiteral("tertiaryBorderColor"), border},
        {QStringLiteral("textColor"), text},
        {QStringLiteral("lineColor"), line},
        {QStringLiteral("mainBkg"), surface},
        {QStringLiteral("nodeBkg"), surface},
        {QStringLiteral("nodeTextColor"), text},
        {QStringLiteral("nodeBorder"), border},
        {QStringLiteral("edgeLabelBackground"), background},
        {QStringLiteral("stateBkg"), surface},
        {QStringLiteral("stateLabelColor"), text},
        {QStringLiteral("transitionColor"), line},
        {QStringLiteral("transitionLabelColor"), text},
        {QStringLiteral("labelBackgroundColor"), surface},
        {QStringLiteral("actorBkg"), surface},
        {QStringLiteral("actorBorder"), border},
        {QStringLiteral("actorTextColor"), text},
        {QStringLiteral("actorLineColor"), line},
        {QStringLiteral("signalColor"), line},
        {QStringLiteral("signalTextColor"), text},
        {QStringLiteral("labelTextColor"), text},
        {QStringLiteral("noteBkgColor"), surface},
        {QStringLiteral("noteTextColor"), text},
        {QStringLiteral("noteBorderColor"), border},
        {QStringLiteral("fontFamily"), QStringLiteral("Inter, sans-serif")},
        {QStringLiteral("fontSize"), QStringLiteral("13px")},
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("theme"), QStringLiteral("base")},
                                     {QStringLiteral("themeVariables"), themeVariables}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

MermaidRenderer::MermaidRenderer(QObject* parent)
    : QObject(parent), executablePath_(discoverExecutable()), process_(new QProcess(this)) {
    cacheDirectory_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDirectory_.isEmpty()) {
        cacheDirectory_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                          QStringLiteral("/ketplus");
    }
    cacheDirectory_ += QStringLiteral("/mermaid");
    QDir().mkpath(cacheDirectory_);

    connect(process_, &QProcess::finished, this,
            [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                if (!currentJob_.has_value()) {
                    return;
                }
                const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0 &&
                                       QFileInfo(currentJob_->outputPath).size() > 0;
                QString error = QString::fromUtf8(process_->readAllStandardError()).trimmed();
                if (!succeeded && error.isEmpty()) {
                    error = QStringLiteral("mmdc exited with code %1").arg(exitCode);
                }
                finishCurrent(succeeded, error);
            });
    connect(process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && currentJob_.has_value()) {
            finishCurrent(false, process_->errorString());
        }
    });
}

MermaidRenderer::~MermaidRenderer() {
    disconnect(process_, nullptr, this, nullptr);
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(500)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
    if (currentJob_) {
        QFile::remove(currentJob_->outputPath);
        QFile::remove(QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".mmd")));
        QFile::remove(QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".json")));
    }
}

bool MermaidRenderer::isAvailable() const noexcept { return !executablePath_.isEmpty(); }

const QString& MermaidRenderer::executablePath() const noexcept { return executablePath_; }

MermaidRenderer::Result MermaidRenderer::request(const QString& source, const bool darkTheme, const bool raster) {
    const QString cacheKey = keyFor(source, darkTheme, raster);
    const QString outputPath = outputPathFor(cacheKey, raster);
    if (!isAvailable()) {
        executablePath_ = discoverExecutable();
    }
    if (!isAvailable()) {
        return {State::Unavailable, cacheKey, {}, {}};
    }
    if (QFileInfo(outputPath).size() > 0) {
        return {State::Ready, cacheKey, outputPath, {}};
    }
    if (const auto failure = failures_.constFind(cacheKey); failure != failures_.constEnd()) {
        return {State::Failed, cacheKey, {}, *failure};
    }
    if (queuedKeys_.contains(cacheKey) ||
        (currentJob_.has_value() && currentJob_->cacheKey == cacheKey)) {
        return {State::Pending, cacheKey, {}, {}};
    }

    queue_.append({cacheKey, source, outputPath, darkTheme});
    queuedKeys_.insert(cacheKey);
    startNext();
    return {State::Pending, cacheKey, {}, {}};
}

QString MermaidRenderer::discoverExecutable() {
    if (qEnvironmentVariableIsSet("KETPLUS_MMDC")) {
        const QString configured = qEnvironmentVariable("KETPLUS_MMDC");
        return isExecutableFile(configured) ? configured : QString();
    }

    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("mmdc"));
    if (!fromPath.isEmpty()) {
        return fromPath;
    }

    const QString home = QDir::homePath();
    const QStringList fixedCandidates{
        QStringLiteral("/opt/homebrew/bin/mmdc"),
        QStringLiteral("/usr/local/bin/mmdc"),
        home + QStringLiteral("/.volta/bin/mmdc"),
        home + QStringLiteral("/.asdf/shims/mmdc"),
        home + QStringLiteral("/.local/share/mise/shims/mmdc"),
        home + QStringLiteral("/.local/share/fnm/aliases/default/bin/mmdc"),
        home + QStringLiteral("/.npm-global/bin/mmdc"),
    };
    for (const auto& candidate : fixedCandidates) {
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }

    QDir nvmVersions(home + QStringLiteral("/.nvm/versions/node"));
    const QFileInfoList nodeVersions =
        nvmVersions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo& nodeVersion : nodeVersions) {
        const QString candidate = nodeVersion.absoluteFilePath() + QStringLiteral("/bin/mmdc");
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

QString MermaidRenderer::keyFor(const QString& source, const bool darkTheme, const bool raster) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(cacheVersion);
    const char themeMarker = darkTheme ? '\1' : '\0';
    hash.addData(QByteArrayView(&themeMarker, 1));
    hash.addData(raster ? "png" : "svg");
    hash.addData(source.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

QString MermaidRenderer::outputPathFor(const QString& cacheKey, const bool raster) const {
    return QDir(cacheDirectory_).filePath(cacheKey + (raster ? QStringLiteral(".png") : QStringLiteral(".svg")));
}

void MermaidRenderer::startNext() {
    if (currentJob_.has_value() || queue_.isEmpty() || !isAvailable()) {
        return;
    }

    currentJob_ = queue_.takeFirst();
    const QString inputPath =
        QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".mmd"));
    QFile input(inputPath);
    if (!input.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        input.write(currentJob_->source.toUtf8()) < 0) {
        finishCurrent(false, input.errorString());
        return;
    }
    input.close();

    const QString configPath =
        QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".json"));
    QFile config(configPath);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        config.write(mermaidConfiguration(currentJob_->darkTheme)) < 0) {
        finishCurrent(false, config.errorString());
        return;
    }
    config.close();

    QStringList arguments{QStringLiteral("-i"), inputPath, QStringLiteral("-o"),
                          currentJob_->outputPath, QStringLiteral("-b"),
                          QStringLiteral("transparent"), QStringLiteral("-c"), configPath};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString executableDirectory = QFileInfo(executablePath_).absolutePath();
    environment.insert(QStringLiteral("PATH"),
                       executableDirectory + QDir::listSeparator() +
                           environment.value(QStringLiteral("PATH")));
    process_->setProcessEnvironment(environment);
    if (currentJob_->outputPath.endsWith(QStringLiteral(".png"))) arguments << QStringLiteral("-s") << QStringLiteral("2");
#ifdef Q_OS_WIN
    if (executablePath_.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive) ||
        executablePath_.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive)) {
        arguments.prepend(executablePath_);
        arguments.prepend(QStringLiteral("/C"));
        process_->start(qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe")), arguments);
        return;
    }
#endif
    process_->start(executablePath_, arguments);
}

void MermaidRenderer::finishCurrent(const bool succeeded, const QString& error) {
    if (!currentJob_.has_value()) {
        return;
    }

    const Job completed = *currentJob_;
    QFile::remove(QDir(cacheDirectory_).filePath(completed.cacheKey + QStringLiteral(".mmd")));
    QFile::remove(QDir(cacheDirectory_).filePath(completed.cacheKey + QStringLiteral(".json")));
    queuedKeys_.remove(completed.cacheKey);
    currentJob_.reset();

    if (succeeded) {
        failures_.remove(completed.cacheKey);
        emit diagramReady(completed.cacheKey);
    } else {
        QFile::remove(completed.outputPath);
        QString conciseError = error.simplified();
        if (conciseError.size() > 240) {
            conciseError = conciseError.left(237) + QStringLiteral("…");
        }
        failures_.insert(completed.cacheKey, conciseError);
        emit diagramFailed(completed.cacheKey, conciseError);
    }
    startNext();
}

} // namespace ketplus
