#include "app/ProviderSetupCard.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetRichRecipes.h"
#include "KetTokens.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QLabel* label(const QString& text, const char* name, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(QLatin1String(name));
    value->setTextFormat(Qt::PlainText); // provider messages are not markup
    value->setWordWrap(true);
    return value;
}

QFont monoFont(const QWidget* reference) {
    QFont font = reference->font();
    const QString family = ket::Profile::current().monoFamily();
    if (!family.isEmpty())
        font.setFamily(family);
    font.setStyleHint(QFont::Monospace);
    return font;
}

} // namespace

ProviderSetupCard::ProviderSetupCard(const QVariantMap& guide, QWidget* parent)
    : QFrame(parent), guide_(guide) {
    setObjectName(QStringLiteral("providerSetupCard"));
    setProperty("setupId", guideId());
    setAccessibleName(guide.value(QStringLiteral("title")).toString());
    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 16, 14, 16, 14);
    chrome_geometry::spacing(layout, 12);

    // Outside a turn the summary says it; inside, the reason.
    const QString summary = guide.value(QStringLiteral("summary")).toString();
    layout->addWidget(
        label(summary.isEmpty() ? guide.value(QStringLiteral("reason")).toString() : summary,
              "providerSetupReason", this));

    // The provider's own words, so a search or a support request finds the same text.
    const QString error = guide.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        auto* said = new QHBoxLayout;
        chrome_geometry::spacing(said, 8);
        const QString source = guide.value(QStringLiteral("source")).toString().isEmpty()
                                   ? guide.value(QStringLiteral("name")).toString()
                                   : guide.value(QStringLiteral("source")).toString();
        auto* who = label(tr("%1 said").arg(source), "providerSetupSource", this);
        who->setWordWrap(false);
        said->addWidget(who, 0, Qt::AlignTop);
        auto* words = label(error, "providerSetupError", this);
        words->setFont(monoFont(this));
        words->setTextInteractionFlags(Qt::TextSelectableByMouse);
        said->addWidget(words, 1);
        layout->addLayout(said);
    }

    int number = 0;
    for (const QVariant& entry : guide.value(QStringLiteral("steps")).toList()) {
        const QVariantMap step = entry.toMap();
        if (step.value(QStringLiteral("turn")).toBool())
            continue; // Retry and the like belong to the conversation
        auto* row = new QHBoxLayout;
        chrome_geometry::spacing(row, 10);
        auto* index = new QLabel(QString::number(++number), this);
        index->setObjectName(QStringLiteral("providerSetupStepNumber"));
        index->setAlignment(Qt::AlignCenter);
        chrome_geometry::fixedSize(index, 20, 20);
        row->addWidget(index, 0, Qt::AlignTop);
        auto* body = new QVBoxLayout;
        chrome_geometry::spacing(body, 4);
        body->addWidget(
            label(step.value(QStringLiteral("text")).toString(), "providerSetupStep", this));
        const QString command = step.value(QStringLiteral("command")).toString();
        if (!command.isEmpty()) {
            // The mock's KetCodeBlock: the code block recipe's padding, and Copy in the corner.
            const ket::CodeBlockRecipe recipe =
                ket::codeBlockRecipe(ket::Appearance::instance().theme());
            const int padding = qRound(recipe.padding);
            auto* block = new QFrame(this);
            block->setObjectName(QStringLiteral("providerSetupCommand"));
            auto* blockLayout = new QHBoxLayout(block);
            // Copy sits half a padding from the corner; the code keeps the full padding.
            chrome_geometry::margins(blockLayout, padding, padding / 2, padding / 2, padding / 2);
            chrome_geometry::spacing(blockLayout, 8);
            auto* code = label(command, "providerSetupCommandText", block);
            code->setFont(monoFont(this));
            code->setTextInteractionFlags(Qt::TextSelectableByMouse);
            auto* codeColumn = new QVBoxLayout;
            chrome_geometry::margins(codeColumn, 0, padding - padding / 2, 0,
                                     padding - padding / 2);
            codeColumn->addWidget(code);
            blockLayout->addLayout(codeColumn, 1);
            auto* copy =
                new ket::IconButton(ket::Icons::icon(u"copy"), tr("Copy %1").arg(command), block);
            copy->setObjectName(QStringLiteral("providerSetupCopy"));
            copy->setVariant(ket::ButtonVariant::Ghost);
            copy->setControlSize(ket::ControlSize::Small);
            connect(copy, &QAbstractButton::clicked, this, [this, command] {
                QApplication::clipboard()->setText(command);
                emit commandCopied(command);
            });
            blockLayout->addWidget(copy, 0, Qt::AlignTop);
            auto* gap = new QWidget(this); // the block's margin, less the step spacing
            chrome_geometry::fixedHeight(gap, qRound(recipe.margin) - 4);
            body->addWidget(gap);
            body->addWidget(block);
            auto* type = new ket::Button(tr("Type in Terminal"), ket::ButtonVariant::Ghost, this);
            type->setObjectName(QStringLiteral("providerSetupTypeInTerminal"));
            type->setIcon(ket::Icons::icon(u"terminal"));
            type->setControlSize(ket::ControlSize::Small);
            type->setAccessibleDescription(
                tr("Opens a terminal with the command typed; press Return to run it"));
            connect(type, &QAbstractButton::clicked, this,
                    [this, command] { emit typeInTerminalRequested(command); });
            auto* typeRow = new QHBoxLayout;
            typeRow->addWidget(type);
            typeRow->addStretch(1);
            body->addLayout(typeRow);
        }
        row->addLayout(body, 1);
        layout->addLayout(row);
    }

    still_ = label(guide.value(QStringLiteral("still")).toString(), "providerSetupStill", this);
    still_->hide();
    layout->addWidget(still_);

    // Settings checks the provider again; only a link to another page (Updates) keeps its own
    // action, as the mock's inSettings card.
    const QString action = guide.value(QStringLiteral("primaryAction")).toString();
    const bool navigates = action.startsWith(QStringLiteral("settings/")) &&
                           action != QStringLiteral("settings/providers");
    primaryAction_ = navigates ? action : QString();
    primaryText_ =
        navigates ? guide.value(QStringLiteral("primary")).toString() : tr("Check again");
    primary_ = new ket::Button(primaryText_, ket::ButtonVariant::Primary, this);
    primary_->setObjectName(QStringLiteral("providerSetupPrimary"));
    connect(primary_, &QAbstractButton::clicked, this, [this] {
        if (primaryAction_.isEmpty())
            emit checkAgainRequested();
        else
            emit actionRequested(primaryAction_);
    });
    auto* actions = new QHBoxLayout;
    actions->addWidget(primary_);
    actions->addStretch(1);
    layout->addLayout(actions);

    applyColors();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            [this] { applyColors(); });
}

void ProviderSetupCard::setChecking(const bool checking) {
    primary_->setEnabled(!checking);
    primary_->setText(checking ? tr("Checking…") : primaryText_);
    if (checking)
        still_->hide();
}

void ProviderSetupCard::showStillFailing() {
    if (!still_->text().isEmpty())
        still_->show();
}

void ProviderSetupCard::applyColors() {
    const auto theme = ket::Appearance::instance().theme();
    const auto color = [theme](QStringView name) {
        return ket::Tokens::color(name, theme).name(QColor::HexArgb);
    };
    setStyleSheet(
        QStringLiteral("QLabel#providerSetupReason { color:%1; }"
                       "QLabel#providerSetupSource { color:%2; }"
                       "QLabel#providerSetupError { color:%1; }"
                       "QLabel#providerSetupStep { color:%3; }"
                       "QLabel#providerSetupStepNumber { color:%1; background:%4; border:1px solid "
                       "%5; border-radius:10px; font-weight:500; }"
                       "QFrame#providerSetupCommand { background:%4; border:1px solid %5; "
                       "border-radius:%7px; }"
                       "QLabel#providerSetupCommandText { color:%3; background:transparent; }"
                       "QLabel#providerSetupStill { color:%6; }")
            .arg(color(u"text-secondary"), color(u"text-muted"), color(u"text-main"),
                 color(u"panel-bg-subtle"), color(u"panel-border"), color(u"danger"))
            .arg(InterfaceZoomMetrics::geometry(qRound(ket::codeBlockRecipe(theme).radius),
                                                InterfaceZoomMetrics::current())));
    update();
}

void ProviderSetupCard::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    const auto theme = ket::Appearance::instance().theme();
    const bool danger = guide_.value(QStringLiteral("tone")).toString() == QStringLiteral("danger");
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal radius = InterfaceZoomMetrics::geometry(8, InterfaceZoomMetrics::current());
    painter.setPen(
        QPen(ket::Tokens::color(danger ? u"danger-border" : u"warning-border", theme), 1));
    painter.setBrush(ket::Tokens::color(u"panel-bg", theme));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

} // namespace ketplus
