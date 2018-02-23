#include <QtTest>

#include "tst_torctlsocket.h"
#include "tst_tormanager.h"
#include "tst_torcontroller.h"

// Note: This is equivalent to QTEST_APPLESS_MAIN for multiple test classes.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    int status = 0;

    {
        TestTorCtlSocket tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    {
        TestTorManager tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    {
        TestTorController tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    return status;
}

