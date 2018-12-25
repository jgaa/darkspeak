#include <QtTest>
#include <iostream>
#include "logfault/logfault.h"

#include "tst_torctlsocket.h"
#include "tst_tormanager.h"
#include "tst_torcontroller.h"

// Note: This is equivalent to QTEST_APPLESS_MAIN for multiple test classes.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    logfault::LogManager::Instance().AddHandler(
                std::make_unique<logfault::StreamHandler>(
                    std::clog, logfault::LogLevel::DEBUGGING));

    LFLOG_DEBUG << "Pwd is " << app.applicationDirPath();

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

