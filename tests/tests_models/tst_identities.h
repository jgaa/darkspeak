#ifndef TST_DSENGINE_H
#define TST_DSENGINE_H

#include <QtTest>

class TestIdentitiesModel : public QObject
{
    Q_OBJECT

public:
    TestIdentitiesModel();

private slots:
    void test_create_identity();
};

#endif // TST_DSENGINE_H

