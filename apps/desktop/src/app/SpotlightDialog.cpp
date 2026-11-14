#include "app/SpotlightDialog.h"
#include "KetCommandPalette.h"
#include "KetIcons.h"
#include <QCloseEvent>
#include <QEvent>
#include <QLineEdit>
#include <QShowEvent>
#include <QSignalBlocker>
#include <algorithm>

namespace ketplus {
SpotlightDialog::SpotlightDialog(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint),
      palette_(new ket::CommandPalette(this)) {
    palette_->setParent(this);
    setObjectName("spotlightDialog");
    setWindowTitle(tr("ketPlus Quick search"));
    // Agent Workspace embeds a native QQuickView. A native popup keeps the kit
    // overlay above that child window on every platform, not behind it.
    setAttribute(Qt::WA_TranslucentBackground);
    if (parent)
        parent->installEventFilter(this);
    else
        resize(900, 700);
    query_ = palette_->searchField();
    query_->setObjectName("spotlightQuery");
    query_->setAccessibleName(tr("Search files and Atlases"));
    palette_->setPlaceholderText(tr("Search files and Atlases…"));
    connect(palette_, &ket::CommandPalette::queryChanged, this, &SpotlightDialog::filter);
    connect(palette_, &ket::CommandPalette::closed, this, &QWidget::close);
    connect(palette_, &ket::CommandPalette::activated, this, [this](const ket::CommandItem& item) {
        // The index may have refreshed between rendering and activation.
        for (const auto& entry : entries_) {
            if (entry.id == item.id && entry.available) {
                emit entryActivated(entry.id);
                break;
            }
        }
    });
    filter();
}
void SpotlightDialog::setEntries(const QVector<SpotlightEntry>& entries) {
    entries_ = entries;
    filter();
}
void SpotlightDialog::setQuery(const QString& query) { query_->setText(query); }
void SpotlightDialog::reposition() {
    if (parentWidget())
        setGeometry(QRect(parentWidget()->mapToGlobal(QPoint(0, 0)), parentWidget()->size()));
}
void SpotlightDialog::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    reposition();
    raise();
    const auto query = query_->text();
    palette_->open();
    query_->setText(query);
}
void SpotlightDialog::closeEvent(QCloseEvent* event) {
    const QSignalBlocker blocker(palette_);
    palette_->dismiss();
    QWidget::closeEvent(event);
}
bool SpotlightDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        reposition();
    return QWidget::eventFilter(watched, event);
}
void SpotlightDialog::filter() {
    const auto terms = query_->text().simplified().split(' ', Qt::SkipEmptyParts);
    QVector<QPair<int, SpotlightEntry>> matches;
    int unavailable = 0;
    for (const auto& entry : entries_) {
        const auto text =
            entry.title + " " + entry.subtitle + " " + entry.category + " " + entry.searchText;
        int score = 0;
        bool match = true;
        for (const auto& term : terms) {
            if (!text.contains(term, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
            if (entry.title.startsWith(term, Qt::CaseInsensitive))
                score += 2;
            else if (entry.title.contains(term, Qt::CaseInsensitive))
                ++score;
        }
        if (!match)
            continue;
        if (!entry.available) {
            ++unavailable;
            continue;
        }
        matches.append({score, entry});
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    QList<ket::CommandGroup> groups;
    int count = 0;
    for (const auto& match : matches) {
        if (++count > 200)
            break;
        const auto& entry = match.second;
        auto category = entry.category.toUpper();
        if (category == "ATLASES")
            category = "ATLAS";
        if (groups.isEmpty() || groups.last().title != category)
            groups.append({category, {}});
        groups.last().items.append(
            {entry.id, entry.title, entry.subtitle,
             ket::Icons::codeForLucide(entry.kind == "atlas" ? u"network" : u"file-code")});
    }
    palette_->setEmptyText(tr("No matching files or Atlases"),
                           unavailable > 0 ? tr("Matching entries are unavailable. Refresh the "
                                                "repository index or restore their files.")
                                           : tr("Try a file name or path in this repository."));
    palette_->setGroups(groups);
}
} // namespace ketplus
