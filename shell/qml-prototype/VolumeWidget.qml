import Quickshell
import Quickshell.Services.Pipewire
import QtQuick

Text {
    id: vol

    readonly property var sink: Pipewire.defaultAudioSink
    readonly property real volume: sink?.audio?.volume ?? 0
    readonly property bool muted: sink?.audio?.muted ?? false

    PwObjectTracker {
        objects: [Pipewire.defaultAudioSink]
    }

    function icon() {
        if (muted)
            return "󰝟";
        if (volume < 0.34)
            return "󰕿";
        if (volume < 0.67)
            return "󰖀";
        return "󰕾";
    }

    text: muted ? " 󰝟 " : icon() + " " + Math.round(volume * 100) + "%"
    color: Theme.sapphire
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    font.weight: Theme.fontWeight

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: Quickshell.execDetached(["pavucontrol"])
        onWheel: event => {
            if (!vol.sink?.audio)
                return;
            const step = event.angleDelta.y > 0 ? 0.05 : -0.05;
            vol.sink.audio.volume = Math.max(0, Math.min(1, vol.sink.audio.volume + step));
        }
    }
}
