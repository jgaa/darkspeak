#ifndef IDENTITY_H
#define IDENTITY_H

#include "ds/dscert.h"
#include <QString>
#include <QtGui/QImage>
#include <QUuid>

namespace ds {
namespace core {

struct Identity {
    QString name;
    QByteArray hash;
    QUuid uuid;
    crypto::DsCert::safe_array_t cert;
    QByteArray address;
    QByteArray addressData;
    QString notes;
    QImage avatar;
};

struct IdentityReq {
    QUuid uuid = QUuid::createUuid();
    QString name;
    QString notes;
    QImage avatar;
};


struct IdentityError {
    QUuid uuid;
    QString name;
    QString explanation;
};

}} // identities

Q_DECLARE_METATYPE(ds::core::Identity)
Q_DECLARE_METATYPE(ds::core::IdentityReq)
Q_DECLARE_METATYPE(ds::core::IdentityError)

#endif // IDENTITY_H
