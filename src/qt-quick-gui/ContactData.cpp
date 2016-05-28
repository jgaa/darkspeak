#include "log/WarLog.h"

#include "ContactsModel.h"
#include "ContactData.h"
#include "darkspeak-gui.h"

using namespace std;
using namespace darkspeak;
using namespace war;

ContactData::ContactData()
: QObject (nullptr)
{

}

ContactData::ContactData(const shared_ptr< Api::Buddy >& buddy,
                         ContactsModel *parent)
: QObject(parent), buddy_{buddy}, model_{parent}
{
    load();
}

void ContactData::load()
{
    auto d = buddy_->GetInfo();

    handle = d.id.c_str();
    profileName = d.profile_name.c_str();
    profileText = d.profile_text.c_str();
    ourNickname = d.our_nickname.c_str();
    ourGeneralNotes = d.our_general_notes.c_str();
    createdTime.setTime_t(d.created_time);
    firstContact.setTime_t(d.first_contact);
    lastSeen.setTime_t(d.last_seen);
    anonymity = static_cast<AnonymityLevel>(static_cast<int>(d.anonymity));
    requiredAnonymity = static_cast<AnonymityLevel>(static_cast<int>(d.required_anonymity));

    emit handleChanged();
    emit profileNameChanged();
    emit profileTextChanged();
    emit createdTimeChanged();
    emit firstContactChanged();
    emit ourNicknameChanged();
    emit ourGeneralNotesChanged();
    emit requiredAnonymityChanged();
    emit canSaveChanged();
}


void ContactData::save()
{
    auto d = buddy_->GetInfo();
    d.our_nickname = ourNickname.toStdString();
    d.our_general_notes = ourGeneralNotes.toStdString();
    d.anonymity = static_cast<Api::AnonymityLevel>(static_cast<int>(anonymity));

    buddy_->SetInfo(d);
    // TODO: Signal changes / save to file
    WAR_ASSERT(model_);
    emit model_->onBuddyStateMayHaveChanged(d.id);
}


QString ContactData::getHandle() const
{
    return handle;
}


QString ContactData::getProfileName() const
{
    return profileName;
}


QString ContactData::getProfileText() const
{
    return profileText;
}

QString ContactData::getOurNickname() const
{
    return ourNickname;
}

void ContactData::setOurNickname(const QString &value)
{
    if (ourNickname != value) {
        ourNickname = value;
        emit ourNicknameChanged();
    }
}

QString ContactData::getOurGeneralNotes() const
{
    return ourGeneralNotes;
}

void ContactData::setOurGeneralNotes(const QString &value)
{
    if (ourGeneralNotes != value) {
        ourGeneralNotes = value;
        emit ourGeneralNotesChanged();
    }
}

QDateTime ContactData::getCreatedTime() const
{
    return createdTime;
}

QDateTime ContactData::getFirstContact() const
{
    return firstContact;
}

QDateTime ContactData::getLastSeen() const
{
    return lastSeen;
}


ContactData::AnonymityLevel ContactData::getAnonymity() const
{
    return anonymity;
}

void ContactData::setAnonymity(const ContactData::AnonymityLevel &value)
{
    if (anonymity != value) {
        anonymity = value;
        emit anonymityChanged();
        emit canSaveChanged();
    }
}

ContactData::AnonymityLevel ContactData::getRequiredAnonymity() const
{
    return requiredAnonymity;
}

bool ContactData::getCanSave() const
{
    // TODO: Refactor
    auto info = buddy_->GetInfo();
    info.anonymity = static_cast<Api::AnonymityLevel>(static_cast<int>(anonymity));
    return info.CanBeSaved();

}

