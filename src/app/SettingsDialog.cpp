#include "app/SettingsDialog.h"

#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOption>
#include <QVBoxLayout>

namespace ketplus {

class TypographyPreview final : public QWidget {
  public:
    using QWidget::QWidget;

    void setEditorSettings(const EditorSettings& settings) {
        settings_ = settings.normalized();
        setMinimumHeight(qBound(116, settings_.lineHeightPixels * 3 + 36, 300));
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QStyleOption option;
        option.initFrom(this);
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QFont codeFont = settings_.font();
        painter.setFont(codeFont);
        const QFontMetrics metrics(codeFont);
        const QColor codeColor = palette().color(QPalette::Text);
        const QColor gutterColor = palette().color(QPalette::PlaceholderText);
        const QStringList lines = {
            QStringLiteral("const editor = \"KetPlus\";"),
            QStringLiteral("// Clear type. Calm rhythm."),
            QStringLiteral("editor.open();"),
        };

        const int gutterWidth = metrics.horizontalAdvance(QStringLiteral("00")) + 18;
        int baseline = 18 + metrics.ascent();
        for (qsizetype index = 0; index < lines.size(); ++index) {
            painter.setPen(gutterColor);
            painter.drawText(QRect(12, baseline - metrics.ascent(), gutterWidth - 8,
                                   settings_.lineHeightPixels),
                             Qt::AlignRight | Qt::AlignTop,
                             QString::number(index + 1));
            painter.setPen(codeColor);
            painter.drawText(gutterWidth + 14, baseline, lines.at(index));
            baseline += settings_.lineHeightPixels;
        }
    }

  private:
    EditorSettings settings_{EditorSettings::defaults()};
};

namespace {

QLabel* makeLabel(const QString& text, const QString& role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("kvRole", role);
    label->setWordWrap(true);
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(const EditorSettings& settings, QWidget* parent)
    : QDialog(parent), fontBox_(new QFontComboBox(this)),
      fontSizeBox_(new QSpinBox(this)), lineHeightBox_(new QSpinBox(this)),
      preview_(new TypographyPreview(this)) {
    setWindowTitle(QStringLiteral("Settings"));
    setProperty("kvRole", QStringLiteral("settingsDialog"));
    setModal(true);
    setMinimumWidth(520);

    auto* heading = makeLabel(QStringLiteral("EDITOR"), QStringLiteral("settingsEyebrow"), this);
    auto* title =
        makeLabel(QStringLiteral("Typography"), QStringLiteral("settingsTitle"), this);
    auto* description = makeLabel(
        QStringLiteral("Tune the editor for comfortable reading. Changes also apply to Git diffs."),
        QStringLiteral("settingsDescription"), this);

    fontBox_->setObjectName(QStringLiteral("editorFontFamily"));
    fontBox_->setFontFilters(QFontComboBox::MonospacedFonts);

    fontSizeBox_->setObjectName(QStringLiteral("editorFontSize"));
    fontSizeBox_->setRange(EditorSettings::minimumFontSizePixels,
                           EditorSettings::maximumFontSizePixels);
    fontSizeBox_->setSuffix(QStringLiteral(" px"));

    lineHeightBox_->setObjectName(QStringLiteral("editorLineHeight"));
    lineHeightBox_->setRange(EditorSettings::minimumLineHeightPixels,
                             EditorSettings::maximumLineHeightPixels);
    lineHeightBox_->setSuffix(QStringLiteral(" px"));

    auto* card = new QFrame(this);
    card->setProperty("kvRole", QStringLiteral("settingsCard"));
    auto* form = new QGridLayout(card);
    form->setContentsMargins(16, 14, 16, 16);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(6);
    form->setColumnStretch(1, 1);

    auto addField = [form](const int row, const QString& labelText, const QString& helpText,
                           QWidget* field) {
        auto* label = makeLabel(labelText, QStringLiteral("settingsFieldLabel"), field->parentWidget());
        auto* help = makeLabel(helpText, QStringLiteral("settingsFieldHelp"), field->parentWidget());
        label->setBuddy(field);
        form->addWidget(label, row * 2, 0, Qt::AlignTop);
        form->addWidget(field, row * 2, 1);
        form->addWidget(help, row * 2 + 1, 1);
    };

    addField(0, QStringLiteral("Font"), QStringLiteral("Installed monospaced fonts"), fontBox_);
    addField(1, QStringLiteral("Font size"), QStringLiteral("8–48 pixels"), fontSizeBox_);
    addField(2, QStringLiteral("Line height"),
             QStringLiteral("Vertical space reserved for each line"), lineHeightBox_);

    auto* previewLabel =
        makeLabel(QStringLiteral("PREVIEW"), QStringLiteral("settingsEyebrow"), this);
    preview_->setProperty("kvRole", QStringLiteral("settingsPreview"));
    preview_->setAttribute(Qt::WA_StyledBackground, true);
    preview_->setMinimumHeight(116);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* resetButton = buttons->addButton(QStringLiteral("Reset Defaults"),
                                           QDialogButtonBox::ResetRole);
    resetButton->setProperty("kvRole", QStringLiteral("settingsReset"));
    auto* saveButton = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
    saveButton->setProperty("kvRole", QStringLiteral("primaryAction"));
    saveButton->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(10);
    layout->addWidget(heading);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addSpacing(6);
    layout->addWidget(card);
    layout->addSpacing(6);
    layout->addWidget(previewLabel);
    layout->addWidget(preview_);
    layout->addSpacing(4);
    layout->addWidget(buttons);

    connect(fontBox_, &QFontComboBox::currentFontChanged, this,
            [this] { updatePreview(); });
    connect(fontSizeBox_, &QSpinBox::valueChanged, this, [this](const int fontSize) {
        lineHeightBox_->setMinimum(qMax(EditorSettings::minimumLineHeightPixels, fontSize));
        updatePreview();
    });
    connect(lineHeightBox_, &QSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(resetButton, &QPushButton::clicked, this,
            [this] { setSettings(EditorSettings::defaults()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, [this] {
        emit settingsSaved(this->settings());
        accept();
    });

    setSettings(settings);
}

EditorSettings SettingsDialog::settings() const {
    return EditorSettings{
        .fontFamily = fontBox_->currentFont().family(),
        .fontSizePixels = fontSizeBox_->value(),
        .lineHeightPixels = lineHeightBox_->value(),
    }
        .normalized();
}

void SettingsDialog::setSettings(const EditorSettings& settings) {
    const auto value = settings.normalized();
    fontBox_->setCurrentFont(QFont(value.fontFamily));
    fontSizeBox_->setValue(value.fontSizePixels);
    lineHeightBox_->setMinimum(
        qMax(EditorSettings::minimumLineHeightPixels, value.fontSizePixels));
    lineHeightBox_->setValue(value.lineHeightPixels);
    updatePreview();
}

void SettingsDialog::updatePreview() { preview_->setEditorSettings(settings()); }

} // namespace ketplus
