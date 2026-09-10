#include "preview/MarkdownPreviewPane.h"

#include "preview/MermaidDiagramDialog.h"
#include "preview/MermaidRenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextTable>
#include <QTextTableCell>
#include <QTextTableFormat>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {

class PreviewBrowser final : public QTextBrowser {
  public:
    explicit PreviewBrowser(QWidget* parent = nullptr) : QTextBrowser(parent) {}

    void setImages(QHash<QUrl, QImage> images) { images_ = std::move(images); }
    void setDecorationPalette(const ThemePalette& palette) {
        palette_ = palette;
        viewport()->update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(event->rect());
        paintCodeBlocks(painter);
        paintInlineCode(painter);
        painter.end();
        QTextBrowser::paintEvent(event);
    }

    QVariant loadResource(const int type, const QUrl& name) override {
        if (type == QTextDocument::ImageResource) {
            if (const auto image = images_.constFind(name); image != images_.constEnd()) {
                return *image;
            }
        }
        return QTextBrowser::loadResource(type, name);
    }

  private:
    [[nodiscard]] static bool isCodeBlock(const QTextBlock& block) {
        const QTextBlockFormat format = block.blockFormat();
        return format.hasProperty(QTextFormat::BlockCodeLanguage) ||
               format.hasProperty(QTextFormat::BlockCodeFence);
    }

    [[nodiscard]] QPointF scrollOffset() const {
        return QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    }

    void paintCodeBlocks(QPainter& painter) const {
        const QColor fill(palette_.surfaceRaised.isEmpty() ? palette_.panelSubtle
                                                           : palette_.surfaceRaised);
        const QColor border(palette_.borderStrong.isEmpty() ? palette_.border
                                                            : palette_.borderStrong);
        if (!fill.isValid()) {
            return;
        }

        const qreal documentMargin = document()->documentMargin();
        const qreal cardWidth = document()->size().width() - (documentMargin * 2.0);
        const QPointF offset = scrollOffset();
        auto* layout = document()->documentLayout();
        for (QTextBlock block = document()->begin(); block.isValid();) {
            if (!isCodeBlock(block)) {
                block = block.next();
                continue;
            }

            const QTextBlock first = block;
            QTextBlock last = block;
            while (last.next().isValid() && isCodeBlock(last.next())) {
                last = last.next();
            }

            const QRectF firstRect = layout->blockBoundingRect(first);
            const QRectF lastRect = layout->blockBoundingRect(last);
            QRectF card(documentMargin, firstRect.top(), cardWidth,
                        lastRect.bottom() - firstRect.top());
            card.adjust(0.0, -8.0, 0.0, 8.0);
            card.translate(offset);
            if (card.intersects(viewport()->rect())) {
                painter.setPen(QPen(border, 1.0));
                painter.setBrush(fill);
                painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 7.0, 7.0);
            }
            block = last.next();
        }
    }

    void paintInlineCode(QPainter& painter) const {
        const QColor fill(palette_.accentSubtle.isEmpty() ? palette_.panelSubtle
                                                          : palette_.accentSubtle);
        const QColor border(palette_.accentMuted.isEmpty() ? palette_.border
                                                           : palette_.accentMuted);
        if (!fill.isValid()) {
            return;
        }

        const QPointF offset = scrollOffset();
        auto* documentLayout = document()->documentLayout();
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            if (isCodeBlock(block) || block.layout() == nullptr) {
                continue;
            }
            const QRectF blockRect = documentLayout->blockBoundingRect(block);
            QTextLayout* textLayout = block.layout();
            for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
                const QTextFragment fragment = iterator.fragment();
                const QTextCharFormat format = fragment.charFormat();
                if (!fragment.isValid() || format.isImageFormat() || !format.fontFixedPitch()) {
                    continue;
                }

                const int fragmentStart = fragment.position() - block.position();
                const int fragmentEnd = fragmentStart + fragment.length();
                for (int lineIndex = 0; lineIndex < textLayout->lineCount(); ++lineIndex) {
                    const QTextLine line = textLayout->lineAt(lineIndex);
                    const int start = qMax(fragmentStart, line.textStart());
                    const int end = qMin(fragmentEnd, line.textStart() + line.textLength());
                    if (start >= end) {
                        continue;
                    }

                    const qreal startX = line.cursorToX(start);
                    const qreal endX = line.cursorToX(end);
                    QRectF pill(blockRect.left() + qMin(startX, endX) - 4.0,
                                blockRect.top() + line.y() + 1.0,
                                qAbs(endX - startX) + 8.0, line.height() - 2.0);
                    pill.translate(offset);
                    painter.setPen(QPen(border, 1.0));
                    painter.setBrush(fill);
                    painter.drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 4.0,
                                            4.0);
                }
            }
        }
    }

    QHash<QUrl, QImage> images_;
    ThemePalette palette_;
};

namespace {

constexpr int previewDebounceMilliseconds = 150;
constexpr qreal previewDocumentPadding = 28.0;
constexpr qreal previewDocumentMaxWidth = 760.0;
constexpr qreal previewBodyLineHeight = 150.0;
constexpr qreal previewHeadingLineHeight = 120.0;

bool isClosingFence(const QString& line, const QString& openingFence) {
    const QString value = line.trimmed();
    if (value.size() < openingFence.size()) {
        return false;
    }
    for (const QChar character : value) {
        if (character != openingFence.front()) {
            return false;
        }
    }
    return true;
}

QUrl diagramUrl(const QString& cacheKey) {
    return QUrl(QStringLiteral("ketplus-mermaid://diagram/%1").arg(cacheKey));
}

QUrl diagramOpenUrl(const QString& cacheKey) {
    return QUrl(QStringLiteral("ketplus-mermaid-open://diagram/%1").arg(cacheKey));
}

} // namespace

MarkdownPreviewPane::MarkdownPreviewPane(QWidget* parent)
    : QWidget(parent), browser_(new PreviewBrowser(this)), notice_(new QLabel(this)),
      renderTimer_(new QTimer(this)), mermaidRenderer_(new MermaidRenderer(this)) {
    setObjectName(QStringLiteral("markdownPreview"));
    setProperty("kvRole", QStringLiteral("markdownPreview"));
    setMinimumWidth(280);

    auto* header = new QWidget(this);
    header->setProperty("kvRole", QStringLiteral("previewHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 0, 6, 0);
    headerLayout->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("PREVIEW"), header);
    title->setProperty("kvRole", QStringLiteral("previewHeading"));
    auto* closeButton = new QToolButton(header);
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setToolTip(QStringLiteral("Close Markdown preview"));
    closeButton->setAccessibleName(QStringLiteral("Close Markdown preview"));
    closeButton->setProperty("kvRole", QStringLiteral("previewClose"));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    headerLayout->addWidget(closeButton);

    notice_->setObjectName(QStringLiteral("markdownPreviewNotice"));
    notice_->setProperty("kvRole", QStringLiteral("previewNotice"));
    notice_->setWordWrap(true);
    notice_->hide();

    browser_->setObjectName(QStringLiteral("markdownPreviewBrowser"));
    browser_->setFrameShape(QFrame::NoFrame);
    browser_->setOpenLinks(false);
    browser_->setOpenExternalLinks(false);
    browser_->setReadOnly(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(notice_);
    layout->addWidget(browser_, 1);

    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(previewDebounceMilliseconds);
    connect(renderTimer_, &QTimer::timeout, this, &MarkdownPreviewPane::renderNow);
    connect(closeButton, &QToolButton::clicked, this, &MarkdownPreviewPane::closeRequested);
    connect(mermaidRenderer_, &MermaidRenderer::diagramReady, this,
            [this] { renderTimer_->start(0); });
    connect(mermaidRenderer_, &MermaidRenderer::diagramFailed, this,
            [this] { renderTimer_->start(0); });
    connect(browser_, &QTextBrowser::anchorClicked, this, [this](QUrl url) {
        if (url.path().isEmpty() && url.hasFragment()) {
            browser_->scrollToAnchor(url.fragment());
            return;
        }
        if (url.scheme() == QStringLiteral("ketplus-mermaid-open")) {
            const QString cacheKey = url.path().section(QLatin1Char('/'), -1);
            const QString svgPath = diagramFiles_.value(cacheKey);
            if (!svgPath.isEmpty()) {
                auto* dialog = new MermaidDiagramDialog(svgPath, palette_, this);
                dialog->show();
            }
            return;
        }
        if (url.isRelative() && !filePath_.isEmpty()) {
            url = QUrl::fromLocalFile(QFileInfo(filePath_).absoluteDir().absoluteFilePath(
                url.toString()));
        }
        if (url.isLocalFile()) {
            emit fileOpenRequested(url.toLocalFile());
        } else if (url.scheme() == QStringLiteral("http") ||
                   url.scheme() == QStringLiteral("https") ||
                   url.scheme() == QStringLiteral("mailto")) {
            QDesktopServices::openUrl(url);
        }
    });
}

void MarkdownPreviewPane::setSource(const QString& markdown, const QString& filePath,
                                    const ThemePalette& palette) {
    markdown_ = markdown;
    filePath_ = filePath;
    palette_ = palette;
    hasSource_ = true;
    renderTimer_->start();
}

void MarkdownPreviewPane::showEmpty(const ThemePalette& palette) {
    renderTimer_->stop();
    palette_ = palette;
    hasSource_ = false;
    markdown_.clear();
    filePath_.clear();
    browser_->setImages({});
    applyDocumentStyle();
    browser_->setHtml(QStringLiteral("<p>Open a Markdown file to preview it.</p>"));
    applyBlockTypography();
    setNotice({});
}

void MarkdownPreviewPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { updateDocumentMargin(); });
}

void MarkdownPreviewPane::renderNow() {
    if (!hasSource_) {
        return;
    }

    const int oldMaximum = browser_->verticalScrollBar()->maximum();
    const int oldValue = browser_->verticalScrollBar()->value();
    const double scrollRatio = oldMaximum > 0 ? static_cast<double>(oldValue) / oldMaximum : 0.0;

    PreparedDocument prepared = prepareDocument();
    browser_->setImages(std::move(prepared.images));
    diagramFiles_ = std::move(prepared.diagramFiles);
    browser_->document()->setBaseUrl(filePath_.isEmpty()
                                         ? QUrl()
                                         : QUrl::fromLocalFile(
                                               QFileInfo(filePath_).absolutePath() + QLatin1Char('/')));
    applyDocumentStyle();
    auto markdownFeatures =
        QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub);
    markdownFeatures.setFlag(QTextDocument::MarkdownNoHTML);
    browser_->document()->setMarkdown(prepared.markdown, markdownFeatures);
    applyBlockTypography();
    linkMermaidImages();

    if (prepared.rendererUnavailable) {
        setNotice(QStringLiteral("Mermaid preview needs mmdc. Install it with: "
                                 "npm install -g @mermaid-js/mermaid-cli"));
    } else if (!prepared.rendererError.isEmpty()) {
        setNotice(QStringLiteral("Mermaid preview failed: %1").arg(prepared.rendererError), true);
    } else {
        setNotice({});
    }

    QTimer::singleShot(0, this, [this, scrollRatio] {
        auto* scrollBar = browser_->verticalScrollBar();
        scrollBar->setValue(qRound(scrollRatio * scrollBar->maximum()));
    });
}

MarkdownPreviewPane::PreparedDocument MarkdownPreviewPane::prepareDocument() {
    PreparedDocument prepared;
    const QStringList lines = markdown_.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    static const QRegularExpression openingExpression(
        QStringLiteral(R"(^\s*(`{3,}|~{3,})\s*mermaid(?:\s+.*)?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);

    QStringList output;
    for (qsizetype lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto match = openingExpression.match(lines[lineIndex]);
        if (!match.hasMatch()) {
            output.append(lines[lineIndex]);
            continue;
        }

        const QString openingFence = match.captured(1);
        qsizetype closingLine = lineIndex + 1;
        while (closingLine < lines.size() &&
               !isClosingFence(lines[closingLine], openingFence)) {
            ++closingLine;
        }
        if (closingLine >= lines.size()) {
            output.append(lines.mid(lineIndex));
            break;
        }

        const QString source =
            lines.mid(lineIndex + 1, closingLine - lineIndex - 1).join(QLatin1Char('\n'));
        const MermaidRenderer::Result result = mermaidRenderer_->request(source, palette_.dark);
        if (result.state == MermaidRenderer::State::Ready) {
            const QUrl url = diagramUrl(result.cacheKey);
            const QImage image = renderSvg(result.filePath);
            if (!image.isNull()) {
                prepared.images.insert(url, image);
                prepared.diagramFiles.insert(result.cacheKey, result.filePath);
                output.append(QStringLiteral("[![Mermaid diagram](<%1>)](<%2>)")
                                  .arg(url.toString(),
                                       diagramOpenUrl(result.cacheKey).toString()));
            } else {
                prepared.rendererError = QStringLiteral("mmdc produced an unreadable SVG");
                output.append(lines.mid(lineIndex, closingLine - lineIndex + 1));
            }
        } else if (result.state == MermaidRenderer::State::Pending) {
            output.append(QStringLiteral("> Rendering Mermaid diagram…"));
        } else {
            output.append(lines.mid(lineIndex, closingLine - lineIndex + 1));
            prepared.rendererUnavailable |= result.state == MermaidRenderer::State::Unavailable;
            if (result.state == MermaidRenderer::State::Failed &&
                prepared.rendererError.isEmpty()) {
                prepared.rendererError = result.error;
            }
        }
        lineIndex = closingLine;
    }
    prepared.markdown = output.join(QLatin1Char('\n'));
    return prepared;
}

QImage MarkdownPreviewPane::renderSvg(const QString& filePath) const {
    QSvgRenderer renderer(filePath);
    if (!renderer.isValid()) {
        return {};
    }

    QSize logicalSize = renderer.defaultSize();
    if (logicalSize.isEmpty()) {
        logicalSize = QSize(640, 360);
    }
    const int availableWidth = qMax(240, browser_->viewport()->width() - 48);
    if (logicalSize.width() > availableWidth) {
        logicalSize.scale(availableWidth, 1600, Qt::KeepAspectRatio);
    }
    const qreal pixelRatio = qMin(devicePixelRatioF(), 2.0);
    const QSize pixelSize(qMax(1, qRound(logicalSize.width() * pixelRatio)),
                          qMax(1, qRound(logicalSize.height() * pixelRatio)));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(pixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(logicalSize)));
    return image;
}

void MarkdownPreviewPane::applyDocumentStyle() {
    updateDocumentMargin();
    browser_->setDecorationPalette(palette_);

    QFont bodyFont = browser_->font();
    bodyFont.setPixelSize(14);
    browser_->document()->setDefaultFont(bodyFont);

    const QString codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    browser_->document()->setDefaultStyleSheet(QStringLiteral(R"(
        body {
            color: %1;
            background-color: %2;
            font-family: "%9";
            font-size: 14px;
            line-height: 1.55;
            margin: 0;
        }
        p { margin: 0 0 12px; }
        h1, h2, h3, h4, h5, h6 {
            color: %1;
            font-weight: 650;
            margin: 0;
        }
        h1 {
            font-size: 26px;
            border-bottom: 1px solid %3;
            padding-bottom: 9px;
        }
        h2 {
            font-size: 20px;
            border-bottom: 1px solid %3;
            padding-bottom: 7px;
        }
        h3 { font-size: 16px; }
        h4 { font-size: 14px; }
        h5, h6 { color: %7; font-size: 13px; }
        a { color: %4; text-decoration: none; }
        strong { font-weight: 650; }
        pre {
            color: %1;
            background-color: %5;
            border: 1px solid %3;
            font-family: "%8";
            font-size: 13px;
            line-height: 1.5;
            margin: 8px 0 18px;
            padding: 14px 16px;
            white-space: pre-wrap;
        }
        code {
            color: %4;
            background-color: %5;
            font-family: "%8";
            font-size: 13px;
            padding: 1px 4px;
        }
        pre code { color: %1; background-color: transparent; padding: 0; }
        ul, ol { margin: 2px 0 14px; padding-left: 26px; }
        li { margin: 3px 0; }
        blockquote {
            color: %7;
            background-color: %5;
            border-left: 3px solid %6;
            margin: 8px 0 18px;
            padding: 10px 14px;
        }
        table { border-collapse: collapse; margin: 8px 0 18px; }
        th, td { border: 1px solid %3; padding: 8px 11px; }
        th { color: %1; background-color: %5; font-weight: 650; }
        hr { color: %3; margin: 24px 0; }
        img { margin: 8px 0 16px; }
    )")
                                                   .arg(palette_.textMain,
                                                        palette_.panelBackground,
                                                        palette_.border,
                                                        palette_.accent,
                                                        palette_.panelSubtle,
                                                        palette_.accentMuted,
                                                        palette_.textSecondary,
                                                        codeFont,
                                                        bodyFont.family()));
    browser_->setStyleSheet(QStringLiteral("QTextBrowser { background: %1; color: %2; border: 0; }")
                                .arg(palette_.panelBackground, palette_.textMain));
}

void MarkdownPreviewPane::updateDocumentMargin() {
    const qreal viewportWidth = browser_->viewport()->width();
    const qreal horizontalMargin =
        qMax(previewDocumentPadding, (viewportWidth - previewDocumentMaxWidth) / 2.0);
    browser_->document()->setDocumentMargin(horizontalMargin);
}

void MarkdownPreviewPane::applyBlockTypography() {
    const QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QList<QTextTable*> tables;

    for (QTextBlock block = browser_->document()->begin(); block.isValid(); block = block.next()) {
        bool containsImage = false;
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            if (iterator.fragment().charFormat().isImageFormat()) {
                containsImage = true;
                break;
            }
        }

        QTextBlockFormat format = block.blockFormat();
        const int headingLevel = format.headingLevel();
        const bool isCodeBlock = format.hasProperty(QTextFormat::BlockCodeLanguage) ||
                                 format.hasProperty(QTextFormat::BlockCodeFence);
        const bool isQuote = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        const bool isListItem = block.textList() != nullptr;
        QTextCursor blockCursor(block);
        QTextTable* table = blockCursor.currentTable();
        if (table != nullptr && !tables.contains(table)) {
            tables.append(table);
        }

        if (containsImage) {
            format.setLineHeight(24.0, QTextBlockFormat::MinimumHeight);
        } else if (isCodeBlock) {
            format.setLineHeight(145.0, QTextBlockFormat::ProportionalHeight);
        } else {
            format.setLineHeight(headingLevel > 0 ? previewHeadingLineHeight
                                                  : previewBodyLineHeight,
                                 QTextBlockFormat::ProportionalHeight);
        }

        if (headingLevel > 0) {
            static constexpr qreal headingTopMargins[] = {0.0, 28.0, 26.0, 22.0,
                                                          18.0, 18.0, 18.0};
            static constexpr qreal headingBottomMargins[] = {0.0, 14.0, 12.0, 9.0,
                                                             8.0, 8.0, 8.0};
            const int marginIndex = qBound(1, headingLevel, 6);
            format.setTopMargin(block.position() == 0 ? 0.0
                                                      : headingTopMargins[marginIndex]);
            format.setBottomMargin(headingBottomMargins[marginIndex]);
        } else if (table != nullptr) {
            format.setTopMargin(0.0);
            format.setBottomMargin(0.0);
            format.setLeftMargin(0.0);
            format.setRightMargin(0.0);
        } else if (isCodeBlock) {
            const QTextBlock previous = block.previous();
            const QTextBlock next = block.next();
            const bool previousIsCode =
                previous.isValid() &&
                (previous.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage) ||
                 previous.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
            const bool nextIsCode =
                next.isValid() &&
                (next.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage) ||
                 next.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
            format.setTopMargin(previousIsCode ? 0.0 : 20.0);
            format.setBottomMargin(nextIsCode ? 0.0 : 20.0);
            format.setLeftMargin(18.0);
            format.setRightMargin(18.0);
            format.clearBackground();
        } else if (isQuote) {
            format.setTopMargin(8.0);
            format.setBottomMargin(16.0);
            format.setLeftMargin(18.0);
            format.setRightMargin(12.0);
            format.setBackground(QColor(palette_.panelSubtle));
        } else if (isListItem) {
            format.setTopMargin(2.0);
            format.setBottomMargin(3.0);
        } else if (block.text().isEmpty()) {
            format.setTopMargin(0.0);
            format.setBottomMargin(0.0);
            format.setLineHeight(60.0, QTextBlockFormat::ProportionalHeight);
        } else {
            format.setTopMargin(0.0);
            format.setBottomMargin(14.0);
        }

        blockCursor.setBlockFormat(format);

        if (headingLevel > 0) {
            static constexpr int headingSizes[] = {14, 26, 20, 16, 14, 13, 13};
            QFont headingFont = browser_->document()->defaultFont();
            headingFont.setPixelSize(headingSizes[qBound(1, headingLevel, 6)]);
            headingFont.setWeight(QFont::DemiBold);
            QTextCharFormat headingFormat;
            headingFormat.setFont(headingFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
            headingFormat.setForeground(QColor(palette_.textMain));
            blockCursor.select(QTextCursor::BlockUnderCursor);
            blockCursor.mergeCharFormat(headingFormat);
        }

        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || fragment.charFormat().isImageFormat()) {
                continue;
            }

            QTextCharFormat fragmentFormat;
            const QTextCharFormat sourceFormat = fragment.charFormat();
            const bool isCode = isCodeBlock || sourceFormat.fontFixedPitch() ||
                                sourceFormat.font().family() == QStringLiteral("monospace");
            if (isCode) {
                QFont styledCodeFont = codeFont;
                styledCodeFont.setPixelSize(13);
                fragmentFormat.setFont(styledCodeFont,
                                       QTextCharFormat::FontPropertiesSpecifiedOnly);
                fragmentFormat.setForeground(
                    QColor(isCodeBlock ? palette_.textMain : palette_.accent));
                fragmentFormat.clearBackground();
            } else if (sourceFormat.isAnchor()) {
                fragmentFormat.setForeground(QColor(palette_.accent));
                fragmentFormat.setFontUnderline(false);
            } else {
                fragmentFormat.setForeground(
                    QColor(isQuote ? palette_.textSecondary : palette_.textMain));
            }

            QTextCursor fragmentCursor(browser_->document());
            fragmentCursor.setPosition(fragment.position());
            fragmentCursor.setPosition(fragment.position() + fragment.length(),
                                       QTextCursor::KeepAnchor);
            fragmentCursor.mergeCharFormat(fragmentFormat);
        }
    }

    for (QTextTable* table : std::as_const(tables)) {
        QTextTableFormat tableFormat = table->format();
        tableFormat.setBorder(1.0);
        tableFormat.setBorderBrush(QColor(palette_.border));
        tableFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        tableFormat.setCellPadding(9.0);
        tableFormat.setCellSpacing(0.0);
        tableFormat.setTopMargin(8.0);
        tableFormat.setBottomMargin(18.0);
        tableFormat.setWidth(QTextLength(QTextLength::PercentageLength, 100.0));
        table->setFormat(tableFormat);

        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextCharFormat cellFormat = cell.format();
                cellFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                cellFormat.setBackground(QColor(row == 0 ? palette_.panelSubtle
                                                         : palette_.panelBackground));
                cell.setFormat(cellFormat);

                QTextCursor cellCursor = cell.firstCursorPosition();
                cellCursor.setPosition(cell.lastCursorPosition().position(),
                                       QTextCursor::KeepAnchor);
                QTextCharFormat textFormat;
                textFormat.setForeground(QColor(palette_.textMain));
                if (row == 0) {
                    textFormat.setFontWeight(QFont::DemiBold);
                }
                cellCursor.mergeCharFormat(textFormat);
            }
        }
    }
}

void MarkdownPreviewPane::linkMermaidImages() {
    struct ImageRange final {
        int position;
        int length;
        QTextCharFormat format;
    };
    QList<ImageRange> images;
    for (QTextBlock block = browser_->document()->begin(); block.isValid(); block = block.next()) {
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            QTextCharFormat format = fragment.charFormat();
            if (!format.isImageFormat()) {
                continue;
            }
            const QUrl imageUrl(format.toImageFormat().name());
            if (imageUrl.scheme() != QStringLiteral("ketplus-mermaid")) {
                continue;
            }
            const QString cacheKey = imageUrl.path().section(QLatin1Char('/'), -1);
            format.setAnchor(true);
            format.setAnchorHref(diagramOpenUrl(cacheKey).toString());
            images.append({fragment.position(), fragment.length(), format});
        }
    }

    for (const auto& image : images) {
        QTextCursor cursor(browser_->document());
        cursor.setPosition(image.position);
        cursor.setPosition(image.position + image.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(image.format);
    }
}

void MarkdownPreviewPane::setNotice(const QString& message, const bool error) {
    notice_->setVisible(!message.isEmpty());
    notice_->setText(message);
    notice_->setProperty("noticeState", error ? QStringLiteral("error")
                                               : QStringLiteral("warning"));
    notice_->style()->unpolish(notice_);
    notice_->style()->polish(notice_);
    if (!message.isEmpty() && message != lastStatusMessage_) {
        emit statusMessageRequested(message);
    }
    lastStatusMessage_ = message;
}

} // namespace ketplus
