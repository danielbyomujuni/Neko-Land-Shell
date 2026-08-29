import QtQuick

Text {
    id: btn

    property int pixelSize: Theme.fontSize
    signal leftClicked()
    signal rightClicked()
    signal middleClicked()

    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: pixelSize
    font.weight: Theme.fontWeight
    opacity: mouse.containsMouse ? 0.6 : 1.0

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: event => {
            if (event.button === Qt.LeftButton)
                btn.leftClicked();
            else if (event.button === Qt.RightButton)
                btn.rightClicked();
            else
                btn.middleClicked();
        }
    }
}
