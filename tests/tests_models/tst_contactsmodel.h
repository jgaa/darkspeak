#ifndef TST_CONTACTSMODEL_H
#define TST_CONTACTSMODEL_H

#include <QtTest>
#include "ds/contactsmodel.h"

class TestContactsModel : public QObject
{
    Q_OBJECT
public:
    TestContactsModel();

private slots:
    void test_create_contact();
};

#endif // TST_CONTACTSMODEL_H
