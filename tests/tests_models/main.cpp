#include <QtTest>
#include <iostream>
#include "ds/crypto.h"
#include "tst_identities.h"
#include "tst_contactsmodel.h"
#include "tst_messages.h"
#include "logfault/logfault.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    logfault::LogManager::Instance().AddHandler(
                    std::make_unique<logfault::StreamHandler>(
                        std::clog, logfault::LogLevel::DEBUGGING));


    LFLOG_DEBUG << "Pwd is " << app.applicationDirPath();


    // initialize crypto
    ds::crypto::Crypto crypto;

    int status = 0;

    {
        TestIdentitiesModel tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    {
        TestContactsModel tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    {
        TestMessagesModel tc;
        status |= QTest::qExec(&tc, argc, argv);
    }

    return status;
}

