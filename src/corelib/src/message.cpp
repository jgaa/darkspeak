
#include <QTime>

#include "ds/crypto.h"
#include "ds/message.h"

using namespace ds::crypto;
using namespace std;

namespace ds {
namespace core {

void Message::init()
{
    message_id = crypto::Crypto::generateId();

    // Don't expose the exact time the mesage was composed
    auto secs = QDateTime::currentDateTime().currentSecsSinceEpoch();
    secs /= 60;
    secs *= 60;
    composed_time = QDateTime::fromSecsSinceEpoch(secs);
}

void Message::sign(const DsCert &cert)
{
    assert(!message_id.isEmpty());
    assert(composed_time.isValid());

    signature = cert.sign({
        conversation,
        message_id,
        QString::number(composed_time.currentSecsSinceEpoch()).toUtf8(),
        content,
        sender,
        QString::number(static_cast<int>(encoding)).toUtf8()
   });
}

}} // namespaces
