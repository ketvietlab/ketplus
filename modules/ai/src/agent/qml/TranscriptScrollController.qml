import QtQuick

// Only explicit navigation may resume following. Content growth never changes state.
QtObject {
    property bool followingLatest: true
    readonly property string state: followingLatest ? "FollowingLatest" : "ReadingHistory"

    function beginReading() {
        followingLatest = false;
    }
    function finishNavigation(atLatest, towardLatest) {
        followingLatest = atLatest && towardLatest;
    }
    function jumpToLatest() {
        followingLatest = true;
    }
}
