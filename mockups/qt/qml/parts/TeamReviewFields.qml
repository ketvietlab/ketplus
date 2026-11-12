// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// T-40: how a team reviews, in New team and in the team's Review settings. The rounds a task
// gets before the leader decides on findings left (one or two: the user's rule is at most two
// review passes), and where the review is written: on the board, or as a review comment on the
// task's pull request with the board keeping the verdict and the comment's link.
Column {
    id: fields

    // 1 or 2.
    property int rounds: 2
    // "board" or "pull_request".
    property string place: "board"
    // Shown under the place in the team's settings: when a change applies.
    property string note: ""

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    spacing: px(12)

    KetFieldRow {
        width: parent.width
        label: "Review rounds"
        help: fields.rounds === 1
              ? "Findings in the only round go to the leader, who decides."
              : "Findings in the first round go back to the developer; findings in the second go to the leader."
        Row {
            width: parent.width
            spacing: fields.px(6)
            KetChoice {
                objectName: "teamReviewRoundsOne"
                width: (parent.width - parent.spacing) / 2
                kind: KetTheme.ToggleKind.Radio
                checked: fields.rounds === 1
                text: "One"
                onToggled: if (checked) fields.rounds = 1
            }
            KetChoice {
                objectName: "teamReviewRoundsTwo"
                width: (parent.width - parent.spacing) / 2
                kind: KetTheme.ToggleKind.Radio
                checked: fields.rounds === 2
                text: "Two"
                onToggled: if (checked) fields.rounds = 2
            }
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
