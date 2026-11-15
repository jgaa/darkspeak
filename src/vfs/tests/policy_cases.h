// Included after the shared test helpers; these cases define the architectural
// contract independently of storage implementation details.
void authorizationFixture() {
    using namespace dsvfs;
    QFile file(QStringLiteral(VFS_FIXTURES) + "/authorization-v1.json");
    CHECK(file.open(QIODevice::ReadOnly));
    const auto expected = QJsonDocument::fromJson(file.readAll()).object();
    CHECK(expected["version"].toInt() == authorization_format_version);
    auto policy=PermissionPolicy::deserialize(QJsonDocument(expected["policy"].toObject()).toJson(QJsonDocument::Compact));
    CHECK(policy);
    CHECK(QJsonDocument::fromJson(policy.value().serialize().value()).object()==expected["policy"].toObject());
    CHECK(policy.value().generation==73);
    CHECK(policy.value().resolve(QUuid("22222222-2222-2222-2222-222222222222"),{},u"/pub/title",Operation::Read)==Decision::Allow);
    CHECK(policy.value().resolve(QUuid("22222222-2222-2222-2222-222222222222"),{},u"/pub/title",Operation::Export)==Decision::Deny);
    const auto operations = expected["operations"].toObject();
    CHECK(operations["List"].toInt() == int(Operation::List));
    CHECK(operations["Export"].toInt() == int(Operation::Export));
    CHECK(operations["Recurse"].toInt() == int(Operation::Recurse));
    CHECK(expected["outcomes"].toObject()["Hidden"].toInt() == int(AuthorizationOutcome::Hidden));
}
void permissionSemantics() {
    using namespace dsvfs;
    PermissionPolicy p;
    for (int op = 0; op <= int(Operation::Recurse); ++op)
        p.defaults[Operation(op)] = Decision::Deny;
    p.defaults[Operation::Traverse] = Decision::Allow;
    p.defaults[Operation::Inspect] = Decision::Allow;
    p.defaults[Operation::List] = Decision::Allow;
    p.defaults[Operation::Read] = Decision::Allow;
    auto a = QUuid::createUuid(), b = QUuid::createUuid(), contact = QUuid::createUuid();
    CHECK(p.validate());
    CHECK(p.resolve(contact, {}, u"/", Operation::List) == Decision::Allow);
    CHECK(p.resolve(contact, {}, u"/", Operation::Export) == Decision::Deny);
    p.defaults[Operation::Export] = Decision::Allow;
    p.defaults[Operation::List] = Decision::Deny;
    CHECK(p.resolve(contact, {}, u"/", Operation::List) == Decision::Deny);
    CHECK(p.resolve(contact, {}, u"/", Operation::Export) == Decision::Allow);
    p.defaults[Operation::List] = Decision::Allow;
    p.rules = {{"/pub", Tier::Contact, contact, {{Operation::Read, Decision::Deny}}},
               {"/pub/books", Tier::Group, a, {{Operation::Read, Decision::Allow}}}};
    CHECK(p.resolve(contact, {a}, u"/pub/books/a", Operation::Read) == Decision::Deny);
    p.rules = {{"/", Tier::Group, a, {{Operation::Read, Decision::Deny}}},
               {"/pub", Tier::Group, a, {{Operation::Read, Decision::Allow}}},
               {"/", Tier::Group, b, {{Operation::Read, Decision::Deny}}}};
    CHECK(p.resolve(contact, {a}, u"/pub/a", Operation::Read) == Decision::Allow);
    CHECK(p.resolve(contact, {a, b}, u"/pub/a", Operation::Read) == Decision::Deny);
    p.rules = {{"/secret", Tier::Identity, {}, {}, Decision::Deny},
               {"/secret/books",
                Tier::Contact,
                contact,
                {{Operation::Read, Decision::Allow}, {Operation::List, Decision::Allow}}}};
    CHECK(p.resolve(contact, {}, u"/secret/a", Operation::Read) == Decision::Deny);
    CHECK(p.resolve(contact, {}, u"/secret/books/a", Operation::Read) == Decision::Allow);
    CHECK(p.navigable(contact, {}, u"/secret"));
    CHECK(p.resolve(contact, {}, u"/secret", Operation::List) == Decision::Deny);
    p.rules.append({"/secret", Tier::Contact, contact, {}, Decision::Deny});
    CHECK(p.resolve(contact, {}, u"/secret/books/a", Operation::Read) == Decision::Deny);
}
void permissionProvenance() {
    using namespace dsvfs;
    PermissionPolicy policy;
    for (int op = 0; op <= int(Operation::Recurse); ++op)
        policy.defaults[Operation(op)] = Decision::Deny;
    policy.generation = 73;
    auto contact = QUuid::createUuid(), a = QUuid::createUuid(), b = QUuid::createUuid();
    policy.rules = {{"/", Tier::Group, a, {{Operation::Read, Decision::Allow}}},
                    {"/", Tier::Group, b, {{Operation::Read, Decision::Deny}}},
                    {"/books", Tier::Contact, contact, {{Operation::Read, Decision::Allow}}}};
    auto decision = policy.explain(contact, {a, b}, u"/books/title", Operation::Read);
    CHECK(decision.allowed() && decision.policy_generation == 73 && decision.contributions.size() == 4);
    CHECK(decision.contributions[0].permission == Decision::Allow && !decision.contributions[0].decisive);
    CHECK(decision.contributions[1].permission == Decision::Deny && !decision.contributions[1].decisive);
    CHECK(decision.contributions[2].rule == policy.rules[2].id && decision.contributions[2].decisive);
    policy.rules.removeLast();
    decision = policy.explain(contact, {a, b}, u"/books/title", Operation::Read);
    CHECK(!decision.allowed() && decision.reason == AuthorizationReason::PermissionDenied);
    CHECK(!decision.contributions[0].decisive && decision.contributions[1].decisive);
    policy.rules = {{"/secret", Tier::Identity, {}, {}, Decision::Deny},
                    {"/secret/books", Tier::Identity, {}, {{Operation::Read, Decision::Allow}}, Decision::Allow}};
    CHECK(policy.explain(contact, {}, u"/secret/books/title", Operation::Read).outcome == AuthorizationOutcome::Hidden);
    policy.rules.append({"/secret/books", Tier::Contact, contact, {{Operation::Read, Decision::Allow}}});
    CHECK(policy.explain(contact, {}, u"/secret/books/title", Operation::Read).allowed());
    CHECK(policy.validate());
    const auto serialized = policy.serialize();
    CHECK(serialized);
    auto restored = PermissionPolicy::deserialize(serialized.value());
    CHECK(restored);
    CHECK(restored.value().serialize().value() == serialized.value());
    CHECK(restored.value().rules.first().id == policy.rules.first().id);
    CHECK(restored.value().explain(contact, {}, u"/secret/books/title", Operation::Read).allowed());
    auto malformed = QJsonDocument::fromJson(serialized.value()).object();
    malformed["version"] = 2;
    CHECK(!PermissionPolicy::deserialize(QJsonDocument(malformed).toJson()));
    policy.rules.last().id = policy.rules.first().id;
    CHECK(!policy.validate());
}
void overlaySemantics() {
    using namespace dsvfs;
    auto identity = QUuid::createUuid(), contact = QUuid::createUuid(), group = QUuid::createUuid(),
         other = QUuid::createUuid();
    auto def = [&](QString path, Tier tier, QUuid principal = {}) {
        return MountDefinition{
            QUuid::createUuid(), identity, QUuid::createUuid(), QUuid::createUuid(), path, tier, principal};
    };
    auto root = def("/pub", Tier::Identity), inherited = def("/pub/old", Tier::Identity);
    auto first = def("/pub", Tier::Group, group), second = def("/pub", Tier::Group, other);
    auto child = def("/pub/books", Tier::Group, group), replacement = def("/pub", Tier::Contact, contact);
    auto result = resolveOverlay({root, inherited, first, child}, identity, contact, {group});
    CHECK(result && result.value().mounts.size() == 2);
    CHECK(result.value().mounts[0].id == first.id);
    auto conflict = resolveOverlay({first, second, child}, identity, contact, {group, other});
    CHECK(conflict && conflict.value().conflicts.contains("/pub") && conflict.value().mounts.isEmpty());
    auto replaced = resolveOverlay({root, inherited, first, child, replacement}, identity, contact, {group});
    CHECK(replaced && replaced.value().mounts.size() == 1 && replaced.value().mounts[0].id == replacement.id);
    auto reintroduced = def("/pub/new", Tier::Contact, contact);
    auto restored =
        resolveOverlay({root, inherited, first, child, replacement, reintroduced}, identity, contact, {group});
    CHECK(restored && restored.value().mounts.size() == 2);
    CHECK(!validVirtualPath("/" + QString(766, 'a')));
    CHECK(!validVirtualPath(u"/a/../b") && !validVirtualPath(u"/a//b"));
}
