#ifndef DS_SECRETSTORE_H
#define DS_SECRETSTORE_H

#include <memory>

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <safekeeping/SafeKeeping.h>

namespace ds::core {

/** OS-protected storage for DarkSpeak private material. */
class SecretStore final
{
public:
    explicit SecretStore(const QString& databasePath);
    ~SecretStore();

    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    QByteArray identityCertificate(const QUuid& identity) const;
    void storeIdentityCertificate(const QUuid& identity, const QByteArray& certificate);
    void removeIdentityCertificate(const QUuid& identity);

    QByteArray torServiceKey(const QUuid& identity) const;
    void storeTorServiceKey(const QUuid& identity, const QByteArray& key);
    void removeTorServiceKey(const QUuid& identity);

    void lock() noexcept;

private:
    static std::string namespaceName(const QString& databasePath);
    static std::string identityCertificateName(const QUuid& identity);
    static std::string torServiceKeyName(const QUuid& identity);
    QByteArray read(const std::string& name) const;
    void write(const std::string& name, const QByteArray& value);
    void remove(const std::string& name);

    std::unique_ptr<jgaa::safekeeping::SafeKeeping> vault_;
};

} // namespace ds::core

#endif // DS_SECRETSTORE_H
