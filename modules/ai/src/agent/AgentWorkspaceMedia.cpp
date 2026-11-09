#include "KetAppearance.h"
#include "KetProfile.h"
#include "KetRichRecipes.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/ChatMarkdown.h"
#include "agent/MermaidLabelContrast.h"
#include "agent/TranscriptFileLinks.h"
#include "agent/TranscriptSyntax.h"
#include "agent/TranscriptTableStyle.h"
#include "preview/MermaidRenderer.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScreen>

#include "agent/CodexAppServer.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPainter>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextTable>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ketplus {
namespace {

constexpr int codeStyledProperty = QTextFormat::UserProperty + 1;
constexpr int mermaidStyledProperty = QTextFormat::UserProperty + 2;
constexpr int mermaidInsetProperty = QTextFormat::UserProperty + 3;
constexpr qreal inlineCodePaddingX = 4;
constexpr qreal inlineCodePaddingY = 2;

// The document and its overlay frames use the same scaled recipe dimensions.
qreal recipePixels(qreal value) {
    return ket::Appearance::scale(value, ket::Appearance::instance().zoom());
}

bool isCodeBlock(const QTextBlock& block) {
    return block.isValid() && block.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage);
}

bool sameCodeBlock(const QTextBlock& first, const QTextBlock& second) {
    return isCodeBlock(first) && isCodeBlock(second) &&
           first.blockFormat().property(QTextFormat::BlockCodeLanguage) ==
               second.blockFormat().property(QTextFormat::BlockCodeLanguage) &&
           first.blockFormat().property(QTextFormat::BlockCodeFence) ==
               second.blockFormat().property(QTextFormat::BlockCodeFence) &&
           first.blockFormat().property(QTextFormat::BlockQuoteLevel) ==
               second.blockFormat().property(QTextFormat::BlockQuoteLevel);
}

// The chip behind inline code reaches inlineCodePaddingX past the glyphs on both
// sides. Tracking the character before a code run and the run's last character
// by that much keeps the chip off the neighbouring text, as a browser's padding
// would. Only the layout moves; the text and what copies stay the same.
void spaceAroundInlineCode(const QTextBlock& block) {
    QList<int> spaced;
    bool previousCode = false;
    int previousEnd = -1;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        const auto format = fragment.charFormat();
        const bool code = !format.isImageFormat() && format.fontFixedPitch();
        if (code && !previousCode && previousEnd > block.position())
            spaced.append(previousEnd - 1);
        if (!code && previousCode)
            spaced.append(previousEnd - 1);
        previousCode = code;
        previousEnd = fragment.position() + fragment.length();
    }
    if (previousCode)
        spaced.append(previousEnd - 1);
    for (const int position : std::as_const(spaced)) {
        QTextCursor cursor(block);
        cursor.setPosition(position + 1);
        const auto current = cursor.charFormat();
        if (current.isImageFormat() ||
            (current.fontLetterSpacingType() == QFont::AbsoluteSpacing &&
             qFuzzyCompare(current.fontLetterSpacing(), inlineCodePaddingX)))
            continue;
        QTextCharFormat spacing;
        spacing.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        spacing.setFontLetterSpacing(inlineCodePaddingX);
        cursor.setPosition(position, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(spacing);
    }
}

// Inline code naming a file in the worktree becomes a link the transcript opens. The chip
// splits a span into fragments, so whole runs of code are read before linking.
void linkFileReferences(const QTextBlock& block, TranscriptFileLinks& links,
                        const QColor& linkColor) {
    struct Run final {
        int start;
        int end;
        QString text;
        bool linked;
    };
    QList<Run> runs;
    bool open = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        const auto format = fragment.charFormat();
        if (!fragment.isValid() || format.isImageFormat() || !format.fontFixedPitch()) {
            open = false;
            continue;
        }
        if (!open || runs.last().end != fragment.position())
            runs.append({fragment.position(), fragment.position(), {}, false});
        open = true;
        runs.last().end = fragment.position() + fragment.length();
        runs.last().text += fragment.text();
        runs.last().linked = runs.last().linked || format.isAnchor();
    }
    for (const Run& run : std::as_const(runs)) {
        if (run.linked)
            continue;
        const QString href = links.href(run.text);
        if (href.isEmpty())
            continue;
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(href);
        link.setForeground(linkColor);
        QTextCursor cursor(block);
        cursor.setPosition(run.start);
        cursor.setPosition(run.end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(link);
    }
}

void styleMessageCharacters(const QTextBlock& block, const qreal textPixelSize, const bool heading,
                            const QString& monoFamily, const ket::MarkdownRecipe& recipe,
                            qreal zoom) {
    const int pixelSize = qRound(textPixelSize);
    const int codePixelSize = qMax(8, qRound(ket::Appearance::scale(recipe.codePixels, zoom)));
    // Qt sets inline code in the platform fixed font at full size, which made
    // it the loudest thing in a sentence and a different face from the code
    // blocks. It now uses the code-block family a step smaller in the body
    // colour; messageInlineCode() gives the view the rounded chips drawn behind it.
    struct Change final {
        int start;
        int end;
        QTextCharFormat format;
    };
    QList<Change> changes;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        const auto current = fragment.charFormat();
        if (current.isImageFormat())
            continue;
        QTextCharFormat wanted;
        const bool code = current.fontFixedPitch();
        const int wantedSize = code ? codePixelSize : heading ? pixelSize : 0;
        if (wantedSize > 0 && current.intProperty(QTextFormat::FontPixelSize) != wantedSize)
            wanted.setProperty(QTextFormat::FontPixelSize, wantedSize);
        if (heading && current.intProperty(QTextFormat::FontSizeAdjustment) != 0)
            wanted.setProperty(QTextFormat::FontSizeAdjustment, 0);
        if ((heading ||
             current.fontWeight() > (heading ? recipe.headingWeight : recipe.strongWeight)) &&
            current.fontWeight() != (heading ? recipe.headingWeight : recipe.strongWeight))
            wanted.setFontWeight((heading ? recipe.headingWeight : recipe.strongWeight));
        if (code && current.fontFamilies().toStringList() != QStringList{monoFamily})
            wanted.setFontFamilies({monoFamily});
        if (!wanted.properties().isEmpty())
            changes.append({fragment.position(), fragment.position() + fragment.length(), wanted});
    }
    // Merging splits and joins fragments, so the ranges are gathered first.
    for (const auto& change : std::as_const(changes)) {
        QTextCursor cursor(block);
        cursor.setPosition(change.start);
        cursor.setPosition(change.end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(change.format);
    }
    spaceAroundInlineCode(block);
}

} // namespace

void AgentWorkspaceView::chooseAttachments() {
    addAttachments(
        QFileDialog::getOpenFileNames(this, tr("Attach images or text files"), activePath_));
}

bool AgentWorkspaceView::pasteClipboardImage() {
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasImage())
        return false;
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull())
        return false;
    const QDir directory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                         QStringLiteral("/attachments"));
    const QString path = directory.filePath(
        QStringLiteral("clipboard-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!directory.mkpath(QStringLiteral(".")) || !image.save(path, "PNG")) {
        QMessageBox::warning(this, tr("Cannot paste image"),
                             tr("Could not save the clipboard image."));
        return true;
    }
    addAttachments({path});
    return true;
}

void AgentWorkspaceView::addAttachments(const QStringList& paths) {
    for (const QString& value : paths) {
        const QUrl url(value);
        const QString path = url.isLocalFile() ? url.toLocalFile() : value;
        const QFileInfo info(path);
        const bool image = QImageReader(path).canRead();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) ||
            info.size() > (image ? 20 * 1024 * 1024 : 512 * 1024) ||
            (!image && file.peek(512 * 1024).contains('\0'))) {
            QMessageBox::information(
                this, tr("Cannot attach file"),
                tr("Choose an image up to 20 MB or a text file up to 512 KB."));
            continue;
        }
        bool duplicate = false;
        for (const auto& item : attachments_)
            if (item.toMap().value("path").toString() == path)
                duplicate = true;
        if (!duplicate)
            attachments_.append(QVariantMap{{"path", path},
                                            {"name", info.fileName()},
                                            {"image", image},
                                            {"url", QUrl::fromLocalFile(path).toString()}});
    }
    emit attachmentsChanged();
    saveAttachmentDraft(composerScope_);
}

QUrl AgentWorkspaceView::messageBaseUrl() const {
    return QUrl::fromLocalFile(activePath_ + QDir::separator());
}

bool AgentWorkspaceView::mermaidInstalling() const {
    return mermaidInstaller_ && mermaidInstaller_->state() != QProcess::NotRunning;
}

QVariantList AgentWorkspaceView::mermaidPlaceholderPositions(QObject* document) const {
    QVariantList positions;
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument())
        return positions;
    for (auto block = quickDocument->textDocument()->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            const QString name = fragment.charFormat().toImageFormat().name();
            const bool pending = name == QStringLiteral("qrc:/agent/mermaid-pending.svg");
            if (!pending && name != QStringLiteral("qrc:/agent/mermaid-placeholder.svg"))
                continue;
            // Both occupy the same reserved box; only what the overlay offers
            // differs, so the caller is told which one it is looking at.
            const auto imageFormat = fragment.charFormat().toImageFormat();
            positions.append(QVariantMap{{QStringLiteral("position"), fragment.position()},
                                         {QStringLiteral("pending"), pending},
                                         {QStringLiteral("width"), imageFormat.width()},
                                         {QStringLiteral("height"), imageFormat.height()}});
        }
    }
    return positions;
}

void AgentWorkspaceView::installMermaid() {
    if (!mermaidInstallOffered_ ||
        (mermaidInstaller_ && mermaidInstaller_->state() != QProcess::NotRunning))
        return;
    QString npm = QStandardPaths::findExecutable(QStringLiteral("npm"));
    if (npm.isEmpty()) {
        QStringList candidates{QStringLiteral("/opt/homebrew/bin/npm"),
                               QStringLiteral("/usr/local/bin/npm")};
        const QDir versions(QDir::homePath() + QStringLiteral("/.nvm/versions/node"));
        for (const auto& version :
             versions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time))
            candidates.append(version.absoluteFilePath() + QStringLiteral("/bin/npm"));
        for (const auto& candidate : candidates) {
            if (QFileInfo(candidate).isExecutable()) {
                npm = candidate;
                break;
            }
        }
    }
    mermaidInstallError_.clear();
    mermaidInstallOutput_.clear();
    if (npm.isEmpty()) {
        mermaidInstallError_ = tr("npm was not found. Install Node.js and npm, then retry.");
        ++mermaidRevision_;
        emit mermaidChanged();
        return;
    }
    if (!mermaidInstaller_) {
        mermaidInstaller_ = new QProcess(this);
        mermaidInstaller_->setObjectName(QStringLiteral("mermaidInstaller"));
        mermaidInstaller_->setProcessChannelMode(QProcess::MergedChannels);
        connect(mermaidInstaller_, &QProcess::readyReadStandardOutput, this, [this] {
            mermaidInstallOutput_ =
                (mermaidInstallOutput_ +
                 QString::fromLocal8Bit(mermaidInstaller_->readAllStandardOutput()))
                    .right(8000);
        });
        connect(mermaidInstaller_, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) {
                    mermaidInstallError_ =
                        code == 0 && status == QProcess::NormalExit
                            ? tr("Installation completed. If the preview is still unavailable, "
                                 "check KETPLUS_MMDC and PATH.")
                            : tr("Installation failed (exit %1).\n%2")
                                  .arg(code)
                                  .arg(mermaidInstallOutput_);
                    ++mermaidRevision_;
                    emit mermaidChanged();
                });
        connect(mermaidInstaller_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart)
                        return;
                    mermaidInstallError_ = mermaidInstaller_->errorString();
                    ++mermaidRevision_;
                    emit mermaidChanged();
                });
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QFileInfo(npm).absolutePath() +
                                                   QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    mermaidInstaller_->setProcessEnvironment(environment);
    mermaidInstaller_->setWorkingDirectory(QDir::homePath());
    mermaidInstaller_->start(npm, {QStringLiteral("install"), QStringLiteral("-g"),
                                   QStringLiteral("@mermaid-js/mermaid-cli")});
    ++mermaidRevision_;
    emit mermaidChanged();
}

namespace {
QString mermaidModeKey(const QString& messageKey, const int ordinal) {
    return QStringLiteral("%1#%2").arg(messageKey).arg(ordinal);
}
} // namespace

void AgentWorkspaceView::setMermaidMode(const QString& messageKey, const int ordinal,
                                        const bool rendered) {
    if (rendered)
        mermaidRendered_.insert(mermaidModeKey(messageKey, ordinal));
    else
        mermaidRendered_.remove(mermaidModeKey(messageKey, ordinal));
}

bool AgentWorkspaceView::mermaidRendered(const QString& messageKey, const int ordinal) const {
    return mermaidRendered_.contains(mermaidModeKey(messageKey, ordinal));
}

QString AgentWorkspaceView::renderChatMarkdown(const QString& source, const QString& messageKey) {
    const QString markdown =
        agent_message::qtMarkdownSource(chat_markdown::prepareForDisplay(source));
    if (!markdown.contains(QStringLiteral("mermaid"), Qt::CaseInsensitive)) {
        fenceOutcomes_.remove(messageKey);
        return markdown;
    }
    const auto lines = markdown.split('\n');
    static const QRegularExpression opening(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,})([^\r\n]*)$)"));
    QStringList output;
    QVector<FenceOutcome> outcomes;
    int ordinal = 0;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto match = opening.match(lines[i]);
        if (!match.hasMatch()) {
            output.append(lines[i]);
            continue;
        }
        const auto fence = match.captured(1);
        qsizetype end = i + 1;
        for (; end < lines.size(); ++end) {
            const auto line = lines[end].trimmed();
            if (line.size() >= fence.size() && line == QString(line.size(), fence.front()))
                break;
        }
        const bool mermaid = match.captured(2).trimmed().section(' ', 0, 0).compare(
                                 "mermaid", Qt::CaseInsensitive) == 0;
        const bool complete = end != lines.size();
        const int blockOrdinal = mermaid ? ordinal++ : -1;
        // A block is a diagram only when the reader asked for it, and only once its fence has
        // closed: a half-streamed diagram is never worth rendering. Everything else, Mermaid
        // included, is an ordinary source block.
        if (!mermaid || !complete || !mermaidRendered(messageKey, blockOrdinal)) {
            output.append(complete ? lines.mid(i, end - i + 1) : lines.mid(i));
            outcomes.append({mermaid, blockOrdinal, complete, false});
            if (!complete)
                break;
            i = end;
            continue;
        }
        const auto diagram =
            agent_message::withReadableMermaidLabels(lines.mid(i + 1, end - i - 1).join('\n'));
        const bool installing =
            mermaidInstaller_ && mermaidInstaller_->state() != QProcess::NotRunning;
        const auto result = installing ? MermaidRenderer::Result{}
                                       // Vector: the renderer writes labels as SVG text Qt draws.
                                       : mermaidRenderer_->request(diagram, mermaidPalette_.dark);
        bool image = true;
        if (result.state == MermaidRenderer::State::Ready &&
            QImageReader(result.filePath).canRead()) {
            const auto url = QUrl::fromLocalFile(result.filePath).toString(QUrl::FullyEncoded);
            mermaidImages_.insert(QUrl::fromLocalFile(result.filePath).toString());
            mermaidDiagrams_.insert(QUrl::fromLocalFile(result.filePath).toString(), diagram);
            output.append(QStringLiteral("\n![Mermaid diagram](<%1>)\n").arg(url));
        } else if (result.state == MermaidRenderer::State::Pending) {
            output.append(
                QStringLiteral("\n![Rendering Mermaid diagram](qrc:/agent/mermaid-pending.svg)\n"));
        } else if (result.state == MermaidRenderer::State::Unavailable) {
            mermaidInstallOffered_ = true;
            output.append(
                QStringLiteral("\n![Mermaid unavailable](qrc:/agent/mermaid-placeholder.svg)\n"));
        } else {
            image = false;
            output.append(lines.mid(i, end - i + 1));
            output.append(QStringLiteral("\n> Mermaid preview failed; source preserved above.\n"));
        }
        outcomes.append({true, blockOrdinal, true, image});
        i = end;
    }
    if (fenceOutcomes_.size() > 4096)
        fenceOutcomes_.clear();
    fenceOutcomes_.insert(messageKey, outcomes);
    return output.join('\n');
}

QVariantMap AgentWorkspaceView::mermaidViewerImage(const QUrl& url) {
    const auto diagram = mermaidDiagrams_.constFind(url.toString());
    if (diagram == mermaidDiagrams_.cend())
        return {};
    // The viewer is dark in both themes, so it shows the dark rendering, asked for here in a
    // light theme; until that is ready the light one is shown on its own white ground.
    QString svg = url.toLocalFile();
    bool dark = mermaidPalette_.dark;
    if (!dark && mermaidRenderer_ && mermaidRenderer_->isAvailable()) {
        const auto result = mermaidRenderer_->request(*diagram, true);
        if (result.state == MermaidRenderer::State::Ready &&
            QImageReader(result.filePath).canRead()) {
            svg = result.filePath;
            dark = true;
        }
    }
    QImageReader reader(svg);
    const QSize natural = reader.size();
    if (!natural.isValid())
        return {};
    const QScreen* target = screen() ? screen() : QGuiApplication::primaryScreen();
    const qreal density = target ? target->devicePixelRatio() : 1.0;
    const QSize screenSize = target ? target->size() : QSize(1440, 900);
    constexpr int kMaximumRaster = 8192;
    const qreal longSide =
        qMin<qreal>(kMaximumRaster, 2 * density * qMax(screenSize.width(), screenSize.height()));
    const int naturalLong = qMax(natural.width(), natural.height());
    QSize size;
    QString png;
    if (naturalLong > kMaximumRaster) {
        // Larger than the cap: scaled down to it, so the viewer's 100% is the cap's size.
        size = QSizeF(natural).scaled(kMaximumRaster, kMaximumRaster, Qt::KeepAspectRatio).toSize();
        png = svg + QStringLiteral("-viewer-%1.png").arg(kMaximumRaster);
    } else {
        // A whole multiple of the diagram's own size, named @Nx so the viewer measures its
        // zoom against the diagram rather than against the larger raster.
        const int factor = std::max(
            1, std::min({int(std::ceil(longSide / naturalLong)), kMaximumRaster / naturalLong, 9}));
        size = natural * factor;
        png = svg + QStringLiteral("-viewer@%1x.png").arg(factor);
    }
    if (!QFileInfo::exists(png)) {
        reader.setScaledSize(size);
        QImage image = reader.read();
        if (image.isNull())
            return {};
        if (!dark) {
            QImage ground(image.size(), QImage::Format_ARGB32_Premultiplied);
            ground.fill(Qt::white);
            QPainter painter(&ground);
            painter.drawImage(0, 0, image);
            painter.end();
            image = ground;
        }
        if (!image.save(png))
            return {};
    }
    // `diagram` names the transcript's image, so an open viewer can take the dark rendering
    // once it is ready.
    return {{QStringLiteral("source"), QUrl::fromLocalFile(png)},
            {QStringLiteral("caption"), QStringLiteral("Mermaid diagram")},
            {QStringLiteral("diagram"), url}};
}

QUrl AgentWorkspaceView::messageImageAt(QObject* document, int position) const {
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument() || position < 0 ||
        position >= quickDocument->textDocument()->characterCount() - 1)
        return {};
    QTextCursor cursor(quickDocument->textDocument());
    cursor.setPosition(position);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    const auto format = cursor.charFormat();
    if (!format.isImageFormat())
        return {};
    const QString name = format.toImageFormat().name();
    if (name.startsWith(QStringLiteral("~/")))
        return QUrl::fromLocalFile(QDir::home().filePath(name.mid(2)));
    return messageBaseUrl().resolved(QUrl(name));
}

QString AgentWorkspaceView::messageLinkAt(QObject* document, int position) const {
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    QTextDocument* textDocument = quickDocument->textDocument();
    const auto hrefAt = [textDocument](const int index) {
        if (index < 0 || index >= textDocument->characterCount() - 1)
            return QString{};
        QTextCursor cursor(textDocument);
        cursor.setPosition(index);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        return cursor.charFormat().anchorHref();
    };
    QString href = hrefAt(position);
    if (href.isEmpty())
        href = hrefAt(position - 1);
    return href;
}

qreal AgentWorkspaceView::mermaidFrameHeight(const qreal availableWidth) const {
    return agent_message::mermaidFrameHeight(availableWidth);
}

void AgentWorkspaceView::formatComposerDocument(QObject* document, qreal fontPixelSize) {
    auto* quick = qobject_cast<QQuickTextDocument*>(document);
    if (!quick || !quick->textDocument() || fontPixelSize <= 0)
        return;
    auto* doc = quick->textDocument();
    const qreal height = qFloor(fontPixelSize * ket::Profile::current().composerLineHeight);
    QTextCursor edit(doc);
    bool editing = false;
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        auto format = block.blockFormat();
        if (format.lineHeightType() == QTextBlockFormat::FixedHeight &&
            qFuzzyCompare(format.lineHeight(), height))
            continue;
        // Even an empty edit block emits contentsChanged. Open one only when
        // formatting actually changes, so the composer can settle while idle.
        if (!editing) {
            edit.joinPreviousEditBlock();
            editing = true;
        }
        format.setLineHeight(height, QTextBlockFormat::FixedHeight);
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
    }
    if (editing)
        edit.endEditBlock();
}

void AgentWorkspaceView::formatMessageDocument(QObject* document, qreal fontPixelSize,
                                               qreal availableWidth) {
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument() || fontPixelSize <= 0)
        return;
    QTextDocument* textDocument = quickDocument->textDocument();
    QTextCursor edit(textDocument);
    edit.beginEditBlock();
    const auto theme = ket::Appearance::instance().theme();
    const auto recipe = ket::markdownRecipe(theme);
    const auto codeRecipe = ket::codeBlockRecipe(theme);
    const qreal zoom = ket::Appearance::instance().zoom();
    const auto px = [zoom](qreal value) { return ket::Appearance::scale(value, zoom); };
    const qreal codePadding = px(codeRecipe.padding);
    const qreal codeVerticalPadding = codePadding;
    const qreal mermaidHeaderHeight = px(34);
    const QString monoFamily = monoFontFamily_;
    const QColor linkColor = recipe.link;
    applyTranscriptSyntax(textDocument, theme);
    textDocument->setIndentWidth(px(recipe.listIndent));
    fileLinks_.setRoot(activePath_.isEmpty() ? QString{} : executionPath());
    styleTranscriptTables(textDocument, recipe, zoom, codePadding);
    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        bool containsImage = false;
        bool mermaidView = false;
        qreal mermaidImageWidth = 0;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            containsImage = true;
            auto imageFormat = fragment.charFormat().toImageFormat();
            const QUrl source(imageFormat.name());
            QUrl resolved = messageBaseUrl().resolved(source);
            // A diagram, or what stands in for one, is drawn inside its own frame.
            const qreal diagramWidth = qMax(1.0, availableWidth - 2 * codePadding - 2);
            if (source.toString() == QStringLiteral("qrc:/agent/mermaid-placeholder.svg") ||
                source.toString() == QStringLiteral("qrc:/agent/mermaid-pending.svg")) {
                mermaidView = true;
                mermaidImageWidth = diagramWidth;
                const qreal height = agent_message::mermaidFrameHeight(availableWidth);
                if (qFuzzyCompare(imageFormat.width(), diagramWidth) &&
                    qFuzzyCompare(imageFormat.height(), height))
                    continue;
                imageFormat.setWidth(diagramWidth);
                imageFormat.setHeight(height);
                QTextCursor cursor(textDocument);
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(),
                                   QTextCursor::KeepAnchor);
                cursor.setCharFormat(imageFormat);
                continue;
            }
            if (imageFormat.name().startsWith(QStringLiteral("~/"))) {
                resolved = QUrl::fromLocalFile(QDir::home().filePath(imageFormat.name().mid(2)));
            }
            if (!resolved.isLocalFile())
                continue;
            const bool isMermaid = mermaidImages_.contains(resolved.toString());
            const bool vectorMermaid = isMermaid && QImageReader(resolved.toLocalFile()).format() ==
                                                        QByteArrayLiteral("svg");
            const bool rasterMermaid = isMermaid && !vectorMermaid;
            QImage image =
                qvariant_cast<QImage>(textDocument->resource(QTextDocument::ImageResource, source));
            // The document draws an SVG at its natural size in device-independent pixels, so a
            // Retina screen blew a 1x raster up and blurred the diagram. It is rasterised at
            // the screen's density instead, as the full-size viewer draws it.
            const qreal density = qMax<qreal>(1.0, qGuiApp->devicePixelRatio());
            if (vectorMermaid && (image.isNull() || image.devicePixelRatio() < density)) {
                QImageReader reader(resolved.toLocalFile());
                const QSize natural = reader.size();
                if (natural.isValid())
                    reader.setScaledSize((QSizeF(natural) * density).toSize());
                QImage dense = reader.read();
                if (!dense.isNull()) {
                    dense.setDevicePixelRatio(natural.isValid() ? density : 1.0);
                    image = dense;
                    textDocument->addResource(QTextDocument::ImageResource, source, image);
                }
            }
            if (image.isNull()) {
                image.load(resolved.toLocalFile());
                if (image.isNull())
                    continue;
                textDocument->addResource(QTextDocument::ImageResource, source, image);
            }
            const qreal naturalWidth =
                rasterMermaid ? image.width() / 2.0 : image.deviceIndependentSize().width();
            qreal width = qMin(naturalWidth, qMax(1.0, availableWidth - 2));
            qreal height = width * image.height() / image.width();
            if (!isMermaid) {
                // Inline Markdown photos follow attachment thumbnail bounds too.
                const qreal scale = qMin(1.0, 88.0 / qMax(width, height));
                width *= scale;
                height *= scale;
            }
            if (isMermaid) {
                // The reader asked for this diagram, so it takes the height it needs: its own
                // aspect ratio at the width of the frame, never cropped and never padded. Only
                // that row moves, and it moves once, when the reader switches.
                mermaidView = true;
                width = qMin(naturalWidth, diagramWidth);
                height = width * image.height() / qMax(1, image.width());
                mermaidImageWidth = width;
            }
            if (qFuzzyCompare(imageFormat.width(), width) &&
                qFuzzyCompare(imageFormat.height(), height))
                continue;
            imageFormat.setWidth(width);
            imageFormat.setHeight(height);
            QTextCursor imageCursor(textDocument);
            imageCursor.setPosition(fragment.position());
            imageCursor.setPosition(fragment.position() + fragment.length(),
                                    QTextCursor::KeepAnchor);
            imageCursor.setCharFormat(imageFormat);
        }
        auto format = block.blockFormat();
        if (isCodeBlock(block)) {
            const int codeFontSize = qMax(8, qRound(px(codeRecipe.fontPixels)));
            const qreal lineHeight = std::floor(codeFontSize * codeRecipe.lineHeight);
            const qreal paddingDelta = codePadding - format.doubleProperty(codeStyledProperty);
            format.setLeftMargin(format.leftMargin() + paddingDelta);
            format.setRightMargin(format.rightMargin() + paddingDelta);
            format.setProperty(codeStyledProperty, codePadding);
            format.setNonBreakableLines(false);
            format.setLineHeight(lineHeight, QTextBlockFormat::FixedHeight);
            format.setTopMargin(sameCodeBlock(block.previous(), block) ? 0 : codeVerticalPadding);
            format.setBottomMargin(sameCodeBlock(block, block.next()) ? 0 : codeVerticalPadding);
            QTextCursor cursor(block);
            if (format != block.blockFormat())
                cursor.setBlockFormat(format);
            const auto family = monoFontFamily_;
            const auto charFormat = block.charFormat();
            if (charFormat.intProperty(QTextFormat::FontPixelSize) != codeFontSize ||
                charFormat.fontFamilies().toStringList() != QStringList{family}) {
                QTextCharFormat codeFormat;
                codeFormat.setFontFamilies({family});
                codeFormat.setProperty(QTextFormat::FontPixelSize, codeFontSize);
                cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(codeFormat);
                cursor.mergeBlockCharFormat(codeFormat);
            }
            continue;
        }
        const int headingLevel = format.intProperty(QTextFormat::HeadingLevel);
        const bool heading = headingLevel > 0;
        const qreal textPixelSize = heading ? px(headingLevel == 1   ? recipe.heading1Pixels
                                                 : headingLevel == 2 ? recipe.heading2Pixels
                                                                     : recipe.heading3Pixels)
                                            : fontPixelSize;
        styleMessageCharacters(block, textPixelSize, heading, monoFamily, recipe, zoom);
        linkFileReferences(block, fileLinks_, linkColor);
        const auto heightType =
            containsImage ? QTextBlockFormat::MinimumHeight : QTextBlockFormat::FixedHeight;
        // A heading keeps the body's line height otherwise, which clipped the
        // taller glyphs against the paragraph below.
        const qreal lineHeight = heading ? std::ceil(textPixelSize * recipe.headingLineHeight)
                                         : fontPixelSize * recipe.lineHeight;
        const bool continuesList = block.textList() != nullptr && block.next().isValid() &&
                                   block.next().textList() == block.textList();
        const qreal topMargin = mermaidView ? mermaidHeaderHeight
                                : heading && block.previous().isValid()
                                    ? px(recipe.paragraphSpacing)
                                    : 0;
        const bool tableCell = QTextCursor(block).currentTable() != nullptr;
        const qreal bottomMargin = tableCell       ? 0
                                   : mermaidView   ? codeVerticalPadding
                                   : heading       ? px(recipe.headingSpacingAfter)
                                   : continuesList ? px(recipe.listItemGap)
                                                   : px(recipe.paragraphSpacing);
        // A diagram narrower than its frame sits in the middle of it. The block's own text
        // alignment does not center an inline image, so the left inset does it.
        // The frame is the block's width less the padding on both sides; the diagram is centered
        // in that, and never closer to the left edge than the padding.
        const qreal wantedInset =
            mermaidView
                ? qMax(codePadding, (availableWidth - 2 * codePadding - mermaidImageWidth) / 2)
                : 0.0;
        const qreal appliedInset = format.doubleProperty(mermaidInsetProperty);
        format.setLineHeight(lineHeight, heightType);
        format.setTopMargin(topMargin);
        format.setBottomMargin(bottomMargin);
        if (mermaidView) {
            // The frame padding on both sides, like a code block, plus the centering on the left.
            const qreal paddingDelta = codePadding - format.doubleProperty(mermaidStyledProperty);
            format.setRightMargin(format.rightMargin() + paddingDelta);
            format.setProperty(mermaidStyledProperty, codePadding);
            format.setLeftMargin(format.leftMargin() - appliedInset + wantedInset);
            format.setProperty(mermaidInsetProperty, wantedInset);
        }
        if (format.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            format.setBackground(recipe.quoteBackground);
            format.setLeftMargin(px(recipe.quotePadding));
        }
        if (format != block.blockFormat()) {
            QTextCursor cursor(block);
            cursor.setBlockFormat(format);
        }
    }
    edit.endEditBlock();
}

QVariantList AgentWorkspaceView::messageCodeBlocks(QObject* document) const {
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    auto* textDocument = quickDocument->textDocument();
    auto* layout = textDocument->documentLayout();
    QVariantList frames;
    const qreal codeVerticalPadding = recipePixels(ket::Profile::current().codePadding);
    QRectF bounds;
    bool startsDocument = false;
    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        if (!isCodeBlock(block))
            continue;
        const auto rect = layout->blockBoundingRect(block);
        if (!sameCodeBlock(block.previous(), block))
            startsDocument = !block.previous().isValid();
        bounds = sameCodeBlock(block.previous(), block) ? bounds.united(rect) : rect;
        if (!sameCodeBlock(block, block.next())) {
            // blockBoundingRect starts at the paragraph's edge but is only as wide as the lines
            // inside its margins; the frame adds both so it spans the column like the mock's.
            const auto margins = block.blockFormat();
            const auto padded =
                bounds.adjusted(0, -codeVerticalPadding,
                                margins.leftMargin() + margins.rightMargin(), codeVerticalPadding);
            frames.append(QVariantMap{{"x", padded.x()},
                                      {"y", padded.y()},
                                      {"width", padded.width()},
                                      {"height", padded.height()},
                                      {"startsDocument", startsDocument},
                                      {"endsDocument", !block.next().isValid()}});
        }
    }
    return frames;
}

QVariantMap AgentWorkspaceView::mermaidLayout(QObject* document, const QString& messageKey) const {
    // `identity`: no fence was rewritten, so code frames and source fences already correspond
    // one to one. Otherwise `codeFences` names the fence behind each code frame, or is empty when
    // the two cannot be matched (an indented code block has a frame but no fence).
    QVariantMap layout{{QStringLiteral("blocks"), QVariantList{}},
                       {QStringLiteral("codeFences"), QVariantList{}},
                       {QStringLiteral("identity"), true}};
    const auto outcomes = fenceOutcomes_.constFind(messageKey);
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (outcomes == fenceOutcomes_.constEnd() || !quickDocument || !quickDocument->textDocument())
        return layout;
    layout.insert(QStringLiteral("identity"), false);
    auto* textDocument = quickDocument->textDocument();
    auto* documentLayout = textDocument->documentLayout();
    const auto frames = messageCodeBlocks(document);
    const qreal codePadding = recipePixels(ket::Profile::current().codePadding);
    const qreal codeVerticalPadding = codePadding;
    const qreal mermaidHeaderHeight = recipePixels(34);
    QList<QRectF> views;
    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            const QString name = fragment.charFormat().toImageFormat().name();
            const bool placeholder = name == QStringLiteral("qrc:/agent/mermaid-pending.svg") ||
                                     name == QStringLiteral("qrc:/agent/mermaid-placeholder.svg");
            if (!placeholder &&
                !mermaidImages_.contains(messageBaseUrl().resolved(QUrl(name)).toString()))
                continue;
            // The block's rect gives up the room that centers the diagram; the frame does not.
            const qreal centering =
                block.blockFormat().doubleProperty(mermaidInsetProperty) - codePadding;
            const auto rect = documentLayout->blockBoundingRect(block);
            views.append(
                rect.adjusted(0, -mermaidHeaderHeight, qMax(0.0, centering), codeVerticalPadding));
        }
    }
    QVariantList blocks;
    QVariantList codeFences;
    qsizetype frameIndex = 0;
    qsizetype viewIndex = 0;
    int fence = 0;
    const auto frameMap = [](const QRectF& rect, const int ordinal, const bool rendered) {
        return QVariantMap{
            {QStringLiteral("ordinal"), ordinal},    {QStringLiteral("rendered"), rendered},
            {QStringLiteral("x"), rect.x()},         {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()}, {QStringLiteral("height"), rect.height()}};
    };
    for (const auto& outcome : *outcomes) {
        if (outcome.image) {
            if (viewIndex >= views.size())
                return layout;
            const auto rect = views[viewIndex++];
            blocks.append(frameMap(rect, outcome.ordinal, true));
        } else {
            if (frameIndex >= frames.size())
                return layout;
            const auto frame = frames[frameIndex++].toMap();
            codeFences.append(fence);
            if (outcome.mermaid && outcome.complete)
                blocks.append(frameMap(QRectF(frame[QStringLiteral("x")].toReal(),
                                              frame[QStringLiteral("y")].toReal(),
                                              frame[QStringLiteral("width")].toReal(),
                                              frame[QStringLiteral("height")].toReal()),
                                       outcome.ordinal, false));
        }
        ++fence;
    }
    if (frameIndex != frames.size() || viewIndex != views.size())
        return layout;
    layout.insert(QStringLiteral("blocks"), blocks);
    layout.insert(QStringLiteral("codeFences"), codeFences);
    return layout;
}

QVariantList AgentWorkspaceView::messageInlineCode(QObject* document) const {
    auto* quickDocument = qobject_cast<QQuickTextDocument*>(document);
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    auto* textDocument = quickDocument->textDocument();
    auto* layout = textDocument->documentLayout();
    QVariantList chips;
    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        if (isCodeBlock(block) || !block.layout())
            continue;
        const QTextLayout* textLayout = block.layout();
        // Line positions are relative to the block, whose rect already carries
        // the offsets of any list or table frame around it.
        const QPointF origin = layout->blockBoundingRect(block).topLeft();
        const auto isCode = [](const QTextFragment& fragment) {
            return fragment.isValid() && fragment.charFormat().fontFixedPitch() &&
                   !fragment.charFormat().isImageFormat();
        };
        for (auto it = block.begin(); !it.atEnd();) {
            const auto fragment = it.fragment();
            ++it;
            if (!isCode(fragment))
                continue;
            // The padding splits a run's last character into its own fragment;
            // the run still gets one chip.
            const int start = fragment.position() - block.position();
            int end = start + fragment.length();
            for (; !it.atEnd() && isCode(it.fragment()); ++it)
                end = it.fragment().position() + it.fragment().length() - block.position();
            const QFontMetricsF metrics(fragment.charFormat().font());
            // A span that wraps gets one chip per line, as in a browser.
            for (int index = 0; index < textLayout->lineCount(); ++index) {
                const QTextLine line = textLayout->lineAt(index);
                const int from = qMax(start, line.textStart());
                const int to = qMin(end, line.textStart() + line.textLength());
                if (from >= to)
                    continue;
                const qreal left = line.cursorToX(from);
                // The run's last character is tracked by the padding already
                // (spaceAroundInlineCode); a line that wraps mid-run is not.
                const qreal right = line.cursorToX(to) + (to == end ? 0 : inlineCodePaddingX);
                // The glyphs sit on the line's baseline; the chip hugs the code
                // font rather than the roomy prose line height.
                const qreal top = line.y() + line.ascent() - metrics.ascent();
                chips.append(QVariantMap{
                    {"x", origin.x() + left - inlineCodePaddingX},
                    {"y", origin.y() + top - inlineCodePaddingY},
                    {"width", right - left + inlineCodePaddingX},
                    {"height", metrics.ascent() + metrics.descent() + 2 * inlineCodePaddingY}});
            }
        }
    }
    return chips;
}

QSize AgentWorkspaceView::messageImageSize(const QUrl& source) const {
    // Called synchronously from QML bindings with paths taken from agent/tool
    // output, so never open anything that could block: only regular local
    // files (after symlink resolution) of bounded size, or bundled resources.
    static constexpr qint64 maximumImageBytes = qint64(64) * 1024 * 1024;
    static constexpr qsizetype maximumCachedSizes = 4096;
    const QString key = source.toString();
    if (const auto cached = messageImageSizes_.constFind(key); cached != messageImageSizes_.cend())
        return *cached;
    const auto remember = [&](QSize size) {
        if (messageImageSizes_.size() >= maximumCachedSizes)
            messageImageSizes_.clear();
        // Failures are cached too (invalid QSize) so recycled delegates do not
        // reopen missing or rejected paths.
        messageImageSizes_.insert(key, size);
        return size;
    };
    QString path;
    if (source.isLocalFile()) {
        const QFileInfo info(source.toLocalFile());
        // isFile() follows symlinks; FIFOs, sockets and devices are rejected.
        if (!info.exists() || !info.isFile() || info.size() <= 0 || info.size() > maximumImageBytes)
            return remember({});
        const QString canonical = info.canonicalFilePath();
        const QFileInfo resolved(canonical);
        if (canonical.isEmpty() || !resolved.isFile() || resolved.size() > maximumImageBytes)
            return remember({});
        path = canonical;
    } else if (source.scheme() == QStringLiteral("qrc")) {
        path = QStringLiteral(":") + source.path();
    }
    if (path.isEmpty())
        return remember({});
    // Header-only read: no decode. Matches Image.autoTransform orientation.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (!size.isValid() || size.isEmpty())
        return remember({});
    if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        size.transpose();
    return remember(size);
}

void AgentWorkspaceView::removeAttachment(int index) {
    if (index >= 0 && index < attachments_.size()) {
        attachments_.removeAt(index);
        emit attachmentsChanged();
        saveAttachmentDraft(composerScope_);
    }
}

void AgentWorkspaceView::submitConfiguredPrompt(const QString& prompt, const QVariantMap& options) {
    if (sessionRunning_ || !agentAvailable_ ||
        (prompt.trimmed().isEmpty() && attachments_.isEmpty()))
        return;
    QVariantMap submittedOptions = options;
    QVariantList referencedPaths;
    if (!referencedPathsForPrompt(prompt, referencedPaths))
        return;
    if (!referencedPaths.isEmpty()) {
        submittedOptions.insert(QStringLiteral("files"), referencedPaths);
    }
    settleSkillModel();
    const QVariantList selectedSkills = skillsForPrompt(prompt);
    if (!selectedSkills.isEmpty()) {
        submittedOptions.insert(QStringLiteral("skills"), selectedSkills);
    }
    emit configuredPromptSubmitted(prompt.trimmed(), submittedOptions, attachments_);
    // Runtime validation failures keep the draft intact.
    if (sessionRunning_) {
        attachments_.clear();
        emit attachmentsChanged();
        saveAttachmentDraft(composerScope_);
    }
}

} // namespace ketplus
