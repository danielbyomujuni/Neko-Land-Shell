import Quickshell
import Quickshell.Services.SystemTray
import QtQuick

Row {
    id: tray

    required property var bar

    spacing: 10

    Repeater {
        model: SystemTray.items

        Image {
            id: trayIcon

            required property var modelData

            source: modelData.icon
            width: 15
            height: 15
            anchors.verticalCenter: parent.verticalCenter

            QsMenuAnchor {
                id: menuAnchor
                menu: trayIcon.modelData.menu
                anchor.window: tray.bar
                anchor.rect.x: trayIcon.mapToItem(null, 0, 0).x
                anchor.rect.y: trayIcon.mapToItem(null, 0, trayIcon.height).y
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: event => {
                    if (event.button === Qt.LeftButton && !trayIcon.modelData.onlyMenu)
                        trayIcon.modelData.activate();
                    else if (trayIcon.modelData.hasMenu)
                        menuAnchor.open();
                }
            }
        }
    }
}
