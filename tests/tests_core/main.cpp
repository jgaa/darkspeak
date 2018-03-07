#include <QtTest>

#include "ds/crypto.h"
#include "tst_dsengine.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    qDebug() << "Pwd is " << app.applicationDirPath();


    // initialize openssl
    ds::crypto::Crypto crypto;

    int status = 0;

     {
         TestDsEngine tc;
         status |= QTest::qExec(&tc, argc, argv);
     }


    return status;
}

