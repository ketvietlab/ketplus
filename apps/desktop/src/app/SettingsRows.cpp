#include "app/SettingsRows.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace ketplus::settings_rows {

QWidget* field(const QString& title, const QString& help, QWidget* control, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(20);
    auto* copy = new QWidget(row);
    auto* copyLayout = new QVBoxLayout(copy);
    copyLayout->setContentsMargins(0, 0, 0, 0);
    copyLayout->setSpacing(2);
    auto* titleLabel = new QLabel(title, copy);
    titleLabel->setObjectName(QStringLiteral("settingsWorkspaceFieldLabel"));
    titleLabel->setWordWrap(true);
    copyLayout->addWidget(titleLabel);
    if (!help.isEmpty()) {
        auto* helpLabel = new QLabel(help, copy);
        helpLabel->setObjectName(QStringLiteral("settingsWorkspaceHelp"));
        helpLabel->setWordWrap(true);
        copyLayout->addWidget(helpLabel);
    }
    layout->addWidget(copy, 1, Qt::AlignTop);
    control->setParent(row);
    control->setFixedWidth(200);
    layout->addWidget(control, 0, Qt::AlignRight | Qt::AlignTop);
    return row;
}

QFrame* divider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("settingsWorkspaceDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

} // namespace ketplus::settings_rows
