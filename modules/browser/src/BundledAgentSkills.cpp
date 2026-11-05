#include "BundledAgentSkills.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace ketplus::agent_skills {

namespace {

const char* const skillNames[] = {"ket-atlas", "enterprise-grade", "ket-design-system"};

QString skillsRoot(const QString& applicationDirPath) {
    if (applicationDirPath.isEmpty())
        return {};
    const QDir appDir(applicationDirPath);
    const QString name = QLatin1String(bundledSkillsDirName);
    for (const QString& candidate :
         {appDir.filePath(QStringLiteral("../Resources/") + name),
          appDir.filePath(QStringLiteral("../lib/ketplus/") + name), appDir.filePath(name)}) {
        const QFileInfo info(QDir::cleanPath(candidate));
        if (info.isDir())
            return info.canonicalFilePath();
    }
    return {};
}

} // namespace

QList<BundledSkill> bundledSkills(const QString& applicationDirPath) {
    const QString root = skillsRoot(applicationDirPath);
    if (root.isEmpty())
        return {};
    QList<BundledSkill> skills;
    for (const char* name : skillNames) {
        const QFileInfo file(
            QDir(root).filePath(QLatin1String(name) + QStringLiteral("/SKILL.md")));
        const QString canonical = file.canonicalFilePath();
        // A skill must stay inside the bundle; a link out of it is not a shipped skill.
        if (file.isFile() && canonical.startsWith(root + QLatin1Char('/')))
            skills.append({QLatin1String(name), canonical});
    }
    return skills;
}

QString defaultSkillInstructions(const QList<BundledSkill>& skills) {
    if (skills.isEmpty())
        return {};
    const auto when = [](const QString& name) -> QString {
        if (name == QLatin1String("ket-atlas"))
            return QStringLiteral(
                "the user asks for a mock, mockup, clickable prototype, workflow map, KetAtlas "
                "or atlas.json output, or you are asked to build or update a KetAtlas bundle "
                "(including requests from the KetPlus Atlas screen).");
        if (name == QLatin1String("enterprise-grade"))
            return QStringLiteral(
                "you design or implement UI or frontend work: screens, components, layouts, "
                "styling, mockups or visual polish.");
        return QStringLiteral(
            "you do UI, frontend or mockup work and the user has not named or supplied a "
            "design system. Treat Két Design System as the user's choice and do not ask them "
            "to pick one; this answers the ket-atlas design-system question. When the user "
            "names or supplies another design system, use theirs instead.");
    };
    QString text = QStringLiteral(
        "[KetPlus default skills]\n"
        "KetPlus ships these skills for every workspace. When a task matches, read that "
        "skill's SKILL.md before starting and follow it, reading only the reference files it "
        "points you to. Do not read them for unrelated tasks. A project or user skill with the "
        "same name takes precedence; if it is already loaded, do not read this copy.");
    for (const BundledSkill& skill : skills)
        text +=
            QStringLiteral("\n- %1 (%2): when %3").arg(skill.name, skill.file, when(skill.name));
    return text + QStringLiteral("\n[/KetPlus default skills]");
}

QString defaultSkillInstructions() {
    if (!QCoreApplication::instance())
        return {};
    return defaultSkillInstructions(bundledSkills(QCoreApplication::applicationDirPath()));
}

} // namespace ketplus::agent_skills
