
#include <assert.h>
#include <array>

#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/task.h"
#include "ds/dscert.h"
#include "ds/crypto.h"

#include <QString>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

using namespace ds::crypto;
using namespace std;

namespace ds {
namespace core {

DsEngine *DsEngine::instance_;

DsEngine::DsEngine()
    : settings_{std::make_unique<QSettings>(
                    QStringLiteral("Jgaas Internet"),
                    QStringLiteral("DarkSpeak"))}
{
    initialize();
}

DsEngine::DsEngine(std::unique_ptr<QSettings> settings)
    : settings_{move(settings)}
{
    initialize();
}

DsEngine::~DsEngine()
{
    assert(instance_ == this);
    instance_ = {};
}

DsEngine &DsEngine::instance()
{
    assert(instance_);
    return *instance_;
}

ProtocolManager &DsEngine::getProtocolMgr(ProtocolManager::Transport)
{
    assert(tor_mgr_);
    return *tor_mgr_;
}

const QByteArray& DsEngine::getName(const DsEngine::State state)
{
    static const array<QByteArray, 5> names = {{
        "INITIALIZING",
        "STARTING",
        "RUNNING",
        "CLOSING",
        "TERMINATED"
    }};

    return names.at(static_cast<size_t>(state));
}

bool DsEngine::isOnline() const
{
    if (tor_mgr_) {
        return tor_mgr_->isOnline();
    }

    return false;
}

void DsEngine::createIdentity(const IdentityReq& req)
{
    if (pending_identities_.contains(req.name)) {
        throw ExistsError(QStringLiteral("The name %1 is already in use").arg(req.name));
    }

    Identity id;
    id.name = req.name;
    id.avatar = req.avatar;
    id.notes = req.notes;

    pending_identities_.insert(req.name, id);

    auto name = req.name;
    whenOnline([this, name]() { tryMakeTransport(name); });

    // Create a RSA cert for the user. This may take some time on slow devices.
    Task::schedule([this, name] {
        // TODO: Take cert type from settings
        try {
            emit certCreated(name, DsCert::create(DsCert::Type::RSA_2048));
        } catch (const std::exception& ex) {
            emit identityError({name, ex.what()});
        }
    });
}

void DsEngine::onCertCreated(const QString &name, const DsCert::ptr_t cert)
{
    if (!pending_identities_.contains(name)) {
        qWarning() << "Received a cert for a non-existing name: " << name;
        return;
    }

    auto& id = pending_identities_[name];
    id.cert = cert->getCert();
    id.hash = cert->getHash();

    addIdentityIfReady(name);
}

void DsEngine::addIdentityIfReady(const QString &name)
{
    if (!pending_identities_.contains(name)) {
        const auto msg = QStringLiteral("No pending identity with that name");
        qWarning() << msg << ": " << name;
        emit identityError({name, msg});
        return;
    }

    {
        const auto& pi = pending_identities_[name];
        if (pi.cert.isEmpty() || pi.address.isEmpty()) {
            qDebug() << "The identity " << name << " is still not ready";
            return;
        }
    }

    auto id = pending_identities_.take(name);
    qDebug() << "Identity " << id.name << " is ready";
    emit identityCreated(id);
}

void DsEngine::whenOnline(std::function<void ()> fn)
{
    if (isOnline()) {
        fn();
    } else {
        when_online_.append(move(fn));
    }
}

void DsEngine::online()
{
    while(!when_online_.isEmpty()) {
        try {
            if (auto fn = when_online_.takeFirst()) {
                fn();
            }
        } catch (std::exception& ex) {
            qWarning() << "Failed to execute function when getting online: " << ex.what();
        }
    }
}

void DsEngine::onTransportHandleReady(const TransportHandle &th)
{
    const auto name = th.identityName;
    if (!pending_identities_.contains(name)) {
        const auto msg = QStringLiteral("No pending identity with that name");
        qWarning() << msg << ": " << name;
        emit identityError({name, msg});
        return;
    }

    auto& pi = pending_identities_[name];
    if (pi.address.isEmpty()) {
        pi.address = th.handle;
    } else {
        qDebug() << "The identity " << name << " already have a transport";
        return;
    }

    emit retryIdentityReady(name);
}

void DsEngine::onTransportHandleError(const TransportHandleError &th)
{
    auto name = th.identityName;
    qDebug() << "Transport-handle creaton failed: " << name
             << ". I Will try again.";
    whenOnline([this, name]() { tryMakeTransport(name); });
}

void DsEngine::close()
{

}

void DsEngine::start()
{
    setState(State::STARTING);
    tor_mgr_ = ProtocolManager::create(*settings_, ProtocolManager::Transport::TOR);

    connect(tor_mgr_.get(), &ProtocolManager::online, this, &DsEngine::online);

    connect(tor_mgr_.get(), &ProtocolManager::transportHandleReady,
            this, &DsEngine::onTransportHandleReady);

    connect(tor_mgr_.get(), &ProtocolManager::transportHandleError,
            this, &DsEngine::onTransportHandleError,
            Qt::QueuedConnection);

    connect(this, &DsEngine::retryIdentityReady,
            this, &DsEngine::addIdentityIfReady,
            Qt::QueuedConnection);
}


void DsEngine::initialize()
{
    assert(!instance_);
    instance_ = this;
    qDebug() << "Settings are in " << settings_->fileName();

    auto data_path = QStandardPaths::writableLocation(
                QStandardPaths::AppDataLocation);
    if (!QDir(data_path).exists()) {
        qDebug() << "Creating path: " << data_path;
        QDir().mkpath(data_path);
    }

    switch(auto version = settings_->value("version").toInt()) {
        case 0: { // First use
            qInfo() << "First use - initializing settings_->";
            settings_->setValue("version", 1);
        } break;
    default:
        qDebug() << "Settings are OK at version " << version;
    }

    if (settings_->value("dbpath", "").toString().isEmpty()) {
        QString dbpath = data_path;
#ifdef QT_DEBUG
        dbpath += "/darkspeak-debug.db";
#else
        dbpath += "/darkspeak.db";
#endif
        settings_->setValue("dbpath", dbpath);
    }

    if (settings_->value("log-path", "").toString().isEmpty()) {
        QString logpath = data_path;
#ifdef QT_DEBUG
        logpath += "/darkspeak-debug.log";
#else
        logpath += "/darkspeak.log";
#endif
        settings_->setValue("log-path", logpath);
    }

    database_ = std::make_unique<Database>(*settings_);

    connect(this, &DsEngine::certCreated, this, &DsEngine::onCertCreated);
}

void DsEngine::setState(DsEngine::State state)
{
    if (state_ != state) {
        auto old = state_;
        state_ = state;
        qDebug() << "DsEngine: Changing state from "
                 << getName(old) << " to " << getName(state);
        emit changedState(old, state);
    }
}

void DsEngine::tryMakeTransport(const QString &name)
{
    TransportHandleReq req{name};

    try {
        tor_mgr_->createTransportHandle(req);
    } catch (const std::exception& ex) {
        qDebug() << "Failed to create transport for " << name
                 << " (will try again later): "
                 << ex.what();

        if (isOnline()) {
            QTimer::singleShot(1000, this, [this, name]() {
                whenOnline([this, name]() { tryMakeTransport(name); });
            });

            // TODO: Fail after n retries
        } else {
            whenOnline([this, name]() { tryMakeTransport(name); });
        }
    }
}


}} // namespaces
