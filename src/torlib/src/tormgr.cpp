#include <cassert>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ds/torcontroller.h>
#include "ds/tormgr.h"
#include "logfault/logfault.h"

namespace ds {
namespace tor {


TorMgr::TorMgr(const TorConfig& config)
    : config_{config}
{
    connect(&process_, &QProcess::readyReadStandardOutput,
            this, &TorMgr::onProcessOutput);
    connect(&process_, &QProcess::readyReadStandardError,
            this, &TorMgr::onProcessOutput);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping_) {
            const auto message = process_.errorString();
            LFLOG_ERROR << "Embedded Tor process error: " << message;
            emit error(message);
            emit offline();
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &TorMgr::onProcessFinished);
}

TorController *TorMgr::getController() { return ctl_.get(); }

QString TorMgr::getTorVersion() const
{
    return ctl_ ? ctl_->getVersion() : QString{};
}

/* To start using a tor service, we need to connect over TCP to
 * the contol channel of a running server.
 * Then we need to get the authentication methods from the
 * PROTOCOLINFO command, and based on that,
 * perform one of the following authentication operations
 * (listed in the preffered order):
 *  - PASSWORD
 *  - SAFECOOKIE
 *  - COOKIE (depricated)
 *
 * We don't support NULL. Too suspicious!
 */

void TorMgr::start()
{
    if (config_.mode == TorConfig::Mode::SYSTEM) {
        startUseSystemInstance();
    } else {
        startPrivateInstance();
    }
}

void TorMgr::stop()
{
    stopping_ = true;
    if (existing_ctl_) {
        existing_ctl_->stop();
        existing_ctl_.reset();
        waiting_for_existing_tor_ = false;
    }
    if (ctl_) {
        ctl_->stop();
    }
    if (config_.mode == TorConfig::Mode::PRIVATE) {
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();
        } else {
            emit stopped();
        }
    } else if (!ctl_) {
        emit stopped();
    }
}

void TorMgr::updateConfig(const TorConfig &config)
{
    config_ = config;
}

void TorMgr::createService(const QUuid& service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    ctl_->createService(service);
}

void TorMgr::startService(const ServiceProperties &service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    ctl_->startService(service);
}

void TorMgr::stopService(const QUuid& service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    try {
        ctl_->stopService(service);
    } catch(const TorController::NoSuchServiceError&) {
        LFLOG_WARN << "Cannot stop non-existing service with id " << service.toString();
        emit serviceStopped(service);
    }


}

void TorMgr::onTorStateUpdate(TorController::TorState state, int progress, const QString &summary)
{
    emit torStateUpdate(state, progress, summary);
}

void TorMgr::onStateUpdate(TorController::CtlState state)
{
    emit stateUpdate(state);
    switch(state) {
    case TorController::CtlState::DISCONNECTED:
        emit offline();
        break;
    case TorController::CtlState::CONNECTING:
        break;
    case TorController::CtlState::AUTHENTICATING:
        break;
    case TorController::CtlState::CONNECTED:
        emit started();
        break;
    case TorController::CtlState::ONLINE:
        emit online();
        break;
    case TorController::CtlState::STOPPING:
        break;
    case TorController::CtlState::STOPPED:
        emit stopped();
        if (config_.mode == TorConfig::Mode::PRIVATE
                && !stopping_ && process_.state() != QProcess::NotRunning) {
            QTimer::singleShot(250, this, [this]() {
                if (ctl_ && ctl_->getCtlState() == TorController::CtlState::STOPPED) {
                    ctl_->stop();
                }
                startControlConnection();
            });
        }
        break;
    }
}

void TorMgr::onServiceCreated(const ServiceProperties &service)
{
    emit serviceCreated(service);
}

void TorMgr::onServiceFailed(const QUuid& service, const QByteArray &reason)
{
    emit serviceFailed(service, reason);
}

void TorMgr::onServiceStarted(const QUuid& service, const bool newService)
{
    emit serviceStarted(service, newService);
}

void TorMgr::onServiceStopped(const QUuid& service)
{
    emit serviceStopped(service);
}

void TorMgr::startUseSystemInstance()
{
    assert(!ctl_);
    createController();
    ctl_->start();

}

bool TorMgr::preparePrivateInstance()
{
    const auto appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) {
        runtime = appData;
    }
    if (appData.isEmpty()) {
        emit error(QStringLiteral("Unable to determine private Tor directories"));
        return false;
    }

    tor_data_dir_ = QDir(appData).filePath(QStringLiteral("tor"));
    tor_runtime_dir_ = QDir(runtime).filePath(QStringLiteral("darkspeak"));
    if (!QDir().mkpath(tor_data_dir_) || !QDir().mkpath(tor_runtime_dir_)) {
        emit error(QStringLiteral("Unable to create private Tor directories"));
        return false;
    }

    const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner;
    QFile::setPermissions(tor_data_dir_, ownerOnly);
    QFile::setPermissions(tor_runtime_dir_, ownerOnly);

    config_.ctl_socket = QDir(tor_runtime_dir_).filePath(QStringLiteral("control.sock"));
    config_.socks_socket = QDir(tor_runtime_dir_).filePath(QStringLiteral("socks.sock"));
    config_.ctl_port = 0;
    config_.socks_host = QHostAddress::LocalHost;

    // QNetworkProxy, used by the existing connection layer, accepts TCP
    // SOCKS endpoints but not Unix sockets. Reserve an ephemeral loopback
    // port so embedded Tor never uses the conventional public ports.
    if (!socks_reservation_.listen(QHostAddress::LocalHost, 0)) {
        emit error(QStringLiteral("Unable to reserve an embedded Tor SOCKS port"));
        return false;
    }
    config_.socks_port = socks_reservation_.serverPort();
    socks_reservation_.close();

    if (torDataDirectoryLocked()) {
        const auto lockPath = QDir(tor_data_dir_).filePath(QStringLiteral("lock"));
        bool ownedByCurrentUser = false;
#ifdef Q_OS_UNIX
        struct stat dataStat{};
        ownedByCurrentUser = ::stat(lockPath.toLocal8Bit().constData(), &dataStat) == 0
                && dataStat.st_uid == static_cast<uid_t>(::geteuid());
#else
        ownedByCurrentUser = QFileInfo(lockPath).isReadable();
#endif
        if (!ownedByCurrentUser) {
            emit error(QStringLiteral("The embedded Tor data directory is in use by another user"));
            return false;
        }

        const auto answer = QMessageBox::question(
            nullptr, QObject::tr("Tor is already running"),
            QObject::tr("An embedded Tor instance is already using this DarkSpeak data directory.\n\n"
                         "Do you want DarkSpeak to ask it to shut down?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            emit error(QStringLiteral("An existing embedded Tor instance was left running"));
            return false;
        }

        waiting_for_existing_tor_ = true;
        stopExistingInstance();
        return false;
    }

    QFile::remove(config_.ctl_socket);
    QFile::remove(config_.socks_socket);
    return true;
}

bool TorMgr::torDataDirectoryLocked() const
{
#ifdef Q_OS_UNIX
    const auto path = QDir(tor_data_dir_).filePath(QStringLiteral("lock"));
    const auto fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const auto locked = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return locked;
#else
    return QFile::exists(QDir(tor_data_dir_).filePath(QStringLiteral("lock")));
#endif
}

void TorMgr::startPrivateInstance()
{
    if (process_.state() != QProcess::NotRunning) {
        return;
    }
    stopping_ = false;
    if (!preparePrivateInstance()) {
        if (!waiting_for_existing_tor_) {
            emit offline();
        }
        return;
    }

    launchPrivateInstance();
}

void TorMgr::launchPrivateInstance()
{

    auto executable = config_.tor_command;
    if (!executable.contains(QDir::separator())) {
        executable = QStandardPaths::findExecutable(executable);
    }
    if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) {
        const auto message = QStringLiteral("Tor executable was not found");
        LFLOG_ERROR << message;
        emit error(message);
        emit offline();
        return;
    }

    auto args = TorConfig::privateArguments(tor_data_dir_, config_.ctl_socket,
                                            config_.socks_socket, config_.socks_port);
    args << QStringLiteral("--Log") << QStringLiteral("notice stdout");

    LFLOG_NOTICE << "Starting private Tor instance";
    process_.start(executable, args);
    connect(&process_, &QProcess::started, this, [this]() {
        QTimer::singleShot(250, this, &TorMgr::startControlConnection);
    });
}

void TorMgr::stopExistingInstance()
{
    auto existingConfig = config_;
    existingConfig.mode = TorConfig::Mode::SYSTEM;
    existing_ctl_ = std::make_shared<TorController>(existingConfig);
    connect(existing_ctl_.get(), &TorController::autenticated, this, [this]() {
        existing_shutdown_requested_ = true;
        try {
            existing_ctl_->shutdown();
        } catch (const std::exception& ex) {
            emit error(QStringLiteral("Unable to stop the existing Tor instance: %1")
                       .arg(QString::fromUtf8(ex.what())));
            QTimer::singleShot(0, this, [this]() {
                if (existing_ctl_) {
                    existing_ctl_->stop();
                }
            });
        }
    });
    connect(existing_ctl_.get(), &TorController::authFailed, this, [this]() {
        emit error(QStringLiteral("Unable to authenticate to the existing Tor instance"));
        QTimer::singleShot(0, this, [this]() {
            if (existing_ctl_) {
                existing_ctl_->stop();
            }
        });
    });
    connect(existing_ctl_.get(), &TorController::stopped, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            if (!existing_ctl_) {
                return;
            }
            if (!existing_shutdown_requested_) {
                emit error(QStringLiteral("Unable to stop the existing Tor instance"));
                existing_ctl_->stop();
                existing_ctl_.reset();
                waiting_for_existing_tor_ = false;
                emit offline();
                return;
            }
            existing_ctl_->stop();
            existing_ctl_.reset();
            waitForExistingInstance();
        });
    });
    existing_ctl_->start();
}

void TorMgr::waitForExistingInstance(int attempt)
{
    if (!torDataDirectoryLocked()) {
        waiting_for_existing_tor_ = false;
        existing_shutdown_requested_ = false;
        launchPrivateInstance();
        return;
    }
    if (attempt >= 100) {
        waiting_for_existing_tor_ = false;
        existing_shutdown_requested_ = false;
        emit error(QStringLiteral("The existing Tor instance did not shut down"));
        emit offline();
        return;
    }
    QTimer::singleShot(100, this, [this, attempt]() {
        waitForExistingInstance(attempt + 1);
    });
}

void TorMgr::startControlConnection()
{
    if (stopping_ || process_.state() == QProcess::NotRunning) {
        return;
    }
    if (!ctl_) {
        createController();
    }
    try {
        ctl_->start();
    } catch (const std::exception& ex) {
        LFLOG_DEBUG << "Tor control connection is not ready yet: " << ex.what();
        QTimer::singleShot(250, this, &TorMgr::startControlConnection);
    }
    QTimer::singleShot(1000, this, [this]() {
        if (!stopping_ && ctl_
                && ctl_->getCtlState() == TorController::CtlState::CONNECTING) {
            ctl_->stop();
        }
    });
}

void TorMgr::createController()
{
    ctl_ = std::make_shared<TorController>(config_);

    connect(ctl_.get(), &TorController::stateUpdate,
            this, &TorMgr::onStateUpdate);
    connect(ctl_.get(), &TorController::torStateUpdate,
            this, &TorMgr::onTorStateUpdate);
    connect(ctl_.get(), &TorController::serviceCreated,
            this, &TorMgr::onServiceCreated);
    connect(ctl_.get(), &TorController::serviceFailed,
            this, &TorMgr::onServiceFailed);
    connect(ctl_.get(), &TorController::serviceStarted,
            this, &TorMgr::onServiceStarted);
    connect(ctl_.get(), &TorController::serviceStopped,
            this, &TorMgr::onServiceStopped);
}

void TorMgr::onProcessOutput()
{
    const auto output = process_.readAllStandardOutput() + process_.readAllStandardError();
    for (const auto& line : output.split('\n')) {
        if (!line.trimmed().isEmpty()) {
            LFLOG_DEBUG << "Tor: " << line.trimmed().toStdString();
        }
    }
}

void TorMgr::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    onProcessOutput();
    ctl_.reset();
    if (!stopping_) {
        const auto message = QStringLiteral("Embedded Tor stopped unexpectedly (exit code %1)").arg(exitCode);
        LFLOG_ERROR << message;
        emit error(message);
        emit offline();
    }
    emit stopped();
    Q_UNUSED(status)
}

}} // namespaces
