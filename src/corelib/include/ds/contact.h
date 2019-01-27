#ifndef CONTACT_H
#define CONTACT_H

#include <QString>
#include <QtGui/QImage>
#include <QDateTime>

namespace ds {
namespace core {

struct Contact {

    enum InitiatedBy {
        ME,
        THEM
    };

    // Reference to the database-id to the Identity that own this contact
    int identity = {};

    // Our name for the contact. If unset, same as nickname
    QString name;

    // UUID for this contact. Used if we need a folder-name or file-name
    // uniqely for this contanct. We don't want to use a hash for this, as
    // potential contacts - for example the US government trying to
    // prove what wistleblower a journalist communicated with.
    QByteArray uuid;

    // The contacts own, chosen nickname
    QString nickname;

    // A hash from the pubkey.
    // Used for database lookups.
    QByteArray hash;

    // Our notes about the contact
    QString notes;

    // The group we place the contact in. If unset; 'Default'
    QString group;

    // The users public key. Stored as base-64 of the 32 bit binary key
    QByteArray pubkey;

    // The users onion address and port
    QByteArray address;

    // An optional image the contact can chose
    QImage avatar;

    // When the contact was created
    QDateTime crated;

    // Who sent the initial addme request?
    InitiatedBy whoInitiated = InitiatedBy::ME;

    // Updated when we receive data from the contact, rounded to minute.
    QDateTime lastSeen;

    // true if the contact is blocked
    bool blocked = false;

    // True if the contact rejected our addme request
    bool rejected = false;

    // true if we want to connect automatically when the related Identity is online
    bool autoConnect = true;

};

struct ContactReq {
    int identity = {};
    QString name;
    QString nickname;
    QString notes;
    QString group;
    QByteArray contactHandle;
    QImage avatar;
    QByteArray pubkey;
    QByteArray address;
    Contact::InitiatedBy whoInitiated = Contact::ME;
    bool autoConnect = true;
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
