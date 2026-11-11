#include "agent/WorkReportPane.h"

#include "KetAppearance.h"
#include "KetTokens.h"
#include "ui/InterfaceFont.h"

#include <QQmlContext>
#include <QQuickView>
#include <QUrl>
#include <QVBoxLayout>

namespace ketplus {

WorkReportPane::WorkReportPane(QWidget* parent) : QWidget(parent), view_(new QQuickView) {
    setObjectName(QStringLiteral("workReportPane"));
    auto* container = QWidget::createWindowContainer(view_, this);
    container->setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(container);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    const auto applyTheme = [this] {
        view_->setColor(ket::Tokens::color(u"page-bg", ket::Appearance::instance().theme()));
    };
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this, applyTheme);
    applyTheme();
    view_->rootContext()->setContextProperty(QStringLiteral("reportHost"), this);
    static_cast<void>(InterfaceUiFont::resolvedFamily());
    view_->setSource(QUrl(QStringLiteral("qrc:/agent/WorkReportPane.qml")));
}

// The scene goes before this host, so no binding reads a destroyed reportHost.
WorkReportPane::~WorkReportPane() { view_->setSource(QUrl()); }

void WorkReportPane::showConversation(const QString& conversationId) {
    if (conversationId == conversationId_ && !report_.isEmpty())
        return;
    conversationId_ = conversationId;
    report_.clear();
    message_.clear();
    loading_ = !conversationId.isEmpty();
    emit reportChanged();
}

void WorkReportPane::acceptReport(const QString& conversationId, const QVariantMap& report,
                                  const QString& error) {
    if (conversationId != conversationId_)
        return;
    const auto version = [](const QVariantMap& ledger) {
        return ledger.value(QStringLiteral("version")).toLongLong();
    };
    // A failed refresh keeps the report already shown.
    if (!error.isEmpty() && !report_.isEmpty()) {
        loading_ = false;
        emit reportChanged();
        return;
    }
    if (error.isEmpty() && !report_.isEmpty() && version(report) < version(report_)) {
        loading_ = false;
        emit reportChanged();
        return;
    }
    report_ = error.isEmpty() ? report : QVariantMap{};
    message_ = error;
    loading_ = false;
    emit reportChanged();
}

} // namespace ketplus
