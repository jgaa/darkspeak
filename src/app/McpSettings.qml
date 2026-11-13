import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    width: parent ? parent.width : implicitWidth
    spacing: appearance.spacing
    property string selectedKey: ""
    property string newSecret: ""
    property var allowedIdentities: []
    property var grantedCapabilities: []
    function toggle(list, value, enabled) {
        const next = list.filter(function(item) { return item !== value; });
        if (enabled) next.push(value);
        return next;
    }
    function loadKey(key) {
        selectedKey = key.id;
        keyName.text = key.name;
        allowedIdentities = key.identities.slice();
        grantedCapabilities = key.capabilities.slice();
        expires.text = key.expires ? new Date(Number(key.expires) * 1000).toISOString() : "";
        keyEnabled.checked = key.enabled;
        newSecret = "";
    }
    function expirySeconds() {
        if (expires.text.trim() === "") return 0;
        const parsed = Date.parse(expires.text);
        return Number.isFinite(parsed) ? Math.floor(parsed / 1000) : -1;
    }
    Component.onCompleted: {
        if (typeof identities !== "undefined" && identities.current)
            allowedIdentities = [String(identities.current.uuid).replace(/[{}]/g, "")];
    }
    Component.onDestruction: newSecret = ""
    CheckBox {
        id: enabledControl
        text: qsTr("Enable local MCP access")
        checked: settingsSession.values["mcp/enabled"]
    }
    Label { text: qsTr("Loopback address") }
    TextField {
        id: address
        Layout.fillWidth: true
        text: settingsSession.values["mcp/address"]
        maximumLength: 64
    }
    Label { text: qsTr("Port") }
    SpinBox { id: port; from: 1; to: 65535; value: settingsSession.values["mcp/port"]; editable: true }
    Label { text: qsTr("Approval timeout (seconds)") }
    SpinBox { id: timeout; from: 1; to: 300; value: settingsSession.values["mcp/approvalTimeout"]; editable: true }
    Label {
        Layout.fillWidth: true
        text: qsTr("Endpoint changes apply on Save. Key and policy actions below apply immediately. MCP requests always require a bearer key; sensitive reads and changes also require approval.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }
    Label { text: qsTr("API keys"); font.bold: true }
    Repeater {
        model: mcp ? mcp.keys : []
        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Button {
                    id: keyButton
                    Layout.fillWidth: true
                    text: modelData.name + (modelData.enabled ? "" : qsTr(" (disabled)"))
                    contentItem: Label { text: keyButton.text; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere }
                    onClicked: root.loadKey(modelData)
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.id + "\n" + qsTr("Created: %1 · Last used: %2").arg(new Date(Number(modelData.created) * 1000).toLocaleString())
                        .arg(modelData.lastUsed ? new Date(Number(modelData.lastUsed) * 1000).toLocaleString() : qsTr("Never"))
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
            }
            Button {
                text: qsTr("Revoke")
                onClicked: mcp.revokeKey(modelData.id)
            }
        }
    }
    Button {
        text: qsTr("New key")
        onClicked: {
            root.selectedKey = "";
            keyName.text = "";
            root.allowedIdentities = identities.current ? [String(identities.current.uuid).replace(/[{}]/g, "")] : [];
            root.grantedCapabilities = [];
            expires.text = "";
            keyEnabled.checked = true;
            root.newSecret = "";
        }
    }
    TextField { id: keyName; placeholderText: qsTr("Key name"); maximumLength: 128; Layout.fillWidth: true }
    Label {
        Layout.fillWidth: true
        text: root.selectedKey ? qsTr("Key ID: %1").arg(root.selectedKey) : ""
        visible: text.length > 0
        textFormat: Text.PlainText
        wrapMode: Text.WrapAnywhere
    }
    CheckBox { id: keyEnabled; text: qsTr("Key enabled"); checked: true }
    Label { text: qsTr("Allowed identities") }
    Repeater {
        model: mcp ? mcp.identities : []
        CheckBox {
            required property var modelData
            text: modelData.name + " · " + modelData.id
            checked: root.allowedIdentities.indexOf(modelData.id) >= 0
            onClicked: root.allowedIdentities = root.toggle(root.allowedIdentities, modelData.id, checked)
        }
    }
    Label { text: qsTr("Capabilities (read_only restricts mutations)") }
    Flow {
        Layout.fillWidth: true
        Repeater {
            model: mcp ? mcp.capabilities : []
            CheckBox {
                required property string modelData
                text: modelData
                checked: root.grantedCapabilities.indexOf(modelData) >= 0
                onClicked: root.grantedCapabilities = root.toggle(root.grantedCapabilities, modelData, checked)
            }
        }
    }
    TextField {
        id: expires
        maximumLength: 24
        placeholderText: qsTr("Expiration in UTC (2026-12-01T00:00:00Z); blank for none")
        Layout.fillWidth: true
        Accessible.name: placeholderText
    }
    Button {
        text: root.selectedKey ? qsTr("Save key permissions") : qsTr("Create key")
        enabled: !!mcp && keyName.text.length > 0 && root.allowedIdentities.length > 0 && root.expirySeconds() >= 0
        onClicked: {
            if (root.selectedKey) {
                mcp.updateKey(root.selectedKey, keyName.text, root.allowedIdentities, root.grantedCapabilities, root.expirySeconds(), keyEnabled.checked);
            } else {
                const created = mcp.createKey(keyName.text, root.allowedIdentities, root.grantedCapabilities, root.expirySeconds());
                if (created.id) { root.selectedKey = created.id; root.newSecret = created.secret; }
            }
        }
    }
    Label {
        visible: root.newSecret.length > 0
        text: qsTr("Copy this secret now. It is shown once and cannot be retrieved later.")
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    TextArea {
        visible: root.newSecret.length > 0
        text: root.newSecret
        textFormat: TextEdit.PlainText
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.WrapAnywhere
        Layout.fillWidth: true
    }
    Button { visible: root.newSecret.length > 0; text: qsTr("Hide secret"); onClicked: root.newSecret = "" }
    Label { text: qsTr("Persistent approval decisions"); font.bold: true }
    Repeater {
        model: mcp ? mcp.rules : []
        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: modelData.key + " · " + modelData.tool + " · " + modelData.identity + "\n"
                    + (modelData.contact || qsTr("All contacts")) + " · " + modelData.decision
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Button { text: qsTr("Edit"); onClicked: {
                policyKey.text = modelData.key; policyIdentity.text = modelData.identity;
                policyTool.text = modelData.tool; policyContact.text = modelData.contact;
                policyDecision.currentIndex = ["Ask", "Allow", "Deny"].indexOf(modelData.decision);
            } }
            Button { text: qsTr("Remove"); onClicked: mcp.removeRule(modelData.id) }
        }
    }
    TextField { id: policyKey; placeholderText: qsTr("API-key ID"); maximumLength: 36; Layout.fillWidth: true }
    TextField { id: policyIdentity; placeholderText: qsTr("Local identity UUID"); maximumLength: 36; Layout.fillWidth: true }
    TextField { id: policyTool; placeholderText: qsTr("Tool name"); maximumLength: 64; Layout.fillWidth: true }
    TextField { id: policyContact; placeholderText: qsTr("Contact UUID; empty means all contacts"); maximumLength: 36; Layout.fillWidth: true }
    ComboBox { id: policyDecision; model: ["Ask", "Allow", "Deny"] }
    Button {
        text: qsTr("Save approval policy")
        onClicked: mcp.setRule(policyKey.text, policyIdentity.text, policyTool.text, policyContact.text, policyDecision.currentText)
    }
    Label { text: qsTr("Identity defaults and contact overrides"); font.bold: true }
    Repeater {
        model: mcp ? mcp.contactPolicies : []
        Button {
            required property var modelData
            Layout.fillWidth: true
            text: modelData.identity + " · " + (modelData.contact || qsTr("Default")) + " · " + modelData.permission + " · " + modelData.decision
            onClicked: {
                contactIdentity.text = modelData.identity; contactTarget.text = modelData.contact;
                contactPermission.currentIndex = contactPermission.model.indexOf(modelData.permission);
                contactDecision.currentIndex = contactDecision.model.indexOf(modelData.decision);
            }
        }
    }
    TextField { id: contactIdentity; placeholderText: qsTr("Local identity UUID"); maximumLength: 36; Layout.fillWidth: true }
    TextField { id: contactTarget; placeholderText: qsTr("Contact UUID; empty changes the identity default"); maximumLength: 36; Layout.fillWidth: true }
    ComboBox { id: contactPermission; model: ["send_messages", "read_message_content", "read_profiles", "send_files", "read_files"] }
    ComboBox { id: contactDecision; model: ["Allow", "Ask", "Deny", "Inherit"] }
    Button {
        text: qsTr("Save contact policy")
        enabled: contactTarget.text.length > 0 || contactDecision.currentText !== "Inherit"
        onClicked: mcp.setContactPolicy(contactIdentity.text, contactTarget.text, contactPermission.currentText, contactDecision.currentText)
    }
    Label { text: mcp ? mcp.error : ""; textFormat: Text.PlainText; color: appearance.error; visible: text.length > 0 }
    function commit() {
        settingsSession.values["mcp/enabled"] = enabledControl.checked;
        settingsSession.values["mcp/address"] = address.text;
        settingsSession.values["mcp/port"] = port.value;
        settingsSession.values["mcp/approvalTimeout"] = timeout.value;
    }
}
