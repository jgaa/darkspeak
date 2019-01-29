#ifndef IDENTITY_H
#define IDENTITY_H

#include "ds/dscert.h"
#include <QString>
#include <QtGui/QImage>

namespace ds {
namespace core {

struct Identity {
    QString name;
    QByteArray hash;
    crypto::DsCert::safe_array_t cert;
    QByteArray address;
    QByteArray addressData;
    QString notes;
    QImage avatar;
};

struct IdentityReq {
    QString name;
    QString notes;
    QImage avatar;
};


struct IdentityError {
    QString name;
    QString explanation;
};

}} // identities

Q_DECLARE_METATYPE(ds::core::Identity)
Q_DECLARE_METATYPE(ds::core::IdentityReq)
Q_DECLARE_METATYPE(ds::core::IdentityError)

#endif // IDENTITY_H
