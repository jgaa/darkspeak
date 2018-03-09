#ifndef CONTACT_H
#define CONTACT_H

#include <QString>
#include <QtGui/QImage>

namespace ds {
namespace core {

enum class InitiatedBy {
    ME,
    THEM
};

struct Contact {
    QString name;
    QString nickname;
    QString notes;
    QString group;
    QByteArray hash;
    QByteArray pubkey;
    QByteArray address;
    QImage avatar;
    InitiatedBy whoInitiated = InitiatedBy::ME;

    int getInitiatedBy() const noexcept {
        return static_cast<int>(whoInitiated);
    }
};

struct ContactReq {
    QString name;
    QString nickname;
    QString notes;
    QString group;
    QByteArray contactHandle;
    QImage avatar;
    InitiatedBy whoInitiated = InitiatedBy::ME;
};

struct ContactError {
    QString name;
    QString hash;
    QString explanation;
};

}} // namepsaces

Q_DECLARE_METATYPE(ds::core::Contact)
Q_DECLARE_METATYPE(ds::core::ContactReq)
Q_DECLARE_METATYPE(ds::core::ContactError)

#endif // CONTACT_H
