#ifndef TORADDRESS_H
#define TORADDRESS_H

#include <QMetaType>
#include <QString>

namespace ds::tor {

struct TorAddress {
    QString service_id; // canonical lowercase v3 identifier, without .onion
    quint16 port = 0;

    static TorAddress parse(const QString& value);
    static bool isValidServiceId(const QString& value) noexcept;

    QString host() const;
    QString toString() const;
};

} // namespace ds::tor

Q_DECLARE_METATYPE(ds::tor::TorAddress)

#endif // TORADDRESS_H
