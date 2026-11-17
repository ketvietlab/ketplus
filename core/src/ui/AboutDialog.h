#pragma once

#include "ui/BrandLockup.h"
#include <QString>
#include <QUrl>

class QWidget;

namespace ketplus {

struct AboutInformation {
    QString revision;
    QUrl source;
    QUrl notices;
    QUrl licenses;
};

// Resolve only installed layouts, never a source/build-tree fallback. Also used by
// packaging tests to exercise each platform's directory contract independently.
AboutInformation aboutInformation(BrandLockup::Edition edition, const QString& applicationDirectory,
                                  const QString& revision);

// Shared by both editions and Terminal, without depending on any Pro module.
void showAboutDialog(QWidget* parent, BrandLockup::Edition edition);
void showAboutDialog(QWidget* parent, BrandLockup::Edition edition,
                     const AboutInformation& information);

} // namespace ketplus
