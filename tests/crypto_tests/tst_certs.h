#ifndef TST_CERTS_H
#define TST_CERTS_H

#include <QtTest>

class TestCerts : public QObject
{
    Q_OBJECT
public:
    TestCerts() = default;

private slots:
    void test_create_cert();
};

#endif // TST_CERTS_H
