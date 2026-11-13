#include "test_runtime.h"
#include "ds/avatarutil.h"
#include "ds/crypto.h"
#include "ds/manager.h"
#include "ds/torprotocolmanager.h"
#include "test_support.h"
#include "transport.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QSaveFile>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sodium.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds::mcp {
namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("UnsafeTestBootstrap");
}
QByteArray protectedRead(const QString& path) {
    const auto fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0);
    struct stat info{};
    const bool safe = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                      (info.st_mode & 0777) == 0600 && info.st_nlink == 1 && info.st_size <= 16384;
    if (!safe) {
        ::close(fd);
        require(false);
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        require(false);
    }
    return file.readAll();
}
QString validateProfile(const QString& requested) {
    require(QDir::isAbsolutePath(requested));
    const auto root = QDir::cleanPath(requested);
    const auto temporary = QFileInfo(QDir::tempPath()).canonicalFilePath();
    require(root.startsWith(temporary + "/ds-mcp-"));
    QString path = root;
    int levels = 0;
    while (path != temporary) {
        struct stat info{};
        require(lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
                info.st_uid == geteuid() && (info.st_mode & 0777) == 0700);
        path = QFileInfo(path).absolutePath();
        require(++levels <= 4);
    }
    require(protectedRead(root + "/.darkspeak-test-profile-v1") == "darkspeak-mcp-test-v1\n");
    return root;
}
void writeProtected(const QString& path, const QJsonObject& data) {
    QSaveFile output(path);
    require(output.open(QIODevice::WriteOnly));
    require(output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    require(output.write(bytes) == bytes.size() && output.commit());
}
QByteArray secret() {
    QByteArray bytes(32, Qt::Uninitialized);
    randombytes_buf(bytes.data(), bytes.size());
    const auto encoded = bytes.toHex();
    sodium_memzero(bytes.data(), bytes.size());
    return encoded;
}
} // namespace
bool testRuntimeRequested(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--mcp-test-profile" || QByteArray(argv[i]) == "--activate-mcp-test")
            return true;
    return false;
}
int runTestRuntime(int argc, char* argv[]) {
    try {
        // Apply isolation before constructing QApplication, settings or services.
        require(argc == 4 && QByteArray(argv[1]) == "--activate-mcp-test" &&
                QByteArray(argv[2]) == "--mcp-test-profile");
        const auto root = validateProfile(QString::fromLocal8Bit(argv[3]));
        const auto config = QJsonDocument::fromJson(protectedRead(root + "/bootstrap.json")).object();
        require(config.value("version").toInt() == 1);
        const auto port = [&](const char* name) {
            const auto value = config.value(name).toDouble();
            require(value >= 1024 && value <= 65535 && value == int(value));
            return int(value);
        };
        const int ordinary_port = port("mcp_port"), controller_port = port("controller_port");
        const int control_port = port("tor_control_port"), socks_port = port("tor_socks_port");
        require(QSet<int>{ordinary_port, controller_port, control_port, socks_port}.size() == 4);
        require(config.value("label").toString() == "A" || config.value("label").toString() == "B");
        const auto passphrase = config.value("vault_passphrase").toString().toUtf8();
        require(passphrase.size() == 64);
        umask(0077);
        const auto parent = getppid();
        require(parent > 1 && prctl(PR_SET_PDEATHSIG, SIGTERM) == 0 && getppid() == parent);
        const rlimit no_core{0, 0};
        require(setrlimit(RLIMIT_CORE, &no_core) == 0);
        for (const auto& name : {"home", "config", "data", "data/downloads", "cache", "runtime", "inputs"}) {
            const auto path = root + "/" + name;
            require(QDir().mkpath(path) && !QFileInfo(path).isSymLink());
            require(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                    QFileDevice::ExeOwner));
        }
        qputenv("HOME", (root + "/home").toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (root + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (root + "/cache").toUtf8());
        qputenv("XDG_RUNTIME_DIR", (root + "/runtime").toUtf8());
        qputenv("DARKSPEAK_PROFILE_ROOT", root.toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", passphrase);
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + "/config");
        QCoreApplication::setOrganizationName("DarkSpeak-test");
        QCoreApplication::setApplicationName("DarkSpeak-test");
        QApplication app(argc, argv);
        app.setQuitOnLastWindowClosed(false);
        QLockFile lock(root + "/instance.lock");
        require(lock.tryLock(0));
        // Free-form Qt diagnostic text can contain paths or peer-controlled data.
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
        ds::crypto::Crypto crypto;
        installTestDiagnostics();
        core::ProtocolManager::addFactory(core::ProtocolManager::Transport::TOR, [](QSettings& settings) {
            return std::make_shared<ds::prot::TorProtocolManager>(settings);
        });
        QSettings settings;
        settings.setValue("appAutoConnect", false);
        settings.setValue("useDarkSpeakFolder", false);
        settings.setValue("dbpath", root + "/data/application.sqlite");
        settings.setValue("downloadLocation", root + "/data/downloads");
        settings.setValue("logLevelApp", 0);
        settings.setValue("logLevelStdout", 0);
        settings.setValue("logLevelFile", 0);
        settings.setValue("torMode", 0);
        settings.setValue("torCtlHost", "127.0.0.1");
        settings.setValue("torCtlPort", control_port);
        settings.setValue("torCtlAuthMode",
                          0); // SAFECOOKIE, no unauthenticated controller
        settings.setValue("torSocksHost", "127.0.0.1");
        settings.setValue("torSocksPort", socks_port);
        settings.sync();
        ds::models::Manager manager;
        auto& engine = core::DsEngine::instance();
        auto* identities = engine.getIdentityManager();
        require(!config.contains("identity_count") ||
                (config.value("identity_count").isDouble() &&
                 config.value("identity_count").toDouble() == config.value("identity_count").toInt()));
        require(!config.contains("public_bio") || config.value("public_bio").isBool());
        require(!config.contains("preview_images") || config.value("preview_images").isBool());
        require(!config.contains("preview_videos") || config.value("preview_videos").isBool());
        const auto identity_count = config.value("identity_count").toInt(1);
        require(identity_count >= 1 && identity_count <= 2);
        const auto public_bio = config.value("public_bio").toBool(true);
        if (identities->rowCount({}) == 0) {
            for (int index = 0; index < identity_count; ++index) {
                core::IdentityData data;
                const auto label = config.value("label").toString();
                data.uuid = QUuid::createUuid();
                data.name = "Peer " + label + QString::number(index + 1);
                data.publicBio = public_bio;
                data.previewImages = config.value("preview_images").toBool(false);
                data.previewVideo = config.value("preview_videos").toBool(false);
                data.bio = label == "A" ? QString::fromUtf8("Peer A — Earth observer.")
                                        : QString::fromUtf8("Peer B — Lunar explorer.");
                if (identity_count > 1)
                    data.bio += QStringLiteral(" Identity %1.").arg(index + 1);
                data.cert = ds::crypto::DsCert::create();
                data.hash = data.cert->getHash().toByteArray();
                data.avatar = core::normalizeAvatar(
                    QImage(root + "/inputs/" + (label == "A" ? "earth.jpg" : "apollo11.jpg")));
                require(!data.avatar.isNull());
                data.allowMimeTypes = {"image/jpeg", "audio/ogg", "application/x-matroska", "video/matroska", "video/x-matroska"};
                auto* owner = identities->addIdentity(data);
                require(owner);
                // Same creation and transport services as
                // IdentityManager::createIdentity, scheduled independently from
                // application readiness.
                engine.whenOnline(
                    [&engine, owner] { engine.createNewTransport(owner->getName().toUtf8(), owner->getUuid()); });
            }
        }
        require(identities->rowCount({}) == identity_count);
        auto* owner = identities->identityFromRow(0);
        Service service(engine);
        service.setContactDecision([&manager](int id, bool accept) {
            return manager.notificationsModel()->decideContactRequest(id, accept);
        });
        const auto created = service.createKey(
            "scenario", {owner->getUuid().toString(QUuid::WithoutBraces)},
            {"read_status", "read_identities", "read_contacts", "read_profiles", "read_presence",
             "read_conversations", "read_message_content", "send_messages", "create_contacts", "read_events",
             "read_diagnostics", "send_files", "read_files", "remove_contacts"});
        require(!created.isEmpty());
        QJsonArray provisioned_identities;
        for (int index = 0; index < identity_count; ++index) {
            auto* identity = identities->identityFromRow(index);
            const auto id = identity->getUuid().toString(QUuid::WithoutBraces);
            const auto key = index == 0 ? created : service.createKey(
                "scenario", {id}, {"read_status", "read_identities", "read_contacts", "read_profiles",
                "read_presence", "read_conversations", "read_message_content", "send_messages",
                "create_contacts", "remove_contacts", "read_events", "read_diagnostics", "send_files", "read_files"});
            require(!key.isEmpty());
            provisioned_identities.append(QJsonObject{{"identity", id}, {"secret", key.value("secret").toString()}});
        }
        auto controller_secret = secret();
        TestSupport support(engine, service, QFileInfo(root + "/inputs").canonicalFilePath(),
                            controller_secret);
        require(service.configure(true, "127.0.0.1", ordinary_port, 60));
        Transport controller(service, "127.0.0.1", controller_port, &support);
        controller.start();
        writeProtected(root + "/provision.json",
                       {{"version", 1},
                        {"identities", provisioned_identities},
                        {"identity", owner->getUuid().toString(QUuid::WithoutBraces)},
                        {"ordinary_secret", created.value("secret").toString()},
                        {"controller_secret", QString::fromLatin1(controller_secret)},
                        {"mcp_port", ordinary_port},
                        {"controller_port", controller_port}});
        sodium_memzero(controller_secret.data(), controller_secret.size());
        // A lost harness cannot leave private application processes running
        // forever.
        QTimer::singleShot(620000, &app, &QCoreApplication::quit);
        return app.exec();
    } catch (...) {
        std::fputs("Isolated test bootstrap failed\n", stderr);
        return 2;
    }
}
} // namespace ds::mcp
