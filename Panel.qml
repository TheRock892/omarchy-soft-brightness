import QtQuick
import Quickshell.Io
import qs.Ui
import qs.Commons

// Bar icon + popup slider for omarchy-soft-brightness: the external monitor
// here has no working DDC/CI, so brightness is faked with a per-monitor
// black click-through overlay instead of real hardware control (see
// ~/Projects/omarchy-arm/omarchy-soft-brightness/README.md). This panel is
// just a thin face on the "omarchy-soft-brightness" CLI, which already owns
// monitor auto-detection, the daemon lifecycle, and the minimum-brightness
// floor -- the panel never talks to the daemon's socket directly.
Panel {
  id: root
  moduleName: "omarchy-soft-brightness"
  ipcTarget: "omarchy-soft-brightness"

  readonly property string monitorOverride: String(root.setting("monitor", ""))
  readonly property int refreshIntervalSec: Math.max(2, Math.min(30, Number(root.setting("refreshIntervalSec", 5)) || 5))
  readonly property var monitorArgs: monitorOverride !== "" ? ["--monitor", monitorOverride] : []

  property int level: 100 // 0 (darkest) .. 100 (full brightness), mirrors the daemon's scale
  property bool available: false
  property bool loaded: false
  property real wheelAccumulator: 0

  function refresh() {
    if (statusProc.running) return
    statusProc.command = ["omarchy-soft-brightness"].concat(root.monitorArgs, ["status"])
    statusProc.running = true
  }

  function setLevel(pct) {
    var n = Math.max(10, Math.min(100, Math.round(pct)))
    root.level = n
    if (setProc.running) setProc.running = false
    setProc.command = ["omarchy-soft-brightness"].concat(root.monitorArgs, [n + "%"])
    setProc.running = true
  }

  Process {
    id: statusProc
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var n = parseInt(text.trim(), 10)
        root.loaded = true
        if (!isNaN(n)) {
          root.available = true
          if (!slider.dragging) root.level = n
        } else {
          root.available = false
        }
      }
    }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        root.available = false
        root.loaded = true
      }
    }
  }

  Process {
    id: setProc
  }

  Timer {
    id: refreshTimer
    interval: root.refreshIntervalSec * 1000
    running: root.opened
    repeat: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰍹"
    tooltipText: root.available ? ("External monitor: " + root.level + "%") : "Soft brightness: no external monitor"
    onPressed: function(b) { root.toggle() }
    onWheelMoved: function(delta) {
      if (!root.available) return
      var wheel = Util.wheelSteps(root.wheelAccumulator, delta)
      root.wheelAccumulator = wheel.remainder
      if (wheel.steps === 0) return
      root.setLevel(root.level + wheel.steps * 5)
    }
  }

  // showTooltip() snapshots tooltipText into a plain string when the pointer
  // enters the icon; it does not keep tracking it. Re-push it on every level
  // change so the tooltip reflects the value while the wheel is scrolled.
  onLevelChanged: if (button.tooltipHovered) root.bar.showTooltip(button, button.tooltipText)

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(280))
    contentHeight: panel.fittedContentHeight(column.implicitHeight)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(10)

        PanelSectionHeader {
          text: "SOFT BRIGHTNESS"
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
        }

        Text {
          visible: !root.available
          width: parent.width
          textFormat: Text.PlainText
          text: root.loaded ? "No external monitor detected." : "Checking…"
          color: Qt.darker(root.bar.foreground, 1.4)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          wrapMode: Text.WordWrap
        }

        Row {
          visible: root.available
          width: parent.width
          spacing: Style.space(4)

          PanelSlider {
            id: slider
            width: parent.width - Style.space(44) - Style.space(4)
            bar: root.bar
            minimum: 10
            maximum: 100
            step: 1
            integer: true
            value: root.level
            tickCount: 10
            onMoved: function(v) { root.level = Math.round(v) }
            onReleased: function(v) { root.setLevel(v) }
          }

          Text {
            textFormat: Text.PlainText
            text: Math.round(slider.liveValue) + "%"
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            font.bold: true
            width: Style.space(44)
            horizontalAlignment: Text.AlignRight
            anchors.verticalCenter: parent.verticalCenter
          }
        }

        Text {
          visible: root.available
          width: parent.width
          textFormat: Text.PlainText
          text: "Dims this monitor with a click-through overlay (no real DDC/CI on this hardware) — not actual backlight control."
          color: Qt.darker(root.bar.foreground, 1.5)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
          wrapMode: Text.WordWrap
        }
      }
    }
  }

  Component.onCompleted: refresh()
  onOpenedChanged: if (opened) refresh()
}
