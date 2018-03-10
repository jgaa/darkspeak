
#include <memory>
#include <QSettings>
#include <QSignalSpy>

#include "ds/dsengine.h"
#include "ds/contactsmodel.h"
#include "ds/contact.h"
#include "ds/dscert.h"
#include "tst_contactsmodel.h"

TestContactsModel::TestContactsModel()
{
}

void TestContactsModel::test_create_contact()
{
    QByteArray cert{"-----BEGIN RSA PRIVATE KEY-----\n"
                    "MIICXQIBAAKBgQDQi1vRvVZbzXhMGyeiz6GaKFGXVBIxhxkMXRRMcbHgn8Mx1rxt\n"
                    "1VfPyJvuu22lpHV9N+hYQmguY8T9eyyXJ2z2K2cKGZoR6G4giKjLa2lvG5BSChHU\n"
                    "rEG2mz5MpvOE203aTBW0DmADUFDVAvDivASyx2KzxlaK4sUBjBD4RgwY6wIEAQUY\n"
                    "tQKBgBnkbvU7NsRb44LI91OkyU49ui2U0qbccCgx4/6aUr4x3BUS+RT7bN9Wu+5m\n"
                    "cLr1ExHNnxdKnJJ8k1MfmehaSniq5/mJwYxfCD6bsaiqYmn10lf47RzEmC4T+a/0\n"
                    "zMUU+32d8FbczpSelSJP+X1syVWqIMfTmCzDBM88ChFwK6z9AkEA8Oz39TXz9SpA\n"
                    "aPmnMHGtfZpj7TKYgLmT2BnFIeJUZI0DmjsqdqM78izHZ59Ci2M30Lg6DMUnzkBf\n"
                    "46LinTzsbwJBAN2XuN+lNiVFifdsSrPnBocQYQgpt9N++l7IQICZ9RiG4+wTaoDR\n"
                    "R8nM6iTx+5MyFzS4brN+4YYXR1ElPDZjEUUCQQCFaWCpm5Ok0y0Ffl9hxXQPhyqe\n"
                    "jIaEWhQZOKxCimecp9UOiJQCx5uYFR06kISChpFeMCGkjW6JPyPTZKZxGYxXAkAa\n"
                    "HJ1k1owVn5Jd5FWnrk2jsonWwo4Myc5asX7GpSsAadba9m9XBsHvHLvQb6SqAdsd\n"
                    "Y0B84m8pt0IGJLBs65MNAkBpd8mZQiLc1VHDY/2NPHaC2Fy4sdI/y38DFGKnwD/l\n"
                    "LYnD7uFnhVVU+avNatvOpEkTInRVy/DWHxYh3gWv44hl\n"
                    "-----END RSA PRIVATE KEY-----"};
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
    cr.contactHandle = cr.contactHandle = engine.getIdentityHandle(cert, addr);

    qDebug() << "Trying to create contact from handle: " << cr.contactHandle;
    engine.createContact(cr);
    QCOMPARE(spy_rows_inserted.wait(3000), true);

    auto map = ds::core::DsEngine::fromJson(cr.contactHandle);
    auto pubkey = map["pubkey"].toByteArray();

    const auto dscert = ds::crypto::DsCert::createFromPubkey(pubkey);
    const auto hash = dscert->getHash();

    QCOMPARE(cmodel.rowCount() , 1);
    QCOMPARE(cmodel.data(cmodel.index(0, cmodel.fieldIndex("name"), {})).toString(), cr.name);
    QCOMPARE(cmodel.hashExists(hash), true);
    QCOMPARE(cmodel.hashExists(QByteArray("Not a hash")), false);
}
