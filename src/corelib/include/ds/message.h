#ifndef MESSAGE_H
#define MESSAGE_H

#include <QByteArray>
#include <QDateTime>


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

struct Message
{
    int id = {};
    Direction direction = Direction::OUTGOING;
    QByteArray conversation;
    int contact = {};
    QByteArray message_id;
    QDateTime composed_time;
    QDateTime sent_time;
    QDateTime received_time;
    QByteArray content;
    QByteArray from;
    Encoding encoding = Encoding::US_ACSII;
    QByteArray signature;
};

struct MessageReq
{
    QByteArray conversation;
    QByteArray content;
    QByteArray from;
    Encoding encoding = Encoding::US_ACSII;
};

}} // Namespaces

#endif // MESSAGE_H
