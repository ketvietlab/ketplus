#include "KetAppearance.h"
#include "KetFieldRow.h"
#include "KetProfile.h"
#include "KetSelect.h"
#include "app/PixelSelect.h"
#include "app/SettingsDialog.h"
#include "app/SettingsFontNames.h"
#include "ui/InterfaceZoom.h"
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
        setMinimumHeight(2 * inset + labelPixels + 3 * gap + interfacePixels() +
                         2 * settings_.editor.lineHeightPixels +
                         settings_.terminal.lineHeightPixels);
        updateGeometry();
        update();
    }

  private:
    // The interface text as the application shows it, at the zoom.
    [[nodiscard]] int interfacePixels() const {
        return InterfaceZoomMetrics::scale(settings_.interfaceFontSizePixels,
                                           settings_.interfaceZoomPercent);
    }

  protected:
    // The mock's sample: a sentence in the interface font, code in the editor font and a shell
    // prompt in the terminal font, each at its size.
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        const bool dark = currentThemePalette().dark;
        const QColor main(themeTokenColor(QStringLiteral("text-main"), dark));
        const QColor muted(themeTokenColor(QStringLiteral("text-muted"), dark));
        painter.fillPath(frame, QColor(themeTokenColor(QStringLiteral("panel-bg-subtle"), dark)));
        painter.setPen(QColor(themeTokenColor(QStringLiteral("panel-border"), dark)));
        painter.drawPath(frame);
        painter.setClipPath(frame);

        int top = inset;
        QFont labelFont = font();
        labelFont.setPixelSize(labelPixels);
        labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        top = drawLines(painter, top, labelFont, labelPixels, muted, QStringLiteral("PREVIEW"));

        QFont interfaceFont = font();
        interfaceFont.setPixelSize(interfacePixels());
        top = drawLines(painter, top + gap, interfaceFont, interfacePixels(), main,
                        QStringLiteral("A space for what you are creating."));

        top = drawLines(
            painter, top + gap, settings_.editor.font(), settings_.editor.lineHeightPixels, main,
            QStringLiteral("const workspace = \"develop\";\nagent.continue(workspace);"));

        QFont terminalFont(settings_.terminalFontFamily.isEmpty()
                               ? ket::Profile::current().monoFamily()
                               : settings_.terminalFontFamily);
        terminalFont.setPixelSize(settings_.terminal.fontSizePixels);
        drawLines(painter, top + gap, terminalFont, settings_.terminal.lineHeightPixels, muted,
                  QStringLiteral("\u279C ketplus-desktop git:(develop)"));
    }

  private:
    static constexpr int inset = 16;
    static constexpr int gap = 8;
    static constexpr int labelPixels = 11;

    // Draws `text` line by line from `top` and returns the bottom of its last line.
    int drawLines(QPainter& painter, const int top, const QFont& sampleFont, const int lineHeight,
                  const QColor& color, const QString& text) {
        painter.setFont(sampleFont);
        painter.setPen(color);
        const QFontMetrics metrics(sampleFont);
        int lineTop = top;
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            const QRect area(inset, lineTop, width() - 2 * inset,
                             qMax(lineHeight, metrics.height()));
            painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, line);
            lineTop += lineHeight;
        }
        return lineTop;
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

    interfaceFontSizeBox_ = pixelSelect(
        QStringLiteral("interfaceFontSize"), AppearanceSettings::minimumInterfaceFontSizePixels,
        AppearanceSettings::maximumInterfaceFontSizePixels, content);
    interfaceFontSizeBox_->setAccessibleName(tr("Interface font size"));
    interfaceFontSizeBox_->findChild<ket::Select*>()->setAccessibleName(tr("Interface font size"));
    layout->addWidget(
        new ket::FieldRow(tr("Interface font size"), interfaceFontSizeBox_, {}, content));
    auto* editorFont = new ket::Select(content);
    editorFontBox_ = editorFont;
    editorFontBox_->setObjectName(QStringLiteral("editorFontFamily"));
    editorFontBox_->setProperty("fontListLoaded", false);
    editorFontBox_->setAccessibleName(tr("Editor font"));
    new FontListLoader(editorFont);
    layout->addWidget(new ket::FieldRow(tr("Editor font"), editorFont, {}, content));
    editorFontSizeBox_ =
        pixelSelect(QStringLiteral("editorFontSize"), EditorSettings::minimumFontSizePixels,
                    EditorSettings::maximumFontSizePixels, content);
    editorLineHeightBox_ =
        pixelSelect(QStringLiteral("editorLineHeight"), EditorSettings::minimumLineHeightPixels,
                    EditorSettings::maximumLineHeightPixels, content);

    for (const auto& [label, control] :
         {std::pair{tr("Editor font size"), editorFontSizeBox_},
          std::pair{tr("Editor line height"), editorLineHeightBox_}}) {
        control->setAccessibleName(label);
        control->findChild<ket::Select*>()->setAccessibleName(label);
        layout->addWidget(new ket::FieldRow(label, control, {}, content));
    }
    preview_ = new TypographyPreview(content);
    preview_->setObjectName(QStringLiteral("typographyPreview"));
    layout->addWidget(preview_);
    layout->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("typographySettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    const QList<ketplus::PixelSelect*> sizeBoxes = {interfaceFontSizeBox_, editorFontSizeBox_};
    for (ketplus::PixelSelect* box : sizeBoxes) {
        connect(box, &ketplus::PixelSelect::valueChanged, this, [this, box] {
            if (box == editorFontSizeBox_) {
                editorLineHeightBox_->setMinimum(
                    qMax(EditorSettings::minimumLineHeightPixels, box->value()));
            }
            updatePreview();
            applyChanges();
        });
    }
    connect(editorLineHeightBox_, &ketplus::PixelSelect::valueChanged, this, [this] {
        updatePreview();
        applyChanges();
    });
    connect(editorFontBox_, &QComboBox::currentIndexChanged, this, [this] {
        editorFontFamily_ = settings_fonts::selectedFamily(editorFontBox_);
        updatePreview();
        applyChanges();
    });
    return scroll;
}

void SettingsDialog::updatePreview() {
    if (preview_ != nullptr) {
        preview_->setAppearanceSettings(appearanceSettings());
    }
}

} // namespace ketplus
