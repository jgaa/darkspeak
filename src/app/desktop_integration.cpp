#include "desktop_integration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>

namespace {

const QString entryPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("autostart/org.darkspeak.DarkSpeak.desktop"));
}

const QString iconPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("darkspeak/icons/org.darkspeak.DarkSpeak.svg"));
}

const QString darkSpeakFolderPath()
{
    return QDir(QDir::homePath()).filePath(QStringLiteral("DarkSpeak"));
}

const QString darkSpeakDownloadsPath()
{
    return QDir(darkSpeakFolderPath()).filePath(QStringLiteral("Downloads"));
}

const QString darkSpeakLogPath()
{
    return QDir(darkSpeakFolderPath()).filePath(QStringLiteral("Log/darkspeak.log"));
}

bool createPrivateDirectory(const QString &path)
{
    if (!QDir().mkpath(path)) {
        return false;
    }
    return QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

QString desktopExecArgument(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

}

DesktopIntegration::DesktopIntegration(QObject *parent)
    : QObject(parent)
{
}

bool DesktopIntegration::darkSpeakFolderForced() const
{
#ifdef DARKSPEAK_FLATPAK_BUILD
    return true;
#else
    return false;
#endif
}

bool DesktopIntegration::initializeDarkSpeakFolder()
{
    QSettings settings;
    const auto enabled = darkSpeakFolderForced()
        || settings.value(QStringLiteral("useDarkSpeakFolder"), true).toBool();
    if (!enabled) {
        return true;
    }

    if (!updateDarkSpeakFolder(false)) {
        return false;
    }
    settings.setValue(QStringLiteral("useDarkSpeakFolder"), true);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool DesktopIntegration::setDarkSpeakFolderEnabled(bool enabled)
{
    if (darkSpeakFolderForced()) {
        return initializeDarkSpeakFolder();
    }

    QSettings settings;
    if (enabled && !updateDarkSpeakFolder(true)) {
        return false;
    }
    settings.setValue(QStringLiteral("useDarkSpeakFolder"), enabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool DesktopIntegration::updateDarkSpeakFolder(bool forcePaths) const
{
    if (QDir::homePath().isEmpty()
        || !createPrivateDirectory(darkSpeakFolderPath())
        || !createPrivateDirectory(darkSpeakDownloadsPath())
        || !createPrivateDirectory(QFileInfo(darkSpeakLogPath()).absolutePath())) {
        qWarning() << "Unable to create the DarkSpeak folder at" << darkSpeakFolderPath();
        return false;
    }

    QSettings settings;
    if (forcePaths || settings.value(QStringLiteral("downloadLocation")).toString().isEmpty()) {
        settings.setValue(QStringLiteral("downloadLocation"), darkSpeakDownloadsPath());
    }
    if (forcePaths || settings.value(QStringLiteral("logPath")).toString().isEmpty()) {
        settings.setValue(QStringLiteral("logPath"), darkSpeakLogPath());
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool DesktopIntegration::initializeAutostart(bool enabled)
{
    // Do not remove an entry merely because an older build had no setting.
    return enabled ? setAutostartEnabled(true) : true;
}

bool DesktopIntegration::setAutostartEnabled(bool enabled)
{
    QString errorMessage;
    if (updateAutostart(enabled, &errorMessage)) {
        return true;
    }
    qWarning().noquote() << errorMessage;
    return false;
}

bool DesktopIntegration::updateAutostart(bool enabled, QString *errorMessage) const
{
    const auto entry = entryPath();
    if (!enabled) {
        return !QFileInfo::exists(entry) || QFile::remove(entry)
            || (errorMessage != nullptr && (*errorMessage =
                QStringLiteral("Unable to remove autostart entry: %1").arg(entry), false));
    }

    const auto icon = iconPath();
    if (!QDir().mkpath(QFileInfo(icon).absolutePath())) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to create icon directory: %1").arg(QFileInfo(icon).absolutePath());
        return false;
    }
    QFile resource(QStringLiteral(":/icons/darkspeak.svg"));
    if (!resource.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to read bundled application icon");
        return false;
    }
    QFile iconFile(icon);
    if (!iconFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || iconFile.write(resource.readAll()) < 0) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to write application icon: %1").arg(icon);
        return false;
    }

    const auto directory = QFileInfo(entry).absoluteDir();
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to create autostart directory: %1").arg(directory.absolutePath());
        return false;
    }
    QFile entryFile(entry);
    if (!entryFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to write autostart entry: %1").arg(entry);
        return false;
    }
    const auto contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nVersion=1.0\nName=DarkSpeak\n"
        "Comment=Secure peer-to-peer instant messenger over Tor\nExec=%1 --autostart\n"
        "Icon=%2\nCategories=Network;Chat;\nHidden=false\nStartupNotify=true\n"
        "Terminal=false\nX-GNOME-Autostart-enabled=true\n")
        .arg(desktopExecArgument(QCoreApplication::applicationFilePath()), icon);
    if (entryFile.write(contents.toUtf8()) < 0) {
        if (errorMessage != nullptr) *errorMessage = QStringLiteral("Unable to write autostart entry: %1").arg(entry);
        return false;
    }
    return true;
}
