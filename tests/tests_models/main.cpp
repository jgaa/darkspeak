#include <QtTest>

#include "ds/crypto.h"
#include "tst_identities.h"
#include "tst_contactsmodel.h"
#include "tst_messages.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    qDebug() << "Pwd is " << app.applicationDirPath();


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

