#include "KetAppearance.h"
#include "KetFieldRow.h"
#include "KetProfile.h"
#include "KetSelect.h"
#include "KetStepSlider.h"
#include "app/PixelSelect.h"
#include "app/SettingsDialog.h"
#include "app/SettingsFontNames.h"
#include "ui/Theme.h"
#include <QAbstractItemView>
#include <QEvent>
#include <QFontDatabase>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ketplus {

namespace settings_fonts {

QString label(const QString& family) {
    if (family.startsWith(QLatin1String("KET Sans")))
        return QStringLiteral("KÉT") + family.mid(3);
    return family;
}

QString selectedFamily(const QComboBox* select) {
    const QString family = select->currentData().toString();
    return family.isEmpty() ? select->currentText() : family;
}

void selectFamily(QComboBox* select, const QString& family) {
    int index = select->findData(family);
    if (index < 0)
        index = select->findText(family);
    if (index < 0) {
        select->addItem(label(family), family);
        index = select->count() - 1;
    }
    select->setCurrentIndex(index);
}

} // namespace settings_fonts

namespace {

class FontListLoader final : public QObject {
  public:
    explicit FontListLoader(ket::Select* select)
        : QObject(select), select_(select), view_(select->view()) {
        select->installEventFilter(this);
        view_->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (!loaded_ &&
            (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress ||
             event->type() == QEvent::KeyPress ||
             (object == view_ && event->type() == QEvent::Show))) {
            loaded_ = true;
            const QString selected = settings_fonts::selectedFamily(select_);
            QStringList families;
            for (const auto& family : QFontDatabase::families())
                if (QFontDatabase::isFixedPitch(family))
                    families.append(family);
            families.sort(Qt::CaseInsensitive);
            const QSignalBlocker blocker(select_);
            select_->clear();
            for (const auto& family : families)
                select_->addItem(settings_fonts::label(family), family);
            if (!selected.isEmpty())
                settings_fonts::selectFamily(select_, selected);
            select_->setProperty("fontListLoaded", true);
        }
        return QObject::eventFilter(object, event);
    }

  private:
    ket::Select* select_;
    QAbstractItemView* view_;
    bool loaded_{false};
};

PixelSelect* pixelSelect(const QString& name, int minimum, int maximum, QWidget* parent) {
    auto* select = new PixelSelect(parent);
    select->setObjectName(name);
    select->setRange(minimum, maximum);
    return select;
}
} // namespace

class TypographyPreview final : public QWidget {
  public:
    using QWidget::QWidget;

    void setAppearanceSettings(const AppearanceSettings& settings) {
        settings_ = settings.normalized();
        setMinimumHeight(
            100 + settings_.interfaceFontSizePixels + 2 * settings_.editor.lineHeightPixels +
            2 * settings_.terminal.lineHeightPixels + 2 * settings_.preview.lineHeightPixels);
        updateGeometry();
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        const bool dark = currentThemePalette().dark;
        painter.fillPath(frame, QColor(themeTokenColor(QStringLiteral("panel-bg-subtle"), dark)));
        painter.setPen(QColor(themeTokenColor(QStringLiteral("border-default"), dark)));
        painter.drawPath(frame);
        painter.setClipPath(frame);

        int top = 18;
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QFont labelFont = font();
        labelFont.setPixelSize(qRound(ket::Appearance::instance().scaled(11)));
        labelFont.setWeight(QFont::DemiBold);
        painter.setFont(labelFont);
        painter.drawText(14, top, QStringLiteral("LIVE PREVIEW"));
        top += 27;

        QFont interfaceFont = font();
        interfaceFont.setPixelSize(settings_.interfaceFontSizePixels);
        painter.setFont(interfaceFont);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(14, top, QStringLiteral("Appearance settings"));
        top += settings_.interfaceFontSizePixels + 20;

        drawSample(painter, top, settings_.editor.font(), settings_.editor.lineHeightPixels,
                   QStringLiteral("const scale = 1.0;\nrender(settings);"));
        top += settings_.editor.lineHeightPixels * 2 + 17;

        QFont terminalFont(settings_.terminalFontFamily.isEmpty()
                               ? ket::Profile::current().monoFamily()
                               : settings_.terminalFontFamily);
        terminalFont.setPixelSize(settings_.terminal.fontSizePixels);
        drawSample(painter, top, terminalFont, settings_.terminal.lineHeightPixels,
                   QStringLiteral("$ ketplus .\nWorkspace ready"));
        top += settings_.terminal.lineHeightPixels * 2 + 17;

        QFont previewFont = font();
        previewFont.setPixelSize(settings_.preview.fontSizePixels);
        drawSample(painter, top, previewFont, settings_.preview.lineHeightPixels,
                   QStringLiteral("Preview\nDocumentation stays legible."));
    }

  private:
    void drawSample(QPainter& painter, const int top, const QFont& sampleFont, const int lineHeight,
                    const QString& text) {
        painter.setFont(sampleFont);
        painter.setPen(palette().color(QPalette::Text));
        const QFontMetrics metrics(sampleFont);
        int baseline = top + metrics.ascent();
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            painter.drawText(14, baseline, line);
            baseline += lineHeight;
        }
    }

    AppearanceSettings settings_{AppearanceSettings::defaults()};
};

QWidget* SettingsDialog::createTypographyCard() {
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto* title = new QLabel(tr("Typography"), content);
    title->setProperty("kvRole", QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    auto* help = new QLabel(tr("Adjust readability across your workspace."), content);
    help->setProperty("kvRole", QStringLiteral("settingsDescription"));
    help->setWordWrap(true);
    layout->addWidget(help);

    auto* editorFont = new ket::Select(content);
    editorFontBox_ = editorFont;
    editorFontBox_->setObjectName(QStringLiteral("editorFontFamily"));
    editorFontBox_->setProperty("fontListLoaded", false);
    editorFontBox_->setAccessibleName(tr("Editor font"));
    new FontListLoader(editorFont);
    layout->addWidget(new ket::FieldRow(tr("Editor font"), editorFont, {}, content));
    interfaceFontSizeBox_ = pixelSelect(
        QStringLiteral("interfaceFontSize"), AppearanceSettings::minimumInterfaceFontSizePixels,
        AppearanceSettings::maximumInterfaceFontSizePixels, content);
    interfaceFontSizeBox_->hide(); // Appearance's zoom slider owns this preference's UI.
    editorFontSizeBox_ =
        pixelSelect(QStringLiteral("editorFontSize"), EditorSettings::minimumFontSizePixels,
                    EditorSettings::maximumFontSizePixels, content);
    editorLineHeightBox_ =
        pixelSelect(QStringLiteral("editorLineHeight"), EditorSettings::minimumLineHeightPixels,
                    EditorSettings::maximumLineHeightPixels, content);
    terminalFontSizeBox_ = pixelSelect(QStringLiteral("terminalFontSize"),
                                       TextTypographySettings::minimumFontSizePixels,
                                       TextTypographySettings::maximumFontSizePixels, content);
    terminalLineHeightBox_ = pixelSelect(QStringLiteral("terminalLineHeight"),
                                         TextTypographySettings::minimumLineHeightPixels,
                                         TextTypographySettings::maximumLineHeightPixels, content);
    previewFontSizeBox_ = pixelSelect(QStringLiteral("previewFontSize"),
                                      TextTypographySettings::minimumFontSizePixels,
                                      TextTypographySettings::maximumFontSizePixels, content);
    previewLineHeightBox_ = pixelSelect(QStringLiteral("previewLineHeight"),
                                        TextTypographySettings::minimumLineHeightPixels,
                                        TextTypographySettings::maximumLineHeightPixels, content);

    for (const auto& [label, control] :
         {std::pair{tr("Editor font size"), editorFontSizeBox_},
          std::pair{tr("Editor line height"), editorLineHeightBox_},
          std::pair{tr("Terminal font size"), terminalFontSizeBox_},
          std::pair{tr("Terminal line height"), terminalLineHeightBox_},
          std::pair{tr("Preview font size"), previewFontSizeBox_},
          std::pair{tr("Preview line height"), previewLineHeightBox_}}) {
        control->setAccessibleName(label);
        control->findChild<ket::Select*>()->setAccessibleName(label);
        layout->addWidget(new ket::FieldRow(label, control, {}, content));
    }
    connect(zoomStep_, &ket::StepSlider::currentIndexChanged, interfaceFontSizeBox_,
            [this](int index) {
                interfaceFontSizeBox_->setValue(index +
                                                AppearanceSettings::minimumInterfaceFontSizePixels);
            });
    connect(interfaceFontSizeBox_, &PixelSelect::valueChanged, zoomStep_, [this](int value) {
        const QSignalBlocker blocker(zoomStep_);
        zoomStep_->setCurrentIndex(value - AppearanceSettings::minimumInterfaceFontSizePixels);
    });
    preview_ = new TypographyPreview(content);
    preview_->setObjectName(QStringLiteral("typographyPreview"));
    layout->addWidget(preview_);
    layout->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("typographySettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    const QList<ketplus::PixelSelect*> sizeBoxes = {interfaceFontSizeBox_, editorFontSizeBox_,
                                                    terminalFontSizeBox_, previewFontSizeBox_};
    for (ketplus::PixelSelect* box : sizeBoxes) {
        connect(box, &ketplus::PixelSelect::valueChanged, this, [this, box] {
            if (box == editorFontSizeBox_) {
                editorLineHeightBox_->setMinimum(
                    qMax(EditorSettings::minimumLineHeightPixels, box->value()));
            } else if (box == terminalFontSizeBox_) {
                terminalLineHeightBox_->setMinimum(
                    qMax(TextTypographySettings::minimumLineHeightPixels, box->value()));
            } else if (box == previewFontSizeBox_) {
                previewLineHeightBox_->setMinimum(
                    qMax(TextTypographySettings::minimumLineHeightPixels, box->value()));
            }
            updatePreview();
            updateDirtyState();
        });
    }
    for (ketplus::PixelSelect* box :
         {editorLineHeightBox_, terminalLineHeightBox_, previewLineHeightBox_}) {
        connect(box, &ketplus::PixelSelect::valueChanged, this, [this] {
            updatePreview();
            updateDirtyState();
        });
    }
    connect(editorFontBox_, &QComboBox::currentTextChanged, this, [this] {
        editorFontFamily_ = settings_fonts::selectedFamily(editorFontBox_);
        updatePreview();
        updateDirtyState();
    });
    return scroll;
}

void SettingsDialog::updatePreview() {
    if (preview_ != nullptr) {
        preview_->setAppearanceSettings(appearanceSettings());
    }
}

} // namespace ketplus
