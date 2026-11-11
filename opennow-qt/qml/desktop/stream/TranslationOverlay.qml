import QtQuick
import OpenNOW

// Translation overlay: renders the live EN -> VI translation above the stream
// video in the SAME window. It never captures input (enabled: false, no
// MouseArea, no Keys handler) and never touches the stream surface, so the
// session keeps running untouched underneath it.
//
// The C++ translationEngine context property is registered by the translation
// engine (parallel work). Every engine access below is guarded by hasEngine,
// so this overlay stays inert instead of logging errors until the engine
// lands. Visibility derives straight from translationEngine.hasTranslation:
// translationCleared hides it immediately (no fade, no timer) and a new
// translationReady replaces the text in one frame (direct binding, no
// animation), so there is no flicker.
Item {
    id: root
    anchors.fill: parent
    z: 6
    enabled: false

    readonly property bool hasEngine: typeof translationEngine !== "undefined" && translationEngine !== null
    readonly property bool engineOn: root.hasEngine && translationEngine.enabled
    readonly property bool hasText: root.hasEngine && translationEngine.hasTranslation
    // One entry per translated line: {original, translated, x, y, w, h}
    // with x/y/w/h normalized 0..1 in full-frame coordinates. In-place
    // mode draws one box per entry, exactly over the English line.
    readonly property var blocks: root.hasEngine ? translationEngine.translationBlocks : []
    readonly property string mode: !root.hasEngine ? "off"
        : String(translationEngine.overlayMode || "inplace")
    // The parent screen (DesktopStreamScreen) exposes `streaming`; fall back
    // to showing on engine state alone when hosted anywhere else.
    readonly property bool parentStreaming: parent && parent.streaming !== undefined
        ? Boolean(parent.streaming) : true
    readonly property bool showing: root.engineOn && root.hasText && root.mode !== "off" && root.parentStreaming

    visible: root.showing

    readonly property int baseFontSize: root.hasEngine ? Number(translationEngine.fontSizePx || 22) : 22
    readonly property bool showOriginal: root.hasEngine && translationEngine.showOriginal === true
    // Subtitle mode shows the live blocks joined as lines — never
    // lastTranslatedText/lastOriginalText: those are the TEST section's
    // fields, and a "Dịch thử" result must never leak onto the stream.
    readonly property string blockTranslatedText: root.blocks.map(function(b) {
        return String(b.translated || "")
    }).join("\n")
    readonly property string blockOriginalText: root.blocks.map(function(b) {
        return String(b.original || "")
    }).join("\n")

    readonly property string timingText: {
        if (!root.hasEngine)
            return ""
        const ocr = Number(translationEngine.lastOcrMs || 0)
        const tr = Number(translationEngine.lastTranslateMs || 0)
        let text = qsTr("OCR %1ms · Dịch %2ms").arg(ocr).arg(tr)
        if (translationEngine.lastCacheHit === true)
            text += " · " + qsTr("Cache hit")
        return text
    }

    // One box shape for both modes: semi-transparent dark background, white
    // wrapped text that shrinks (never overflows the box), optional small
    // original underneath.
    component TranslationBox: Rectangle {
        id: box
        required property string translated
        required property string original
        required property bool showOriginalText
        required property int baseFontSize
        color: "black"
        opacity: 0.85
        radius: DesktopTokens.px(12)
        Column {
            anchors.fill: parent
            anchors.margins: DesktopTokens.px(12)
            spacing: DesktopTokens.px(4)
            Text {
                id: translatedLabel
                width: parent.width
                height: parent.height - (originalLabel.visible ? originalLabel.height + parent.spacing : 0)
                text: box.translated
                color: "white"
                font.family: Theme.bodyFont
                font.pixelSize: box.baseFontSize
                fontSizeMode: Text.Fit
                minimumPixelSize: 12
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            Text {
                id: originalLabel
                visible: box.showOriginalText && box.original !== ""
                width: parent.width
                text: box.original
                color: Qt.rgba(1, 1, 1, 0.65)
                font.family: Theme.bodyFont
                font.pixelSize: Math.max(11, Math.round(box.baseFontSize * 0.6))
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                maximumLineCount: 2
            }
        }
    }

    // (a) In-place: one Vietnamese box per English line, drawn exactly
    // over the line it translates (the box is padded 8px so the text sits
    // on the original, and never smaller than the font needs).
    Repeater {
        id: blockRepeater
        model: root.showing && root.mode === "inplace" ? root.blocks : []
        delegate: TranslationBox {
            required property var modelData
            readonly property real blockW: Math.max(DesktopTokens.px(60),
                Math.min(Number(modelData.w || 0) * root.width + DesktopTokens.px(16), root.width))
            readonly property real blockH: Math.max(root.baseFontSize + DesktopTokens.px(24),
                Math.min(Number(modelData.h || 0) * root.height + DesktopTokens.px(16), root.height))
            x: Math.max(0, Math.min(Number(modelData.x || 0) * root.width - DesktopTokens.px(8),
                root.width - blockW))
            y: Math.max(0, Math.min(Number(modelData.y || 0) * root.height - DesktopTokens.px(8),
                root.height - blockH))
            width: blockW
            height: blockH
            translated: String(modelData.translated || "")
            original: String(modelData.original || "")
            showOriginalText: root.showOriginal
            baseFontSize: root.baseFontSize
        }
    }

    // Top edge of the highest in-place box, for the timing pill.
    readonly property real topBlockY: {
        let top = root.height
        for (let i = 0; i < root.blocks.length; i++) {
            const y = Number(root.blocks[i].y || 0) * root.height
            if (y < top)
                top = y
        }
        return top
    }

    // (b) Subtitle: full-width bar pinned to the bottom of the screen.
    TranslationBox {
        id: subtitleBox
        visible: root.showing && root.mode === "subtitle"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: DesktopTokens.px(16)
        anchors.rightMargin: DesktopTokens.px(16)
        anchors.bottomMargin: DesktopTokens.px(16)
        height: DesktopTokens.px(90)
        translated: root.blockTranslatedText
        original: root.blockOriginalText
        showOriginalText: root.showOriginal
        baseFontSize: root.baseFontSize
    }

    // Timing pill, only while a translation is shown. In-place it sits
    // just above the highest translated line; in subtitle mode above the
    // bar. Right-aligned, clamped inside the screen.
    Rectangle {
        id: statusPill
        visible: root.showing
        width: statusLabel.implicitWidth + DesktopTokens.px(20)
        height: DesktopTokens.px(24)
        radius: height / 2
        color: "black"
        opacity: 0.7
        x: Math.max(0, root.width - width - DesktopTokens.px(16))
        y: root.mode === "inplace"
            ? Math.max(0, root.topBlockY - height - DesktopTokens.px(6))
            : Math.max(0, root.height - DesktopTokens.px(90 + 16) - height - DesktopTokens.px(6))
        Text {
            id: statusLabel
            anchors.centerIn: parent
            text: root.timingText
            color: "white"
            font.family: Theme.monoFont
            font.pixelSize: DesktopTokens.px(11)
        }
    }
}
