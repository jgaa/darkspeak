
#include <memory>
#include <QSettings>

#include "tst_identities.h"
#include "ds/dsengine.h"
#include "ds/identitiesmodel.h"

TestIdentitiesModel::TestIdentitiesModel()
{

}

void TestIdentitiesModel::test_create_identity()
{
    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));
    ds::models::IdentitiesModel idmodel(engine.settings());
    QSignalSpy spy_rows_inserted(&idmodel, &ds::models::IdentitiesModel::rowsInserted);

    // Start the engine, connect to Tor, get ready
    engine.start();

    // Create an identity.
    ds::core::IdentityReq req;
    req.name = "testid";
    engine.createIdentity(req);

    QCOMPARE(spy_rows_inserted.wait(3000), true);
    QCOMPARE(idmodel.rowCount() , 1);
    QCOMPARE(idmodel.data(idmodel.index(0, idmodel.fieldIndex("name"), {}), Qt::DisplayRole).toString(), req.name);
    QCOMPARE(idmodel.identityExists(req.name), true);
    QCOMPARE(idmodel.identityExists("nonexistant"), false);
}

