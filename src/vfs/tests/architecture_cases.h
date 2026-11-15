void assignmentBoundaries() {
    Fixture f;
    auto sub = get(f.backend->mkdir(f.source.config.id, f.source.root, "restricted"));
    f.file("inside", "inside", sub);
    f.file("outside", "outside");
    auto assigned = f.vfs->assignments(f.identity).value().front();
    get(f.vfs->withdrawAssignment(assigned.id, f.request));
    auto resource = f.vfs->resources().value().front();
    auto restricted = get(f.vfs->assignStorage({{}, f.identity, resource.id, sub, false}, f.request));
    error(f.vfs->addMount(f.identity, {{}, {}, f.source.config.id, f.source.root, "escape", {}}, f.request),
          ErrorCode::PermissionDenied);
    f.mount("restricted", sub);
    auto node = f.mounted("restricted");
    auto child = get(f.vfs->lookup(f.identity, node.id, "inside", f.request));
    auto reader = get(f.vfs->prepareRead(f.identity, child.id, {}, f.request));
    auto pending = get(f.vfs->createFile(f.identity, node.id, {{}, "new", ConflictPolicy::Fail, {{3}, {}}}, f.request));
    get(pending.writer->writeAt(0, "new"));
    get(pending.writer->markVerified({0, 3}));
    int invalidated = 0;
    QObject context;
    auto subscription = get(f.vfs->subscribeDirectory(
        f.identity, node.id, {&context, [&](dsvfs::Change c) { invalidated += c.invalidated; }}, f.request));
    get(f.vfs->withdrawAssignment(restricted.id, f.request));
    CHECK(!reader.current());
    error(reader.reader->readAt(0, 1), ErrorCode::MissingEntry);
    error(pending.writer->commit(), ErrorCode::MissingEntry);
    get(f.vfs->refresh());
    QCoreApplication::processEvents();
    CHECK(invalidated == 1);
    get(f.vfs->shutdown());
    f.vfs.reset();
    dsvfs::Vfs reopened(f.temporary.path() + "/vfs");
    get(reopened.open());
    CHECK(reopened.assignments(f.identity).value().isEmpty());
    CHECK(!reopened.registerNamespace(f.source.config.id, f.backend, SymlinkPolicy::Never, QUuid::createUuid()));
    CHECK(reopened.registerNamespace(f.source.config.id, f.backend, SymlinkPolicy::Never, f.identity));
    error(reopened.stat(f.identity, child.id, f.request), ErrorCode::MissingEntry);
    get(reopened.shutdown());
}
void effectiveTopology() {
    using namespace dsvfs;
    Fixture f;
    auto group = QUuid::createUuid(), other = QUuid::createUuid();
    auto a = get(f.backend->mkdir(f.source.config.id, f.source.root, "A"));
    auto b = get(f.backend->mkdir(f.source.config.id, f.source.root, "B"));
    f.file("direct", "direct");
    f.file("ordinary", "hidden by recursion", a);
    f.file("book", "book", b);
    auto assign = f.vfs->assignments(f.identity).value().front();
    auto define = [&](QString path, QUuid source, Tier tier, QUuid principal = QUuid{}) {
        return get(f.vfs->defineMount({{}, f.identity, assign.id, source, path, tier, principal}, f.request));
    };
    auto pub = define("/pub", f.source.root, Tier::Identity);
    auto old = define("/pub/old", a, Tier::Identity);
    auto books = define("/pub/books", b, Tier::Group, group);
    auto request = f.request;
    request.groups = {group};
    auto policy = std::make_shared<PermissionPolicy>();
    for (int i = 0; i <= int(Operation::Recurse); ++i)
        policy->defaults[Operation(i)] = Decision::Allow;
    policy->rules = {{"/pub", Tier::Identity, {}, {{Operation::Recurse, Decision::Deny}}}};
    request.permissions = policy;
    request = get(f.vfs->compose(f.identity, request));
    auto pubnode = get(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "pub", request));
    auto page = get(f.vfs->list(f.identity, pubnode.id, {100, {}}, request));
    QSet<QString> names;
    for (auto entry : page.entries)
        names.insert(entry.name);
    CHECK(names.contains("direct") && names.contains("books") && names.contains("old") && !names.contains("A") &&
          !names.contains("B"));
    auto child = get(f.vfs->lookup(f.identity, pubnode.id, "books", request));
    CHECK(get(f.vfs->lookup(f.identity, child.id, "book", request)).type == EntryType::File);
    auto competing = define("/pub/books", a, Tier::Group, other);
    int conflicts = 0;
    QObject listener;
    QObject::connect(f.vfs.get(), &Vfs::conflictDetected, &listener, [&](auto, auto, auto) { ++conflicts; });
    auto conflict_request = f.request;
    conflict_request.groups = {group, other};
    conflict_request = get(f.vfs->compose(f.identity, conflict_request));
    get(f.vfs->compose(f.identity, conflict_request));
    CHECK(conflicts == 1);
    auto currentpub = get(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "pub", conflict_request));
    error(f.vfs->lookup(f.identity, currentpub.id, "books", conflict_request), ErrorCode::MissingEntry);
    auto contact = define("/pub", b, Tier::Contact, f.request.scope);
    auto replaced = get(f.vfs->compose(f.identity, f.request));
    auto replacement = get(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "pub", replaced));
    CHECK(replacement.id == f.vfs->nodeId(f.identity, contact.id, b).value());
    error(f.vfs->stat(f.identity, pubnode.id, replaced), ErrorCode::MissingEntry);
    CHECK(get(f.vfs->list(f.identity, replacement.id, {100, {}}, replaced)).entries.size() == 1);
    auto reintroduced = define("/pub/new", a, Tier::Contact, f.request.scope);
    auto fresh = get(f.vfs->compose(f.identity, f.request));
    CHECK(get(f.vfs->lookup(f.identity, replacement.id, "new", fresh)).id ==
          f.vfs->nodeId(f.identity, reintroduced.id, a).value());
    error(f.vfs->list(f.identity, pubnode.id, {100, {}}, request), ErrorCode::MissingEntry);
    auto descriptors = f.vfs->effectiveMounts(f.identity, fresh);
    CHECK(descriptors && !descriptors.value().isEmpty());
    auto stable = f.vfs->nodeId(f.identity, reintroduced.id, a).value();
    get(f.vfs->relocateDefinition(reintroduced.id, "/pub/renamed", f.request));
    auto moved = get(f.vfs->compose(f.identity, f.request));
    CHECK(get(f.vfs->lookup(f.identity, replacement.id, "renamed", moved)).id == stable);
    get(f.backend->mkdir(f.source.config.id, b, "container"));
    define("/pub/container/nested", a, Tier::Contact, f.request.scope);
    auto nested_request = get(f.vfs->compose(f.identity, f.request));
    auto container = get(f.vfs->lookup(f.identity, replacement.id, "container", nested_request));
    error(f.vfs->remove(f.identity, container.id, nested_request), ErrorCode::DirectoryNotEmpty);
    error(f.vfs->rename(f.identity, container.id, "moved", nested_request), ErrorCode::DirectoryNotEmpty);
    error(f.vfs->createFile(f.identity, container.id, {{}, "nested", ConflictPolicy::Fail, {{1}, {}}}, nested_request),
          ErrorCode::Conflict);
    (void)pub;
    (void)old;
    (void)books;
    (void)competing;
}
void navigationOnlyReopening() {
    using namespace dsvfs;
    Fixture f;
    auto assigned = f.vfs->assignments(f.identity).value().front();
    f.file("book", "book");
    get(f.vfs->defineMount(
        {{}, f.identity, assigned.id, f.source.root, "/secret/books", Tier::Contact, f.request.scope}, f.request));
    auto p = std::make_shared<PermissionPolicy>();
    for (int i = 0; i <= int(Operation::Recurse); ++i)
        p->defaults[Operation(i)] = Decision::Allow;
    p->rules = {{"/secret", Tier::Identity, {}, {}, Decision::Deny},
                {"/secret/books",
                 Tier::Contact,
                 f.request.scope,
                 {{Operation::Inspect, Decision::Allow},
                  {Operation::List, Decision::Allow},
                  {Operation::Read, Decision::Allow}}}};
    auto request = f.request;
    request.permissions = p;
    request = get(f.vfs->compose(f.identity, request));
    auto secret = get(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "secret", request));
    auto page = get(f.vfs->list(f.identity, secret.id, {100, {}}, request));
    CHECK(page.entries.size() == 1 && page.entries[0].name == "books");
    auto file = get(f.vfs->lookup(f.identity, page.entries[0].id, "book", request));
    CHECK(get(f.vfs->prepareRead(f.identity, file.id, {}, request)).info.size == 4);
    auto catalog = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, request));
    get(catalog->close());
    auto hidden = f.request;
    hidden.authorize = [](Target t, Operation) -> Result<bool> { return t.mount.isNull(); };
    hidden = get(f.vfs->compose(f.identity, hidden));
    error(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "secret", hidden), ErrorCode::MissingEntry);
}
void boundedFilteredContinuation() {
    dsvfs::Limits limits;
    limits.max_batch = 2;
    Fixture f(limits);
    auto a = f.file("a", "a"), b = f.file("b", "b");
    f.file("c", "c");
    f.mount();
    auto node = f.mounted();
    auto request = f.request;
    request.authorize = [a, b](dsvfs::Target t, dsvfs::Operation) -> Result<bool> {
        return t.source != a.id && t.source != b.id;
    };
    auto first = get(f.vfs->list(f.identity, node.id, {1, {}}, request));
    CHECK(first.entries.isEmpty() && !first.next_cursor.isEmpty());
    f.file("aa", "new hidden prefix");
    auto second = get(f.vfs->list(f.identity, node.id, {1, first.next_cursor}, request));
    CHECK(second.entries.size() == 1 && second.entries.front().name == "c");
}
void externalFilesOnly() {
    using namespace dsvfs;
    Fixture f;
    auto root = f.temporary.path() + "/external-source", outside = f.temporary.path() + "/outside";
    CHECK(QDir().mkdir(root));
    CHECK(QDir().mkdir(outside));
    {
        QFile file(outside + "/file");
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("external") == 8);
    }
    CHECK(::symlink("../outside", QFile::encodeName(root + "/directory").constData()) == 0);
    CHECK(::symlink("directory/file", QFile::encodeName(root + "/file").constData()) == 0);
    auto raw = QFile::encodeName(root) + "/" + QByteArray(255, char(0xff));
    int fd = ::open(raw.constData(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(::write(fd, "x", 1) == 1);
    CHECK(::close(fd) == 0);
    auto readonly = std::make_shared<ReadOnlyFileManager>(f.temporary.path() + "/external-index");
    ReadOnlyNamespaceConfig config;
    config.common = {QUuid::createUuid(), root};
    config.symlinks = SymlinkPolicy::Always;
    auto source = get(readonly->createSource(config));
    bool ready = false;
    for (int i = 0; i < 5000; ++i) {
        if (get(readonly->namespaceInfo(config.common.id)).status == NamespaceStatus::Ready) {
            ready = true;
            break;
        }
        QCoreApplication::processEvents();
    }
    CHECK(ready);
    CHECK(f.vfs->registerNamespace(config.common.id, readonly, SymlinkPolicy::Always));
    auto restricted = assignment(*f.vfs, f.identity, config.common.id, source.root, f.request);
    auto capable = assignment(*f.vfs, f.identity, config.common.id, source.root, f.request, true);
    auto makeMount = [&](QString name, QUuid assignment_id) {
        return get(
            f.vfs->addMount(f.identity, {{}, {}, config.common.id, source.root, name, {}, assignment_id}, f.request));
    };
    makeMount("restricted", restricted.id);
    makeMount("external", capable.id);
    auto ordinary = f.mounted("restricted"), enabled = f.mounted("external");
    error(f.vfs->lookup(f.identity, ordinary.id, "file", f.request), ErrorCode::MissingEntry);
    error(f.vfs->lookup(f.identity, enabled.id, "directory", f.request), ErrorCode::MissingEntry);
    auto file = get(f.vfs->lookup(f.identity, enabled.id, "file", f.request));
    auto policy = std::make_shared<PermissionPolicy>();
    for (int i = 0; i <= int(Operation::Recurse); ++i)
        policy->defaults[Operation(i)] = Decision::Allow;
    policy->rules = {{"/external/file", Tier::Contact, f.request.scope, {{Operation::Read, Decision::Deny}}}};
    auto denied = f.request;
    denied.permissions = policy;
    error(f.vfs->prepareRead(f.identity, file.id, {}, denied), ErrorCode::MissingEntry);
    auto prepared = get(f.vfs->prepareRead(f.identity, file.id, {}, f.request));
    CHECK(get(prepared.reader->readAt(0, 100)) == "external");
    auto ascii = f.request;
    ascii.allow_utf8 = false;
    auto catalog = get(f.vfs->exportCatalog(f.identity, ordinary.id, true, ascii));
    auto bytes = get(catalog->prepareRead());
    std::shared_ptr<IFileReader> input = std::move(bytes.reader);
    get(f.vfs->validateCatalog(input, catalog->size(), catalog->sha256()));
    get(input->close());
    get(catalog->close());
    get(f.vfs->withdrawAssignment(capable.id, f.request));
    CHECK(!prepared.current());
    error(prepared.reader->readAt(0, 100), ErrorCode::MissingEntry);
    get(f.vfs->shutdown());
    get(readonly->shutdown());
    CHECK(::unlink(raw.constData()) == 0);
}
