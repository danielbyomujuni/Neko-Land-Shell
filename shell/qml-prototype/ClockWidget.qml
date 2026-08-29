import Quickshell
import QtQuick

Text {
    SystemClock {
        id: clock
        precision: SystemClock.Minutes
    }

    text: "  " + Qt.formatDateTime(clock.date, "ddd d MMM HH:mm")
    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    font.weight: Theme.fontWeight
}
