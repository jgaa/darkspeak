#include <QtTest>

// add necessary includes here

#include "ds/tormgr.h"

class TestTorManager : public QObject
{
    Q_OBJECT

public:
    TestTorManager();
    ~TestTorManager();

private slots:
    void test_case1();

};

TestTorManager::TestTorManager()
{

}

TestTorManager::~TestTorManager()
{

}

void TestTorManager::test_case1()
{

}

//QTEST_APPLESS_MAIN(TestTorManager)

#include "tst_tormanager.moc"
