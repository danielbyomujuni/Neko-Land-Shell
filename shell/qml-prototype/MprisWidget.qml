import Quickshell.Services.Mpris
import QtQuick

Rectangle {
    id: pill

    readonly property var player: Mpris.players.values.length > 0 ? Mpris.players.values[0] : null
    readonly property bool playing: player && player.playbackState === MprisPlaybackState.Playing

    function playerIcon() {
        if (!player)
            return "▶";
        if (!playing)
            return "";
        const name = (player.identity ?? "").toLowerCase();
        if (name.includes("spotify"))
            return "󰓇";
        if (name.includes("mpv"))
            return "";
        return "▶";
    }

    function trackText() {
        if (!player)
            return "";
        const title = (player.trackTitle ?? "").substring(0, 20);
        const artist = player.trackArtist ?? "";
        return playerIcon() + " " + artist + " - " + title;
    }

    visible: player !== null
    color: Theme.base
    radius: 16
    implicitWidth: label.implicitWidth + 22
    implicitHeight: label.implicitHeight + 6

    Text {
        id: label
        anchors.centerIn: parent
        text: pill.trackText()
        color: Theme.green
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        font.weight: Theme.fontWeight
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: event => {
            if (!pill.player)
                return;
            if (event.button === Qt.LeftButton)
                pill.player.togglePlaying();
            else if (event.button === Qt.MiddleButton)
                pill.player.previous();
            else
                pill.player.next();
        }
    }
}
