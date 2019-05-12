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

QValidator::State IdentityNameValidator::validate(QString &name, int &) const
{
    if (!Contact::validateNick(name)) {
        return Intermediate;
    }

    if (DsEngine::instance().getIdentityManager()->exists(name)) {
        return Intermediate;
    }

    return Acceptable;
}

}}
