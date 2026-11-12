// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// T-40: how a team reviews, in New team and in the team's Review settings. The review passes
// a pull request gets before the leader decides on findings left (one to five, two by default:
// the count is per PR and counts every reviewer's pass), and where the review is written: on
// the board, or as a review comment on the task's pull request with the board keeping the
// verdict and the comment's link.
Column {
    id: fields

    // 1 to 5.
    property int rounds: 2
    // "board" or "pull_request".
    property string place: "board"
    // Shown under the place in the team's settings: when a change applies.
    property string note: ""

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    spacing: px(12)

    KetFieldRow {
        width: parent.width
        label: "Max review passes per PR"
        help: fields.rounds === 1
              ? "Findings in the only pass go to the leader, who decides."
              : "Findings go back to the developer until pass " + fields.rounds
                + "; findings in pass " + fields.rounds + " go to the leader, who decides."
        KetSelect {
            objectName: "teamReviewRounds"
            width: parent.width
            model: ["1", "2", "3", "4", "5"]
            currentIndex: Math.max(0, Math.min(4, fields.rounds - 1))
            onActivated: index => fields.rounds = index + 1
        }
    }
    KetFieldRow {
        width: parent.width
        label: "Review on"
        help: (fields.place === "pull_request"
               ? "The reviewer reads the same diff, read-only. Its findings are posted as a comment on the task's pull request; the board keeps the verdict and the link. A task without a pull request is reviewed on the board."
               : "The reviewer's findings are kept in the task's history on the board.")
              + (fields.note !== "" ? " " + fields.note : "")
        Row {
            width: parent.width
            spacing: fields.px(6)
            KetChoice {
                objectName: "teamReviewPlaceBoard"
                width: (parent.width - parent.spacing) / 2
                kind: KetTheme.ToggleKind.Radio
                checked: fields.place === "board"
                text: "Board"
                onToggled: if (checked) fields.place = "board"
            }
            KetChoice {
                objectName: "teamReviewPlacePullRequest"
                width: (parent.width - parent.spacing) / 2
                kind: KetTheme.ToggleKind.Radio
                checked: fields.place === "pull_request"
                text: "Pull request"
                onToggled: if (checked) fields.place = "pull_request"
            }
        }
    }
}
