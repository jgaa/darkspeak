#ifndef TST_TORMANAGER_H
#define TST_TORMANAGER_H

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


#endif // TST_TORMANAGER_H
