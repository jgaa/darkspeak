#ifndef IDENTITYNAMEVALIDATOR_H
#define IDENTITYNAMEVALIDATOR_H

#include <QValidator>

namespace ds {
namespace models {


class IdentityNameValidator : public QValidator
{
public:
    IdentityNameValidator();

    // QValidator interface
public:
    State validate(QString &, int &) const override;
};

}}

#endif // IDENTITYNAMEVALIDATOR_H
