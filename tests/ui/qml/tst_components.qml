import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import org.darkspeak 1.0
import "../../../src/app" as UI

TestCase {
    id: testCase
    name: "ThemeComponents"
    when: windowShown
    visible: true
    width: 1000
    height: 700
    property QtObject dependencySupport: QtObject {
        property string installCommand: "sudo apt install ffmpeg"
        property int dismissals: 0
        property bool suppressed: false
        function dismiss(again) { dismissals++; suppressed = again; }
    }
    Component {
        id: dependencyDialog
        UI.VideoPreviewDependencyDialog { support: testCase.dependencySupport }
    }
    Component {
        id: fileCardFixture
        Item {
            id: fileHost
            width: 640
            height: 480
            property alias cList: fileHost
            property alias cFile: fixtureFile
            property var cData: ({stateName: "Offered", composedTime: new Date()})
            QmlIdentityReq { id: fixtureFile }
            UI.FileComponent {}
        }
    }
    Component {
        id: gallery
        UI.UiHarness {}
    }
    Component {
        id: settingsDialog
        UI.SettingsDialog {}
    }
    Component {
        id: pageHost
        UI.PrimaryPage {
            pageNumber: 3
            pageSource: Qt.resolvedUrl("FixturePage.qml").toString()
        }
    }
    function init() {
        failOnWarning(/.*/);
    }
    function cleanup() {
        testCase.height = 700;
        uiTest.populate(false);
        appearance.scale = 100;
        appearance.cancelPreview();
        appearance.theme = "Native";
        appearance.mode = 0;
        manager.navigate(3);
    }
    function test_bundled_components_compile() {
        const names = ["Events.qml", "EventsSettings.qml", "AboutDialog.qml", "AcceptContactDialog.qml", "AppearanceSettings.qml", "BioDialog.qml", "Chat.qml", "Contacts.qml", "Conversations.qml", "CustomWindowSizeDialog.qml", "EditContactDialog.qml", "EditIdentityDialog.qml", "EditUploadFileDialog.qml", "EmptyState.qml", "FileComponent.qml", "FileSelecter.qml", "FormattedTime.qml", "GeneralSettings.qml", "Header.qml", "Home.qml", "HomeForm.ui.qml", "Identities.qml", "ImportTorServiceDialog.qml", "Log.qml", "LogSettings.qml", "MainToolBar.qml", "MessageBubble.qml", "MessageComponent.qml", "PathsSettings.qml", "PrimaryPage.qml", "ResourceLimitRow.qml", "ResourceLimitsSettings.qml", "RoundedImage.qml", "ScaleControl.qml", "ScaleSelector.qml", "ScaledToolButton.qml", "SettingsDialog.qml", "StatusBadge.qml", "TorSettings.qml", "UiDialog.qml", "UiHarness.qml", "main.qml"];
        for (const name of names) {
            const component = Qt.createComponent(Qt.resolvedUrl("../../../src/app/" + name));
            compare(component.status, Component.Ready, name + ": " + component.errorString());
        }
        const dependency = Qt.createComponent(Qt.resolvedUrl("../../../src/app/VideoPreviewDependencyDialog.qml"));
        compare(dependency.status, Component.Ready, dependency.errorString());
    }
    function test_ffmpeg_dialog_dismissal_and_opt_out_data() {
        return [{tag: "100%", scale: 100, height: 700}, {tag: "compact 200%", scale: 200, height: 320}];
    }
    function test_ffmpeg_dialog_dismissal_and_opt_out(data) {
        appearance.scale = data.scale;
        testCase.height = data.height;
        dependencySupport.dismissals = 0;
        dependencySupport.suppressed = false;
        dependencySupport.installCommand = "sudo apt install ffmpeg";
        const dialog = createTemporaryObject(dependencyDialog, testCase);
        dialog.open(); tryCompare(dialog, "visible", true);
        verify(dialog.x >= 0 && dialog.y >= 0);
        verify(dialog.x + dialog.width <= testCase.width && dialog.y + dialog.height <= testCase.height);
        const command = findChild(dialog.contentItem, "ffmpegInstallCommand");
        compare(command.text, "sudo apt install ffmpeg"); compare(command.readOnly, true);
        const checkbox = findChild(dialog.contentItem, "dontShowFfmpegWarningAgain");
        compare(checkbox.checked, false);
        const flickable = dialog.contentItem.contentItem;
        flickable.contentY = Math.max(0, flickable.contentHeight - flickable.height);
        wait(10);
        mouseClick(checkbox); compare(checkbox.checked, true);
        dialog.accept(); tryCompare(dialog, "visible", false);
        compare(dependencySupport.dismissals, 1); compare(dependencySupport.suppressed, true);
        dependencySupport.installCommand = "";
        const generic = createTemporaryObject(dependencyDialog, testCase);
        generic.open(); tryCompare(generic, "visible", true);
        compare(findChild(generic.contentItem, "ffmpegInstallCommand").visible, false);
        generic.reject(); tryCompare(generic, "visible", false);
        compare(dependencySupport.suppressed, false);
    }
    function test_file_card_shows_and_clears_preview_without_accepting_original() {
        const view = createTemporaryObject(fileCardFixture, testCase);
        verify(view !== null);
        const preview = findChild(view, "mediaPreview");
        verify(preview !== null);
        compare(preview.visible, false);
        compare(view.cFile.state, File.FS_OFFERED);
        view.cFile.previewSource = Qt.resolvedUrl("../../assets/mcp/earth.jpg").toString();
        tryCompare(preview, "visible", true);
        verify(preview.width <= view.width);
        verify(preview.sourceSize.width <= 320 && preview.sourceSize.height <= 320);
        compare(view.cFile.state, File.FS_OFFERED);
        view.cFile.previewSource = "";
        tryCompare(preview, "visible", false);
        compare(preview.Layout.preferredHeight, 0);
        compare(view.cFile.state, File.FS_OFFERED);
    }
    function test_scale_bounds_and_cancel() {
        appearance.beginPreview();
        appearance.scale = 2000;
        compare(appearance.scale, 200);
        appearance.scale = 1;
        compare(appearance.scale, 20);
        appearance.cancelPreview();
        compare(appearance.scale, 100);
    }
    function test_native_virtualized_gallery() {
        const view = createTemporaryObject(gallery, testCase);
        verify(view !== null);
        const percentage = findChild(view.header, "scalePercentage");
        compare(percentage.displayText, appearance.scale + "%");
        compare(percentage.editable, false);
        appearance.scale = 110;
        compare(percentage.displayText, "110%");
        const list = findChild(view.contentItem, "fixtureHistory");
        verify(list !== null);
        tryVerify(function () {
            return mockMessages.delegates > 0;
        });
        for (const scale of [20, 50, 100, 150, 200]) {
            appearance.scale = scale;
            wait(30);
            verify(mockMessages.delegates < 300, "Only visible messages and the nearby buffer are instantiated");
            verify(list.width > 0 && list.height > 0);
        }
        appearance.scale = 100;
        list.positionViewAtIndex(9000, ListView.Beginning);
        wait(30);
        verify(mockMessages.delegates < 300);
        verify(uiTest.captureWindow(view, "messages-native.png"));
    }
    function test_scale_control_geometry_and_popup() {
        const view = createTemporaryObject(gallery, testCase);
        const selector = findChild(view.header, "scalePercentage");
        const plus = findChild(view.header, "zoomInButton");
        const minus = findChild(view.header, "zoomOutButton");
        const indicator = findChild(selector, "scaleIndicator");
        for (const scale of [20, 50, 100, 150, 200]) {
            appearance.scale = scale;
            wait(20);
            fuzzyCompare(selector.width, 72 * scale / 100, 0.51);
            fuzzyCompare(selector.height, 28 * scale / 100, 0.51);
            fuzzyCompare(plus.width, 28 * scale / 100, 0.51);
            fuzzyCompare(minus.height, 28 * scale / 100, 0.51);
            fuzzyCompare(indicator.width, 12 * scale / 100, 0.51);
            selector.popup.open();
            tryCompare(selector.popup, "opened", true);
            const list = selector.popup.contentItem;
            tryCompare(list, "count", 7);
            tryVerify(function () {
                return list.itemAtIndex(0) !== null;
            });
            fuzzyCompare(list.itemAtIndex(0).height, 28 * scale / 100, 0.51);
            selector.popup.close();
            tryCompare(selector.popup, "opened", false);
        }
        appearance.scale = 100;
        mouseClick(selector, selector.width * 0.85, selector.height / 2);
        tryCompare(selector.popup, "opened", true);
        const list = selector.popup.contentItem;
        tryVerify(function () {
            return list.itemAtIndex(5) !== null;
        });
        mouseClick(list.itemAtIndex(5));
        tryCompare(appearance, "scale", 150);
        tryCompare(selector.popup, "opened", false);
    }
    function test_scale_number_double_click_resets_without_popup() {
        const view = createTemporaryObject(gallery, testCase);
        const selector = findChild(view.header, "scalePercentage");
        const area = findChild(selector, "scaleResetArea");
        for (const scale of [20, 50, 150, 200]) {
            appearance.scale = scale;
            wait(20);
            fuzzyCompare(area.width, selector.width * 0.6, 0.01);
            mouseClick(area);
            compare(appearance.scale, scale);
            compare(selector.popup.visible, false);
            mouseDoubleClickSequence(area);
            compare(appearance.scale, 100);
            compare(selector.popup.visible, false);
        }
        mouseClick(selector, selector.width * 0.85, selector.height / 2);
        tryCompare(selector.popup, "opened", true);
        selector.popup.close();
    }
    function test_shell_and_primary_pages() {
        const component = Qt.createComponent(Qt.resolvedUrl("../../../src/app/main.qml"));
        compare(component.status, Component.Ready, component.errorString());
        const shell = component.createObject(testCase);
        verify(shell !== null, component.errorString());
        for (const page of [3, 1, 2, 4, 6, 0, 3]) {
            manager.navigate(page);
            wait(40);
        }
        mouseWheel(shell.contentItem, 300, 200, 0, 120, Qt.NoButton, Qt.ControlModifier);
        compare(appearance.scale, 110);
        mouseWheel(shell.contentItem, 300, 200, 0, 120, Qt.NoButton, Qt.NoModifier);
        compare(appearance.scale, 110);
        appearance.scale = 100;
        appearance.sidebarCollapsed = false;
        const toggle = findChild(shell.contentItem, "navigationToggle");
        compare(toggle.text, "menu_open");
        mouseClick(toggle);
        compare(appearance.sidebarCollapsed, true);
        compare(toggle.text, "menu");
        mouseClick(toggle);
        compare(appearance.sidebarCollapsed, false);
        compare(toggle.text, "menu_open");
        verify(uiTest.captureWindow(shell, "shell-native.png"));
        shell.destroy();
        wait(20);
    }
    function test_populated_primary_pages() {
        uiTest.populate(true);
        const component = Qt.createComponent(Qt.resolvedUrl("../../../src/app/main.qml"));
        compare(component.status, Component.Ready, component.errorString());
        const shell = component.createObject(testCase);
        verify(shell !== null, component.errorString());
        for (const page of [1, 2, 4, 6, 0]) {
            manager.navigate(page);
            wait(40);
        }
        shell.destroy();
        wait(20);
    }
    function test_settings_forms_load_and_cancel() {
        const dialog = createTemporaryObject(settingsDialog, testCase);
        verify(dialog !== null);
        dialog.open();
        const sections = findChild(dialog.contentItem, "settingsSections");
        verify(sections !== null);
        for (const scale of [20, 50, 100, 150, 200]) {
            appearance.scale = scale;
            for (let i = 0; i < 7; ++i) {
                sections.currentIndex = i;
                wait(10);
            }
        }
        appearance.scale = 170;
        dialog.reject();
        dialog.destroy();
        wait(20);
        compare(appearance.scale, 100);
    }
    function test_all_themes_update_existing_views_and_dialogs() {
        uiTest.populate(true);
        const component = Qt.createComponent(Qt.resolvedUrl("../../../src/app/main.qml"));
        compare(component.status, Component.Ready, component.errorString());
        const shell = component.createObject(testCase);
        verify(shell !== null, component.errorString());
        for (const page of [1, 2, 3, 4, 6, 0]) {
            manager.navigate(page);
            wait(20);
        }
        const view = createTemporaryObject(gallery, testCase);
        const dialog = createTemporaryObject(settingsDialog, testCase);
        dialog.open();
        const sections = findChild(dialog.contentItem, "settingsSections");
        sections.currentIndex = 1;
        tryVerify(function () {
            return findChild(dialog.contentItem, "themeSelector") !== null;
        });
        const selector = findChild(dialog.contentItem, "themeSelector");
        const modeSelector = findChild(dialog.contentItem, "appearanceModeSelector");
        compare(selector.count, 7);
        for (let themeIndex = 0; themeIndex < 7; ++themeIndex) {
            // Exercise the same activation signal as keyboard/mouse selection.
            selector.currentIndex = themeIndex;
            selector.activated(themeIndex);
            compare(appearance.themeIndex, themeIndex);
            const theme = appearance.theme;
            for (const mode of [1, 2]) {
                modeSelector.currentIndex = mode;
                modeSelector.activated(mode);
                compare(appearance.mode, mode);
                if (theme !== "Native")
                    compare(appearance.dark, appearance.darkOnly || mode === 2);
                compare(findChild(dialog.contentItem, "darkOnlyNotice").visible, appearance.darkOnly);
                for (const scale of [20, 50, 100, 150, 200]) {
                    appearance.scale = scale;
                    wait(5);
                    compare(shell.palette.window, appearance.window);
                    compare(view.palette.base, appearance.surface);
                    compare(dialog.palette.button, appearance.raised);
                    compare(modeSelector.palette.button, appearance.raised);
                    compare(modeSelector.contentItem.color, appearance.text);
                    compare(findChild(dialog.contentItem, "appearanceHeading").color, appearance.text);
                    const saveButton = findChild(dialog.footer, "saveSettingsButton");
                    compare(saveButton.palette.button, appearance.raised);
                    compare(saveButton.contentItem.color, appearance.text);
                    verify(dialog.width > 0 && dialog.height > 0);
                }
                appearance.scale = 100;
                if (!appearance.darkOnly || mode === 2) {
                    wait(20);
                    verify(uiTest.captureWindow(view, "theme-" + theme.replace(" ", "-") + "-" + mode + ".png"));
                    verify(uiTest.captureWindow(dialog.contentItem, "settings-" + theme.replace(" ", "-") + "-" + mode + ".png"));
                }
            }
            for (const page of [0, 1, 2, 3, 4]) {
                manager.navigate(page);
                wait(10);
            }
            verify(uiTest.captureWindow(shell, "shell-" + theme.replace(" ", "-") + ".png"));
        }
        dialog.reject();
        dialog.destroy();
        wait(20);
        compare(appearance.theme, "Native");
        compare(appearance.mode, 0);
        compare(appearance.scale, 100);
        shell.destroy();
        wait(20);
    }
    function test_page_retained_on_navigation() {
        manager.navigate(3);
        const host = createTemporaryObject(pageHost, testCase);
        tryVerify(function () {
            return host.item !== null;
        });
        const original = host.item;
        manager.navigate(2);
        compare(host.active, true);
        compare(host.visible, false);
        manager.navigate(3);
        compare(host.item, original);
        compare(host.item.visits, 1);
    }
}
