import Quickshell.Wayland
import QtQuick

Text {
    id: title

    readonly property var rewrites: [
        [/^(.*) — Mozilla Firefox$/, "󰈹  "],
        [/^(.*) — Zen Browser$/, "󰈹  Zen - "],
        [/^(.*) - Google Chrome$/, "  "],
        [/^(.*) - Visual Studio Code$/, "󰨞  "],
        [/^(.*) - VSCodium$/, "󰨞  "],
        [/^(.*) - nvim$/, "  "],
        [/^(.*) - Obsidian(.*)$/, "󱓧  "],
        [/^(.*) - fish$/, "  "],
        [/^yazi: (.*)$/, "  "]
    ]

    function rewrite(t) {
        if (t === "nwg-look")
            return "  GTK Settings";
        if (t === "Qt6 Configuration Tool")
            return "  QT Settings";
        if (t === "blueman-manager")
            return "Bluetooth Settings";
        for (const [re, prefix] of rewrites) {
            const m = t.match(re);
            if (m)
                return prefix + m[1];
        }
        return t;
    }

    text: rewrite(ToplevelManager.activeToplevel?.title ?? "")
    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
    font.weight: Theme.fontWeight
    elide: Text.ElideRight
    maximumLineCount: 1
    width: Math.min(implicitWidth, 480)
}
