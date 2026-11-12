import QtQuick
import QtQuick.Controls
import QtTest
import "../../../src/app" as UI

TestCase {
    id: testCase
    name: "Events"
    when: windowShown
    visible: true
    width: 1200
    height: 800
    Component {
        id: page
        UI.Events {}
    }
    Component {
        id: form
        UI.EventsSettings {}
    }
    function init() {
        failOnWarning(/.*/);
        appearance.scale = 100;
        events.clear();
    }
    function cleanup() {
        appearance.scale = 100;
        appearance.theme = "Native";
        appearance.mode = 0;
        manager.navigate(3);
    }
    function test_shell_dark_theme_list_and_details() {
        uiTest.populateEvents();
        appearance.theme = "Coffee";
        appearance.mode = 2;
        const component = Qt.createComponent(Qt.resolvedUrl("../../../src/app/main.qml"));
        compare(component.status, Component.Ready, component.errorString());
        const shell = component.createObject(testCase);
        verify(shell !== null);
        shell.width = 1400;
        shell.height = 800;
        manager.navigate(6);
        tryVerify(function () {
            return findChild(shell.contentItem, "eventsList") !== null;
        });
        const list = findChild(shell.contentItem, "eventsList");
        tryVerify(function () {
            return list.itemAtIndex(0) !== null;
        });
        waitForRendering(shell.contentItem);
        const item = list.itemAtIndex(0);
        mouseClick(item, 10, item.height / 2);
        const detail = findChild(shell.contentItem, "eventDetail");
        verify(detail.visible);
        compare(item.contentItem.color, appearance.selectedText);
        verify(item.text.indexOf("<fixture>.txt") !== -1);
        compare(item.contentItem.textFormat, Text.PlainText);
        verify(uiTest.captureWindow(shell, "events-coffee-dark-wide.png"));
        shell.width = 800;
        waitForRendering(shell.contentItem);
        verify(findChild(shell.contentItem, "eventsBack").visible);
        verify(uiTest.captureWindow(shell, "events-coffee-dark-narrow.png"));
        shell.destroy();
        wait(20);
    }
    function test_settings_draft_and_cancel() {
        settingsSession.begin();
        const original = settingsSession.values["events/enabled"];
        const view = createTemporaryObject(form, testCase);
        const enabledControl = findChild(view, "eventsEnabled");
        const precisionControl = findChild(view, "eventPrecision");
        const filenamesControl = findChild(view, "eventsRecordFilenames");
        compare(filenamesControl.checked, true);
        filenamesControl.checked = false;
        enabledControl.checked = !original;
        precisionControl.currentIndex = 4;
        compare(settingsSession.values["events/enabled"], original);
        view.commit();
        compare(settingsSession.values["events/enabled"], !original);
        compare(settingsSession.values["events/recordFilenames"], false);
        compare(settingsSession.values["events/precisionSeconds"], 3600);
        settingsSession.cancel();
        settingsSession.begin();
        compare(settingsSession.values["events/enabled"], original);
        compare(settingsSession.values["events/precisionSeconds"], 60);
        compare(settingsSession.values["events/recordFilenames"], true);
        settingsSession.cancel();
    }
    function test_responsive_details_and_clear_data() {
        return [
            {
                tag: "20%",
                scale: 20
            },
            {
                tag: "100%",
                scale: 100
            },
            {
                tag: "200%",
                scale: 200
            }
        ];
    }
    function test_responsive_details_and_clear(data) {
        appearance.scale = data.scale;
        uiTest.populateEvents();
        const view = createTemporaryObject(page, testCase, {
            width: 1100 * appearance.factor,
            height: 600 * appearance.factor
        });
        verify(view !== null);
        const list = findChild(view, "eventsList");
        tryVerify(function () {
            return list.count >= 2 && list.itemAtIndex(0) !== null;
        });
        compare(view.wide, true);
        const item = list.itemAtIndex(0);
        tryVerify(function () {
            return item.width > 0 && list.height > 0;
        });
        waitForRendering(view);
        mouseClick(item, 5 * appearance.factor, item.height / 2);
        verify(view.selectedEvent !== null);
        compare(findChild(view, "eventDetail").visible, true);
        view.width = 600 * appearance.factor;
        compare(view.wide, false);
        compare(list.visible, false);
        const back = findChild(view, "eventsBack");
        verify(back.visible);
        waitForRendering(view);
        mouseClick(back, back.width / 2, back.height / 2);
        compare(view.selectedEvent, null);
        verify(list.visible);
        waitForRendering(view);
        mouseClick(findChild(view, "clearEvents"));
        const confirmation = findChild(view, "clearEventsDialog");
        tryCompare(confirmation, "opened", true);
        confirmation.reject();
        verify(list.count >= 2);
        confirmation.open();
        confirmation.accept();
        tryCompare(list, "count", 1);
        compare(view.selectedEvent, null);
    }
}
