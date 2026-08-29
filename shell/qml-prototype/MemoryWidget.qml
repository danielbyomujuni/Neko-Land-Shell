import Quickshell
import Quickshell.Io
import QtQuick

Text {
    id: mem

    property string used: "?"

    text: " Mem " + used + "GiB"
    color: Theme.peach
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    font.weight: Theme.fontWeight

    Process {
        id: memProc
        command: ["sh", "-c", "awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}END{printf \"%.1f\", (t-a)/1048576}' /proc/meminfo"]
        running: true
        stdout: SplitParser {
            onRead: data => mem.used = data.trim()
        }
    }

    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: memProc.running = true
    }
}
