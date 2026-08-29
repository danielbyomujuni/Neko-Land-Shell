import Quickshell
import Quickshell.Hyprland
import QtQuick

Row {
    id: wsRow

    required property var bar
    readonly property var monitor: Hyprland.monitorFor(bar.screen)

    spacing: 5

    Repeater {
        model: Hyprland.workspaces.values
            .filter(ws => ws.monitor && wsRow.monitor && ws.monitor.name === wsRow.monitor.name)
            .sort((a, b) => a.id - b.id)

        Text {
            required property var modelData

            // focused: on the focused monitor; active: visible on its own monitor
            text: (modelData.focused || modelData.active) ? "" : ""
            color: modelData.focused ? Theme.red : (modelData.active ? Theme.blue : Theme.surface2)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
            font.weight: Theme.fontWeight

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onClicked: Hyprland.dispatch("workspace " + parent.modelData.id)
            }
        }
    }
}
