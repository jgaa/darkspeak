#include "ds/secretstore.h"

#include <QCryptographicHash>
#include <QByteArray>
#include <QUuid>

#include <cstddef>
#include <stdexcept>

namespace ds::core {

namespace {

std::span<const std::byte> bytesOf(const QByteArray& value)
{
    return {reinterpret_cast<const std::byte*>(value.constData()),
            static_cast<size_t>(value.size())};
}

QByteArray fromBytes(const std::vector<std::byte>& value)
{
    return {reinterpret_cast<const char*>(value.data()),
            static_cast<qsizetype>(value.size())};
}

std::string uuidName(const char* prefix, const QUuid& identity, const char* suffix)
{
    return std::string(prefix) + identity.toString(QUuid::WithoutBraces).toStdString()
           + suffix;
}

std::optional<std::string> testPassphrase()
{
    const auto value = qEnvironmentVariable("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE");
    if (value.isEmpty()) {
        return std::nullopt;
    }
    return value.toStdString();
}

} // namespace

SecretStore::SecretStore(const QString& databasePath)
{
    jgaa::safekeeping::SafeKeeping::setLinuxVaultRootName("org.darkspeak.DarkSpeak");
    const auto passphrase = testPassphrase();
    if (passphrase.has_value()) {
        jgaa::safekeeping::SafeKeeping::CreateOptions createOptions;
        createOptions.createSystemVaultSlot = false;
        createOptions.passphrase = passphrase;
        jgaa::safekeeping::SafeKeeping::UnlockOptions unlockOptions;
        unlockOptions.trySystemVaultFirst = false;
        unlockOptions.passphrase = passphrase;
        const auto name = namespaceName(databasePath);
        vault_ = jgaa::safekeeping::SafeKeeping::open(name, unlockOptions);
        if (!vault_) {
            vault_ = jgaa::safekeeping::SafeKeeping::createNew(name, createOptions).instance;
        }
    } else {
        vault_ = jgaa::safekeeping::SafeKeeping::openOrCreate(namespaceName(databasePath));
    }
    if (!vault_ || !vault_->isUnlocked()) {
        throw std::runtime_error("DarkSpeak OS secret vault is locked or unavailable");
    }
}

SecretStore::~SecretStore()
{
    lock();
}

std::string SecretStore::namespaceName(const QString& databasePath)
{
    const auto digest = QCryptographicHash::hash(databasePath.toUtf8(), QCryptographicHash::Sha256)
                             .toHex().left(32);
    return "darkspeak-" + digest.toStdString();
}

std::string SecretStore::identityCertificateName(const QUuid& identity)
{
    return uuidName("identity-", identity, "-certificate");
}

std::string SecretStore::torServiceKeyName(const QUuid& identity)
{
    return uuidName("identity-", identity, "-tor-key");
}

QByteArray SecretStore::read(const std::string& name) const
{
    const auto value = vault_->retrieveSecretBytes(name);
    if (!value.has_value()) {
        throw std::runtime_error("required DarkSpeak secret is missing from the OS vault");
    }
    return fromBytes(*value);
}

void SecretStore::write(const std::string& name, const QByteArray& value)
{
    if (value.isEmpty() || !vault_->storeSecret(name, bytesOf(value))) {
        throw std::runtime_error("failed to store DarkSpeak secret in the OS vault");
    }
}

void SecretStore::remove(const std::string& name)
{
    if (!vault_->removeSecret(name)) {
        throw std::runtime_error("failed to remove DarkSpeak secret from the OS vault");
    }
}

QByteArray SecretStore::identityCertificate(const QUuid& identity) const
{
    return read(identityCertificateName(identity));
}

void SecretStore::storeIdentityCertificate(const QUuid& identity, const QByteArray& certificate)
{
    write(identityCertificateName(identity), certificate);
}

void SecretStore::removeIdentityCertificate(const QUuid& identity)
{
    remove(identityCertificateName(identity));
}

QByteArray SecretStore::torServiceKey(const QUuid& identity) const
{
    return read(torServiceKeyName(identity));
}

void SecretStore::storeTorServiceKey(const QUuid& identity, const QByteArray& key)
{
    write(torServiceKeyName(identity), key);
}

void SecretStore::removeTorServiceKey(const QUuid& identity)
{
    remove(torServiceKeyName(identity));
}

void SecretStore::lock() noexcept
{
    if (vault_) {
        (void)vault_->lock();
    }
}

} // namespace ds::core
