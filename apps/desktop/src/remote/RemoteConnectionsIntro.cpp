#include "remote/RemoteConnectionsIntro.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "KetTokens.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "ui/SymbolIcon.h"
#include "ui/TypeScale.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>

#include <utility>

namespace ketplus {
namespace {

constexpr auto displayIcon = "monitor";
constexpr auto chatsIcon = "messages-square";
constexpr auto shieldIcon = "shield";
constexpr auto unplugIcon = "unplug";
constexpr auto logInIcon = "log-in";

// Below this width (Medium pixels) the scene goes under the copy and the steps stack.
constexpr int narrowWidth = 860;

// A Két Symbol glyph in the stylesheet's text colour for its role.
class GlyphLabel final : public QLabel {
  public:
    GlyphLabel(QByteArray body, const int size, QWidget* parent = nullptr)
        : QLabel(parent), body_(std::move(body)) {
        setProperty("remoteRole", QStringLiteral("introIcon"));
        chrome_geometry::fixedSize(this, size, size);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        symbol_icon::paint(painter, body_, palette().color(QPalette::WindowText), rect());
    }

  private:
    QByteArray body_;
};

QLabel* text(const QString& value, const char* role, QWidget* parent = nullptr) {
    auto* label = new QLabel(value, parent);
    label->setProperty("remoteRole", role);
    label->setWordWrap(true);
    return label;
}

QFrame* divider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("remoteRole", QStringLiteral("divider"));
    chrome_geometry::fixedHeight(line, 1);
    return line;
}

// An icon beside muted text, its first line level with the icon.
QWidget* iconNote(const char* icon, const int size, const QString& value, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    chrome_geometry::margins(layout, 0, 0, 0, 0);
    chrome_geometry::spacing(layout, 10);
    auto* glyph = new GlyphLabel(icon, size, row);
    layout->addWidget(glyph, 0, Qt::AlignTop);
    layout->addWidget(text(value, "introNote", row), 1);
    return row;
}

} // namespace

// This Mac's Agent Workspace and a phone running KetPlus Mobile, both waiting on the same kind of
// question: captures of the two mocks (mockups/qt/assets/connections), painted as in the mock.
class ConnectionsScene final : public QWidget {
  public:
    explicit ConnectionsScene(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("remoteConnectionsScene"));
        setAccessibleName(
            QStringLiteral("KetPlus Agent Workspace on a Mac and KetPlus Mobile on a phone"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setNarrow(false);
    }

    void setColors(const RemoteConnectionsIntro::SceneColors& colors) {
        if (colors.dark != colors_.dark)
            cache_.clear();
        colors_ = colors;
        update();
    }

    // The narrow scene is as wide as the mock's, or the space it has.
    [[nodiscard]] QSize sizeHint() const override {
        return {InterfaceZoomMetrics::geometry(500, chrome_geometry::basePixels(this)), height()};
    }

    void setNarrow(const bool narrow) {
        narrow_ = narrow;
        chrome_geometry::fixedHeight(this, narrow ? 380 : 400);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const double s = InterfaceZoomMetrics::factor(chrome_geometry::basePixels(this));
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(frame, 16 * s, 16 * s);
        QLinearGradient glow(frame.topLeft(), frame.bottomLeft());
        glow.setColorAt(0, colors_.glow);
        glow.setColorAt(0.7, colors_.page);
        painter.fillPath(clip, glow);
        painter.save();
        painter.setClipPath(clip);

        // The desktop window, with a title bar above its capture.
        const QRectF desktop(24 * s, 44 * s, width() - 79 * s, 286 * s);
        shadow(painter, desktop, 9 * s, 14 * s, 0.05);
        QPainterPath window;
        window.addRoundedRect(desktop, 9 * s, 9 * s);
        painter.fillPath(window, colors_.panel);
        const QRectF bar(desktop.left(), desktop.top(), desktop.width(), 26 * s);
        QPainterPath barPath;
        barPath.addRoundedRect(bar.adjusted(0, 0, 0, 9 * s), 9 * s, 9 * s);
        QPainterPath barClip;
        barClip.addRect(bar);
        painter.fillPath(barPath.intersected(barClip), colors_.page);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors_.strong);
        for (int dot = 0; dot < 3; ++dot)
            painter.drawEllipse(
                QRectF(bar.left() + (12 + dot * 10) * s, bar.center().y() - 3 * s, 6 * s, 6 * s));
        caption(painter, bar, displayIcon, 12, QStringLiteral("This Mac"), Qt::AlignCenter);
        painter.fillRect(QRectF(bar.left(), bar.bottom() - 1, bar.width(), 1), colors_.border);
        const QRectF shot = desktop.adjusted(1, bar.height(), -1, -1);
        QPainterPath shotPath;
        shotPath.addRoundedRect(shot.adjusted(0, -9 * s, 0, 0), 8 * s, 8 * s);
        QPainterPath shotClip;
        shotClip.addRect(shot);
        capture(painter, QStringLiteral("desktop-question"), shot, shotPath.intersected(shotClip));
        painter.setPen(QPen(colors_.strong, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(window);

        // The phone. Its bezel is near-black in both themes, like the hardware.
        const double bezel = 4 * s;
        const double screenWidth = (narrow_ ? 150 : 160) * s;
        const QRectF phone(width() - 18 * s - screenWidth - 2 * bezel, (narrow_ ? 14 : 26) * s,
                           screenWidth + 2 * bezel, screenWidth * 852 / 393 + 2 * bezel);
        shadow(painter, phone, 26 * s, 18 * s, 0.16);
        QPainterPath body;
        body.addRoundedRect(phone, 26 * s, 26 * s);
        painter.fillPath(body, ket::Tokens::color(u"page-bg", ket::Theme::Dark));
        painter.setPen(QPen(ket::Tokens::color(u"panel-border", ket::Theme::Dark), 1));
        painter.drawPath(body);
        const QRectF screen = phone.adjusted(bezel, bezel, -bezel, -bezel);
        QPainterPath screenPath;
        screenPath.addRoundedRect(screen, 22 * s, 22 * s);
        capture(painter, QStringLiteral("mobile-question"), screen, screenPath);

        const QRectF line((narrow_ ? 15 : 25) * s, height() - (narrow_ ? 12 : 23) * s - 16 * s,
                          width(), 16 * s);
        caption(painter, line, chatsIcon, 13,
                QStringLiteral("Your conversations. On every screen."), Qt::AlignLeft);
        painter.restore();

        painter.setPen(QPen(colors_.border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(clip);
    }

  private:
    // A soft drop shadow: a few widening, fading copies of the shape below it.
    static void shadow(QPainter& painter, const QRectF& shape, const double radius,
                       const double offset, const double alpha) {
        painter.save();
        painter.setPen(Qt::NoPen);
        for (int step = 1; step <= 6; ++step) {
            const double grow = offset * step / 6.0;
            QColor color(0, 0, 0);
            color.setAlphaF(alpha / 3.0 * (1.0 - step / 7.0));
            painter.setBrush(color);
            painter.drawRoundedRect(
                shape.translated(0, offset * 0.6).adjusted(-grow, 0, grow, grow), radius + grow,
                radius + grow);
        }
        painter.restore();
    }

    // Muted text after a glyph, centred in `area` or from its left edge.
    void caption(QPainter& painter, const QRectF& area, const char* icon, const int iconSize,
                 const QString& value, const Qt::Alignment alignment) const {
        const int base = chrome_geometry::basePixels(this);
        const double s = InterfaceZoomMetrics::factor(base);
        QFont font = this->font();
        font.setPixelSize(TypeScale::pixels(TypeStep::Caption, base));
        const QFontMetricsF metrics(font);
        const double glyph = iconSize * s;
        const double gap = 6 * s;
        const double total = glyph + gap + metrics.horizontalAdvance(value);
        const double left =
            alignment & Qt::AlignHCenter ? area.center().x() - total / 2 : area.left();
        symbol_icon::paint(painter, icon, colors_.muted,
                           QRectF(left, area.center().y() - glyph / 2, glyph, glyph));
        painter.save();
        painter.setFont(font);
        painter.setPen(colors_.muted);
        painter.drawText(QRectF(left + glyph + gap, area.top(), total, area.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, value);
        painter.restore();
    }

    // A capture cropped to fill `area` from its top-left corner, as the mock's PreserveAspectCrop.
    void capture(QPainter& painter, const QString& name, const QRectF& area,
                 const QPainterPath& shape) {
        const qreal ratio = devicePixelRatioF();
        const QSize pixels = (area.size() * ratio).toSize();
        const QString key = name + QLatin1Char(colors_.dark ? 'd' : 'l');
        QPixmap& scaled = cache_[key];
        if (scaled.isNull() || scaled.size() != pixels) {
            const QPixmap source(
                QStringLiteral(":/connections/%1-%2.png")
                    .arg(name, colors_.dark ? QStringLiteral("dark") : QStringLiteral("light")));
            if (source.isNull())
                return;
            const double scale = qMax(double(pixels.width()) / source.width(),
                                      double(pixels.height()) / source.height());
            const QSize crop(qMin(source.width(), qCeil(pixels.width() / scale)),
                             qMin(source.height(), qCeil(pixels.height() / scale)));
            scaled = source.copy(QRect(QPoint(0, 0), crop))
                         .scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(ratio);
        }
        painter.save();
        painter.setClipPath(shape, Qt::IntersectClip);
        painter.drawPixmap(area.topLeft(), scaled);
        painter.restore();
    }

    RemoteConnectionsIntro::SceneColors colors_;
    QHash<QString, QPixmap> cache_;
    bool narrow_{false};
};

RemoteConnectionsIntro::RemoteConnectionsIntro(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("remoteConnectionsWelcome"));
    auto* root = new QVBoxLayout(this);
    chrome_geometry::margins(root, 0, 0, 0, 0);
    chrome_geometry::spacing(root, 0);

    auto* header = new QHBoxLayout;
    chrome_geometry::spacing(header, 7);
    auto* title = text(QStringLiteral("Connections"), "introTitle", this);
    title->setObjectName(QStringLiteral("remoteWelcomeTitle"));
    title->setWordWrap(false);
    header->addWidget(title, 1);
    header->addWidget(new GlyphLabel(unplugIcon, 14, this), 0, Qt::AlignVCenter);
    auto* state = text(QStringLiteral("Not connected"), "introState", this);
    state->setObjectName(QStringLiteral("remoteWelcomeState"));
    state->setWordWrap(false);
    header->addWidget(state, 0, Qt::AlignVCenter);
    root->addLayout(header);

    expired_ = new QFrame(this);
    expired_->setObjectName(QStringLiteral("remoteExpiredBanner"));
    expired_->setProperty("remoteRole", QStringLiteral("notice"));
    expired_->setProperty("remoteTone", QStringLiteral("warning"));
    auto* expiredLayout = new QVBoxLayout(expired_);
    chrome_geometry::margins(expiredLayout, 14, 10, 14, 10);
    chrome_geometry::spacing(expiredLayout, 2);
    expiredLayout->addWidget(text(QStringLiteral("Your login expired"), "noticeTitle", expired_));
    expiredLayout->addWidget(text(QStringLiteral("Please sign in again. Local work is preserved."),
                                  "noticeText", expired_));
    expired_->hide();
    chrome_geometry::addSpacing(root, 24);
    root->addWidget(expired_);

    notice_ = text({}, "noticeText", this);
    notice_->setObjectName(QStringLiteral("remoteWelcomeNotice"));
    notice_->hide();
    root->addWidget(notice_);
    chrome_geometry::addSpacing(root, 24);

    hero_ = new QBoxLayout(QBoxLayout::LeftToRight);
    chrome_geometry::spacing(hero_, 28);
    copy_ = new QWidget(this);
    auto* copy = new QVBoxLayout(copy_);
    chrome_geometry::margins(copy, 0, 10, 0, 10);
    chrome_geometry::spacing(copy, 0);
    copy->addWidget(text(QStringLiteral("KETPLUS CONNECTIONS"), "eyebrow", copy_));
    chrome_geometry::addSpacing(copy, 16);
    auto* headline = text(QStringLiteral("Your workspace.\nWithin reach."), "heroTitle", copy_);
    headline->setObjectName(QStringLiteral("remoteWelcomeHeadline"));
    copy->addWidget(headline);
    chrome_geometry::addSpacing(copy, 20);
    heroText_ =
        text(QStringLiteral("Continue a conversation from your phone, or connect a headless "
                            "Linux machine to work in its repositories from this desktop."),
             "heroCopy", copy_);
    heroText_->setObjectName(QStringLiteral("remoteWelcomeText"));
    copy->addWidget(heroText_);
    chrome_geometry::addSpacing(copy, 28);
    signIn_ = new ket::Button(QStringLiteral("Sign in"), ket::ButtonVariant::Primary, copy_);
    signIn_->setObjectName(QStringLiteral("remoteSignInPromptButton"));
    signIn_->setProperty("remotePrimary", true);
    signIn_->setIcon(QIcon(symbol_icon::pixmap(logInIcon, Qt::white, QSize(16, 16))));
    chrome_geometry::iconSize(signIn_, QSize(15, 15));
    connect(signIn_, &QAbstractButton::clicked, this, &RemoteConnectionsIntro::signInRequested);
    copy->addWidget(signIn_, 0, Qt::AlignLeft);
    chrome_geometry::addSpacing(copy, 11);
    copy->addWidget(
        text(QStringLiteral("Use your KetSuite account to get started."), "introNote", copy_));
    chrome_geometry::addSpacing(copy, 30);
    copy->addWidget(iconNote(displayIcon, 16,
                             QStringLiteral("Your local workspace keeps working.\nSign in when "
                                            "you’re ready to connect a device."),
                             copy_));
    copy->addStretch();
    hero_->addWidget(copy_, 45);
    scene_ = new ConnectionsScene(this);
    hero_->addWidget(scene_, 55);
    root->addLayout(hero_);

    chrome_geometry::addSpacing(root, 54);
    root->addWidget(divider(this));
    chrome_geometry::addSpacing(root, 28);

    stepsHeader_ = new QGridLayout;
    chrome_geometry::verticalSpacing(stepsHeader_, 8);
    auto* stepsTitle = text(QStringLiteral("From this Mac to your phone"), "introSection", this);
    stepsTitle->setWordWrap(false);
    stepsHeader_->addWidget(stepsTitle, 0, 0);
    stepsNote_ =
        text(QStringLiteral("Three steps. You approve the connection."), "introNote", this);
    root->addLayout(stepsHeader_);
    chrome_geometry::addSpacing(root, 28);

    steps_ = new QGridLayout;
    chrome_geometry::horizontalSpacing(steps_, 28);
    chrome_geometry::verticalSpacing(steps_, 22);
    const std::pair<const char*, const char*> stepTexts[] = {
        {"Sign in on this Mac", "Use the same KetSuite account on both devices."},
        {"Scan with KetPlus Mobile", "Open the app on your phone and scan the pairing code."},
        {"Confirm, then continue", "Match the codes and approve your phone on this Mac."}};
    for (int index = 0; index < 3; ++index) {
        auto* cell = new QWidget(this);
        cell->setObjectName(QStringLiteral("remoteWelcomeStep%1").arg(index + 1));
        auto* row = new QHBoxLayout(cell);
        chrome_geometry::margins(row, 0, 0, 0, 0);
        chrome_geometry::spacing(row, 12);
        auto* numberColumn = new QVBoxLayout;
        chrome_geometry::spacing(numberColumn, 6);
        auto* bar = new QFrame(cell);
        bar->setProperty("remoteRole", QStringLiteral("introStepBar"));
        chrome_geometry::fixedHeight(bar, 2);
        numberColumn->addWidget(bar);
        auto* number = text(QStringLiteral("0%1").arg(index + 1), "introStepNumber", cell);
        number->setWordWrap(false);
        numberColumn->addWidget(number);
        numberColumn->addStretch();
        row->addLayout(numberColumn);
        auto* words = new QVBoxLayout;
        chrome_geometry::margins(words, 0, 8, 0, 0);
        chrome_geometry::spacing(words, 7);
        auto* stepTitle = text(QString::fromUtf8(stepTexts[index].first), "introStepTitle", cell);
        cell->setAccessibleName(QStringLiteral("Step %1: %2. %3")
                                    .arg(index + 1)
                                    .arg(QString::fromUtf8(stepTexts[index].first),
                                         QString::fromUtf8(stepTexts[index].second)));
        words->addWidget(stepTitle);
        words->addWidget(text(QString::fromUtf8(stepTexts[index].second), "introNote", cell));
        words->addStretch();
        row->addLayout(words, 1);
        stepCells_.append(cell);
    }
    root->addLayout(steps_);

    chrome_geometry::addSpacing(root, 28);
    root->addWidget(divider(this));
    chrome_geometry::addSpacing(root, 16);
    root->addWidget(iconNote(shieldIcon, 15,
                             QStringLiteral("You choose which devices can connect. Review or "
                                            "revoke their access here at any time."),
                             this));
    applyWidth();
}

void RemoteConnectionsIntro::setExpired(const bool expired) { expired_->setVisible(expired); }

void RemoteConnectionsIntro::setSceneColors(const SceneColors& colors) {
    scene_->setColors(colors);
}

void RemoteConnectionsIntro::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyWidth();
}

void RemoteConnectionsIntro::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        applyWidth();
}

void RemoteConnectionsIntro::applyWidth() {
    const int base = chrome_geometry::basePixels(this);
    const bool narrow = width() < InterfaceZoomMetrics::geometry(narrowWidth, base);
    if (laidOut_ && narrow == narrow_)
        return;
    laidOut_ = true;
    narrow_ = narrow;

    hero_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    hero_->setStretchFactor(copy_, narrow ? 0 : 45);
    hero_->setStretchFactor(scene_, narrow ? 0 : 55);
    hero_->setAlignment(scene_, narrow ? Qt::AlignLeft : Qt::Alignment());
    chrome_geometry::maximumWidth(scene_, narrow ? 500 : QWIDGETSIZE_MAX);
    chrome_geometry::maximumWidth(heroText_, narrow ? 480 : 380);
    scene_->setNarrow(narrow);

    // The note sits on the heading's line, or under it when narrow.
    stepsHeader_->removeWidget(stepsNote_);
    if (narrow)
        stepsHeader_->addWidget(stepsNote_, 1, 0);
    else
        stepsHeader_->addWidget(stepsNote_, 0, 1, Qt::AlignRight | Qt::AlignVCenter);
    stepsNote_->setAlignment(narrow ? Qt::AlignLeft : Qt::AlignRight);
    stepsNote_->setWordWrap(narrow);

    for (QWidget* cell : std::as_const(stepCells_))
        steps_->removeWidget(cell);
    for (int index = 0; index < stepCells_.size(); ++index)
        steps_->addWidget(stepCells_[index], narrow ? index : 0, narrow ? 0 : index);
    for (int column = 0; column < 3; ++column)
        steps_->setColumnStretch(column, narrow ? (column == 0 ? 1 : 0) : 1);
}

} // namespace ketplus
