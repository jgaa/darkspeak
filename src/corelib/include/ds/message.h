#ifndef MESSAGE_H
#define MESSAGE_H

#include <QByteArray>
#include <QDateTime>

#include "ds/dscert.h"

namespace ds {
namespace core {

enum class Encoding
{
    US_ACSII,
    UTF8
};

enum class Direction {
    OUTGOING,
    INCOMING
};

class Message
{
public:
    Direction direction = Direction::OUTGOING;

    // Hash key for conversation
    // For p2p, it's the receivers certificate hash.
    QByteArray conversation;
    QByteArray message_id;
    QDateTime composed_time;
    QDateTime sent_time;
    QDateTime received_time;
    QByteArray content;

    // Senders certificate hash.
    QByteArray sender;
    Encoding encoding = Encoding::US_ACSII;
    QByteArray signature; // conversation | message_id | composed_time | content | sender | encoding

    void init();
    void sign(const crypto::DsCert& cert);
    bool validate(const crypto::DsCert& cert) const;
};

}} // Namespaces

#endif // MESSAGE_H
