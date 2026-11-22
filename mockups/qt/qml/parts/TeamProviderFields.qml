// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock
import KetPlusMock.Native

// Change provider…: one provider, model and effort for every member of the team at once, or
// for the members left checked. Each member's next turn starts a fresh context on the new
// provider with the conversation so far as a transcript it may read (the provider handoff);
// a member running a turn switches when that turn ends, and nothing is interrupted. A
// reviewer runs on Codex only with Full access (ketplus#295), which the user confirms here.
Column {
    id: fields
    property string language: "en"
    function copy(source) { return Mock.teamText(language, source) }
    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    // The team's members: { number, title, role, provider, state }.
    property var members: []
    // The team's permission label ("Read only", "Workspace", "Full access").
    property string permission: "Workspace"

    property string provider: "claude"
    property string model: Fixtures.providerModels.claude[0].name
    property string effort: "High"
    // The member numbers left checked.
    property var chosen: []
    // The user agreed to give the team Full access so a reviewer can run on Codex.
    property bool fullAccess: false

    readonly property var ready: Fixtures.providers.filter(p => p.id !== "ketrouter")
    readonly property var efforts: ["Low", "Medium", "High"]
    readonly property bool reviewerNeedsAccess:
        provider === "codex" && permission !== "Full access"
        && members.some(m => m.role === "reviewer" && chosen.indexOf(m.number) >= 0)
    // What Switch sends: the reviewer stays where it is without Full access.
    readonly property var switching: members.filter(m => chosen.indexOf(m.number) >= 0
        && !(m.role === "reviewer" && reviewerNeedsAccess && !fullAccess))
    readonly property var running: switching.filter(m => m.state === "working")

    function labelOf(id) {
        const entry = Fixtures.providers.find(p => p.id === id)
        return entry ? entry.label : id
    }
    function reset(currentProvider) {
        provider = currentProvider === "codex" ? "claude" : "codex"
        model = Fixtures.providerModels[provider][0].name
        effort = provider === "claude" ? "High" : "Medium"
        chosen = members.map(m => m.number)
        fullAccess = false
    }
    function choose(id) {
        provider = id
        model = Fixtures.providerModels[id][0].name
        effort = id === "claude" ? "High" : "Medium"
    }
    function toggle(number, on) {
        const without = chosen.filter(n => n !== number)
        chosen = on ? without.concat([number]) : without
    }
    function roleText(role) {
        return role === "leader" ? copy("Leader") : role === "reviewer" ? copy("Reviewer") : copy("Developer")
    }

    spacing: px(12)

    // A select box: four providers or more never fit a row of choices.
    KetFieldRow {
        width: parent.width
        stacked: true
        label: fields.copy("Provider")
        help: fields.copy("Only providers that are set up on this computer can be chosen.")
        KetSelect {
            objectName: "teamProvider"
            width: parent.width
            model: fields.ready.map(p => Fixtures.providerChoice(p))
            currentIndex: Math.max(0, fields.ready.findIndex(p => p.id === fields.provider))
            onActivated: index => fields.choose(fields.ready[index].id)
        }
    }
    Row {
        width: parent.width
        spacing: fields.px(12)
        KetFieldRow {
            width: (parent.width - parent.spacing) * 0.62
            stacked: true
            label: fields.copy("Model")
            KetSelect {
                objectName: "teamProviderModel"
                width: parent.width
                model: Fixtures.providerModels[fields.provider].map(m => m.name)
                currentIndex: Math.max(0, Fixtures.providerModels[fields.provider].findIndex(m => m.name === fields.model))
                onActivated: index => fields.model = Fixtures.providerModels[fields.provider][index].name
            }
        }
        KetFieldRow {
            width: (parent.width - parent.spacing) * 0.38
            stacked: true
            label: fields.copy("Effort")
            KetSelect {
                objectName: "teamProviderEffort"
                width: parent.width
                model: fields.efforts
                currentIndex: Math.max(0, fields.efforts.indexOf(fields.effort))
                onActivated: index => fields.effort = fields.efforts[index]
            }
        }
    }
    KetFieldRow {
        width: parent.width
        stacked: true
        label: fields.copy("Members")
        help: fields.copy("Every member switches unless you clear it.")
        Column {
            width: parent.width
            spacing: fields.px(4)
            KetCheckbox {
                objectName: "teamProviderAll"
                text: fields.copy("All members")
                checked: fields.chosen.length === fields.members.length
                onToggled: fields.chosen = checked ? fields.members.map(m => m.number) : []
            }
            Repeater {
                model: fields.members
                KetCheckbox {
                    required property var modelData
                    objectName: "teamProviderMember_" + modelData.number
                    width: parent.width
                    wordWrap: true
                    checked: fields.chosen.indexOf(modelData.number) >= 0
                    text: "@agent/" + modelData.number + " · " + modelData.title + "\n"
                          + fields.roleText(modelData.role) + " · " + modelData.provider
                          + (modelData.state === "working" ? " · " + fields.copy("switches when its turn ends") : "")
                    onToggled: fields.toggle(modelData.number, checked)
                }
            }
        }
    }
    KetBanner {
        visible: fields.reviewerNeedsAccess
        width: parent.width
        tone: KetTheme.BannerTone.Warning
        title: fields.copy("A Codex reviewer needs Full access")
        message: fields.copy("Claude keeps a review read-only. Codex reviews only when the team starts members with Full access, which skips permission prompts for every member.")
    }
    KetCheckbox {
        objectName: "teamProviderFullAccess"
        visible: fields.reviewerNeedsAccess
        text: fields.copy("Give the team Full access so the reviewer runs on Codex")
        checked: fields.fullAccess
        onToggled: fields.fullAccess = checked
    }
    KetBanner {
        width: parent.width
        tone: KetTheme.BannerTone.Info
        title: fields.switching.length === 0 ? fields.copy("No member is chosen")
                                             : fields.copy("Each member starts a fresh context")
        message: fields.copy("A member's next turn runs on ") + fields.labelOf(fields.provider)
                 + fields.copy(" with the conversation so far as a transcript it reads when needed; nothing is replayed.")
                 + (fields.running.length > 0
                    ? " " + fields.running.length + fields.copy(" running now switch when their turn ends; no turn is interrupted.")
                    : "")
                 + (fields.reviewerNeedsAccess && !fields.fullAccess ? " " + fields.copy("The reviewer stays on its provider.") : "")
    }
}
