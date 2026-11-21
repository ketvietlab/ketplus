// The tree's own check of its geometry, as the mock's Sidebar.layoutProblems: the section and the
// conversations of no team start at their repository's name, a team's members at the team's name
// with their marks under the team's mark,
// and every conversation is one line of the kit's height that fits the sidebar.
#include "KetAppearance.h"
#include "KetSidebarTree.h"
#include "workspace/SidebarRepositoryList.h"

#include <cmath>

namespace ketplus {
namespace {

QString where(const QWidget* part) {
    const QString name = part->accessibleName();
    return name.isEmpty() ? part->objectName() : name;
}

int leftIn(const QWidget* list, const QWidget* part, const QRect& rect) {
    return part->mapTo(list, rect.topLeft()).x();
}

} // namespace

QStringList SidebarRepositoryList::layoutProblems() const {
    QStringList problems;
    int rows = 0;
    const auto check = [&problems](const bool holds, const QString& problem) {
        if (!holds)
            problems << problem;
    };
    for (QWidget* group : groups_) {
        if (!group->isVisible())
            continue;
        auto* header = group->findChild<ket::SidebarRepositoryHeader*>(
            QStringLiteral("productRepositoryGroupButton"), Qt::FindDirectChildrenOnly);
        if (header == nullptr)
            continue;
        const int heading = leftIn(this, header, header->nameRect());
        for (auto* section : group->findChildren<ket::SidebarSectionLabel*>()) {
            if (!section->isVisible())
                continue;
            const int edge = leftIn(this, section, section->textRect());
            check(std::abs(edge - heading) <= 1,
                  QStringLiteral("%1: section text at %2, heading at %3")
                      .arg(where(section))
                      .arg(edge)
                      .arg(heading));
        }
        for (auto* branch : group->findChildren<ket::SidebarTeamBranch*>()) {
            if (!branch->isVisible())
                continue;
            // A team heads its members: their titles start at its name.
            const int name = leftIn(this, branch, branch->nameRect());
            const int mark = leftIn(this, branch, branch->markRect());
            for (auto* member : branch->parentWidget()->findChildren<ket::SidebarRow*>()) {
                if (!member->isVisible())
                    continue;
                const int title = leftIn(this, member, member->titleRect());
                check(std::abs(title - name) <= 1, QStringLiteral("%1: title at %2, team %3 at %4")
                                                       .arg(where(member))
                                                       .arg(title)
                                                       .arg(branch->name())
                                                       .arg(name));
                const int memberMark = leftIn(this, member, member->markRect());
                check(std::abs(memberMark - mark) <= 1,
                      QStringLiteral("%1: member mark at %2, team mark at %3")
                          .arg(where(member))
                          .arg(memberMark)
                          .arg(mark));
            }
        }
        for (auto* row : group->findChildren<ket::SidebarRow*>()) {
            if (!row->isVisible())
                continue;
            ++rows;
            if (row->level() == 0) {
                const int edge = leftIn(this, row, row->titleRect());
                check(std::abs(edge - heading) <= 1,
                      QStringLiteral("%1: title at %2, heading at %3")
                          .arg(where(row))
                          .arg(edge)
                          .arg(heading));
            }
            // The recipe is in design pixels; the row scales it by the zoom.
            const int height = qRound(
                ket::Appearance::scale(row->recipe().height, ket::Appearance::instance().zoom()));
            check(row->height() == height, QStringLiteral("%1: %2 px high, one line is %3")
                                               .arg(where(row))
                                               .arg(row->height())
                                               .arg(height));
            check(row->titleRect().left() < row->titleRect().right() &&
                      row->mapTo(this, QPoint(row->width(), 0)).x() <= width(),
                  QStringLiteral("%1: does not fit the sidebar").arg(where(row)));
        }
    }
    if (rows == 0)
        problems << QStringLiteral("no conversation rows to check");
    return problems;
}

} // namespace ketplus
