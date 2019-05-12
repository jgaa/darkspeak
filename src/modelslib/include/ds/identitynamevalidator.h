#ifndef IDENTITYNAMEVALIDATOR_H
#define IDENTITYNAMEVALIDATOR_H

#include <QValidator>

namespace ds {
namespace models {


class IdentityNameValidator : public QValidator
{
    Q_OBJECT
public:
    IdentityNameValidator();

    Q_PROPERTY(QString currentName READ getCurrentName WRITE setCurrentName NOTIFY currentNameChanged)

    void setCurrentName(QString name);
    QString getCurrentName() const noexcept;

signals:
    void currentNameChanged();

    // QValidator interface
public:
    State validate(QString &, int &) const override;

private:
    QString currentName_; // Allow this
};

}}


#endif // IDENTITYNAMEVALIDATOR_H
