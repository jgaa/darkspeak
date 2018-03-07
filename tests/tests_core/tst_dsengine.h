#ifndef TST_DSENGINE_H
#define TST_DSENGINE_H

#include <QtTest>

class TestDsEngine : public QObject
{
    Q_OBJECT

public:
    TestDsEngine();

private slots:
    void test_create_identity();
    void test_create_identity_when_still_offline();
};

#endif // TST_DSENGINE_H

