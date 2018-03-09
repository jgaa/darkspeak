
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
}

