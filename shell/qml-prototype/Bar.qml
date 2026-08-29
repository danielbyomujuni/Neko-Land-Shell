import Quickshell
import QtQuick
import QtQuick.Layouts

PanelWindow {
    id: root

    required property var modelData
    screen: modelData

    anchors {
        top: true
        left: true
        right: true
    }
    implicitHeight: 40
    color: "transparent"

    Rectangle {
        id: barBox
        anchors.fill: parent
        anchors.margins: 8
        color: Theme.crust
        radius: 16
        border.width: 2
        border.color: Theme.base

        // Left: launcher, workspaces, mpris, window title
        RowLayout {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 10
            spacing: 0

            IconButton {
                text: "󰣇"
                pixelSize: 16
                onLeftClicked: Quickshell.execDetached(["sh", "-c", "~/.config/rofi/launchers/type-7/launcher.sh"])
                onMiddleClicked: Quickshell.execDetached(["pkill", "-9", "rofi"])
            }

            Workspaces {
                Layout.leftMargin: 16
                bar: root
            }

            MprisWidget {
                Layout.leftMargin: 20
            }

            WindowTitle {
                Layout.leftMargin: 20
                Layout.rightMargin: 26
            }
        }

        // Center: clock
        ClockWidget {
            anchors.centerIn: parent
        }

        // Right: memory, tray, volume, buttons
        RowLayout {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 8
            spacing: 0

            MemoryWidget {
                Layout.rightMargin: 16
            }

            TrayWidget {
                Layout.rightMargin: 13
                bar: root
            }

            VolumeWidget {
                Layout.rightMargin: 13
            }

            IconButton {
                text: "" // color picker
                pixelSize: 12
                onLeftClicked: Quickshell.execDetached(["sh", "-c", "hyprpicker -an && notify-send 'Colour copied to clipboard'"])
            }

            IconButton {
                Layout.leftMargin: 24
                text: "" // screenshot
                pixelSize: 13
                onLeftClicked: Quickshell.execDetached(["sh", "-c", "~/.config/waybar/scripts/screenshot_full.sh"])
                onRightClicked: Quickshell.execDetached(["sh", "-c", "~/.config/waybar/scripts/screenshot_area.sh"])
            }

            IconButton {
                Layout.leftMargin: 24
                text: "󰸉" // wallpaper
                pixelSize: 16
                onLeftClicked: Quickshell.execDetached(["waypaper"])
                onRightClicked: Quickshell.execDetached(["waypaper", "--random"])
            }

            IconButton {
                Layout.leftMargin: 21
                Layout.rightMargin: 15
                text: "" // power
                pixelSize: 13
                onLeftClicked: Quickshell.execDetached(["sh", "-c", "~/.config/rofi/powermenu.sh"])
            }
        }
    }
}
