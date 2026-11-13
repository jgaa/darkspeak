#include "service.h"
#include "ds/base32.h"
#include "ds/base58.h"
#include "ds/crypto.h"
#include "ds/dsengine.h"
#include "ds/eventjournal.h"
#include "ds/secretstore.h"
#include "ds/textpolicy.h"
#include "logfault/logfault.h"
#include "transport.h"
#ifdef DARKSPEAK_WITH_TEST_MCP
#include "test_support.h"
#endif
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QUuid>
#include <QtEndian>
#include <climits>
#include <cmath>
#include <sodium.h>

namespace ds::mcp {
using namespace ds::core;
namespace {
QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
qint64 now() {
    return QDateTime::currentSecsSinceEpoch();
}
QByteArray encode(const QStringList& list) {
    return QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact);
}
QStringList decode(const QVariant& value) {
    QStringList list;
    for (const auto& entry : QJsonDocument::fromJson(value.toByteArray()).array())
        list << entry.toString();
    return list;
}
QJsonArray array(const QStringList& list) {
    return QJsonArray::fromStringList(list);
}
QByteArray digest(const QByteArray& value) {
    QByteArray result(crypto_generichash_BYTES, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char*>(result.data()), result.size(),
                       reinterpret_cast<const unsigned char*>(value.constData()), value.size(), nullptr, 0);
    return result;
}
QByteArray randomBytes(int size) {
    QByteArray result(size, Qt::Uninitialized);
    randombytes_buf(result.data(), result.size());
    return result;
}
const Tool& definition(const QString& name) {
    for (const auto& tool : tools())
        if (tool.name == name)
            return tool;
    throw Error("UnknownTool");
}
Decision decision(const QString& value) {
    if (value == "Allow")
        return Decision::Allow;
    if (value == "Deny")
        return Decision::Deny;
    if (value == "Ask")
        return Decision::Ask;
    throw Error("InvalidDecision");
}
QString decisionName(int value) {
    return value == int(Decision::Allow) ? "Allow" : value == int(Decision::Deny) ? "Deny" : "Ask";
}
QString identityUuid(Identity* identity) {
    return identity->getUuid().toString(QUuid::WithoutBraces);
}
Contact::ptr_t contactFor(DsEngine& engine, const QString& identity, const QString& id) {
    if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
        throw Error("InvalidContact");
    auto* owner = engine.getIdentityManager()->identityFromUuid(QUuid(identity));
    if (!owner)
        throw Error("AccessDenied");
    QSqlQuery permitted(engine.getDb());
    permitted.prepare("SELECT id FROM contact WHERE uuid=? AND identity=?");
    permitted.addBindValue(QUuid(id));
    permitted.addBindValue(owner->getId());
    if (!permitted.exec() || !permitted.next())
        throw Error("AccessDenied");
    auto contact = engine.getContactManager()->getContact(QUuid(id));
    if (!contact || !contact->getIdentity() || identityUuid(contact->getIdentity()) != identity)
        throw Error("AccessDenied");
    return contact;
}
void bounded(const QString& value, int maximum, bool nonempty = true) {
    if ((nonempty && value.isEmpty()) || value.size() > maximum || value.toUtf8().size() > maximum)
        throw Error("InvalidArguments");
}
QString approvalArguments(const QJsonObject& args) {
    const auto json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented));
    QString result;
    result.reserve(json.size());
    for (const auto ch : json) {
        if (ch.category() == QChar::Other_Format || ch.unicode() == 0x2028 || ch.unicode() == 0x2029)
            result += QStringLiteral("\\u%1").arg(uint(ch.unicode()), 4, 16, QLatin1Char('0'));
        else
            result += ch;
    }
    return result;
}
int pageSize(const QJsonObject& args) {
    return args.value("limit").toInt(32);
}
} // namespace
const QList<Tool>& tools() {
    static const QList<Tool> registry{
        {"status", "read_status", {}, {"identity"}, {"identity"}},
        {"identities", "read_identities", {}, {}, {}},
        {"identity_info", "read_identities", {}, {"identity"}, {"identity"}},
        {"contacts", "read_contacts", {}, {"identity", "cursor", "limit"}, {"identity"}},
        {"contact", "read_contacts", {}, {"identity", "contact"}, {"identity", "contact"}},
        {"contact_requests", "read_contacts", {}, {"identity", "cursor", "limit"}, {"identity"}},
        {"contact_add",
         "create_contacts",
         {},
         {"identity", "sharing_id", "name", "message", "idempotency_key"},
         {"identity", "sharing_id", "name", "idempotency_key"},
         true,
         true},
        {"contact_approve",
         "create_contacts",
         {},
         {"identity", "request", "idempotency_key"},
         {"identity", "request", "idempotency_key"},
         true,
         true},
        {"contact_reject",
         "reject_contacts",
         {},
         {"identity", "request", "idempotency_key"},
         {"identity", "request", "idempotency_key"},
         true,
         true},
        {"contact_remove",
         "remove_contacts",
         {},
         {"identity", "contact", "idempotency_key"},
         {"identity", "contact", "idempotency_key"},
         true,
         true},
        {"profile",
         "read_profiles",
         "read_profiles",
         {"identity", "contact", "idempotency_key"},
         {"identity", "contact", "idempotency_key"},
         true},
        {"presence", "read_presence", {}, {"identity", "contact"}, {"identity", "contact"}},
        {"conversations", "read_conversations", {}, {"identity", "cursor", "limit"}, {"identity"}},
        {"messages",
         "read_message_content",
         "read_message_content",
         {"identity", "conversation", "cursor", "limit", "idempotency_key"},
         {"identity", "conversation", "idempotency_key"},
         true},
        {"message_send",
         "send_messages",
         "send_messages",
         {"identity", "contacts", "text", "idempotency_key"},
         {"identity", "contacts", "text", "idempotency_key"},
         true,
         true},
        {"events", "read_events", {}, {"identity", "cursor", "limit"}, {"identity"}},
        {"diagnostics", "read_diagnostics", {}, {"identity"}, {"identity"}},
        {"operation_status", {}, {}, {"operation"}, {"operation"}}
#ifdef DARKSPEAK_WITH_TEST_MCP
        ,
        {"bio_probe",
         "read_profiles",
         "read_profiles",
         {"identity", "sharing_id", "idempotency_key"},
         {"identity", "sharing_id", "idempotency_key"},
         true,
         true},
        {"bio_preview",
         "read_profiles",
         "read_profiles",
         {"identity", "sharing_id", "idempotency_key"},
         {"identity", "sharing_id", "idempotency_key"},
         true},
        {"file_offer",
         "send_files",
         "send_files",
         {"identity", "contact", "asset", "idempotency_key"},
         {"identity", "contact", "asset", "idempotency_key"},
         true,
         true},
        {"file_accept",
         "read_files",
         "read_files",
         {"identity", "contact", "transfer", "idempotency_key"},
         {"identity", "contact", "transfer", "idempotency_key"},
         true,
         true},
        {"files",
         "read_files",
         "read_files",
         {"identity", "contact", "idempotency_key"},
         {"identity", "contact", "idempotency_key"},
         true}
#endif
    };
    return registry;
}
void Reply::finish(QJsonObject result, QString code) {
    std::lock_guard lock(mutex);
    if (ready)
        return;
    value = std::move(result);
    error = std::move(code);
    ready = true;
}
Service::Service(DsEngine& engine, QObject* parent)
    : QObject(parent), engine_(engine), store_(engine.getDb()), run_id_(uuid()) {
    if (sodium_init() < 0)
        throw Error("CryptographyUnavailable");
    fingerprint_key_ = engine_.secretStore().mcpFingerprintKey();
    if (fingerprint_key_.size() != crypto_generichash_KEYBYTES)
        throw Error("InvalidFingerprintSecret");
    clock_.start();
    refreshCredentials();
    connect(&timer_, &QTimer::timeout, this, &Service::tick);
    timer_.start(250);
    auto& journal = engine_.events();
    connect(&journal, &EventJournal::operationalEvent, this,
            [this](int kind, qint64 identity, qint64 contact, int outcome) {
                auto* owner = engine_.getIdentityManager()->identityFromId(int(identity));
                if (!owner)
                    return;
                QStringList targets;
                if (contact) {
                    auto target = engine_.getContactManager()->getContact(int(contact));
                    if (!target)
                        return;
                    targets << target->getUuid().toString(QUuid::WithoutBraces);
                }
                event(QStringLiteral("application.%1.%2").arg(kind).arg(outcome), {}, identityUuid(owner), {},
                      {}, targets);
            });
    connect(&engine_, &DsEngine::onlineStateChanged, this,
            [this](ProtocolManager::State, ProtocolManager::State) {
                for (const auto& entry : identities())
                    event("application.transport", {}, entry.toMap().value("id").toString(), {}, {});
            });
    const auto watch_contact = [this](const Contact::ptr_t& contact) {
        if (!transport_ || watched_contacts_.contains(contact.get()) || watched_contacts_.size() >= 4096)
            return;
        watched_contacts_.insert(contact.get());
        connect(contact.get(), &QObject::destroyed, this,
                [this, address = contact.get()] { watched_contacts_.remove(address); });
        connect(contact.get(), &Contact::onlineStatusChanged, this,
                [this, weak = std::weak_ptr<Contact>(contact)] {
                    if (!transport_)
                        return;
                    if (auto target = weak.lock())
                        if (auto* owner = target->getIdentity())
                            event("application.presence", {}, identityUuid(owner), {}, {},
                                  {target->getUuid().toString(QUuid::WithoutBraces)});
                });
    };
    connect(engine_.getContactManager(), &ContactManager::contactAdded, this, watch_contact);
    connect(engine_.getContactManager(), &ContactManager::contactDeleted, this,
            [this](const QUuid&) { resetEventAuthorization(); });
    // Watch lazily materialized existing contacts too, without preloading an
    // unbounded contact collection at endpoint startup.
    connect(engine_.getContactManager(), &ContactManager::contactTouched, this, watch_contact);
    connect(&engine_, &DsEngine::closing, this, &Service::stop);
}
Service::~Service() {
    try {
        stop();
    } catch (...) {
        transport_.reset();
    }
    sodium_memzero(fingerprint_key_.data(), fingerprint_key_.size());
}
bool Service::enabled() const {
    return bool(transport_);
}
bool Service::active() const {
    return !pending_.isEmpty() || dispatches_.load() > 0 || clock_.elapsed() - last_activity_ < 3000;
}
bool Service::fail(const QString& code) {
    error_ = code;
    emit changed();
    return false;
}
void Service::setUiAvailable(bool available) {
    ui_available_ = available;
    if (!available)
        invalidate([](const Pending& p) { return p.deadline > 0; });
}
bool Service::configure(bool enable, const QString& address, int port, int timeout) {
    try {
        const QHostAddress host(address);
        if (host.isNull() || !host.isLoopback() || address.contains('%') || port < 1 || port > 65535 ||
            timeout < 1 || timeout > 300)
            throw Error("InvalidEndpoint");
        if (!enable) {
            stop();
            stopped_ = false;
            timeout_ = timeout;
            error_.clear();
            emit changed();
            return true;
        }
        const auto literal = host.toString();
        if (!transport_ || literal != address_ || port != port_) {
            auto replacement = std::make_unique<Transport>(*this, literal, port);
            replacement
                ->start(); // patched adapter binds synchronously; failure leaves the current listener intact.
            transport_.swap(replacement);
            address_ = literal;
            port_ = port;
            LFLOG_INFO << "MCP listening on loopback port " << port;
        }
        timeout_ = timeout;
        stopped_ = false;
        engine_.events().setOperationalCollectionEnabled(true);
        error_.clear();
        emit changed();
        return true;
    } catch (...) {
        return fail("McpConfigurationFailed");
    }
}
void Service::stop() {
    engine_.events().setOperationalCollectionEnabled(false);
    stopped_ = true;
    try {
        invalidate([](const Pending&) { return true; });
    } catch (...) {
        for (const auto& p : pending_)
            if (auto reply = p.reply.lock())
                reply->finish({}, "PersistenceFailed");
        pending_.clear();
        error_ = "PersistenceFailed";
        // Recovery cancels durable pending records if shutdown could not update them.
    }
    transport_.reset(); // workers never wait synchronously for the Qt thread.
    emit changed();
}
QByteArray Service::mac(const QByteArray& data) const {
    QByteArray out(crypto_generichash_BYTES, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char*>(out.data()), out.size(),
                       reinterpret_cast<const unsigned char*>(data.constData()), data.size(),
                       reinterpret_cast<const unsigned char*>(fingerprint_key_.constData()),
                       fingerprint_key_.size());
    return out;
}
QString Service::seal(const QString& key, const QString& identity, const QString& domain,
                      quint64 value) const {
    const auto binding = mac("handle-v1:" + key.toUtf8() + ':' + identity.toUtf8() + ':' + domain.toUtf8());
    QByteArray plain(8, Qt::Uninitialized);
    qToBigEndian(value, plain.data());
    const auto nonce = randomBytes(crypto_secretbox_NONCEBYTES);
    QByteArray sealed(plain.size() + crypto_secretbox_MACBYTES, Qt::Uninitialized);
    crypto_secretbox_easy(reinterpret_cast<unsigned char*>(sealed.data()),
                          reinterpret_cast<const unsigned char*>(plain.constData()), plain.size(),
                          reinterpret_cast<const unsigned char*>(nonce.constData()),
                          reinterpret_cast<const unsigned char*>(binding.constData()));
    return QString::fromLatin1(
        (nonce + sealed).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
quint64 Service::open(const QString& token, const QString& key, const QString& identity,
                      const QString& domain) const {
    if (token.size() != 64)
        throw Error("InvalidCursor");
    const auto raw = QByteArray::fromBase64(token.toLatin1(), QByteArray::Base64UrlEncoding |
                                                                  QByteArray::AbortOnBase64DecodingErrors);
    if (raw.size() != crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + 8)
        throw Error("InvalidCursor");
    const auto binding = mac("handle-v1:" + key.toUtf8() + ':' + identity.toUtf8() + ':' + domain.toUtf8());
    QByteArray plain(8, Qt::Uninitialized);
    if (crypto_secretbox_open_easy(
            reinterpret_cast<unsigned char*>(plain.data()),
            reinterpret_cast<const unsigned char*>(raw.constData() + crypto_secretbox_NONCEBYTES),
            raw.size() - crypto_secretbox_NONCEBYTES, reinterpret_cast<const unsigned char*>(raw.constData()),
            reinterpret_cast<const unsigned char*>(binding.constData())) != 0)
        throw Error("InvalidCursor");
    return qFromBigEndian<quint64>(plain.constData());
}
void Service::refreshCredentials() {
    {
        std::lock_guard lock(credentials_mutex_);
        credentials_.clear();
    }
    QList<Credential> credentials;
    QList<EventView> views;
    auto q = store_.query("SELECT id,verifier,expires,identities,capabilities FROM mcp_keys WHERE enabled=1");
    while (q.next()) {
        credentials << Credential{q.value(0).toString(), q.value(1).toByteArray(), q.value(2).toLongLong()};
        views << EventView{q.value(0).toString(), decode(q.value(3)), q.value(2).toLongLong(),
                           decode(q.value(4)).contains("read_events")};
    }
    event_views_ = std::move(views);
    resetEventAuthorization();
    std::lock_guard lock(credentials_mutex_);
    credentials_ = std::move(credentials);
}
QString Service::authenticate(const std::string& bearer) {
    constexpr std::string_view prefix = "Bearer ";
    if (!bearer.starts_with(prefix) || bearer.size() != prefix.size() + 64)
        return {};
    const auto candidate = digest(QByteArray(bearer.data() + prefix.size(), 64));
    QString result;
    std::lock_guard lock(credentials_mutex_);
    for (const auto& item : credentials_) {
        const bool matches =
            item.verifier.size() == candidate.size() &&
            sodium_memcmp(item.verifier.constData(), candidate.constData(), candidate.size()) == 0;
        if (matches && (!item.expires || item.expires > now()))
            result = item.id;
    }
    if (!result.isEmpty())
        used_keys_.insert(result);
    return result;
}
void Service::requestActivity() {
    if (!activity_notification_.exchange(true))
        QMetaObject::invokeMethod(
            this,
            [this] {
                activity_notification_.store(false);
                last_activity_ = clock_.elapsed();
                emit changed();
            },
            Qt::QueuedConnection);
}
bool Service::reserveDispatch() {
    auto count = dispatches_.load();
    while (count < 32)
        if (dispatches_.compare_exchange_weak(count, count + 1))
            return true;
    return false;
}
QStringList Service::capabilities() const {
    QStringList result{"read_only", "create_identities", "read_files", "send_files"};
    for (const auto& tool : tools())
        if (!tool.capability.isEmpty() && !result.contains(tool.capability))
            result << tool.capability;
    result.sort();
    return result;
}
QVariantList Service::identities() const {
    QVariantList result;
    auto* manager = engine_.getIdentityManager();
    for (int row = 0; row < manager->rowCount({}); ++row)
        if (auto* identity = manager->identityFromRow(row))
            result << QVariantMap{{"id", identityUuid(identity)}, {"name", identity->getName()}};
    return result;
}
QVariantList Service::keys() const {
    try {
        QVariantList result;
        auto q = store_.query("SELECT id,name,identities,capabilities,created,expires,last_used,enabled FROM "
                              "mcp_keys ORDER BY created");
        while (q.next())
            result << QVariantMap{{"id", q.value(0)},
                                  {"name", q.value(1)},
                                  {"identities", decode(q.value(2))},
                                  {"capabilities", decode(q.value(3))},
                                  {"created", q.value(4)},
                                  {"expires", q.value(5)},
                                  {"lastUsed", q.value(6)},
                                  {"enabled", q.value(7).toBool()}};
        return result;
    } catch (...) {
        return {};
    }
}
QVariantMap Service::createKey(const QString& name, const QStringList& identities,
                               const QStringList& capabilities, qint64 expires) {
    try {
        auto count = store_.query("SELECT count(*) FROM mcp_keys");
        count.next();
        if (count.value(0).toInt() >= 64)
            throw Error("KeyLimit");
        const auto id = uuid();
        auto secret = randomBytes(32).toHex();
        store_.query("INSERT INTO mcp_keys(id,name,verifier,identities,capabilities,created,expires,enabled) "
                     "VALUES(?,?,?,?,?,?,?,0)",
                     {id, QString{}, digest(secret), encode({}), encode({}), now(), 0});
        if (!updateKey(id, name, identities, capabilities, expires, true)) {
            store_.query("DELETE FROM mcp_keys WHERE id=?", {id});
            sodium_memzero(secret.data(), secret.size());
            return {};
        }
        QVariantMap result{{"id", id}, {"secret", QString::fromLatin1(secret)}};
        sodium_memzero(secret.data(), secret.size());
        return result;
    } catch (...) {
        fail("KeyCreationFailed");
        return {};
    }
}
bool Service::updateKey(const QString& id, const QString& name, const QStringList& identities,
                        const QStringList& granted, qint64 expires, bool enable) {
    try {
        bounded(name, 128);
        if (identities.isEmpty() || identities.size() > 64 || granted.size() > 32 || expires < 0 ||
            expires > 253402300799LL || (expires && expires <= now()))
            throw Error("InvalidKey");
        for (const auto& item : identities)
            if (QUuid(item).isNull() || QUuid(item).toString(QUuid::WithoutBraces) != item ||
                !engine_.getIdentityManager()->identityFromUuid(QUuid(item)))
                throw Error("InvalidIdentity");
        for (const auto& item : granted)
            if (!capabilities().contains(item))
                throw Error("InvalidCapability");
        store_.query("UPDATE mcp_keys SET name=?,identities=?,capabilities=?,expires=?,enabled=? WHERE id=?",
                     {name, encode(identities), encode(granted), expires, enable, id});
        refreshCredentials();
        invalidate([&id](const Pending& p) { return p.key == id; });
        error_.clear();
        emit changed();
        return true;
    } catch (...) {
        return fail("KeyUpdateFailed");
    }
}
bool Service::revokeKey(const QString& id) {
    try {
        store_.query("UPDATE mcp_keys SET enabled=0 WHERE id=?", {id});
        refreshCredentials();
        invalidate([&id](const Pending& p) { return p.key == id; });
        emit changed();
        return true;
    } catch (...) {
        return fail("KeyRevocationFailed");
    }
}
QVariantList Service::rules() const {
    try {
        QVariantList result;
        auto q = store_.query("SELECT id,key_id,identity,tool,contact,decision FROM mcp_rules");
        while (q.next())
            result << QVariantMap{{"id", q.value(0)},       {"key", q.value(1)},
                                  {"identity", q.value(2)}, {"tool", q.value(3)},
                                  {"contact", q.value(4)},  {"decision", decisionName(q.value(5).toInt())}};
        return result;
    } catch (...) {
        return {};
    }
}
bool Service::removeRule(const QString& id) {
    try {
        auto q = store_.query("SELECT key_id,identity,tool FROM mcp_rules WHERE id=?", {id});
        if (!q.next())
            throw Error("UnknownRule");
        const auto key = q.value(0).toString(), identity = q.value(1).toString(),
                   tool = q.value(2).toString();
        store_.query("DELETE FROM mcp_rules WHERE id=?", {id});
        invalidate(
            [&](const Pending& p) { return p.key == key && p.identity == identity && p.tool == tool; });
        emit changed();
        return true;
    } catch (...) {
        return fail("PolicyUpdateFailed");
    }
}
bool Service::setRule(const QString& key, const QString& identity, const QString& tool,
                      const QString& contact, const QString& value) {
    try {
        if (!definition(tool).gated)
            throw Error("InvalidPolicyTool");
        const auto choice = decision(value);
        if (QUuid(identity).toString(QUuid::WithoutBraces) != identity ||
            !engine_.getIdentityManager()->identityFromUuid(QUuid(identity)))
            throw Error("InvalidIdentity");
        auto owner = store_.query("SELECT id FROM mcp_keys WHERE id=?", {key});
        if (!owner.next())
            throw Error("InvalidKey");
        if (!contact.isEmpty())
            contactFor(engine_, identity, contact);
        auto count = store_.query("SELECT count(*) FROM mcp_rules");
        count.next();
        if (count.value(0).toInt() >= 4096) {
            auto existing = store_.query(
                "SELECT id FROM mcp_rules WHERE key_id=? AND identity=? AND tool=? AND contact=?",
                {key, identity, tool, contact});
            if (!existing.next())
                throw Error("PolicyLimit");
        }
        store_.query("INSERT INTO mcp_rules VALUES(?,?,?,?,?,?) ON CONFLICT(key_id,identity,tool,contact) DO "
                     "UPDATE SET decision=excluded.decision",
                     {uuid(), key, identity, tool, contact, int(choice)});
        invalidate(
            [&](const Pending& p) { return p.key == key && p.identity == identity && p.tool == tool; });
        emit changed();
        return true;
    } catch (...) {
        return fail("PolicyUpdateFailed");
    }
}
QVariantList Service::contactPolicies() const {
    try {
        QVariantList result;
        auto q = store_.query("SELECT identity,contact,permission,decision FROM mcp_contacts");
        while (q.next())
            result << QVariantMap{{"identity", q.value(0)},
                                  {"contact", q.value(1)},
                                  {"permission", q.value(2)},
                                  {"decision", decisionName(q.value(3).toInt())}};
        return result;
    } catch (...) {
        return {};
    }
}
bool Service::setContactPolicy(const QString& identity, const QString& contact, const QString& permission,
                               const QString& value) {
    try {
        if (!QStringList{"send_messages", "read_message_content", "read_profiles", "send_files", "read_files"}
                 .contains(permission))
            throw Error("InvalidPermission");
        if (QUuid(identity).toString(QUuid::WithoutBraces) != identity ||
            !engine_.getIdentityManager()->identityFromUuid(QUuid(identity)))
            throw Error("InvalidIdentity");
        if (!contact.isEmpty())
            contactFor(engine_, identity, contact);
        if (value == "Inherit" && contact.isEmpty())
            throw Error("InvalidDecision");
        if (value == "Inherit")
            store_.query("DELETE FROM mcp_contacts WHERE identity=? AND contact=? AND permission=?",
                         {identity, contact, permission});
        else {
            auto count = store_.query("SELECT count(*) FROM mcp_contacts");
            count.next();
            if (count.value(0).toInt() >= 4096) {
                auto existing = store_.query(
                    "SELECT decision FROM mcp_contacts WHERE identity=? AND contact=? AND permission=?",
                    {identity, contact, permission});
                if (!existing.next())
                    throw Error("PolicyLimit");
            }
            store_.query("INSERT INTO mcp_contacts VALUES(?,?,?,?) ON CONFLICT(identity,contact,permission) "
                         "DO UPDATE SET decision=excluded.decision",
                         {identity, contact, permission, int(decision(value))});
        }
        invalidate([&](const Pending& p) {
            return p.identity == identity && definition(p.tool).permission == permission &&
                   (contact.isEmpty() || p.targets.contains(contact));
        });
        resetEventAuthorization();
        emit changed();
        return true;
    } catch (...) {
        return fail("PolicyUpdateFailed");
    }
}
QStringList Service::contactsForIdentity(const QString& identity) const {
    QStringList result;
    auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
    if (!owner)
        return result;
    auto q =
        store_.query("SELECT uuid FROM contact WHERE identity=? ORDER BY id LIMIT 4096", {owner->getId()});
    while (q.next())
        result << q.value(0).toUuid().toString(QUuid::WithoutBraces);
    return result;
}
void Service::authorize(const QString& key, const Tool& tool, const QString& identity,
                        const QStringList& targets) const {
    auto q = store_.query("SELECT identities,capabilities,expires,enabled FROM mcp_keys WHERE id=?", {key});
    if (!q.next() || !q.value(3).toBool() || (q.value(2).toLongLong() && q.value(2).toLongLong() <= now()))
        throw Error("AccessDenied");
    const auto granted = decode(q.value(1));
    if ((!tool.capability.isEmpty() && !granted.contains(tool.capability)) ||
        (tool.mutates && granted.contains("read_only")))
        throw Error("AccessDenied");
    if (!identity.isEmpty() && !decode(q.value(0)).contains(identity))
        throw Error("AccessDenied");
    if (!identity.isEmpty() && !engine_.getIdentityManager()->identityFromUuid(QUuid(identity)))
        throw Error("AccessDenied");
    for (const auto& target : targets)
        contactFor(engine_, identity, target);
}
Evaluation Service::approval(const QString& key, const Tool& tool, const QString& identity,
                             const QStringList& targets) const {
    QList<Rule> rules;
    auto q = store_.query("SELECT contact,decision FROM mcp_rules WHERE key_id=? "
                          "AND identity=? AND tool=?",
                          {key, identity, tool.name});
    while (q.next())
        rules << Rule{key, identity, tool.name, q.value(0).toString(), Decision(q.value(1).toInt())};
    QList<Decision> policies;
    const auto policy_targets =
        targets.isEmpty() && !tool.permission.isEmpty() ? QStringList{QString{}} : targets;
    for (const auto& target : policy_targets) {
        auto p = store_.query("SELECT decision FROM mcp_contacts WHERE identity=? AND "
                              "permission=? AND "
                              "contact IN ('',?) ORDER BY length(contact) DESC LIMIT 1",
                              {identity, tool.permission, target});
        policies << (p.next() ? Decision(p.value(0).toInt()) : Decision::Allow);
    }
    return evaluate(rules, key, identity, tool.name, targets, policies, tool.gated);
}
QStringList Service::validate(const Tool& tool, const QString& identity, QJsonObject& args, bool resolve,
                              const QString& key) const {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!tool.fields.contains(it.key()))
            throw Error("InvalidArguments");
    for (const auto& required : tool.required)
        if (!args.contains(required))
            throw Error("InvalidArguments");
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() == "contacts") {
            if (!it->isArray() || it->toArray().isEmpty() || it->toArray().size() > 8)
                throw Error("InvalidArguments");
            continue;
        }
        if (it.key() == "limit" || it.key() == "after") {
            const auto number = it->toDouble(-1);
            if (!it->isDouble() || !std::isfinite(number) || std::floor(number) != number || number < 0 ||
                number > INT_MAX)
                throw Error("InvalidArguments");
            if (it.key() == "limit" && (number < 1 || number > 64))
                throw Error("InvalidArguments");
        } else if (!it->isString())
            throw Error("InvalidArguments");
    }
    if (!identity.isEmpty() &&
        (QUuid(identity).isNull() || QUuid(identity).toString(QUuid::WithoutBraces) != identity))
        throw Error("InvalidIdentity");
    for (const auto& name : {"sharing_id", "name", "message", "text", "idempotency_key", "cursor",
                             "operation", "contact", "request", "conversation"}) {
        if (args.contains(name))
            bounded(args.value(name).toString(),
                    QString(name) == "text"                                     ? 65536
                    : (QString(name) == "cursor" || QString(name) == "message") ? 4096
                                                                                : 1024,
                    QString(name) != "message" && QString(name) != "cursor");
    }
    if (args.contains("name"))
        bounded(args.value("name").toString(), 128);
    if (args.contains("message"))
        bounded(args.value("message").toString(), 4096, false);
    if (tool.gated) {
        const auto retry = args.value("idempotency_key").toString();
        bounded(retry, 128);
        for (const auto ch : retry)
            if (ch.unicode() < 33 || ch.unicode() > 126)
                throw Error("InvalidIdempotencyKey");
    }
    if (tool.fields.contains("limit") && !args.contains("limit"))
        args["limit"] = 32;
    if (tool.fields.contains("cursor") && !args.contains("cursor"))
        args["cursor"] = QString{};
    if (tool.fields.contains("message") && !args.contains("message"))
        args["message"] = QString{};
    QByteArray sharing_key;
    bool sharing_operation = tool.name == "contact_add";
#ifdef DARKSPEAK_WITH_TEST_MCP
    sharing_operation = sharing_operation || tool.name == "bio_probe" || tool.name == "bio_preview";
#endif
    if (sharing_operation) {
        auto sharing = args.value("sharing_id").toString().trimmed();
        const auto colon = sharing.indexOf(':');
        if (colon > 0)
            sharing = sharing.mid(colon + 1);
        QByteArray payload;
        try {
            payload = ds::crypto::b58tobin_check<QByteArray>(sharing.toStdString(), 70, {11, 176});
        } catch (...) {
            throw Error("InvalidSharingId");
        }
        if (payload.size() != 70 || payload[0] != 2 ||
            qFromBigEndian<quint16>(payload.constData() + 68) == 0 ||
            !ds::crypto::onionv3isValid(ds::crypto::onionv3encode(payload.mid(33, 35))))
            throw Error("InvalidSharingId");
        sharing_key = payload.mid(1, 32);
        args["sharing_id"] = sharing;
    }
    QStringList targets;
    if (args.contains("contact"))
        targets << args.value("contact").toString();
    if (args.contains("contacts")) {
        for (const auto& item : args.value("contacts").toArray()) {
            if (!item.isString())
                throw Error("InvalidArguments");
            const auto id = item.toString();
            if (targets.contains(id))
                throw Error("DuplicateTarget");
            targets << id;
        }
        targets.sort();
        args["contacts"] = array(targets);
    }
    if (!resolve)
        return targets;
    if (!sharing_key.isEmpty()) {
        auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
        if (!owner || owner->getCert()->getSigningPubKey().toByteArray() == sharing_key)
            throw Error("InvalidContact");
        const auto cert = ds::crypto::DsCert::createFromPubkey(sharing_key);
        auto existing = store_.query("SELECT uuid FROM contact WHERE identity=? AND hash=?",
                                     {owner->getId(), cert->getHash().toByteArray()});
        if (existing.next()) {
            if (tool.name == "contact_add")
                throw Error("ContactExists");
            targets << existing.value(0).toUuid().toString(QUuid::WithoutBraces);
        }
    }
    if (!args.value("cursor").toString().isEmpty()) {
        const auto domain = tool.name == "events" ? "events:" + run_id_ + ":" + event_authorization_
                                                  : tool.name + ":" + args.value("conversation").toString();
        open(args.value("cursor").toString(), key, identity, domain);
    }
    if (args.contains("conversation")) {
        auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
        if (!owner)
            throw Error("AccessDenied");
        if (QUuid(args.value("conversation").toString()).isNull())
            throw Error("InvalidConversation");
        auto q = store_.query("SELECT participants FROM conversation WHERE uuid=? AND identity=?",
                              {QUuid(args.value("conversation").toString()), owner->getId()});
        if (!q.next())
            throw Error("AccessDenied");
        targets << q.value(0).toUuid().toString(QUuid::WithoutBraces);
    }
    for (const auto& target : targets)
        contactFor(engine_, identity, target);
#ifdef DARKSPEAK_WITH_TEST_MCP
    if (TestSupport::isOperation(tool.name)) {
        if (!test_support_)
            throw Error("TestUnavailable");
        test_support_->validate(tool.name, args);
    }
#endif
    if (args.contains("request")) {
        auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
        if (!owner)
            throw Error("AccessDenied");
        auto q = store_.query(
            "SELECT id,substr(data,1,32769) FROM notification WHERE "
            "id=? AND identity=? AND type=0",
            {QVariant::fromValue(open(args.value("request").toString(), key, identity, "request")),
             owner->getId()});
        if (!q.next())
            throw Error("AccessDenied");
        if (q.value(1).toByteArray().size() > 32768)
            throw Error("ResourceLimit");
        const auto details = DsEngine::fromJson(q.value(1).toByteArray());
        if (QJsonDocument::fromVariant(details).toJson(QJsonDocument::Compact).size() > 32 * 1024)
            throw Error("ResourceLimit");
        args["_request_details"] = QJsonObject::fromVariantMap(details);
    }
    return targets;
}
QString Service::retryEpoch() {
    auto q = store_.query("SELECT token,created FROM mcp_retry_epoch WHERE singleton=1");
    const auto current = now();
    if (q.next() && current >= q.value(1).toLongLong() && current - q.value(1).toLongLong() < 86400)
        return q.value(0).toString();
    const auto token = uuid();
    store_.query("INSERT INTO mcp_retry_epoch VALUES(1,?,?) ON CONFLICT(singleton) DO UPDATE SET "
                 "token=excluded.token,created=excluded.created",
                 {token, current});
    return token;
}
void Service::prune() {
    store_.query("DELETE FROM mcp_operations WHERE created<? AND state NOT IN "
                 "('Pending','Approved','Executing','IndeterminateOperation')",
                 {now() - 86400});
}
QJsonObject Service::status(const QString& key, const QString& id) const {
    auto q = store_.query(
        "SELECT identity,tool,targets,state,reference FROM mcp_operations WHERE id=? AND key_id=?",
        {id, key});
    if (!q.next())
        throw Error("OperationUnavailable");
    const auto identity = q.value(0).toString();
    const auto& tool = definition(q.value(1).toString());
    const auto targets = decode(q.value(2));
    // Removing a contact must not hide the caller's own completed removal, but
    // capability/identity and current policy still apply to the retained target.
    authorize(key, tool, identity, {});
    if (tool.name != "contact_remove" || q.value(3).toString() != "Completed")
        for (const auto& target : targets)
            contactFor(engine_, identity, target);
    if (approval(key, tool, identity, targets).denied)
        throw Error("AccessDenied");
    return {{"operation", id}, {"state", q.value(3).toString()}, {"reference", q.value(4).toString()}};
}
void Service::submit(QString key, QString tool_name, QJsonObject args, std::shared_ptr<Reply> reply) {
    --dispatches_;
    last_activity_ = clock_.elapsed();
    try {
        if (stopped_ || !transport_)
            throw Error("McpDisabled");
        const auto& tool = definition(tool_name);
        if (clock_.elapsed() - rate_window_ >= 60000) {
            calls_.clear();
            rate_window_ = clock_.elapsed();
        }
        // Status remains available under ordinary tool load; HTTP and parser
        // limits still apply to it.
        if (tool_name != "operation_status" && ++calls_[key] > 120)
            throw Error("RateLimited");
        const auto identity = args.value("identity").toString();
        authorize(key, tool, identity, {});
        validate(tool, identity, args, false, key);
        QStringList targets;
        store_.query("UPDATE mcp_keys SET last_used=? WHERE id=?", {now(), key});
        event("mcp.request.received", key, identity, tool.name, {}, targets);
        if (tool_name == "operation_status") {
            reply->finish(status(key, args.value("operation").toString()));
            emit changed();
            return;
        }
        if (!tool.gated) {
            targets = validate(tool, identity, args, true, key);
            authorize(key, tool, identity, targets);
            if (approval(key, tool, identity, targets).denied)
                throw Error("AccessDenied");
            if (tool_name == "identities") {
                QJsonArray result;
                auto q = store_.query("SELECT identities FROM mcp_keys WHERE id=?", {key});
                q.next();
                for (const auto& id : decode(q.value(0)))
                    if (auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(id)))
                        result.append(QJsonObject{{"id", id}, {"name", owner->getName()}});
                reply->finish({{"items", result}});
            } else if (tool_name == "events")
                reply->finish(events(key, identity, args));
            else
                reply->finish(execute(tool, identity, args, {}, key, clock_.elapsed() + 120000));
            emit changed();
            return;
        }
        const auto retry = args.value("idempotency_key").toString();
        // A server-issued random epoch is never reused, including after wall
        // clock rollback. Only the current epoch admits new work. Retained
        // records from previous epochs still support matching retries.
        const auto split = retry.indexOf(':');
        const auto epoch = retry.left(split);
        if (split != 36 || QUuid(epoch).isNull() || QUuid(epoch).toString(QUuid::WithoutBraces) != epoch ||
            split + 1 >= retry.size())
            throw Error("InvalidIdempotencyKey");
        const auto retry_hash = mac("retry-v1:" + retry.toUtf8());
        auto canonical = args;
        canonical.remove("idempotency_key");
        // Qt's JSON object serialization orders keys; defaults are normalized in
        // validate. The domain, identity and target set are part of the MAC.
        const auto fingerprint = mac("arguments-v1:" + tool.name.toUtf8() + ':' + identity.toUtf8() + ':' +
                                     QJsonDocument(canonical).toJson(QJsonDocument::Compact));
        auto existing = store_.query("SELECT id,fingerprint,created FROM mcp_operations WHERE key_id=? AND "
                                     "identity=? AND tool=? AND retry_hash=?",
                                     {key, identity, tool.name, retry_hash});
        if (existing.next()) {
            if (existing.value(2).toLongLong() + 86400 < now())
                throw Error("IdempotencyExpired");
            if (existing.value(1).toByteArray() != fingerprint)
                throw Error("IdempotencyConflict");
            reply->finish(status(key, existing.value(0).toString()));
            emit changed();
            return;
        }
        if (epoch != retryEpoch())
            throw Error("IdempotencyExpired");
        targets = validate(tool, identity, args, true, key);
        authorize(key, tool, identity, targets);
        const auto decision = approval(key, tool, identity, targets);
        if (decision.denied)
            throw Error("AccessDenied");
        if (decision.approval && !ui_available_)
            throw Error("ApprovalUnavailable");
        int per_key = 0, total_approvals = 0, key_approvals = 0;
        for (const auto& p : pending_) {
            per_key += p.key == key;
            const auto waiting = p.deadline > 0;
            total_approvals += waiting;
            key_approvals += waiting && p.key == key;
        }
        if (pending_.size() >= 64 || per_key >= 8 ||
            (decision.approval && (total_approvals >= 32 || key_approvals >= 4)))
            throw Error("OperationLimit");
        prune();
        auto retained =
            store_.query("SELECT "
                         "count(*),COALESCE(sum(length(targets)+length(reference)+length(identity)+length("
                         "tool)+length(key_id)+length(state)+160),0) FROM mcp_operations");
        retained.next();
        if (retained.value(0).toInt() >= 10000 || retained.value(1).toLongLong() > 16 * 1024 * 1024 - 4096)
            throw Error("RetentionLimit");
        const auto id = uuid();
        store_.query("INSERT INTO "
                     "mcp_operations(id,key_id,identity,tool,targets,retry_hash,fingerprint,state,created) "
                     "VALUES(?,?,?,?,?,?,?,?,?)",
                     {id, key, identity, tool.name, encode(targets), retry_hash, fingerprint,
                      decision.approval ? "Pending" : "Approved", now()});
        pending_.insert(id, Pending{id, key, identity, tool.name, targets, args,
                                    decision.approval ? clock_.elapsed() + timeout_ * 1000 : 0, reply});
        if (decision.approval)
            event("mcp.approval.requested", key, identity, tool.name, id, targets);
        else
            QTimer::singleShot(0, this, [this, id] { run(id); });
        emit changed();
    } catch (const Error& error) {
        reply->finish({}, QString::fromLatin1(error.what()));
        event("mcp.request.rejected", key, {}, tool_name, {});
        emit changed();
    } catch (...) {
        reply->finish({}, "InternalError");
        emit changed();
    }
}
QVariantList Service::pending() const {
    try {
        QVariantList result;
        for (const auto& p : pending_)
            if (p.deadline) {
                auto q = store_.query("SELECT name FROM mcp_keys WHERE id=?", {p.key});
                q.next();
                result << QVariantMap{
                    {"id", p.id},
                    {"key", q.value(0)},
                    {"identity", p.identity},
                    {"tool", p.tool},
                    {"contacts", p.targets},
                    {"arguments", approvalArguments(p.args)},
                    {"remaining", qMax<qint64>(0, (p.deadline - clock_.elapsed() + 999) / 1000)}};
            }
        return result;
    } catch (...) {
        return {};
    }
}
QVariantList Service::activity() const {
    try {
        QVariantList result;
        for (const auto& p : pending_) {
            auto key = store_.query("SELECT name FROM mcp_keys WHERE id=?", {p.key});
            key.next();
            result << QVariantMap{{"operation", p.id},     {"tool", p.tool},
                                  {"key", key.value(0)},   {"identity", p.identity},
                                  {"contacts", p.targets}, {"state", p.deadline ? "Pending" : "Approved"}};
        }
        result.append(history());
        return result;
    } catch (...) {
        return {};
    }
}
QVariantList Service::history() const {
    QVariantList result;
    for (const auto& row : history_)
        result << row;
    return result;
}
bool Service::decide(const QString& id, const QString& value, const QString& scope) {
    try {
        auto it = pending_.find(id);
        if (it == pending_.end() || !it->deadline || it->deadline <= clock_.elapsed() || !ui_available_)
            throw Error("ApprovalUnavailable");
        if (value != "Allow once" && value != "Allow always" && value != "Deny" && value != "Deny always")
            throw Error("InvalidDecision");
        if (scope != "Selected contacts" && scope != "All contacts")
            throw Error("InvalidScope");
        const auto p = *it;
        authorize(p.key, definition(p.tool), p.identity, p.targets);
        if (approval(p.key, definition(p.tool), p.identity, p.targets).denied) {
            finish(id, "Cancelled");
            throw Error("AccessDenied");
        }
        if (value.endsWith("always")) {
            // Persist the entire multi-contact decision atomically. Invalidate
            // other affected approvals only after commit; this exact decision
            // is applied to the selected pending operation below.
            const auto targets =
                scope == "All contacts" || p.targets.isEmpty() ? QStringList{QString{}} : p.targets;
            auto count = store_.query("SELECT count(*) FROM mcp_rules");
            count.next();
            if (count.value(0).toInt() + targets.size() > 4096)
                throw Error("PolicyLimit");
            auto db = store_.db();
            if (!db.transaction())
                throw Error("PersistenceFailed");
            try {
                for (const auto& target : targets)
                    store_.query(
                        "INSERT INTO mcp_rules VALUES(?,?,?,?,?,?) ON CONFLICT(key_id,identity,tool,contact) "
                        "DO UPDATE SET decision=excluded.decision",
                        {uuid(), p.key, p.identity, p.tool, target,
                         int(value.startsWith("Allow") ? Decision::Allow : Decision::Deny)});
                if (!db.commit())
                    throw Error("PersistenceFailed");
            } catch (...) {
                db.rollback();
                throw;
            }
            invalidate([&](const Pending& other) {
                return other.id != id && other.key == p.key && other.identity == p.identity &&
                       other.tool == p.tool;
            });
        }
        if (value.startsWith("Deny"))
            finish(id, "Denied");
        else {
            store_.query("UPDATE mcp_operations SET state='Approved' WHERE id=? AND state='Pending'", {id});
            pending_[id].deadline = 0;
            event("mcp.approval.granted", p.key, p.identity, p.tool, id, p.targets);
            QTimer::singleShot(0, this, [this, id] { run(id); });
        }
        error_.clear();
        emit changed();
        return true;
    } catch (...) {
        return fail("ApprovalDecisionFailed");
    }
}
void Service::run(const QString& id) {
    auto it = pending_.find(id);
    if (it == pending_.end() || it->deadline)
        return;
    const auto p = *it;
    bool executing = false;
    try {
        if (stopped_) {
            finish(id, "Cancelled");
            return;
        }
        const auto& tool = definition(p.tool);
        authorize(p.key, tool, p.identity, p.targets);
        if (approval(p.key, tool, p.identity, p.targets).denied) {
            finish(id, "Cancelled");
            return;
        }
        auto args = p.args;
        args.remove("_request_details");
        if (validate(tool, p.identity, args, true, p.key) != p.targets || args != p.args) {
            finish(id, "Cancelled");
            return;
        }
        store_.query("UPDATE mcp_operations SET state='Executing' WHERE id=?", {id});
        executing = true;
        event("mcp.operation.started", p.key, p.identity, p.tool, id, p.targets);
        // Domain services are synchronous admission/commit calls, never waits
        // for peer delivery. Normal protocol queues own subsequent delivery.
        auto result = execute(tool, p.identity, args, id, p.key, clock_.elapsed() + 120000);
        finish(id, "Completed", std::move(result));
    } catch (const Error& error) {
        Q_UNUSED(error)
        finish(id, executing && definition(p.tool).mutates ? "IndeterminateOperation" : "Failed");
    } catch (...) {
        finish(id, executing && definition(p.tool).mutates ? "IndeterminateOperation" : "Failed");
    }
}
void Service::finish(const QString& id, const QString& state, QJsonObject result) {
    auto it = pending_.find(id);
    if (it == pending_.end())
        return;
    const auto p = *it;
    auto final_state = state;
    try {
        auto durable = store_.query("SELECT state,reference FROM mcp_operations WHERE id=?", {id});
        if (durable.next() && durable.value(0).toString() == "Completed") {
            final_state = "Completed";
            result["reference"] = durable.value(1).toString();
        }
        store_.query(
            "UPDATE mcp_operations SET state=?,reference=COALESCE(?,reference) WHERE id=?",
            {final_state,
             result.contains("reference") ? QVariant(result.value("reference").toString()) : QVariant{}, id});
    } catch (...) {
        // Leave the durable Executing marker in place: restart recovery will
        // report an uncertain outcome. Do not return a safely retryable failure.
        if (auto reply = p.reply.lock())
            reply->finish({{"operation", id}, {"state", "IndeterminateOperation"}});
        pending_.erase(it);
        error_ = "PersistenceFailed";
        emit changed();
        return;
    }
    if (auto reply = p.reply.lock()) {
        result["operation"] = id;
        result["state"] = final_state;
        reply->finish(std::move(result));
    }
    pending_.erase(it);
    event("mcp.operation." + final_state.toLower(), p.key, p.identity, p.tool, id, p.targets);
    auto key = store_.query("SELECT name FROM mcp_keys WHERE id=?", {p.key});
    key.next();
    history_.push_front(QVariantMap{{"operation", id},
                                    {"tool", p.tool},
                                    {"key", key.value(0)},
                                    {"identity", p.identity},
                                    {"contacts", p.targets},
                                    {"state", final_state},
                                    {"time", QDateTime::currentDateTime()}});
    while (history_.size() > 100)
        history_.pop_back();
    last_activity_ = clock_.elapsed();
    emit changed();
}
void Service::invalidate(const std::function<bool(const Pending&)>& affected) {
    QStringList ids;
    for (const auto& p : pending_)
        if (affected(p))
            ids << p.id;
    for (const auto& id : ids)
        finish(id, "Cancelled");
}
void Service::tick() {
    try {
        const auto elapsed = clock_.elapsed();
        bool usage_changed = false;
        if (elapsed - last_usage_write_ >= 1000) {
            QSet<QString> used;
            {
                std::lock_guard lock(credentials_mutex_);
                used.swap(used_keys_);
            }
            usage_changed = !used.isEmpty();
            for (const auto& key : used)
                store_.query("UPDATE mcp_keys SET last_used=? WHERE id=?", {now(), key});
            last_usage_write_ = elapsed;
        }
        QStringList expired;
        for (const auto& p : pending_) {
            if (p.deadline && p.deadline <= elapsed)
                expired << p.id;
            else {
                try {
                    authorize(p.key, definition(p.tool), p.identity, p.targets);
                } catch (...) {
                    expired << p.id;
                }
            }
        }
        for (const auto& id : expired)
            finish(id, pending_[id].deadline && pending_[id].deadline <= elapsed ? "Expired" : "Cancelled");
        const auto second = elapsed / 1000;
        if (usage_changed || active() != notified_active_ ||
            (!pending_.isEmpty() && second != notified_second_)) {
            notified_active_ = active();
            notified_second_ = second;
            emit changed();
        }
    } catch (...) {
        fail("PersistenceFailed");
    }
}
void Service::resetEventAuthorization() {
    event_authorization_ = uuid();
    dropped_events_.clear();
}
bool Service::eventTargetsVisible(const QJsonObject& data) const {
    const auto identity = data.value("identity").toString();
    auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
    if (!owner)
        return false;
    for (const auto& item : data.value("contacts").toArray()) {
        const auto target = item.toString();
        auto exists = store_.query("SELECT id FROM contact WHERE identity=? AND uuid=?",
                                   {owner->getId(), QUuid(target)});
        if (!exists.next())
            return false;
        auto denies = store_.query("SELECT permission FROM mcp_contacts WHERE identity=? AND contact IN "
                                   "('',?) GROUP BY permission HAVING COALESCE(MAX(CASE WHEN contact<>'' "
                                   "THEN decision END),MAX(CASE WHEN contact='' THEN decision END))=?",
                                   {identity, target, int(Decision::Deny)});
        if (denies.next())
            return false;
    }
    return true;
}
void Service::event(const QString& type, const QString& key, const QString& identity, const QString& tool,
                    const QString& operation, const QStringList& targets) {
    if (!transport_ && !type.startsWith("mcp."))
        return;
    QJsonObject data{{"type", type},           {"key", key},
                     {"identity", identity},   {"tool", tool},
                     {"operation", operation}, {"contacts", array(targets)},
                     {"run", run_id_},         {"time", QString::number(now())}};
    QStringList audience;
    try {
        if (eventTargetsVisible(data))
            for (const auto& view : event_views_)
                if (view.enabled && (!view.expires || view.expires > now()) &&
                    view.identities.contains(identity) && (key.isEmpty() || view.key == key))
                    audience << view.key;
    } catch (...) { /* Fail closed for event visibility on persistence failure. */
    }
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact).size() + audience.size() * 64;
    if (bytes > 8192)
        return;
    if (type.startsWith("mcp."))
        LFLOG_DEBUG << "MCP event=" << type.toStdString() << " tool=" << tool.toStdString()
                    << " operation=" << operation.toStdString();
    events_.push_back(Event{++sequence_, std::move(data), bytes, audience});
    event_bytes_ += bytes;
    while (events_.size() > 2048 || event_bytes_ > 4 * 1024 * 1024) {
        const auto& dropped = events_.front();
        try {
            const auto identity = dropped.data.value("identity").toString();
            if (eventTargetsVisible(dropped.data))
                for (const auto& view : event_views_)
                    if (view.enabled && (!view.expires || view.expires > now()) &&
                        view.identities.contains(identity) && dropped.audience.contains(view.key))
                        dropped_events_[view.key][identity] = dropped.sequence;
        } catch (...) {
            resetEventAuthorization();
        }
        event_bytes_ -= dropped.bytes;
        events_.pop_front();
    }
}
QJsonObject Service::events(const QString& key, const QString& identity, const QJsonObject& args) {
    quint64 after = 0;
    const auto token = args.value("cursor").toString();
    const auto domain = "events:" + run_id_ + ":" + event_authorization_;
    if (!token.isEmpty())
        after = open(token, key, identity, domain);
    if (after > sequence_)
        throw Error("InvalidCursor");
    QJsonArray items;
    quint64 last = after;
    const bool lost = !token.isEmpty() && dropped_events_.value(key).value(identity) > after;
    for (const auto& e : events_) {
        if (e.sequence <= after)
            continue;
        last = e.sequence;
        const auto& d = e.data;
        if (!e.audience.contains(key) || d.value("identity").toString() != identity ||
            (!d.value("key").toString().isEmpty() && d.value("key").toString() != key))
            continue;
        if (!eventTargetsVisible(d))
            continue;
        items.append(d);
        if (items.size() >= pageSize(args))
            break;
    }
    return {{"items", items}, {"cursor", seal(key, identity, domain, last)}, {"buffer_loss", lost}};
}
QJsonObject Service::execute(const Tool& tool, const QString& identity, const QJsonObject& args,
                             const QString& operation, const QString& key, qint64 deadline) {
    const auto checkpoint = [this, deadline] {
        if (deadline && clock_.elapsed() >= deadline)
            throw Error("ExecutionDeadline");
    };
    checkpoint();
    auto* owner = engine_.getIdentityManager()->identityFromUuid(QUuid(identity));
    if (!owner)
        throw Error("AccessDenied");
    const auto id = owner->getId();
    const auto domain = tool.name + ":" + args.value("conversation").toString();
    const auto cursor = args.value("cursor").toString();
    const auto after = cursor.isEmpty() ? 0 : open(cursor, key, identity, domain);
    if (after > INT_MAX)
        throw Error("InvalidCursor");
    const auto limit = pageSize(args);
    const auto& name = tool.name;
#ifdef DARKSPEAK_WITH_TEST_MCP
    if (TestSupport::isOperation(name)) {
        if (!test_support_)
            throw Error("TestUnavailable");
        return test_support_->execute(name, owner, args);
    }
#endif
    if (name == "status" || name == "diagnostics")
        return {{"identity", identity},         {"ready", engine_.getState() == DsEngine::RUNNING},
                {"online", engine_.isOnline()}, {"run", run_id_},
                {"retry_epoch", retryEpoch()},  {"retry_retention_seconds", 86400}};
    if (name == "identity_info")
        return {{"identity", identity},
                {"name", owner->getName()},
                {"sharing_id", QString::fromLatin1(owner->getB58EncodedIdetity())}};
    if (name == "contact" || name == "presence" || name == "profile") {
        const auto contact = contactFor(engine_, identity, args.value("contact").toString());
        if (name == "profile") {
            const auto bio = contact->getBio();
            bounded(bio, 320 * 1024, false);
            return {{"contact", args.value("contact")}, {"bio", bio}, {"available", contact->hasBio()}};
        }
        if (name == "presence")
            return {{"contact", args.value("contact")},
                    {"online", contact->isOnline()},
                    {"connection", int(contact->getOnlineStatus())}};
        return {{"contact", args.value("contact")},
                {"name", contact->getName()},
                {"state", int(contact->getState())},
                {"blocked", contact->isBlocked()}};
    }
    if (name == "contacts" || name == "contact_requests" || name == "conversations" || name == "messages") {
        QString sql;
        QVariantList values;
        if (name == "contacts") {
            sql = "SELECT id,uuid,name,state FROM contact WHERE identity=? AND id>? "
                  "ORDER BY id LIMIT ?";
            values = {id, QVariant::fromValue(after), limit};
        }
        if (name == "contact_requests") {
            sql = "SELECT id,timestamp FROM notification WHERE identity=? AND type=0 "
                  "AND id>? ORDER BY id "
                  "LIMIT ?";
            values = {id, QVariant::fromValue(after), limit};
        }
        if (name == "conversations") {
            sql = "SELECT id,uuid,participants FROM conversation WHERE identity=? "
                  "AND id>? ORDER BY id LIMIT ?";
            values = {id, QVariant::fromValue(after), limit};
        }
        if (name == "messages") {
            auto conversation = engine_.getConversationManager()->getConversation(
                QUuid(args.value("conversation").toString()));
            if (!conversation || conversation->getIdentityId() != id)
                throw Error("AccessDenied");
            sql = "SELECT id,content,direction,state,composed_time,message_id FROM "
                  "message WHERE "
                  "conversation_id=? AND id>? ORDER BY id LIMIT ?";
            values = {conversation->getId(), QVariant::fromValue(after), limit};
        }
        auto q = store_.query(sql, values);
        QJsonArray items;
        quint64 last = after;
        qsizetype bytes = 0;
        while (q.next()) {
            checkpoint();
            QJsonObject row;
            if (name == "contacts")
                row = {{"contact", q.value(1).toUuid().toString(QUuid::WithoutBraces)},
                       {"name", q.value(2).toString()},
                       {"state", q.value(3).toInt()}};
            if (name == "contact_requests") {
                row["time"] = q.value(1).toString();
                row["request"] = seal(key, identity, "request", q.value(0).toULongLong());
            }
            if (name == "conversations") {
                row["contact"] = q.value(2).toUuid().toString(QUuid::WithoutBraces);
                row["conversation"] = q.value(1).toUuid().toString(QUuid::WithoutBraces);
            }
            if (name == "messages") {
                row["message"] = QString::fromLatin1(q.value(5).toByteArray().toHex());
                row["text"] = q.value(1).toString();
                row["direction"] = q.value(2).toInt();
                row["state"] = q.value(3).toInt();
                row["time"] = q.value(4).toString();
            }
            const auto size = QJsonDocument(row).toJson(QJsonDocument::Compact).size();
            if (size > 320 * 1024)
                throw Error("ResultLimit");
            if (bytes + size > 320 * 1024)
                break;
            bytes += size;
            last = q.value(0).toInt();
            items.append(row);
        }
        return {{"items", items},
                {"cursor", seal(key, identity, domain, last)},
                {"may_have_more", items.size() == limit || q.isValid()}};
    }
    if (name == "contact_add") {
        // Existing GUI entry point performs sharing-ID validation and uses the
        // same contact services; observe the result instead of trusting void.
        auto before = store_.query("SELECT COALESCE(max(id),0) FROM contact WHERE identity=?", {id});
        before.next();
        const auto previous = before.value(0).toInt();
        owner->addContact({{"identity", id},
                           {"identityId", args.value("sharing_id").toString()},
                           {"name", args.value("name").toString()},
                           {"addmeMessage", args.value("message").toString()},
                           {"autoConnect", true}});
        auto q = store_.query("SELECT uuid FROM contact WHERE identity=? AND id>? "
                              "ORDER BY id LIMIT 1",
                              {id, previous});
        if (!q.next())
            throw Error("ContactCreationFailed");
        return {{"reference", q.value(0).toUuid().toString(QUuid::WithoutBraces)}};
    }
    if (name == "contact_approve" || name == "contact_reject") {
        if (!contact_decision_ ||
            !contact_decision_(int(open(args.value("request").toString(), key, identity, "request")),
                               name == "contact_approve"))
            throw Error("ContactDecisionFailed");
        return {};
    }
    if (name == "contact_remove") {
        const auto target = args.value("contact").toString();
        engine_.getContactManager()->deleteContact(QUuid(target));
        auto q = store_.query("SELECT id FROM contact WHERE uuid=?", {QUuid(target)});
        if (q.next())
            throw Error("ContactRemovalFailed");
        return {{"reference", target}};
    }
    if (name == "message_send") {
        // Preflight every recipient before the first mutation. Service-level
        // checks are repeated at dispatch so changed peer preferences apply.
        for (const auto& target : args.value("contacts").toArray())
            if (!contactFor(engine_, identity, target.toString())->canSendText(args.value("text").toString()))
                throw Error("PeerPolicyDenied");
        QJsonArray refs;
        for (const auto& target : args.value("contacts").toArray()) {
            checkpoint();
            auto contact = contactFor(engine_, identity, target.toString());
            auto conversation = engine_.getConversationManager()->getConversation(contact.get());
            const bool sent =
                conversation->sendMessageWithCommit(args.value("text").toString(), [&](int message_id) {
                    auto persisted = store_.query("SELECT message_id FROM message WHERE id=?", {message_id});
                    if (!persisted.next())
                        throw Error("PersistenceFailed");
                    auto committed = refs;
                    committed.append(QString::fromLatin1(persisted.value(0).toByteArray().toHex()));
                    const bool last = committed.size() == args.value("contacts").toArray().size();
                    store_.query("UPDATE mcp_operations SET reference=?,state=? WHERE id=?",
                                 {QString::fromUtf8(QJsonDocument(committed).toJson(QJsonDocument::Compact)),
                                  last ? "Completed" : "Executing", operation});
                    refs = committed;
                });
            if (!sent)
                throw Error("MessageSendFailed");
        }
        return {{"reference", QString::fromUtf8(QJsonDocument(refs).toJson(QJsonDocument::Compact))},
                {"messages", refs}};
    }
    throw Error("UnknownTool");
}
} // namespace ds::mcp
