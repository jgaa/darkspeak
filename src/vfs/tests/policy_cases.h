// Included after the shared test helpers; these cases define the architectural
// contract independently of storage implementation details.
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
