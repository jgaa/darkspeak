#ifndef TST_DSENGINE_H
#define TST_DSENGINE_H

#include <QtTest>
#include "ds/contact.h"
#include "ds/dsengine.h"

class Verifier : public QObject
{
    Q_OBJECT
public:


public slots:
    void contactCreated(const ds::core::Contact&);

public:
    bool verified = false;
};

class TestDsEngine : public QObject
{
    Q_OBJECT

public:
    TestDsEngine();

private slots:
    void test_create_identity();
    void test_create_identity_when_still_offline();
    void test_get_identity_handle();
    void test_create_contact();
};

#endif // TST_DSENGINE_H

