#include "include/ds/identitynamevalidator.h"

#include "ds/dsengine.h"
#include "ds/identitymanager.h"
#include "logfault/logfault.h"
#include "ds/contact.h"

using namespace std;
using namespace ds::core;


namespace ds {
namespace models {

IdentityNameValidator::IdentityNameValidator()
{

}

void IdentityNameValidator::setCurrentName(QString name)
{
    if (name.compare(currentName_) != 0) {
        emit currentNameChanged();
        currentName_ = name;
    }
}

QString IdentityNameValidator::getCurrentName() const noexcept
{
    return currentName_;
}


QValidator::State IdentityNameValidator::validate(QString &name, int &) const
{
    if (!Contact::validateNick(name)) {
        return Intermediate;
    }

    if (!currentName_.isEmpty()) {
        if (currentName_.compare(name, Qt::CaseInsensitive) == 0) {
            return Acceptable;
        }
    }

    if (DsEngine::instance().getIdentityManager()->exists(name)) {
        return Intermediate;
    }

    return Acceptable;
}

}}
