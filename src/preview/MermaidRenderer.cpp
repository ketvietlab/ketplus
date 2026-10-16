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

static void initializeMermaidThemeResources() {
    [[maybe_unused]] static const bool initialized = [] {
        Q_INIT_RESOURCE(ketplus_mermaid_themes);
        return true;
    }();
}

namespace ketplus {
namespace {

// v4: labels are SVG text rows, not HTML in <foreignObject>, which Qt SVG does not draw.
// v5: node labels carry an explicit fill; cached state diagrams drew them black.
// v6: Qt-readable class colours, Két theme, early stylesheets and rounded rectangles.
// v7: canonical KétSuite theme snapshots and 12px diagram corners.
constexpr auto cacheVersion = "ketplus-mermaid-v7";

bool isExecutableFile(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isExecutable();
}

QByteArray mermaidConfiguration(const bool darkTheme) {
    initializeMermaidThemeResources();
    QFile resource(darkTheme ? QStringLiteral(":/ketplus/mermaid/dark.json")
                             : QStringLiteral(":/ketplus/mermaid/light.json"));
    if (!resource.open(QIODevice::ReadOnly))
        return {};
    QJsonObject configuration = QJsonDocument::fromJson(resource.readAll()).object();
    if (configuration.isEmpty())
        return {};
    QJsonObject variables = configuration.value(QStringLiteral("themeVariables")).toObject();
    const QString text = variables.value(QStringLiteral("textColor")).toString();
    // Mermaid state SVG labels need an explicit fill in Qt; the canonical browser theme
    // supplies the remaining colors, geometry and semantic classes unchanged.
    variables.insert(QStringLiteral("stateLabelColor"), text);
    configuration.insert(QStringLiteral("themeVariables"), variables);
    configuration.insert(QStringLiteral("themeCSS"),
                         configuration.value(QStringLiteral("themeCSS")).toString() +
                             QStringLiteral("\n.label text{fill:%1;}").arg(text));
    return QJsonDocument(configuration).toJson(QJsonDocument::Compact);
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
                        const QByteArray flattened = qtReadableSvg(output.readAll());
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
        QFile::remove(
            QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".mmd")));
        QFile::remove(
            QDir(cacheDirectory_).filePath(currentJob_->cacheKey + QStringLiteral(".json")));
    }
}

QByteArray MermaidRenderer::qtReadableSvg(const QByteArray& svg) {
    // classDef and `style` colours arrive as `fill:#c0392b !important`, both inline and in
    // the stylesheet. Qt SVG reads the flag as part of the colour, rejects it and paints the
    // shape black with black labels. Inline styles already win, and each class rule follows
    // the theme rule it overrides, so dropping the flag keeps the cascade. Only a flag that
    // ends a declaration goes; a label that says "!important" keeps it.
    static const QRegularExpression important(QStringLiteral(R"re(\s*!\s*important(?=\s*[;}"]))re"),
                                              QRegularExpression::CaseInsensitiveOption);
    QString source = QString::fromUtf8(svg);
    static const QRegularExpression cssContent(
        QStringLiteral(R"re(<style\b[^>]*>.*?</style>|\sstyle="[^"]*")re"),
        QRegularExpression::DotMatchesEverythingOption);
    auto css = cssContent.globalMatch(source);
    QList<QRegularExpressionMatch> cssMatches;
    while (css.hasNext())
        cssMatches.append(css.next());
    for (auto it = cssMatches.crbegin(); it != cssMatches.crend(); ++it) {
        const QString cleaned = QString(it->captured(0)).remove(important);
        source.replace(it->capturedStart(), it->capturedLength(), cleaned);
    }
    // Qt applies a stylesheet only to elements parsed after it. Mermaid sequences put
    // actor boxes before <style>, so move styles to the start of the SVG root.
    static const QRegularExpression stylesheet(QStringLiteral(R"re(<style\b[^>]*>.*?</style>)re"),
                                               QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression svgRoot(QStringLiteral(R"re(<svg\b[^>]*>)re"));
    QString styles;
    auto sheets = stylesheet.globalMatch(source);
    while (sheets.hasNext())
        styles += sheets.next().captured(0);
    source.remove(stylesheet);
    const auto root = svgRoot.match(source);
    if (root.hasMatch())
        source.insert(root.capturedEnd(), styles);

    // SVG rx/ry work in both Qt and browsers; CSS border-radius does not round SVG rects.
    // Use the canonical diagram's 12px radius, retaining larger radii for pill/stadium shapes.
    static const QRegularExpression rectangle(QStringLiteral(R"re(<rect\b[^>]*>)re"));
    QString rounded;
    qsizetype copied = 0;
    auto rectangles = rectangle.globalMatch(source);
    while (rectangles.hasNext()) {
        const auto rect = rectangles.next();
        rounded += QStringView(source).mid(copied, rect.capturedStart() - copied);
        copied = rect.capturedEnd();
        QString element = rect.captured(0);
        for (const QString& axis : {QStringLiteral("rx"), QStringLiteral("ry")}) {
            const QRegularExpression attribute(QStringLiteral(R"re(\s%1="([^"]*)")re").arg(axis));
            const auto existing = attribute.match(element);
            if (existing.hasMatch()) {
                bool numeric = false;
                const double radius = existing.captured(1).toDouble(&numeric);
                if (numeric && radius < 12.0)
                    element.replace(existing.capturedStart(), existing.capturedLength(),
                                    QStringLiteral(" %1=\"12\"").arg(axis));
            } else {
                const qsizetype end = element.endsWith(QStringLiteral("/>")) ? element.size() - 2
                                                                             : element.size() - 1;
                element.insert(end, QStringLiteral(" %1=\"12\"").arg(axis));
            }
        }
        rounded += element;
    }
    rounded += QStringView(source).mid(copied);
    return flattenLabelRows(rounded.toUtf8());
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

MermaidRenderer::Result MermaidRenderer::request(const QString& source, const bool darkTheme,
                                                 const bool raster) {
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
    return QDir(cacheDirectory_)
        .filePath(cacheKey + (raster ? QStringLiteral(".png") : QStringLiteral(".svg")));
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
    const QByteArray configuration = mermaidConfiguration(currentJob_->darkTheme);
    if (configuration.isEmpty()) {
        finishCurrent(false, QStringLiteral("The bundled Mermaid theme is unavailable."));
        return;
    }
    QFile config(configPath);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        config.write(configuration) < 0) {
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
    environment.insert(QStringLiteral("PATH"), executableDirectory + QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    process_->setProcessEnvironment(environment);
    if (currentJob_->outputPath.endsWith(QStringLiteral(".png")))
        arguments << QStringLiteral("-s") << QStringLiteral("2");
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
