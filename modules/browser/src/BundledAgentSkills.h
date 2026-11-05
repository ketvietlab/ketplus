#pragma once

#include <QList>
#include <QString>

namespace ketplus::agent_skills {

// The directory name below Contents/Resources (macOS) or next to the executable.
inline constexpr char bundledSkillsDirName[] = "agent-skills";

struct BundledSkill {
    QString name;
    QString file; // Canonical path of the skill's SKILL.md.
};

// Skills KetPlus ships for every workspace, in policy order. Missing skills are omitted.
// applicationDirPath may be the desktop's Contents/MacOS or the daemon's Contents/Helpers.
[[nodiscard]] QList<BundledSkill> bundledSkills(const QString& applicationDirPath);

// When agents read the bundled skills without the user mentioning them. Empty when the
// application ships none.
[[nodiscard]] QString defaultSkillInstructions(const QList<BundledSkill>& skills);

// defaultSkillInstructions for the running application.
[[nodiscard]] QString defaultSkillInstructions();

} // namespace ketplus::agent_skills
