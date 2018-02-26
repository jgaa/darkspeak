#ifndef TST_TORCONTROLLER_H
#define TST_TORCONTROLLER_H

#include <QtTest>

// add necessary includes here

#include "ds/tormgr.h"

class TestTorController : public QObject
{
    Q_OBJECT

public:
    TestTorController() = default;
    ~TestTorController() = default;

private slots:
    void test_auth_cookie();
    void test_auth_safecookie();
    void test_auth_hashedpassword();
    void test_ready();
};


#endif // TST_TORCONTROLLER_H
