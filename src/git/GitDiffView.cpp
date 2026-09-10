#include "git/GitDiffView.h"

#include "editor/EditorWidget.h"
#include "ui/Theme.h"

#include <ScintillaMessages.h>
#include <ScintillaStructures.h>

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QRegularExpression>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {
namespace {

enum class DiffLineKind {
    Context,
    Addition,
    Deletion,
    Hunk,
    Meta,
};

struct DiffLine final {
    DiffLineKind kind{DiffLineKind::Context};
    int oldLine{0};
    int newLine{0};
    QString marker;
    QString text;
    QVector<QTextLayout::FormatRange> syntaxFormats;
};

constexpr int syntaxColorsRole = Qt::UserRole + 1;

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

QColor qtColor(const sptr_t value) {
    return QColor(static_cast<int>(value & 0xff), static_cast<int>((value >> 8) & 0xff),
                  static_cast<int>((value >> 16) & 0xff));
}

QColor blended(const QString& baseValue, const QString& accentValue, const qreal amount) {
    const QColor base(baseValue);
    const QColor accent(accentValue);
    return QColor::fromRgbF(base.redF() * (1.0 - amount) + accent.redF() * amount,
                            base.greenF() * (1.0 - amount) + accent.greenF() * amount,
                            base.blueF() * (1.0 - amount) + accent.blueF() * amount);
}

QString scopeText(const GitDiffMode mode) {
    switch (mode) {
    case GitDiffMode::Staged:
        return QStringLiteral("STAGED");
    case GitDiffMode::Unstaged:
        return QStringLiteral("WORKING TREE");
    case GitDiffMode::Combined:
        return QStringLiteral("ALL CHANGES");
    }
    return {};
}

QString scopeProperty(const GitDiffMode mode) {
    switch (mode) {
    case GitDiffMode::Staged:
        return QStringLiteral("staged");
    case GitDiffMode::Unstaged:
        return QStringLiteral("unstaged");
    case GitDiffMode::Combined:
        return QStringLiteral("combined");
    }
    return {};
}

} // namespace

class GitDiffModel final : public QAbstractTableModel {
  public:
    explicit GitDiffModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : lines_.size();
    }

    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }

    QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= lines_.size()) {
            return {};
        }

        const auto& line = lines_.at(index.row());
        if (role == Qt::FontRole) {
            return codeFont_;
        }
        if (role == Qt::DisplayRole) {
            if (line.kind == DiffLineKind::Hunk || line.kind == DiffLineKind::Meta) {
                return index.column() == 0 ? line.text : QString();
            }
            switch (index.column()) {
            case 0:
                return line.oldLine > 0 ? QString::number(line.oldLine) : QString();
            case 1:
                return line.newLine > 0 ? QString::number(line.newLine) : QString();
            case 2:
                return line.marker;
            case 3:
                return line.text;
            default:
                return {};
            }
        }

        if (role == Qt::TextAlignmentRole) {
            if (index.column() < 2) {
                return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            }
            if (index.column() == 2) {
                return QVariant::fromValue(Qt::AlignCenter);
            }
        }

        if (role == Qt::ForegroundRole) {
            if (line.kind == DiffLineKind::Hunk) {
                return QColor(palette_.accentActive);
            }
            if (line.kind == DiffLineKind::Meta || index.column() < 3) {
                return QColor(palette_.textMuted);
            }
            return QColor(palette_.textMain);
        }

        if (role == Qt::BackgroundRole) {
            const qreal tint = palette_.dark ? 0.15 : 0.12;
            const qreal gutterTint = palette_.dark ? 0.24 : 0.18;
            if (line.kind == DiffLineKind::Addition) {
                return blended(palette_.panelBackground, palette_.positive,
                               index.column() < 3 ? gutterTint : tint);
            }
            if (line.kind == DiffLineKind::Deletion) {
                return blended(palette_.panelBackground, palette_.danger,
                               index.column() < 3 ? gutterTint : tint);
            }
            if (line.kind == DiffLineKind::Hunk) {
                return QColor(palette_.accentSubtle);
            }
            if (line.kind == DiffLineKind::Meta) {
                return QColor(palette_.panelSubtle);
            }
            if (index.column() < 2) {
                return QColor(palette_.panelSubtle);
            }
            return QColor(palette_.panelBackground);
        }

        if (role == Qt::ToolTipRole && index.column() >= 2) {
            return line.text;
        }
        if (role == syntaxColorsRole && index.column() == 3) {
            QVariantList colors;
            for (const auto& range : line.syntaxFormats) {
                colors.append(range.format.foreground().color());
            }
            return colors;
        }
        return {};
    }

    void setDiff(const QString& diff) {
        beginResetModel();
        lines_.clear();
        additions_ = 0;
        deletions_ = 0;
        rawDiff_ = diff;

        QVector<DiffLine> parsedLines;
        static const QRegularExpression hunkExpression(
            QStringLiteral(R"(^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@)"));
        int oldLine = 0;
        int newLine = 0;
        const QStringList sourceLines = diff.split(QLatin1Char('\n'));
        for (qsizetype index = 0; index < sourceLines.size(); ++index) {
            const QString& source = sourceLines.at(index);
            if (index == sourceLines.size() - 1 && source.isEmpty()) {
                continue;
            }

            const auto match = hunkExpression.match(source);
            if (match.hasMatch()) {
                oldLine = match.captured(1).toInt();
                newLine = match.captured(2).toInt();
                parsedLines.append({.kind = DiffLineKind::Hunk, .text = source});
            } else if (source.startsWith(QStringLiteral("diff --git ")) ||
                       source.startsWith(QStringLiteral("index ")) ||
                       source.startsWith(QStringLiteral("--- ")) ||
                       source.startsWith(QStringLiteral("+++ "))) {
                continue;
            } else if (source.startsWith(QLatin1Char('+'))) {
                parsedLines.append({.kind = DiffLineKind::Addition,
                                    .newLine = newLine++,
                                    .marker = QStringLiteral("+"),
                                    .text = source.mid(1)});
                ++additions_;
            } else if (source.startsWith(QLatin1Char('-'))) {
                parsedLines.append({.kind = DiffLineKind::Deletion,
                                    .oldLine = oldLine++,
                                    .marker = QString::fromUtf8("−"),
                                    .text = source.mid(1)});
                ++deletions_;
            } else if (source.startsWith(QLatin1Char(' '))) {
                parsedLines.append({.kind = DiffLineKind::Context,
                                    .oldLine = oldLine++,
                                    .newLine = newLine++,
                                    .text = source.mid(1)});
            } else {
                parsedLines.append({.kind = DiffLineKind::Meta, .text = source});
            }
        }
        lines_ = hideUnchangedRows_ ? compactContext(parsedLines) : parsedLines;
        endResetModel();
    }

    void setHideUnchangedRows(const bool hide) {
        hideUnchangedRows_ = hide;
        if (!rawDiff_.isEmpty()) {
            setDiff(rawDiff_);
        }
    }

    void applySyntax(const QString& filePath, EditorWidget& syntaxEngine,
                     const ThemePalette& palette) {
        struct SourceLocation final {
            qsizetype byteOffset{-1};
            bool fromNewText{false};
        };

        QVector<SourceLocation> locations(lines_.size());
        QByteArray oldText;
        QByteArray newText;
        for (qsizetype row = 0; row < lines_.size(); ++row) {
            auto& line = lines_[row];
            line.syntaxFormats.clear();
            const QByteArray content = line.text.toUtf8();
            if (line.kind == DiffLineKind::Context || line.kind == DiffLineKind::Deletion) {
                locations[row].byteOffset = oldText.size();
                oldText.append(content).append('\n');
            }
            if (line.kind == DiffLineKind::Context || line.kind == DiffLineKind::Addition) {
                locations[row] = {.byteOffset = newText.size(), .fromNewText = true};
                newText.append(content).append('\n');
            }
        }

        const auto applyDocument = [&](const QByteArray& source, const bool newTextSource) {
            syntaxEngine.setText(source);
            syntaxEngine.configureLexerForPath(filePath);
            syntaxEngine.applyTheme(palette);

            QByteArray styledBytes(source.size() * 2 + 2, '\0');
            Scintilla::TextRangeFull styledRange{
                .chrg = {.cpMin = 0, .cpMax = source.size()},
                .lpstrText = styledBytes.data(),
            };
            syntaxEngine.send(message(Scintilla::Message::GetStyledTextFull), 0,
                              reinterpret_cast<sptr_t>(&styledRange));
            QHash<int, QTextCharFormat> formatsByStyle;

            for (qsizetype row = 0; row < lines_.size(); ++row) {
                auto& line = lines_[row];
                const auto& location = locations.at(row);
                if (location.byteOffset < 0 || location.fromNewText != newTextSource ||
                    line.text.isEmpty()) {
                    continue;
                }

                int utf16Start = 0;
                qsizetype bytePosition = location.byteOffset;
                while (utf16Start < line.text.size()) {
                    const int unitCount = line.text.at(utf16Start).isHighSurrogate() &&
                                                  utf16Start + 1 < line.text.size() &&
                                                  line.text.at(utf16Start + 1).isLowSurrogate()
                                              ? 2
                                              : 1;
                    const int style = static_cast<unsigned char>(
                        styledBytes.at(bytePosition * 2 + 1));
                    const QByteArray encoded = line.text.mid(utf16Start, unitCount).toUtf8();

                    auto cachedFormat = formatsByStyle.constFind(style);
                    if (cachedFormat == formatsByStyle.cend()) {
                        QTextCharFormat format;
                        format.setForeground(qtColor(syntaxEngine.send(
                            message(Scintilla::Message::StyleGetFore),
                            static_cast<uptr_t>(style))));
                        format.setFontWeight(syntaxEngine.send(
                                                 message(Scintilla::Message::StyleGetBold),
                                                 static_cast<uptr_t>(style))
                                                 ? QFont::Bold
                                                 : QFont::Normal);
                        format.setFontItalic(syntaxEngine.send(
                                                  message(Scintilla::Message::StyleGetItalic),
                                                  static_cast<uptr_t>(style)) != 0);
                        cachedFormat = formatsByStyle.insert(style, format);
                    }
                    const QTextCharFormat& format = cachedFormat.value();

                    if (!line.syntaxFormats.isEmpty() &&
                        line.syntaxFormats.last().start + line.syntaxFormats.last().length ==
                            utf16Start &&
                        line.syntaxFormats.last().format == format) {
                        line.syntaxFormats.last().length += unitCount;
                    } else {
                        line.syntaxFormats.append(
                            {.start = utf16Start, .length = unitCount, .format = format});
                    }
                    utf16Start += unitCount;
                    bytePosition += encoded.size();
                }
            }
        };

        applyDocument(oldText, false);
        applyDocument(newText, true);
        if (!lines_.isEmpty()) {
            emit dataChanged(index(0, 3), index(lines_.size() - 1, 3),
                             {Qt::ForegroundRole, syntaxColorsRole});
        }
    }

    void applyTheme(const ThemePalette& palette) {
        palette_ = palette;
        if (!lines_.isEmpty()) {
            emit dataChanged(index(0, 0), index(lines_.size() - 1, 3),
                             {Qt::ForegroundRole, Qt::BackgroundRole});
        }
    }

    void setCodeFont(const QFont& font) {
        codeFont_ = font;
        if (!lines_.isEmpty()) {
            emit dataChanged(index(0, 0), index(lines_.size() - 1, 3), {Qt::FontRole});
        }
    }

    [[nodiscard]] bool spansColumns(const int row) const {
        if (row < 0 || row >= lines_.size()) {
            return false;
        }
        const auto kind = lines_.at(row).kind;
        return kind == DiffLineKind::Hunk || kind == DiffLineKind::Meta;
    }

    [[nodiscard]] int additions() const noexcept { return additions_; }
    [[nodiscard]] int deletions() const noexcept { return deletions_; }

    [[nodiscard]] const QVector<QTextLayout::FormatRange>& syntaxFormats(const int row) const {
        static const QVector<QTextLayout::FormatRange> empty;
        return row >= 0 && row < lines_.size() ? lines_.at(row).syntaxFormats : empty;
    }

  private:
    [[nodiscard]] QVector<DiffLine> compactContext(const QVector<DiffLine>& source) const {
        QVector<DiffLine> compacted;
        compacted.reserve(source.size());
        constexpr int keepContext = 2;
        for (int index = 0; index < source.size();) {
            if (source.at(index).kind != DiffLineKind::Context) {
                compacted.append(source.at(index++));
                continue;
            }

            const int start = index;
            while (index < source.size() && source.at(index).kind == DiffLineKind::Context) {
                ++index;
            }
            const int count = index - start;
            if (count <= keepContext * 2 + 2) {
                for (int row = start; row < index; ++row) {
                    compacted.append(source.at(row));
                }
                continue;
            }
            for (int row = start; row < start + keepContext; ++row) {
                compacted.append(source.at(row));
            }
            compacted.append({.kind = DiffLineKind::Meta,
                              .text = QStringLiteral("... %1 unchanged lines hidden ...")
                                          .arg(count - keepContext * 2)});
            for (int row = index - keepContext; row < index; ++row) {
                compacted.append(source.at(row));
            }
        }
        return compacted;
    }

    QVector<DiffLine> lines_;
    QString rawDiff_;
    ThemePalette palette_;
    QFont codeFont_;
    int additions_{0};
    int deletions_{0};
    bool hideUnchangedRows_{true};
};

class DiffTextDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto* model = dynamic_cast<const GitDiffModel*>(index.model());
        if (model == nullptr) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const QString text = index.data(Qt::DisplayRole).toString();
        const QFont codeFont = index.data(Qt::FontRole).value<QFont>();
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing, true);
        painter->fillRect(option.rect, index.data(Qt::BackgroundRole).value<QColor>());
        if (option.state.testFlag(QStyle::State_Selected)) {
            QColor selectionOverlay = option.palette.highlight().color();
            selectionOverlay.setAlpha(28);
            painter->fillRect(option.rect, selectionOverlay);
        }

        const QRect textRect = option.rect.adjusted(6, 0, -6, 0);
        painter->setClipRect(textRect);
        const auto& syntaxFormats = model->syntaxFormats(index.row());
        if (index.column() == 3 && !syntaxFormats.isEmpty()) {
            QTextLayout layout(text, codeFont);
            layout.setFormats(syntaxFormats);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            line.setLineWidth(1.0e7);
            layout.endLayout();
            const qreal top = textRect.top() + (textRect.height() - line.height()) / 2.0;
            layout.draw(painter, QPointF(textRect.left(), top));
        } else {
            painter->setFont(codeFont);
            painter->setPen(index.data(Qt::ForegroundRole).value<QColor>());
            const auto alignment = index.data(Qt::TextAlignmentRole).value<Qt::Alignment>();
            painter->drawText(textRect,
                              (alignment == Qt::Alignment() ? Qt::AlignLeft | Qt::AlignVCenter
                                                            : alignment) |
                                  Qt::TextSingleLine,
                              text);
        }
        painter->restore();
    }
};

GitDiffView::GitDiffView(QWidget* parent)
    : QWidget(parent), model_(new GitDiffModel(this)), table_(new QTableView(this)),
      syntaxEngine_(new EditorWidget(this)),
      fileLabel_(new QLabel(this)), pathLabel_(new QLabel(this)),
      scopeLabel_(new QLabel(this)), statsLabel_(new QLabel(this)),
      openButton_(new QToolButton(this)), contextButton_(new QToolButton(this)) {
    setProperty("kvRole", QStringLiteral("gitDiff"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* titleLayout = new QHBoxLayout;
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);
    fileLabel_->setProperty("kvRole", QStringLiteral("diffFile"));
    scopeLabel_->setProperty("kvRole", QStringLiteral("diffScope"));
    statsLabel_->setProperty("kvRole", QStringLiteral("diffStats"));
    titleLayout->addWidget(fileLabel_);
    titleLayout->addWidget(scopeLabel_);
    titleLayout->addStretch();
    titleLayout->addWidget(statsLabel_);

    pathLabel_->setProperty("kvRole", QStringLiteral("diffPath"));
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    openButton_->setText(QStringLiteral("Open File"));
    openButton_->setProperty("kvRole", QStringLiteral("diffAction"));
    openButton_->setToolTip(QStringLiteral("Open the working file"));

    contextButton_->setProperty("kvRole", QStringLiteral("diffAction"));
    contextButton_->setCheckable(true);
    contextButton_->setChecked(hideUnchangedRows_);
    contextButton_->setToolTip(QStringLiteral("Toggle unchanged context rows"));
    updateContextButton();

    auto* copyButton = new QToolButton(this);
    copyButton->setText(QStringLiteral("Copy Diff"));
    copyButton->setProperty("kvRole", QStringLiteral("diffAction"));

    auto* refreshButton = new QToolButton(this);
    refreshButton->setText(QString::fromUtf8("↻"));
    refreshButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    refreshButton->setToolTip(QStringLiteral("Refresh this diff"));
    refreshButton->setAccessibleName(QStringLiteral("Refresh this diff"));

    auto* headerTextLayout = new QVBoxLayout;
    headerTextLayout->setContentsMargins(0, 0, 0, 0);
    headerTextLayout->setSpacing(2);
    headerTextLayout->addLayout(titleLayout);
    headerTextLayout->addWidget(pathLabel_);

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(14, 9, 10, 9);
    headerLayout->setSpacing(7);
    headerLayout->addLayout(headerTextLayout, 1);
    headerLayout->addWidget(openButton_);
    headerLayout->addWidget(contextButton_);
    headerLayout->addWidget(copyButton);
    headerLayout->addWidget(refreshButton);

    auto* header = new QWidget(this);
    header->setProperty("kvRole", QStringLiteral("diffHeader"));
    header->setLayout(headerLayout);

    table_->setProperty("kvRole", QStringLiteral("diffTable"));
    table_->setModel(model_);
    table_->setItemDelegate(new DiffTextDelegate(table_));
    table_->setFrameShape(QFrame::NoFrame);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setAlternatingRowColors(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->setColumnWidth(0, 46);
    table_->setColumnWidth(1, 46);
    table_->setColumnWidth(2, 24);
    applyEditorSettings(EditorSettings::defaults());
    syntaxEngine_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(table_, 1);

    connect(openButton_, &QToolButton::clicked, this,
            [this] { emit openFileRequested(filePath_); });
    connect(contextButton_, &QToolButton::toggled, this, [this](const bool checked) {
        hideUnchangedRows_ = checked;
        model_->setHideUnchangedRows(hideUnchangedRows_);
        model_->applySyntax(filePath_, *syntaxEngine_, palette_);
        updateContextButton();
        updateSpans();
    });
    connect(copyButton, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(rawDiff_); });
    connect(refreshButton, &QToolButton::clicked, this,
            [this] { emit refreshRequested(filePath_, mode_); });
}

QString GitDiffView::filePath() const { return filePath_; }

GitDiffMode GitDiffView::mode() const noexcept { return mode_; }

void GitDiffView::setDiff(const QString& filePath, const GitDiffMode mode,
                          const QString& diff, const ThemePalette& palette) {
    filePath_ = filePath;
    mode_ = mode;
    rawDiff_ = diff;
    palette_ = palette;
    openButton_->setEnabled(QFileInfo::exists(filePath_));
    model_->setHideUnchangedRows(hideUnchangedRows_);
    model_->setDiff(diff);
    model_->applyTheme(palette);
    model_->applySyntax(filePath, *syntaxEngine_, palette);
    updateHeader();
    updateSpans();
    table_->scrollToTop();
}

void GitDiffView::applyEditorSettings(const EditorSettings& settings) {
    const auto value = settings.normalized();
    const QFont codeFont = value.font();
    syntaxEngine_->setEditorSettings(value);
    model_->setCodeFont(codeFont);
    table_->setFont(codeFont);
    table_->verticalHeader()->setMinimumSectionSize(value.lineHeightPixels);
    table_->verticalHeader()->setDefaultSectionSize(value.lineHeightPixels);
    table_->viewport()->update();
}

void GitDiffView::applyTheme(const ThemePalette& palette) {
    palette_ = palette;
    model_->applyTheme(palette);
    model_->applySyntax(filePath_, *syntaxEngine_, palette);
}

void GitDiffView::updateHeader() {
    const QFileInfo file(filePath_);
    fileLabel_->setText(file.fileName());
    pathLabel_->setText(filePath_);
    pathLabel_->setToolTip(filePath_);
    scopeLabel_->setText(scopeText(mode_));
    scopeLabel_->setProperty("diffMode", scopeProperty(mode_));
    scopeLabel_->style()->unpolish(scopeLabel_);
    scopeLabel_->style()->polish(scopeLabel_);
    statsLabel_->setText(
        QStringLiteral("+%1  −%2").arg(model_->additions()).arg(model_->deletions()));
}

void GitDiffView::updateContextButton() {
    contextButton_->setText(hideUnchangedRows_ ? QStringLiteral("Show Context")
                                               : QStringLiteral("Hide Context"));
}

void GitDiffView::updateSpans() {
    table_->clearSpans();
    for (int row = 0; row < model_->rowCount(); ++row) {
        if (model_->spansColumns(row)) {
            table_->setSpan(row, 0, 1, 4);
        }
    }
}

} // namespace ketplus
