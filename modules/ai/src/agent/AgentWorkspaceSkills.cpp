#include "BrowserToolLaunch.h"
#include "BundledAgentSkills.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/FileMentions.h"
#include "preview/MermaidRenderer.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include "CodexBrowserTools.h"
#include "agent/CodexAppServer.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QThreadPool>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ketplus {
namespace {

QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString displayPath(const QString& path) {
    const QString relative = QDir::home().relativeFilePath(path);
    return relative == QStringLiteral(".")              ? QStringLiteral("~")
           : relative.startsWith(QStringLiteral("../")) ? path
                                                        : QStringLiteral("~/%1").arg(relative);
}

QString frontMatterValue(const QString& content, const QString& key) {
    if (!content.startsWith(QStringLiteral("---"))) {
        return {};
    }
    const QString prefix = key + QStringLiteral(":");
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (int index = 1; index < lines.size(); ++index) {
        const QString line = lines.at(index).trimmed();
        if (line == QStringLiteral("---")) {
            break;
        }
        if (line.startsWith(prefix)) {
            QString value = line.mid(prefix.size()).trimmed();
            if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
                (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
    }
    return {};
}

QString skillInvocationName(const QVariantMap& skill) {
    const QString name = skill.value(QStringLiteral("name")).toString().trimmed();
    static const QRegularExpression validName(QStringLiteral(R"(^[\p{L}\p{N}][\p{L}\p{N}_.:-]*$)"),
                                              QRegularExpression::UseUnicodePropertiesOption);
    return validName.match(name).hasMatch() ? name : QString{};
}

// A project skill shadows a user skill, and both shadow a skill KetPlus ships.
int skillSourcePriority(const QVariantMap& skill) {
    const QString source = skill.value(QStringLiteral("source")).toString();
    return source == QStringLiteral("Project") ? 0 : source == QStringLiteral("User") ? 1 : 2;
}

bool preferSkill(const QVariantMap& candidate, const QVariantMap& current) {
    const int candidatePriority = skillSourcePriority(candidate);
    const int currentPriority = skillSourcePriority(current);
    if (candidatePriority != currentPriority) {
        return candidatePriority < currentPriority;
    }
    return candidate.value(QStringLiteral("path")).toString() <
           current.value(QStringLiteral("path")).toString();
}

// The ketplus-browser skill ships with the browser tools. While the tools are enabled it is
// offered as a built-in skill in every repository (not auto-enabled: its full text is only
// added to a turn when the user mentions $ketplus-browser or enables it).
QString builtInBrowserSkillFile() {
    return browser_tools::browserSkillFile(browser_tools::loadSettings());
}

QString skillSettingsKey(const QString& activePath) {
    const QByteArray scope = activePath.isEmpty()
                                 ? QByteArrayLiteral("global")
                                 : activePath.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                                                QByteArray::OmitTrailingEquals);
    return QStringLiteral("agent/enabledSkills/%1").arg(QString::fromLatin1(scope));
}

QVariantMap readSkill(const QString& skillFile, const QString& source,
                      const QSet<QString>& enabledSkillPaths) {
    QFile file(skillFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QString content = QString::fromUtf8(file.readAll());
    const QString path = normalizedPath(skillFile);
    const QFileInfo info(skillFile);
    const QString fallbackName = info.dir().dirName();
    QString name = frontMatterValue(content, QStringLiteral("name"));
    if (name.isEmpty()) {
        name = fallbackName;
    }
    QString description = frontMatterValue(content, QStringLiteral("description"));
    if (description.isEmpty()) {
        const QStringList lines = content.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("# "))) {
                description = trimmed.mid(2).trimmed();
                break;
            }
        }
    }
    return QVariantMap{
        {QStringLiteral("id"), path},
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("path"), path},
        {QStringLiteral("displayPath"), displayPath(path)},
        {QStringLiteral("source"), source},
        {QStringLiteral("enabled"), enabledSkillPaths.contains(path)},
        {QStringLiteral("content"), content},
    };
}

QVariantList scanSkills(const QString& activePath, const QSet<QString>& enabledSkillPaths) {
    QVariantList model;
    QSet<QString> seen;
    const QList<QPair<QString, QString>> roots{
        {activePath.isEmpty() ? QString()
                              : QDir(activePath).filePath(QStringLiteral(".agents/skills")),
         QStringLiteral("Project")},
        {activePath.isEmpty() ? QString()
                              : QDir(activePath).filePath(QStringLiteral(".codex/skills")),
         QStringLiteral("Project")},
        {QDir::home().filePath(QStringLiteral(".agents/skills")), QStringLiteral("User")},
        {QDir::home().filePath(QStringLiteral(".codex/skills")), QStringLiteral("User")},
    };
    for (const auto& [rootPath, source] : roots) {
        if (rootPath.isEmpty() || !QFileInfo(rootPath).isDir()) {
            continue;
        }
        QDirIterator iterator(rootPath, QStringList{QStringLiteral("SKILL.md")}, QDir::Files,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = normalizedPath(iterator.next());
            if (path.isEmpty() || seen.contains(path)) {
                continue;
            }
            seen.insert(path);
            const QVariantMap skill = readSkill(path, source, enabledSkillPaths);
            if (!skill.isEmpty()) {
                model.append(skill);
            }
        }
    }
    // Agents are told when to read the default skills (hostAgentInstructions); listing them
    // here lets the user also mention or enable them.
    QStringList builtInSkills{builtInBrowserSkillFile()};
    for (const auto& bundled : agent_skills::bundledSkills(QCoreApplication::applicationDirPath()))
        builtInSkills.append(bundled.file);
    for (const QString& file : std::as_const(builtInSkills)) {
        const QString builtInSkill = normalizedPath(file);
        if (builtInSkill.isEmpty() || seen.contains(builtInSkill)) {
            continue;
        }
        seen.insert(builtInSkill);
        const QVariantMap skill =
            readSkill(builtInSkill, QStringLiteral("Built-in"), enabledSkillPaths);
        if (!skill.isEmpty()) {
            model.append(skill);
        }
    }
    std::sort(model.begin(), model.end(), [](const QVariant& left, const QVariant& right) {
        const QVariantMap a = left.toMap();
        const QVariantMap b = right.toMap();
        if (a.value(QStringLiteral("enabled")).toBool() !=
            b.value(QStringLiteral("enabled")).toBool()) {
            return a.value(QStringLiteral("enabled")).toBool();
        }
        if (a.value(QStringLiteral("source")).toString() !=
            b.value(QStringLiteral("source")).toString()) {
            return a.value(QStringLiteral("source")).toString() <
                   b.value(QStringLiteral("source")).toString();
        }
        return a.value(QStringLiteral("name"))
                   .toString()
                   .localeAwareCompare(b.value(QStringLiteral("name")).toString()) < 0;
    });
    return model;
}

} // namespace

void AgentWorkspaceView::refreshSkills() { rebuildSkillModel(); }

QVariantList AgentWorkspaceView::skillSuggestions(const QString& query) const {
    const QString needle = query.trimmed().toCaseFolded();
    QHash<QString, QVariantMap> uniqueSkills;
    for (const QVariant& value : skills_) {
        QVariantMap skill = value.toMap();
        const QString name = skillInvocationName(skill);
        if (name.isEmpty() || !name.toCaseFolded().contains(needle)) {
            continue;
        }
        const QString key = name.toCaseFolded();
        const auto current = uniqueSkills.constFind(key);
        if (current == uniqueSkills.cend() || preferSkill(skill, current.value())) {
            skill.insert(QStringLiteral("mention"), QStringLiteral("$%1").arg(name));
            uniqueSkills.insert(key, skill);
        }
    }

    QVariantList suggestions;
    suggestions.reserve(uniqueSkills.size());
    for (const QVariantMap& skill : std::as_const(uniqueSkills)) {
        suggestions.append(skill);
    }
    std::sort(suggestions.begin(), suggestions.end(),
              [&needle](const QVariant& left, const QVariant& right) {
                  const QVariantMap a = left.toMap();
                  const QVariantMap b = right.toMap();
                  const QString aName = a.value(QStringLiteral("name")).toString();
                  const QString bName = b.value(QStringLiteral("name")).toString();
                  const bool aStarts = aName.toCaseFolded().startsWith(needle);
                  const bool bStarts = bName.toCaseFolded().startsWith(needle);
                  if (aStarts != bStarts) {
                      return aStarts;
                  }
                  return aName.localeAwareCompare(bName) < 0;
              });
    constexpr qsizetype maximumSuggestions = 8;
    if (suggestions.size() > maximumSuggestions) {
        suggestions.resize(maximumSuggestions);
    }
    return suggestions;
}

QVariantList AgentWorkspaceView::skillsForPrompt(const QString& prompt) const {
    QVariantList selected;
    QSet<QString> selectedPaths;
    const auto append = [&selected, &selectedPaths](const QVariantMap& skill) {
        const QString path = skill.value(QStringLiteral("path")).toString();
        if (!path.isEmpty() && !selectedPaths.contains(path)) {
            selectedPaths.insert(path);
            selected.append(skill);
        }
    };

    for (const QVariant& value : skills_) {
        const QVariantMap skill = value.toMap();
        if (skill.value(QStringLiteral("enabled")).toBool()) {
            append(skill);
        }
    }

    static const QRegularExpression mentionPattern(
        QStringLiteral(R"((?<![\p{L}\p{N}_$])\$([\p{L}\p{N}][\p{L}\p{N}_.:-]*))"),
        QRegularExpression::UseUnicodePropertiesOption);
    auto matches = mentionPattern.globalMatch(prompt);
    while (matches.hasNext()) {
        const QString requestedName = matches.next().captured(1);
        QVariantMap resolved;
        for (const QVariant& value : skills_) {
            const QVariantMap candidate = value.toMap();
            if (skillInvocationName(candidate).compare(requestedName, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (resolved.isEmpty() || preferSkill(candidate, resolved)) {
                resolved = candidate;
            }
        }
        if (!resolved.isEmpty()) {
            append(resolved);
        }
    }
    return selected;
}

void AgentWorkspaceView::steerPrompt(const QString& prompt) {
    if (!sessionRunning_ || !agentAvailable_ || steeringPending_ ||
        (prompt.trimmed().isEmpty() && attachments_.isEmpty())) {
        return;
    }
    QVariantList referencedPaths;
    if (!referencedPathsForPrompt(prompt, referencedPaths)) {
        return;
    }
    settleSkillModel();
    steeringScope_ = composerScope_;
    steeringPrompt_ = prompt.trimmed();
    steeringSubmittedPrompt_ = steeringPrompt_;
    steeringAttachments_ = attachments_;
    const QString pathInstructions = file_mentions::referencedPathInstructions(referencedPaths);
    if (!pathInstructions.isEmpty()) {
        steeringSubmittedPrompt_ = QStringLiteral("%1\n\nUser steering request:\n%2")
                                       .arg(pathInstructions, steeringPrompt_);
    }
    const QString skillInstructions =
        agent_message::selectedSkillInstructions(skillsForPrompt(steeringPrompt_));
    if (!skillInstructions.isEmpty()) {
        steeringSubmittedPrompt_ = QStringLiteral("%1\n\nUser steering request:\n%2")
                                       .arg(skillInstructions, steeringSubmittedPrompt_);
    }
    steeringPending_ = true;
    emit steeringPendingChanged();
    emit presentationStateChanged();
    emit steeringPromptSubmitted(steeringSubmittedPrompt_, steeringAttachments_, steeringPrompt_);
}

void AgentWorkspaceView::finishSteering(const QString& path, const QString& conversationId,
                                        const QString& prompt, const bool accepted) {
    const QString scope = path + ":" + conversationId;
    if (!steeringPending_ || scope != steeringScope_ || prompt != steeringSubmittedPrompt_) {
        return;
    }
    steeringPending_ = false;
    emit steeringPendingChanged();
    emit presentationStateChanged();
    const QVariantList sent = std::exchange(steeringAttachments_, {});
    if (!accepted)
        return;
    // The steer took its attachments along; any added while it was being sent stay.
    if (!sent.isEmpty()) {
        const auto unsent = [&sent](QVariantList attachments) {
            attachments.removeIf([&sent](const QVariant& item) {
                const QString path = item.toMap().value(QStringLiteral("path")).toString();
                return std::any_of(sent.cbegin(), sent.cend(), [&path](const QVariant& taken) {
                    return taken.toMap().value(QStringLiteral("path")).toString() == path;
                });
            });
            return attachments;
        };
        if (scope == composerScope_) {
            attachments_ = unsent(attachments_);
            attachmentDrafts_.insert(scope, attachments_);
            emit attachmentsChanged();
            saveAttachmentDraft(scope);
        } else {
            const QVariantList remaining =
                unsent(attachmentDrafts_.contains(scope) ? attachmentDrafts_.value(scope)
                                                         : loadAttachmentDraft(scope));
            attachmentDrafts_.insert(scope, remaining);
            saveAttachmentDraft(scope, remaining);
        }
    }
    emit steeringAccepted(scope, steeringPrompt_);
}

void AgentWorkspaceView::toggleSkill(const QString& path, const bool enabled) {
    const QString normalized = normalizedPath(path);
    if (normalized.isEmpty()) {
        return;
    }
    if (enabled) {
        enabledSkillPaths_.insert(normalized);
    } else {
        enabledSkillPaths_.remove(normalized);
    }
    saveEnabledSkills();
    rebuildSkillModel();
}

void AgentWorkspaceView::rebuildWorktreeModel() {
    QVariantList model;
    if (sourceWorktrees_.isEmpty() && !activePath_.isEmpty()) {
        model.append(QVariantMap{
            {QStringLiteral("branch"), QStringLiteral("workspace")},
            {QStringLiteral("path"), activePath_},
            {QStringLiteral("displayPath"), displayPath(activePath_)},
            {QStringLiteral("status"),
             QFileInfo(activePath_).isDir()
                 ? (liveSessionPaths_.contains(activePath_) ? QStringLiteral("Agent working")
                                                            : QStringLiteral("Ready"))
                 : QStringLiteral("Worktree missing")},
            {QStringLiteral("running"), liveSessionPaths_.contains(activePath_)},
            {QStringLiteral("relevant"), true},
            {QStringLiteral("selected"), true},
        });
    } else {
        for (const GitWorktree& worktree : sourceWorktrees_) {
            const QString path = normalizedPath(worktree.path);
            const QString branch = worktree.branch.isEmpty()
                                       ? QStringLiteral("Detached %1").arg(worktree.head.left(8))
                                       : worktree.branch;
            const bool runningAgent = liveSessionPaths_.contains(path);
            const bool relevant =
                path == activePath_ || runningAgent || conversationPaths_.contains(path);
            QVariantMap entry{
                {QStringLiteral("branch"), branch},
                {QStringLiteral("path"), path},
                {QStringLiteral("name"), QFileInfo(path).fileName()},
                {QStringLiteral("displayPath"), displayPath(path)},
                {QStringLiteral("searchText"),
                 QStringLiteral("%1 %2 %3")
                     .arg(branch, QFileInfo(path).fileName(), displayPath(path))
                     .toCaseFolded()},
                {QStringLiteral("status"),
                 !QFileInfo(path).isDir()
                     ? QStringLiteral("Worktree missing")
                     : (runningAgent ? QStringLiteral("Agent working") : QStringLiteral("Ready"))},
                {QStringLiteral("running"), runningAgent},
                {QStringLiteral("relevant"), relevant},
                {QStringLiteral("selected"), path == activePath_},
            };
            model.append(entry);
        }
    }
    worktrees_ = model;
    emit worktreesChanged();
}

void AgentWorkspaceView::rebuildSkillModel() {
    // A synchronous scan supersedes one still running on the pool.
    ++skillScanGeneration_;
    skillScanPending_ = false;
    skills_ = scanSkills(activePath_, enabledSkillPaths_);
    emit skillsChanged();
}

// Switching worktrees must not wait for a recursive walk of the skill folders, so path changes
// scan on the pool. Until the result lands the previous list stays; a prompt sent in that window
// settles the scan first, so it never carries another worktree's skills.
void AgentWorkspaceView::rebuildSkillModelLater() {
    const quint64 generation = ++skillScanGeneration_;
    skillScanPending_ = true;
    QThreadPool::globalInstance()->start([self = QPointer<AgentWorkspaceView>(this), generation,
                                          path = activePath_, enabled = enabledSkillPaths_] {
        QVariantList skills = scanSkills(path, enabled);
        QMetaObject::invokeMethod(
            qApp,
            [self, generation, skills = std::move(skills)]() mutable {
                if (!self || self->skillScanGeneration_ != generation)
                    return;
                self->skillScanPending_ = false;
                self->skills_ = std::move(skills);
                emit self->skillsChanged();
            },
            Qt::QueuedConnection);
    });
}

void AgentWorkspaceView::settleSkillModel() {
    if (skillScanPending_)
        rebuildSkillModel();
}

void AgentWorkspaceView::loadEnabledSkills() {
    enabledSkillPaths_.clear();
    const QStringList paths = QSettings().value(skillSettingsKey(activePath_)).toStringList();
    for (const QString& path : paths) {
        const QString normalized = normalizedPath(path);
        if (!normalized.isEmpty()) {
            enabledSkillPaths_.insert(normalized);
        }
    }
}

void AgentWorkspaceView::saveEnabledSkills() const {
    QStringList paths;
    paths.reserve(enabledSkillPaths_.size());
    for (const QString& path : enabledSkillPaths_) {
        paths.append(path);
    }
    paths.sort();
    QSettings().setValue(skillSettingsKey(activePath_), paths);
}

} // namespace ketplus
