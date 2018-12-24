#ifndef TST_MESAGES_H
#define TST_MESAGES_H

#include <QtTest>

class TestMessagesModel : public QObject
{
    Q_OBJECT

public:
    TestMessagesModel();

private slots:
    void test_create_message();
};

#endif // TST_MESAGES_H
