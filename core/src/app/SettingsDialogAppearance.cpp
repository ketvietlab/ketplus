#include "app/SettingsDialog.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {

namespace {

class LazyFontComboBox final : public QComboBox {
  public:
    using QComboBox::QComboBox;

    void showPopup() override {
        if (!fontsLoaded_) {
            const QString selectedFont = currentText();
            QStringList monospacedFonts;
            for (const QString& family : QFontDatabase::families()) {
                if (QFontDatabase::isFixedPitch(family)) {
                    monospacedFonts.append(family);
                }
            }
            monospacedFonts.sort(Qt::CaseInsensitive);

            const QSignalBlocker blocker(this);
            clear();
            addItems(monospacedFonts);
            if (!selectedFont.isEmpty() && findText(selectedFont) < 0) {
                addItem(selectedFont);
            }
            setCurrentText(selectedFont);
            fontsLoaded_ = true;
            setProperty("fontListLoaded", true);
        }
        QComboBox::showPopup();
    }

  private:
    bool fontsLoaded_{false};
};

} // namespace

class TypographyPreview final : public QWidget {
  public:
    using QWidget::QWidget;

    void setAppearanceSettings(const AppearanceSettings& settings) {
        settings_ = settings.normalized();
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        int top = 18;
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QFont labelFont = font();
        labelFont.setPixelSize(9);
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

        QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
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

namespace {

QLabel* makeLabel(const QString& text, const QString& role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("kvRole", role);
    label->setWordWrap(true);
    return label;
}

QSpinBox* makePixelBox(const QString& name, const int minimum, const int maximum, QWidget* parent) {
    auto* box = new QSpinBox(parent);
    box->setObjectName(name);
    box->setRange(minimum, maximum);
    box->setSuffix(QStringLiteral(" px"));
    box->setFixedWidth(88);
    return box;
}

QWidget* makePageHeading(const QString& eyebrow, const QString& title, const QString& description,
                         QWidget* parent) {
    auto* heading = new QWidget(parent);
    auto* layout = new QVBoxLayout(heading);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(eyebrow, QStringLiteral("settingsEyebrow"), heading));
    layout->addWidget(makeLabel(title, QStringLiteral("settingsTitle"), heading));
    layout->addWidget(makeLabel(description, QStringLiteral("settingsDescription"), heading));
    return heading;
}

} // namespace

QWidget* SettingsDialog::createAppearancePage() {
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(30, 26, 30, 34);
    layout->setSpacing(20);
    layout->addWidget(makePageHeading(
        QStringLiteral("APPEARANCE"), QStringLiteral("Make KetPlus comfortable to read"),
        QStringLiteral("Typography is shared across repository windows. Theme packages will "
                       "use the same semantic color contract as the built-in themes."),
        content));

    layout->addWidget(
        makeLabel(QStringLiteral("COLOR THEME"), QStringLiteral("settingsEyebrow"), content));
    layout->addWidget(createThemeCard());
    layout->addWidget(
        makeLabel(QStringLiteral("TYPOGRAPHY"), QStringLiteral("settingsEyebrow"), content));
    layout->addWidget(createTypographyCard());
    layout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("appearanceSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QWidget* SettingsDialog::createThemeCard() {
    auto* card = new QFrame(this);
    card->setProperty("kvRole", QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(9);

    auto* toolbar = new QHBoxLayout;
    themeSearch_ = new QLineEdit(card);
    themeSearch_->setObjectName(QStringLiteral("themeSearch"));
    themeSearch_->setPlaceholderText(QStringLiteral("Search installed themes"));
    themeSearch_->setClearButtonEnabled(true);
    auto* install = new QToolButton(card);
    install->setObjectName(QStringLiteral("installTheme"));
    install->setText(QStringLiteral("Install theme"));
    install->setPopupMode(QToolButton::InstantPopup);
    auto* installMenu = new QMenu(install);
    auto* vsCodeAction = installMenu->addAction(QStringLiteral("Import VS Code theme…"));
    auto* jetBrainsAction = installMenu->addAction(QStringLiteral("Import JetBrains theme…"));
    installMenu->addSeparator();
    auto* catalogAction = installMenu->addAction(QStringLiteral("Browse Open VSX"));
    for (QAction* action : {vsCodeAction, jetBrainsAction, catalogAction}) {
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Available in the theme importer milestone"));
    }
    install->setMenu(installMenu);
    toolbar->addWidget(themeSearch_, 1);
    toolbar->addWidget(install);
    layout->addLayout(toolbar);

    followSystem_ = new QCheckBox(QStringLiteral("Follow system appearance"), card);
    followSystem_->setObjectName(QStringLiteral("followSystemTheme"));
    followSystem_->setToolTip(
        QStringLiteral("Use Két Light or Két Dark with the operating system appearance"));
    layout->addWidget(followSystem_);

    themeList_ = new QListWidget(card);
    themeList_->setObjectName(QStringLiteral("installedThemeList"));
    themeList_->setProperty("kvRole", QStringLiteral("themeList"));
    themeList_->setMinimumHeight(104);
    auto* light = new QListWidgetItem(QStringLiteral("Két Light    Built-in · Light"), themeList_);
    light->setData(Qt::UserRole, static_cast<int>(ThemeManager::Mode::Light));
    auto* dark = new QListWidgetItem(QStringLiteral("Két Dark     Built-in · Dark"), themeList_);
    dark->setData(Qt::UserRole, static_cast<int>(ThemeManager::Mode::Dark));
    layout->addWidget(themeList_);

    connect(themeSearch_, &QLineEdit::textChanged, this, [this](const QString& query) {
        for (int index = 0; index < themeList_->count(); ++index) {
            auto* item = themeList_->item(index);
            item->setHidden(!item->text().contains(query, Qt::CaseInsensitive));
        }
    });
    connect(followSystem_, &QCheckBox::toggled, this, [this](const bool checked) {
        themeList_->setEnabled(!checked);
        if (checked) {
            selectThemeMode(static_cast<int>(ThemeManager::Mode::System));
        } else if (themeList_->currentItem() != nullptr) {
            selectThemeMode(themeList_->currentItem()->data(Qt::UserRole).toInt());
        }
    });
    connect(themeList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
        if (current == nullptr || followSystem_->isChecked()) {
            return;
        }
        selectThemeMode(current->data(Qt::UserRole).toInt());
    });
    return card;
}

QWidget* SettingsDialog::createTypographyCard() {
    auto* card = new QFrame(this);
    card->setProperty("kvRole", QStringLiteral("settingsCard"));
    auto* horizontal = new QHBoxLayout(card);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(0);

    auto* fields = new QWidget(card);
    auto* grid = new QGridLayout(fields);
    grid->setContentsMargins(15, 14, 15, 15);
    grid->setHorizontalSpacing(9);
    grid->setVerticalSpacing(9);
    grid->setColumnStretch(0, 1);
    grid->addWidget(
        makeLabel(QStringLiteral("Surface"), QStringLiteral("settingsFieldHelp"), fields), 0, 0);
    grid->addWidget(makeLabel(QStringLiteral("Size"), QStringLiteral("settingsFieldHelp"), fields),
                    0, 1);
    grid->addWidget(makeLabel(QStringLiteral("Line"), QStringLiteral("settingsFieldHelp"), fields),
                    0, 2);

    editorFontBox_ = new LazyFontComboBox(fields);
    editorFontBox_->setObjectName(QStringLiteral("editorFontFamily"));
    editorFontBox_->setProperty("fontListLoaded", false);
    grid->addWidget(
        makeLabel(QStringLiteral("Editor font"), QStringLiteral("settingsFieldLabel"), fields), 1,
        0);
    grid->addWidget(editorFontBox_, 1, 1, 1, 2);

    interfaceFontSizeBox_ = makePixelBox(
        QStringLiteral("interfaceFontSize"), AppearanceSettings::minimumInterfaceFontSizePixels,
        AppearanceSettings::maximumInterfaceFontSizePixels, fields);
    editorFontSizeBox_ =
        makePixelBox(QStringLiteral("editorFontSize"), EditorSettings::minimumFontSizePixels,
                     EditorSettings::maximumFontSizePixels, fields);
    editorLineHeightBox_ =
        makePixelBox(QStringLiteral("editorLineHeight"), EditorSettings::minimumLineHeightPixels,
                     EditorSettings::maximumLineHeightPixels, fields);
    terminalFontSizeBox_ = makePixelBox(QStringLiteral("terminalFontSize"),
                                        TextTypographySettings::minimumFontSizePixels,
                                        TextTypographySettings::maximumFontSizePixels, fields);
    terminalLineHeightBox_ = makePixelBox(QStringLiteral("terminalLineHeight"),
                                          TextTypographySettings::minimumLineHeightPixels,
                                          TextTypographySettings::maximumLineHeightPixels, fields);
    previewFontSizeBox_ = makePixelBox(QStringLiteral("previewFontSize"),
                                       TextTypographySettings::minimumFontSizePixels,
                                       TextTypographySettings::maximumFontSizePixels, fields);
    previewLineHeightBox_ = makePixelBox(QStringLiteral("previewLineHeight"),
                                         TextTypographySettings::minimumLineHeightPixels,
                                         TextTypographySettings::maximumLineHeightPixels, fields);

    auto addTypographyRow = [grid, fields](const int row, const QString& title, const QString& help,
                                           QWidget* size, QWidget* line) {
        auto* copy = new QWidget(fields);
        auto* copyLayout = new QVBoxLayout(copy);
        copyLayout->setContentsMargins(0, 0, 0, 0);
        copyLayout->setSpacing(2);
        copyLayout->addWidget(makeLabel(title, QStringLiteral("settingsFieldLabel"), copy));
        copyLayout->addWidget(makeLabel(help, QStringLiteral("settingsFieldHelp"), copy));
        grid->addWidget(copy, row, 0);
        grid->addWidget(size, row, 1, Qt::AlignVCenter);
        grid->addWidget(line, row, 2, Qt::AlignVCenter);
    };

    auto* automatic =
        makeLabel(QStringLiteral("Auto"), QStringLiteral("settingsAutomaticValue"), fields);
    automatic->setAlignment(Qt::AlignCenter);
    automatic->setFixedWidth(88);
    addTypographyRow(2, QStringLiteral("Interface text"),
                     QStringLiteral("Navigation, dialogs and controls"), interfaceFontSizeBox_,
                     automatic);
    addTypographyRow(3, QStringLiteral("Editor text"),
                     QStringLiteral("Editor, source code and Git diffs"), editorFontSizeBox_,
                     editorLineHeightBox_);
    addTypographyRow(4, QStringLiteral("Terminal text"),
                     QStringLiteral("Integrated terminal sessions"), terminalFontSizeBox_,
                     terminalLineHeightBox_);
    addTypographyRow(5, QStringLiteral("Preview text"),
                     QStringLiteral("Markdown and document previews"), previewFontSizeBox_,
                     previewLineHeightBox_);

    preview_ = new TypographyPreview(card);
    preview_->setObjectName(QStringLiteral("typographyPreview"));
    preview_->setProperty("kvRole", QStringLiteral("settingsPreview"));
    preview_->setMinimumWidth(235);
    preview_->setMinimumHeight(310);
    horizontal->addWidget(fields, 1);
    horizontal->addWidget(preview_);

    const QList<QSpinBox*> sizeBoxes = {interfaceFontSizeBox_, editorFontSizeBox_,
                                        terminalFontSizeBox_, previewFontSizeBox_};
    for (QSpinBox* box : sizeBoxes) {
        connect(box, &QSpinBox::valueChanged, this, [this, box] {
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
    for (QSpinBox* box : {editorLineHeightBox_, terminalLineHeightBox_, previewLineHeightBox_}) {
        connect(box, &QSpinBox::valueChanged, this, [this] {
            updatePreview();
            updateDirtyState();
        });
    }
    connect(editorFontBox_, &QComboBox::currentTextChanged, this, [this](const QString& family) {
        editorFontFamily_ = family;
        updatePreview();
        updateDirtyState();
    });
    return card;
}

void SettingsDialog::updatePreview() {
    if (preview_ != nullptr) {
        preview_->setAppearanceSettings(appearanceSettings());
    }
}

} // namespace ketplus
