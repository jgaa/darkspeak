
#include <memory>
#include <QSettings>
#include <QSignalSpy>

#include "ds/dsengine.h"
#include "ds/contactsmodel.h"
#include "ds/contact.h"
#include "ds/dscert.h"
#include "tst_contactsmodel.h"

#include "logfault/logfault.h"

TestContactsModel::TestContactsModel()
{
}

void TestContactsModel::test_create_contact()
{
    auto pubkey = QByteArray::fromBase64("jvYAcNYJ0RIVu2axMQ5CIm7m9Cf08+dVz8Q/bgb4lzA=");
    QByteArray addr{"onion:testpzpcswyktnpd:12345"};

    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));
    ds::models::ContactsModel cmodel(engine.settings());
    QSignalSpy spy_rows_inserted(&cmodel, &ds::models::ContactsModel::rowsInserted);

    // Start the engine, connect to Tor, get ready
    engine.start();

    // Create a dummy identity just to meet the database contraint for contact.identity --> identity
    QSqlQuery inject("INSERT INTO identity (uuid, hash, name, cert, address, address_data, created) "
                     "VALUES ('uuid', '', 'test', '', '', '', 0)", engine.getDb());


    // Create a contact.
    ds::core::ContactReq cr;

    cr.identity = inject.lastInsertId().toInt();
    QVERIFY(cr.identity > 0);
    cr.name = "test";
    cr.contactHandle = cr.contactHandle = engine.getIdentityHandle(pubkey, addr);

    LFLOG_DEBUG << "Trying to create contact from handle: " << cr.contactHandle;
    engine.createContact(cr);
    QCOMPARE(spy_rows_inserted.wait(3000), true);

    auto map = ds::core::DsEngine::fromJson(cr.contactHandle);
    auto peer_pubkey = QByteArray::fromBase64(map["pubkey"].toByteArray());

    const auto dscert = ds::crypto::DsCert::createFromPubkey(peer_pubkey);
    const auto hash = dscert->getHash();

    QCOMPARE(cmodel.rowCount() , 1);
    QCOMPARE(cmodel.data(cmodel.index(0, cmodel.fieldIndex("name"), {})).toString(), cr.name);
    QCOMPARE(cmodel.hashExists(hash), true);
    QCOMPARE(cmodel.hashExists(QByteArray("Not a hash")), false);
}
