import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    property var strings
    title: strings ? strings.detectProcess : "Detect Process"
    signal selected(string processName)

    modal: true
    dim: true
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    width: parent.width * 0.65
    height: parent.height * 0.7

    onOpened: backend.refreshProcesses()

    contentItem: ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: strings ? strings.processListDesc : "Select a running process."
            wrapMode: Text.WordWrap
            color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                Layout.fillWidth: true
                text: strings ? strings.refresh : "Refresh"
                icon.name: "view-refresh"
                onClicked: backend.refreshProcesses()
            }

            Label {
                Layout.fillWidth: true
                text: backend.processCount + " " + (strings ? strings.processesFound : "processes found")
                horizontalAlignment: Text.AlignRight
                color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.7)
            }
        }

        Label {
            visible: backend.processCount === 0
            Layout.fillWidth: true
            Layout.margins: 16
            text: strings ? strings.noProcessesFound : "No GPU/Vulkan processes detected. Open your game or emulator and refresh."
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.6)
        }

        ListView {
            id: processList
            visible: backend.processCount > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.processList
            currentIndex: -1

            delegate: Rectangle {
                width: ListView.view.width
                height: 36
                color: processList.currentIndex === index
                    ? Qt.rgba(palette.highlight.r, palette.highlight.g, palette.highlight.b, 0.3)
                    : index % 2 === 0 ? palette.alternateBase : "transparent"
                radius: 2

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8

                    Label {
                        Layout.fillWidth: true
                        text: modelData
                        elide: Text.ElideRight
                        color: palette.text
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        processList.currentIndex = index;
                        backend.setSelectedProcessIndex(index);
                    }
                    onDoubleClicked: {
                        processList.currentIndex = index;
                        backend.setSelectedProcessIndex(index);
                        var name = backend.getSelectedProcessName();
                        if (name.length > 0) {
                            root.selected(name);
                            root.close();
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                Layout.fillWidth: true
                text: strings ? strings.cancel : "Cancel"
                onClicked: root.close()
            }

            Button {
                Layout.fillWidth: true
                text: strings ? strings.selectProcess : "Select"
                enabled: processList.currentIndex >= 0
                onClicked: {
                    var name = backend.getSelectedProcessName();
                    if (name.length > 0) {
                        root.selected(name);
                        root.close();
                    }
                }
            }
        }
    }
}
