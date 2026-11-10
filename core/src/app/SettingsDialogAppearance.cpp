#include "app/SettingsDialog.h"

#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"

#include "KetFieldRow.h"
#include "KetSegmentedControl.h"
#include "KetThemeCard.h"
#include "KetToggle.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ketplus {

namespace {

QLabel* makeLabel(const QString& text, const QString& role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("kvRole", role);
    label->setWordWrap(true);
    return label;
}

QWidget* makePageHeading(const QString& eyebrow, const QString& title, const QString& description,
                         QWidget* parent) {
    auto* heading = new QWidget(parent);
    auto* layout = new QVBoxLayout(heading);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    if (!eyebrow.isEmpty())
        layout->addWidget(makeLabel(eyebrow, QStringLiteral("settingsEyebrow"), heading));
    layout->addWidget(makeLabel(title, QStringLiteral("settingsTitle"), heading));
    layout->addWidget(makeLabel(description, QStringLiteral("settingsDescription"), heading));
    return heading;
}

} // namespace

QWidget* SettingsDialog::createAppearancePage() {
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(20);
    layout->addWidget(makePageHeading(QString{}, QStringLiteral("Appearance"),
                                      QStringLiteral("Choose your theme and interface size."),
                                      content));

    layout->addWidget(
        makeLabel(QStringLiteral("COLOR THEME"), QStringLiteral("settingsEyebrow"), content));
    layout->addWidget(createThemeCard());
    layout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("appearanceSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QWidget* SettingsDialog::createThemeCard() {
    auto* card = new QWidget(this);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);

    followSystem_ = new ket::Checkbox(QStringLiteral("Follow system appearance"), card);
    followSystem_->setObjectName(QStringLiteral("followSystemTheme"));
    followSystem_->setToolTip(
        QStringLiteral("Use Light or Dark with the operating system appearance"));
    layout->addWidget(followSystem_);

    auto* themes = new QHBoxLayout;
    themes->setSpacing(12);
    for (const auto mode : {ThemeManager::Mode::Light, ThemeManager::Mode::Dark}) {
        const bool light = mode == ThemeManager::Mode::Light;
        auto* preview = new ket::ThemeCard(light ? tr("Light") : tr("Dark"),
                                           light ? ket::Theme::Light : ket::Theme::Dark, card);
        preview->setObjectName(light ? QStringLiteral("lightThemeCard")
                                     : QStringLiteral("darkThemeCard"));
        preview->setProperty("themeMode", static_cast<int>(mode));
        themeCards_.append(preview);
        themes->addWidget(preview);
        connect(preview, &QAbstractButton::clicked, this,
                [this, mode] { selectThemeMode(static_cast<int>(mode)); });
    }
    layout->addLayout(themes);
    auto* accent = new QWidget(card);
    auto* accentRow = new QHBoxLayout(accent);
    accentRow->setContentsMargins(0, 0, 0, 0);
    accentRow->addWidget(new ket::AccentSwatch(accent));
    accentRow->addWidget(new QLabel(tr("Indigo"), accent));
    accentRow->addStretch();
    layout->addWidget(new ket::FieldRow(tr("Accent"), accent, {}, card));
    QStringList zoomSteps;
    for (const int percent : InterfaceZoomMetrics::steps())
        zoomSteps.append(QString::number(percent) + u'%');
    zoomControl_ = new ket::SegmentedControl(zoomSteps, card);
    zoomControl_->setObjectName(QStringLiteral("interfaceZoom"));
    zoomControl_->setAccessibleName(tr("Interface zoom"));
    zoomControl_->setCurrentIndex(
        int(InterfaceZoomMetrics::steps().indexOf(InterfaceZoomMetrics::defaultPercent)));
    layout->addWidget(new ket::FieldRow(tr("Interface zoom"), zoomControl_, {}, card));
    connect(zoomControl_, &ket::SegmentedControl::currentChanged, this, [this] {
        updatePreview();
        applyChanges();
    });

    connect(followSystem_, &QAbstractButton::toggled, this, [this](bool checked) {
        selectThemeMode(
            static_cast<int>(checked ? ThemeManager::Mode::System
                                     : (currentThemePalette().dark ? ThemeManager::Mode::Dark
                                                                   : ThemeManager::Mode::Light)));
    });
    return card;
}

} // namespace ketplus
