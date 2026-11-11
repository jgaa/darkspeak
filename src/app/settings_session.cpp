#include "settings_session.h"
#include "appearance.h"
#include "desktop_integration.h"
#include "ds/resourcelimits.h"

#include <QHostAddress>
#include <QFileInfo>
#include <cmath>

namespace {
QVariantMap defaults()
{
    using Limits = ds::core::ResourceLimits;
    return {
        {"appAutoConnect", true}, {"autostart", false}, {"useDarkSpeakFolder", true},
        {"autoAcceptIncomingFiles", false}, {"includeConversationNameInDownloadPath", false},
        {"torMode", 1}, {"torCmd", "tor"}, {"torCtlHost", "127.0.0.1"}, {"torCtlPort", 9051},
        {"torCtlPasswd", ""}, {"torSocksHost", "127.0.0.1"}, {"torSocksPort", 9050},
        {"torServiceFromPort", 1025}, {"torServiceToPort", 29999}, {"torAppHost", "127.0.0.1"},
        {"torCtlAuthMode", 0}, {"dbpath", ""}, {"downloadLocation", ""},
        {"logPath", ""}, {"logLevelFile", 0}, {"logLevelStdout", 0}, {"logLevelApp", 4},
        {"resourceUnapprovedConnections", Limits::defaultUnapprovedConnections},
        {"resourceOutstandingRequests", Limits::defaultOutstandingRequests},
        {"resourcePendingConnectionRequests", Limits::defaultPendingConnectionRequests},
        {"resourceActiveFileTransfers", Limits::defaultActiveFileTransfers},
        {"resourceQueuedFileOffers", Limits::defaultQueuedFileOffers},
        {"resourceRetainedInterruptedTransfers", Limits::defaultRetainedInterruptedTransfers},
        {"resourceAcceptedFileBytes", QVariant::fromValue(Limits::defaultAcceptedFileBytes)},
        {"resourceUnfinishedIncomingBytes", QVariant::fromValue(Limits::defaultUnfinishedIncomingBytes)},
        {"resourceIncomingDiskBytes", QVariant::fromValue(Limits::defaultIncomingDiskBytes)},
        {"resourceMinimumFreeSpacePercent", Limits::defaultMinimumFreeSpacePercent},
        {"resourceSocketOutputBytes", Limits::defaultSocketOutputBytes}
    };
}
}
SettingsSession::SettingsSession(Appearance &appearance, DesktopIntegration &desktop, QObject *parent)
    : QObject(parent), appearance_(appearance), desktop_(desktop)
{
    const auto initial = defaults();
    for (auto it = initial.cbegin(); it != initial.cend(); ++it) values_.insert(it.key(), it.value());
}
void SettingsSession::begin()
{
    QSettings settings;
    original_.clear();
    const auto initial = defaults();
    for (auto it = initial.cbegin(); it != initial.cend(); ++it) {
        const auto value = settings.value(it.key(), it.value());
        original_.insert(it.key(), value);
        values_.insert(it.key(), value);
    }
    error_.clear();
    emit errorChanged();
    appearance_.beginPreview();
}
bool SettingsSession::fail(const QString &error)
{
    error_ = error;
    emit errorChanged();
    return false;
}
bool SettingsSession::save()
{
    const auto integer = [this](const char *key, quint64 minimum, quint64 maximum) {
        bool ok = false;
        const double value = values_.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(value) && std::floor(value) == value
            && value >= minimum && value <= maximum;
    };
    for (const auto *key : {"torCtlPort", "torSocksPort", "torServiceFromPort", "torServiceToPort"})
        if (!integer(key, 1, 65535)) return fail(tr("Tor ports must be between 1 and 65535."));
    if (values_.value("torServiceFromPort").toInt() > values_.value("torServiceToPort").toInt())
        return fail(tr("The first service port must not exceed the last service port."));
    for (const auto *key : {"torCtlHost", "torSocksHost", "torAppHost"}) {
        const auto host = values_.value(QLatin1String(key)).toString();
        if (!host.isEmpty() && host != QStringLiteral("localhost") && QHostAddress(host).isNull())
            return fail(tr("Tor hosts must be IP addresses or localhost."));
    }
    if (!integer("torMode", 0, 1) || !integer("torCtlAuthMode", 0, 1))
        return fail(tr("Select a valid Tor mode and authentication method."));
    if (values_.value("torMode").toInt() == 1 && values_.value("torCmd").toString().trimmed().isEmpty())
        return fail(tr("Specify the Tor command."));
    for (const auto *key : {"logLevelFile", "logLevelStdout", "logLevelApp"})
        if (!integer(key, 0, 6)) return fail(tr("Select a valid logging level."));
    using Limits = ds::core::ResourceLimits;
    const struct { const char *key; quint64 minimum; quint64 maximum; } limits[] = {
        {"resourceUnapprovedConnections", 1, Limits::hardUnapprovedConnections},
        {"resourceOutstandingRequests", 1, Limits::hardOutstandingRequests},
        {"resourcePendingConnectionRequests", 1, Limits::hardPendingConnectionRequests},
        {"resourceActiveFileTransfers", 1, Limits::hardActiveFileTransfers},
        {"resourceQueuedFileOffers", 1, Limits::hardQueuedFileOffers},
        {"resourceRetainedInterruptedTransfers", 1, Limits::hardRetainedInterruptedTransfers},
        {"resourceAcceptedFileBytes", 1024 * 1024, Limits::hardAcceptedFileBytes},
        {"resourceUnfinishedIncomingBytes", 1024 * 1024, Limits::hardUnfinishedIncomingBytes},
        {"resourceIncomingDiskBytes", 1024ULL * 1024 * 1024, Limits::hardIncomingDiskBytes},
        {"resourceMinimumFreeSpacePercent", 1, 100},
        {"resourceSocketOutputBytes", 16 * 1024, Limits::hardSocketOutputBytes}
    };
    for (const auto &limit : limits)
        if (!integer(limit.key, limit.minimum, limit.maximum))
            return fail(tr("A resource limit is outside the supported range."));

    // Validate everything before performing any side effects. Desktop integration
    // failures keep the dialog open; never silently claim that settings were saved.
    QSettings before_save;
    QVariantMap snapshot;
    for (auto it = original_.cbegin(); it != original_.cend(); ++it)
        snapshot.insert(it.key(), before_save.contains(it.key()) ? before_save.value(it.key()) : QVariant{});
    const auto restore_settings = [&snapshot] {
        QSettings settings;
        for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
            if (it.value().isValid()) settings.setValue(it.key(), it.value());
            else settings.remove(it.key());
        }
        settings.sync();
    };
    const bool old_autostart = original_.value("autostart").toBool();
    const bool autostart = values_.value("autostart").toBool();
    if (autostart != old_autostart && !desktop_.setAutostartEnabled(autostart))
        return fail(tr("Unable to update automatic startup."));
    const bool old_folder = original_.value("useDarkSpeakFolder").toBool();
    const bool folder = values_.value("useDarkSpeakFolder").toBool();
    if (folder != old_folder && !desktop_.setDarkSpeakFolderEnabled(folder)) {
        const bool restored = autostart == old_autostart || desktop_.setAutostartEnabled(old_autostart);
        restore_settings();
        return fail(restored ? tr("Unable to update the DarkSpeak folder.")
                             : tr("Unable to update the DarkSpeak folder or restore automatic startup. Check your desktop startup settings."));
    }
    QSettings settings;
    for (auto it = original_.cbegin(); it != original_.cend(); ++it) {
        // Enabling the managed folder supplies download/log defaults. Explicit
        // path edits in this transaction take priority over those defaults.
        if (folder && folder != old_folder && (it.key() == "downloadLocation" || it.key() == "logPath")
            && values_.value(it.key()) == it.value()) continue;
        if (it.key().startsWith(QStringLiteral("resource"))) {
            // Persist validated integer budgets as integers, not QML doubles
            // whose scientific notation cannot be read by ResourceLimits.
            settings.setValue(it.key(), QVariant::fromValue<qulonglong>(
                static_cast<qulonglong>(values_.value(it.key()).toDouble())));
        } else settings.setValue(it.key(), values_.value(it.key()));
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        const bool restored = autostart == old_autostart || desktop_.setAutostartEnabled(old_autostart);
        restore_settings();
        return fail(restored ? tr("Unable to write settings. Check configuration directory permissions.")
                             : tr("Unable to write settings or restore automatic startup. Check configuration permissions and desktop startup settings."));
    }
    appearance_.savePreview();
    return true;
}
void SettingsSession::cancel() { appearance_.cancelPreview(); }
