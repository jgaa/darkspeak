#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QWindow>
#include <QScreen>
#include <cmath>
#include "appearance.h"
#include "settings_session.h"
#include "desktop_integration.h"
#include "ds/textpolicy.h"
#include "ds/mimepreferences.h"
#include "ds/resourcelimits.h"
#ifdef DARKSPEAK_KDE_COLOR_SCHEME
#include <KColorSchemeManager>
#endif

class PreferencesTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("DarkSpeakUiTest");
        QCoreApplication::setApplicationName("preferences");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
    }
    void scale_preview_transaction() {
        Appearance appearance;
        appearance.setScale(110);
        appearance.beginPreview();
        appearance.setScale(175);
        QCOMPARE(QSettings{}.value("appearance/scale").toInt(), 110);
        appearance.cancelPreview();
        QCOMPARE(appearance.scale(), 110);
        appearance.beginPreview();
        appearance.setScale(125);
        appearance.savePreview();
        QCOMPARE(QSettings{}.value("appearance/scale").toInt(), 125);
    }
    void settings_cancel_does_not_write() {
        Appearance appearance;
        DesktopIntegration desktop;
        SettingsSession session(appearance, desktop);
        session.begin();
        const auto before = QSettings{}.value("autoAcceptIncomingFiles", false);
        session.values()->insert("autoAcceptIncomingFiles", !before.toBool());
        appearance.setScale(180);
        session.cancel();
        QCOMPARE(QSettings{}.value("autoAcceptIncomingFiles", false), before);
        QCOMPARE(appearance.scale(), 125);
    }
    void invalid_settings_have_no_side_effects() {
        Appearance appearance;
        DesktopIntegration desktop;
        SettingsSession session(appearance, desktop);
        session.begin();
        session.values()->insert("torServiceFromPort", 2000);
        session.values()->insert("torServiceToPort", 1000);
        QVERIFY(!session.save());
        QVERIFY(!session.error().isEmpty());
        QVERIFY(!QSettings{}.contains("torServiceFromPort"));
        session.cancel();
    }
    void geometry_is_clamped() {
        Appearance appearance;
        QWindow window;
        appearance.resizeWindow(&window, 99999, 99999);
        QVERIFY(window.width() <= window.screen()->availableGeometry().width());
        QVERIFY(window.height() <= window.screen()->availableGeometry().height());
        appearance.saveWindow(&window);
        QWindow restored;
        appearance.restoreWindow(&restored);
        QCOMPARE(restored.size(), window.size());
    }
    void native_palette_and_font_changes() {
        Appearance appearance;
        const auto old_palette = QApplication::palette();
        const auto old_font = QApplication::font();
        auto palette = old_palette;
        palette.setColor(QPalette::Window, QColor("#202020"));
        palette.setColor(QPalette::Text, QColor("#eeeeee"));
        QSignalSpy changes(&appearance, &Appearance::changed);
        QApplication::setPalette(palette);
        QCOMPARE(appearance.window(), QColor("#202020"));
        QVERIFY(appearance.dark());
        QVERIFY(changes.count() > 0);
        auto font = old_font;
        font.setPointSizeF(14);
        QApplication::setFont(font);
        QCOMPARE(appearance.font().pointSizeF(), 14 * appearance.factor());
        QApplication::setPalette(old_palette);
        QApplication::setFont(old_font);
    }
    void asset_fallback_is_compiled() {
        Appearance appearance;
        QCOMPARE(appearance.asset(QStringLiteral("../untrusted.svg")), appearance.asset(QStringLiteral("avatar")));
        QCOMPARE(appearance.asset(QStringLiteral("avatar")).scheme(), QStringLiteral("qrc"));
    }
    void bundled_theme_variants_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("dark_only");
        for (const auto &theme : {"Coffee", "Late Night", "Rosewood", "Windows", "Matrix", "Inferno"}) {
            for (int mode = 0; mode <= 2; ++mode) {
                const auto row = QByteArray(theme) + '-' + QByteArray::number(mode);
                QTest::newRow(row.constData()) << QString::fromLatin1(theme) << mode
                    << (QByteArray(theme) == "Late Night" || QByteArray(theme) == "Matrix");
            }
        }
    }
    void bundled_theme_variants() {
        QFETCH(QString, theme);
        QFETCH(int, mode);
        QFETCH(bool, dark_only);
        Appearance appearance;
        appearance.setTheme(theme);
        appearance.setMode(mode);
        const auto system = QApplication::palette();
        const bool system_dark = system.window().color().lightnessF() < system.text().color().lightnessF();
        QCOMPARE(appearance.theme(), theme);
        QCOMPARE(appearance.darkOnly(), dark_only);
        QCOMPARE(appearance.dark(), dark_only || mode == 2 || (mode == 0 && system_dark));
        QCOMPARE(appearance.variantUnavailable(), dark_only && mode == 1);
        // Body, metadata, selection, and status text must stay readable.
        for (const auto background : {appearance.window(), appearance.surface(), appearance.raised(),
                                      appearance.sidebar(), appearance.incoming(), appearance.outgoing()}) {
            QVERIFY2(contrast(appearance.text(), background) >= 4.5, "Body text contrast");
            QVERIFY2(contrast(appearance.secondaryText(), background) >= 4.5, "Metadata contrast");
        }
        QVERIFY(contrast(appearance.selectedText(), appearance.selected()) >= 4.5);
        for (const auto status : {appearance.success(), appearance.warning(), appearance.error()})
            QVERIFY(contrast(status, appearance.surface()) >= 4.5);
        for (const auto scale : {20, 50, 100, 150, 200}) {
            appearance.setScale(scale);
            QVERIFY(appearance.cornerRadius() >= 0);
            QVERIFY(appearance.bubbleRadius() >= 0);
            QCOMPARE(appearance.dark(), dark_only || mode == 2 || (mode == 0 && system_dark));
        }
        appearance.setScale(100);
        appearance.setTheme("Native");
        appearance.setMode(0);
    }
    void theme_preview_persistence_and_validation() {
        Appearance appearance;
        appearance.setTheme("Coffee");
        appearance.setMode(1);
        const auto before = appearance.palette();
        appearance.beginPreview();
        appearance.setTheme("Matrix");
        appearance.setMode(2);
        QCOMPARE(QSettings{}.value("appearance/theme").toString(), QString("Coffee"));
        appearance.cancelPreview();
        QCOMPARE(appearance.theme(), QString("Coffee"));
        QCOMPARE(appearance.mode(), 1);
        QCOMPARE(appearance.palette(), before);
        appearance.beginPreview();
        appearance.setTheme("Inferno");
        appearance.setMode(2);
        appearance.savePreview();
        QCOMPARE(QSettings{}.value("appearance/theme").toString(), QString("Inferno"));
        Appearance reopened;
        QCOMPARE(reopened.theme(), QString("Inferno"));
        QCOMPARE(reopened.mode(), 2);
        QVERIFY(reopened.dark());
        reopened.setTheme("../../untrusted.qml");
        reopened.setThemeIndex(-1);
        reopened.setThemeIndex(999);
        QCOMPARE(reopened.theme(), QString("Inferno"));
        reopened.setTheme("Native");
        reopened.setMode(0);
        QSettings{}.setValue("appearance/theme", "unknown");
        Appearance invalid;
        QCOMPARE(invalid.theme(), QString("Native"));
        invalid.setScale(100);
        QSettings{}.setValue("appearance/theme", "Native");
    }
    void custom_automatic_follows_system_palette() {
        Appearance appearance;
        appearance.setTheme("Coffee");
        appearance.setMode(0);
        const auto original = QApplication::palette();
        auto system = original;
        system.setColor(QPalette::Window, QColor("#171717"));
        system.setColor(QPalette::Text, QColor("#eeeeee"));
        QApplication::setPalette(system);
        QVERIFY(appearance.dark());
        appearance.setMode(1);
        QVERIFY(!appearance.dark());
        appearance.setMode(0);
        QVERIFY(appearance.dark());
        system.setColor(QPalette::Window, QColor("#eeeeee"));
        system.setColor(QPalette::Text, QColor("#171717"));
        QApplication::setPalette(system);
        QVERIFY(!appearance.dark());
        appearance.setTheme("Matrix");
        QVERIFY(appearance.dark());
        appearance.setTheme("Native");
        QApplication::setPalette(original);
    }
#ifdef DARKSPEAK_KDE_COLOR_SCHEME
    void kde_mode_preview_and_restore() {
        auto *manager = KColorSchemeManager::instance();
        if (!manager->indexForSchemeId(QStringLiteral("BreezeLight")).isValid()
            || !manager->indexForSchemeId(QStringLiteral("BreezeDark")).isValid())
            QSKIP("Breeze color schemes are not installed");
        Appearance appearance;
        appearance.setMode(1);
        QVERIFY(!appearance.dark());
        QVERIFY(!appearance.variantUnavailable());
        QCOMPARE(manager->activeSchemeId(), QStringLiteral("BreezeLight"));
        const auto light_palette = appearance.palette();
        appearance.beginPreview();
        appearance.setMode(2);
        QVERIFY(appearance.dark());
        QVERIFY(!appearance.variantUnavailable());
        QVERIFY(appearance.palette() != light_palette);
        QCOMPARE(manager->activeSchemeId(), QStringLiteral("BreezeDark"));
        QCOMPARE(QSettings{}.value("appearance/mode").toInt(), 1);
        appearance.cancelPreview();
        QCOMPARE(appearance.palette(), light_palette);
        QCOMPARE(appearance.mode(), 1);
        appearance.beginPreview();
        appearance.setMode(2);
        appearance.savePreview();
        QCOMPARE(QSettings{}.value("appearance/mode").toInt(), 2);
        appearance.setMode(0);
        QCOMPARE(manager->activeSchemeId(), QString());
        QCOMPARE(appearance.mode(), 0);
    }
#endif
    void ascii_policy_before_unicode() {
        using ds::core::text_policy::permittedAsciiEnvelope;
        // Envelope.message (16), Message.content (4). Other byte fields remain binary.
        const QByteArray valid = QByteArray::fromHex("8201052203616263");
        QVERIFY(permittedAsciiEnvelope(valid));
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("8201042202c3a9")));
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("820103220100")));
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("8201ff")));
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("ffffffffffffffffffff02")));
        // The same arbitrary bytes in the signature field must not be inspected as text.
        QVERIFY(permittedAsciiEnvelope(QByteArray::fromHex("8201043a02c3a9")));
        // UserInfo.bio.bio_text, and file display names, obey the same identity policy.
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("a2010622040a02c3a9")));
        QVERIFY(!permittedAsciiEnvelope(QByteArray::fromHex("8a01041a02c3a9")));
        QByteArray repetitions;
        for (int i = 0; i < 4097; ++i) repetitions += QByteArray::fromHex("0803");
        QVERIFY(!permittedAsciiEnvelope(repetitions));
    }
    void mime_preferences_are_bounded() {
        using ds::core::parseMimePreferences;
        QVERIFY(parseMimePreferences(QStringLiteral("image/png\nvideo/mp4")).has_value());
        QVERIFY(parseMimePreferences(QStringLiteral("*")).has_value());
        QVERIFY(!parseMimePreferences(QStringLiteral("*\nimage/png")));
        QVERIFY(!parseMimePreferences(QStringLiteral("image/png\nimage/png")));
        QVERIFY(!parseMimePreferences(QStringLiteral("IMAGE/png")));
        QVERIFY(!parseMimePreferences(QString(8193, QChar('a'))));
    }
    void stored_text_policy() {
        using ds::core::text_policy::permittedAscii;
        QVERIFY(permittedAscii(QStringLiteral("text\nwith\ttabs")));
        QVERIFY(!permittedAscii(QString::fromUtf8("Blåbær")));
        QVERIFY(!permittedAscii(QStringLiteral("name\n"), false));
        QVERIFY(!permittedAscii(QString(QChar(127))));
    }    void saved_budgets_round_trip_as_integers() {
        Appearance appearance;
        DesktopIntegration desktop;
        SettingsSession session(appearance, desktop);
        session.begin();
        session.values()->insert("resourceAcceptedFileBytes", double(5ULL * 1024 * 1024 * 1024));
        session.values()->insert("autoAcceptIncomingFiles", true);
        QVERIFY2(session.save(), qPrintable(session.error()));
        const auto limits = ds::core::ResourceLimits::fromSettings(QSettings{});
        QCOMPARE(limits.acceptedFileBytes, quint64(5ULL * 1024 * 1024 * 1024));
        QVERIFY(QSettings{}.value("autoAcceptIncomingFiles").toBool());
    }

private:
    static double contrast(const QColor &first, const QColor &second) {
        const auto luminance = [](const QColor &color) {
            const auto linear = [](double value) {
                return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
            };
            return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
        };
        const auto first_value = luminance(first);
        const auto second_value = luminance(second);
        return (std::max(first_value, second_value) + .05) / (std::min(first_value, second_value) + .05);
    }
    QTemporaryDir directory_;
};
QTEST_MAIN(PreferencesTest)
#include "preferences_test.moc"
