
#include <memory>
#include <QSettings>

#include "tst_messages.h"
#include "ds/crypto.h"
#include "ds/dscert.h"
#include "ds/dsengine.h"
#include "ds/messagemodel.h"


TestMessagesModel::TestMessagesModel()
{

}

void TestMessagesModel::test_create_message()
{
//    auto settings = std::make_unique<QSettings>();
//    settings->clear();
//    settings->setValue("dbpath", ":memory:");
//    ds::core::DsEngine engine(std::move(settings));
//    ds::models::MessageModel msgmodel(engine.settings());

//    auto cert = ds::crypto::DsCert::create();
//    ds::core::Identity identity;

//    auto conversation_id = ds::crypto::Crypto::generateId();

//    identity.cert = cert->getCert();
//    msgmodel.setConversation(1, conversation_id);

//    msgmodel.sendMessage("Test me please", identity, conversation_id);

//    QCOMPARE(msgmodel.rowCount() , 1);

    // TODO: get the message and test signature
}
