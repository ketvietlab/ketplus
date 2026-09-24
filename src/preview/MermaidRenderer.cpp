#include "preview/MermaidRenderer.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace ketplus {
namespace {

// v4: labels are SVG text rows, not HTML in <foreignObject>, which Qt SVG does not draw.
// v5: node labels carry an explicit fill; cached state diagrams drew them black.
constexpr auto cacheVersion = "ketplus-mermaid-v5";

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
    // HTML labels render inside <foreignObject>, which Qt SVG skips: every label vanished.
    const QJsonObject svgLabels{{QStringLiteral("htmlLabels"), false}};
    // Mermaid's state diagram styles only HTML node labels; its SVG text rows get
    // no fill and draw black, unreadable on the dark background.
    const QString themeCss = QStringLiteral(".label text{fill:%1;}").arg(text);
    return QJsonDocument(QJsonObject{{QStringLiteral("theme"), QStringLiteral("base")},
                                     {QStringLiteral("themeCSS"), themeCss},
                                     {QStringLiteral("htmlLabels"), false},
                                     {QStringLiteral("flowchart"), svgLabels},
                                     {QStringLiteral("class"), svgLabels},
                                     {QStringLiteral("state"), svgLabels},
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
                const QString partial = partialPathFor(currentJob_->outputPath);
                bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0 &&
                                 QFileInfo(partial).size() > 0;
                if (succeeded && partial.endsWith(QStringLiteral(".svg"))) {
                    QFile output(partial);
                    if (output.open(QIODevice::ReadWrite)) {
                        const QByteArray flattened = flattenLabelRows(output.readAll());
                        output.resize(0);
                        output.seek(0);
                        succeeded = output.write(flattened) == flattened.size();
                    } else {
                        succeeded = false;
                    }
                }
                // The finished file appears under its cache name only now, so a request made
                // while mmdc was still writing never reads a partial or unprocessed diagram.
                if (succeeded) {
                    QFile::remove(currentJob_->outputPath);
                    succeeded = QFile::rename(partial, currentJob_->outputPath);
                }
                QFile::remove(partial);
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
        QFile::remove(partialPathFor(currentJob_->outputPath));
        QFile::remove(QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".mmd")));
        QFile::remove(QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".json")));
    }
}

QByteArray MermaidRenderer::flattenLabelRows(const QByteArray& svg) {
    const QString source = QString::fromUtf8(svg);
    if (!source.contains(QStringLiteral("text-outer-tspan")))
        return svg;
    // em resolves against the root font size mermaid writes into its stylesheet.
    static const QRegularExpression rootFont(QStringLiteral(R"re(font-size:\s*([0-9.]+)px)re"));
    const auto fontMatch = rootFont.match(source);
    const double fontSize = fontMatch.hasMatch() ? fontMatch.captured(1).toDouble() : 16.0;
    const auto length = [fontSize](const QString& value) {
        const QString trimmed = value.trimmed();
        return trimmed.endsWith(QStringLiteral("em")) ? trimmed.chopped(2).toDouble() * fontSize
                                                      : trimmed.toDouble();
    };
    static const QRegularExpression textElement(QStringLiteral(R"re(<text([^>]*)>(.*?)</text>)re"),
                                                QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression positionAttribute(QStringLiteral(R"re(\s(?:y|dy)="[^"]*")re"));
    static const QRegularExpression rowStart(
        QStringLiteral(R"re(<tspan class="text-outer-tspan row"([^>]*?)/?>)re"));
    static const QRegularExpression yAttribute(QStringLiteral(R"re(\sy="([^"]*)")re"));
    static const QRegularExpression dyAttribute(QStringLiteral(R"re(\sdy="([^"]*)")re"));
    static const QRegularExpression anchorAttribute(QStringLiteral(R"re(text-anchor="[^"]*")re"));
    static const QRegularExpression innerText(
        QStringLiteral(R"re(<tspan[^>]*class="text-inner-tspan"[^>]*>([^<]*)</tspan>)re"));

    QString result;
    result.reserve(source.size());
    qsizetype copied = 0;
    auto texts = textElement.globalMatch(source);
    while (texts.hasNext()) {
        const auto text = texts.next();
        const QString body = text.captured(2);
        if (!body.contains(QStringLiteral("text-outer-tspan")))
            continue;
        result += QStringView(source).mid(copied, text.capturedStart() - copied);
        copied = text.capturedEnd();
        const QString attributes = QString(text.captured(1)).remove(positionAttribute);
        auto rows = rowStart.globalMatch(body);
        QList<QRegularExpressionMatch> starts;
        while (rows.hasNext())
            starts.append(rows.next());
        for (qsizetype row = 0; row < starts.size(); ++row) {
            const auto& start = starts.at(row);
            const qsizetype end =
                row + 1 < starts.size() ? starts.at(row + 1).capturedStart() : body.size();
            const QString rowAttributes = start.captured(1);
            const QString content = body.mid(start.capturedEnd(), end - start.capturedEnd());
            QString line;
            auto words = innerText.globalMatch(content);
            while (words.hasNext())
                line += words.next().captured(1);
            const double y = length(yAttribute.match(rowAttributes).captured(1)) +
                             length(dyAttribute.match(rowAttributes).captured(1));
            const auto anchor = anchorAttribute.match(rowAttributes);
            result +=
                QStringLiteral("<text%1 y=\"%2\"%3>%4</text>")
                    .arg(attributes, QString::number(y, 'f', 2),
                         anchor.hasMatch() && !attributes.contains(QStringLiteral("text-anchor"))
                             ? QLatin1Char(' ') + anchor.captured(0)
                             : QString{},
                         line);
        }
    }
    result += QStringView(source).mid(copied);
    return result.toUtf8();
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

// Keeps the extension last: mmdc picks the output format from it.
QString MermaidRenderer::partialPathFor(const QString& outputPath) {
    const QFileInfo info(outputPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".partial.") +
                               info.suffix());
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

    QStringList arguments{QStringLiteral("-i"), inputPath,
                          QStringLiteral("-o"), partialPathFor(currentJob_->outputPath),
                          QStringLiteral("-b"), QStringLiteral("transparent"),
                          QStringLiteral("-c"), configPath};
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
