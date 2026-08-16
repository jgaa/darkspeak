
#include <regex>
#include <optional>

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QFile>
#include <QSvgRenderer>
#include <QtEndian>
#include <iostream>

#include <qrcodegen.hpp>

#include "ds/manager.h"
#include "ds/base58.h"
#include "ds/base32.h"
#include "ds/bytes.h"
#include "ds/logutil.h"
#include "ds/protocolcodec.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

Manager *Manager::instance_;

namespace {

constexpr auto contactTextPrefix = "darkspeak:";
constexpr auto contactTextPrefixLength = sizeof("darkspeak:") - 1;

bool decodeLegacyIdentity(const QString& identity,
                          QByteArray& publicKey,
                          QString& endpoint)
{
    constexpr auto payloadLength = 1 + 32 + 35 + 2;
    QByteArray data;
    try {
        data = b58tobin_check<QByteArray>(identity.toStdString(), payloadLength, {11, 176});
    } catch (const std::exception&) {
        return false;
    }
    if (data.size() != payloadLength || data.at(0) != 2) {
        return false;
    }

    publicKey = data.mid(1, 32);
    const auto host = QString::fromLatin1(onionv3encode(data.mid(33, 35)));
    auto portBytes = data.mid(68, 2);
    const auto port = qFromBigEndian(bytesToValue<quint16>(portBytes));
    endpoint = QStringLiteral("onion:") + host + QStringLiteral(".onion:")
            + QString::number(port);
    return true;
}

bool setDescriptorFromLegacy(ds::protocol::ContactDescriptor& descriptor,
                             const QString& nickname,
                             const QString& identity)
{
    QByteArray publicKey;
    QString endpoint;
    if (!decodeLegacyIdentity(identity, publicKey, endpoint)) {
        return false;
    }

    ds::protocol::ContactEndpoint contactEndpoint;
    contactEndpoint.setOnionAddress(endpoint);
    descriptor.setVersion(1);
    descriptor.setUserId(publicKey);
    if (!nickname.isEmpty()) {
        descriptor.setNickname(nickname);
    }
    descriptor.setEndpoints({contactEndpoint});
    return true;
}

bool validDescriptor(const ds::protocol::ContactDescriptor& descriptor)
{
    static const QRegularExpression endpointPattern{
        QStringLiteral("^onion:[a-z2-7]{56}\\.onion:[1-9][0-9]{0,4}$")};
    const auto reject = [](const char *reason) {
        LFLOG_TRACE << "Contact descriptor rejected: " << reason;
        return false;
    };
    if (descriptor.version() != 1) {
        return reject("unsupported descriptor version");
    }
    if (descriptor.userId().size() != crypto_sign_PUBLICKEYBYTES) {
        LFLOG_TRACE << "Contact descriptor rejected: user id length is "
                    << descriptor.userId().size() << ", expected "
                    << crypto_sign_PUBLICKEYBYTES;
        return false;
    }
    if (descriptor.endpoints().isEmpty()) {
        return reject("no endpoints");
    }
    if (descriptor.endpoints().size() > 8) {
        return reject("too many endpoints");
    }
    if (!descriptor.unknownFieldNumbers().isEmpty()) {
        return reject("unknown descriptor fields");
    }
    if (descriptor.hasNickname() && descriptor.nickname().toUtf8().size() > 128) {
        return reject("nickname is too long");
    }

    if (descriptor.hasNickname() && descriptor.nickname().contains(QChar{u'\0'})) {
        return reject("nickname contains NUL");
    }

    QSet<QString> endpoints;
    for (const auto& endpoint : descriptor.endpoints()) {
        if (!endpoint.hasOnionAddress()) {
            return reject("endpoint has no onion address");
        }
        if (!endpointPattern.match(endpoint.onionAddress()).hasMatch()) {
            LFLOG_TRACE << "Contact descriptor rejected: endpoint format mismatch; length is "
                        << endpoint.onionAddress().size();
            return false;
        }
        if (!endpoint.unknownFieldNumbers().isEmpty()) {
            return reject("unknown endpoint fields");
        }
        if (endpoints.contains(endpoint.onionAddress())) {
            return reject("duplicate endpoint");
        }
        const auto port = endpoint.onionAddress().section(QLatin1Char(':'), -1).toUShort();
        if (port == 0) {
            return reject("invalid endpoint port");
        }
        endpoints.insert(endpoint.onionAddress());
    }
    return true;
}

/*
 * Keep the parser's failures distinguishable in trace logs.  Clipboard data
 * is untrusted input, so this deliberately reports structure and sizes only.
 */
bool descriptorFromText(const QString& text, ds::protocol::ContactDescriptor& descriptor)
{
    const auto reject = [](const char *reason) {
        LFLOG_TRACE << "Contact ID rejected: " << reason;
        return false;
    };
    if (!text.startsWith(QStringLiteral("darkspeak:"))) {
        return reject("missing prefix");
    }
    if (text.size() > 4096) {
        return reject("text too long");
    }
    if (text.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return reject("whitespace present");
    }
    const auto encoded = text.mid(static_cast<int>(contactTextPrefixLength)).toUtf8();
    if (encoded.isEmpty()) {
        return reject("empty encoded payload");
    }
    if (encoded.size() > 4080) {
        return reject("encoded payload too long");
    }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$")).match(
            QString::fromLatin1(encoded)).hasMatch()) {
        return reject("invalid Base64URL alphabet");
    }
    const auto binary = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
    LFLOG_TRACE << "Contact ID decoded payload size: " << binary.size();
    if (binary.size() < 5) {
        return reject("decoded payload too short");
    }
    if (binary.size() > 3072) {
        return reject("decoded payload too long");
    }
    if (binary.left(3) != QByteArrayLiteral("DSC")) {
        return reject("bad binary magic");
    }
    if (static_cast<unsigned char>(binary.at(3)) != 1) {
        return reject("unsupported binary format version");
    }
    if (binary.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
            != encoded) {
        return reject("non-canonical Base64URL encoding");
    }
    if (!ds::prot::codec::deserialize(QByteArrayView{binary.mid(4)}, descriptor)) {
        return reject("protobuf deserialization failed");
    }
    LFLOG_TRACE << "Contact descriptor decoded: version " << descriptor.version()
                << ", user id length " << descriptor.userId().size()
                << ", endpoints " << descriptor.endpoints().size()
                << ", nickname present: " << (descriptor.hasNickname() ? "yes" : "no");
    return validDescriptor(descriptor);
}

QByteArray contactText(const ds::protocol::ContactDescriptor& descriptor)
{
    if (!validDescriptor(descriptor)) {
        return {};
    }
    QByteArray binary = QByteArrayLiteral("DSC");
    binary.append(char{1});
    binary += ds::prot::codec::serialize(descriptor);
    return QByteArrayLiteral("darkspeak:")
            + binary.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool descriptorToLegacy(const ds::protocol::ContactDescriptor& descriptor,
                        QByteArray& identity,
                        QString& address)
{
    if (!validDescriptor(descriptor) || descriptor.endpoints().size() != 1) {
        return false;
    }
    static const QRegularExpression endpointPattern{
        QStringLiteral("^onion:([a-z2-7]{56})\\.onion:([1-9][0-9]{0,4})$")};
    const auto match = endpointPattern.match(descriptor.endpoints().constFirst().onionAddress());
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const auto port = match.captured(2).toUShort(&ok);
    if (!ok || port == 0) {
        return false;
    }

    try {
        QByteArray binary;
        binary.append(char{2});
        binary += descriptor.userId();
        binary += onionv3decode(match.captured(1).toUtf8());
        appendValueToBytes(qToBigEndian(port), binary);
        identity = b58check_enc<QByteArray>(binary, {11, 176});
    } catch (const std::exception&) {
        return false;
    }
    address = descriptor.endpoints().constFirst().onionAddress();
    return !identity.isEmpty();
}

QImage makeIdentityQrImage(const QString& payload, const QString& identityName)
{
    const auto utf8Payload = payload.toUtf8();
    std::optional<qrcodegen::QrCode> qr;
    try {
        qr.emplace(qrcodegen::QrCode::encodeText(
            utf8Payload.constData(), qrcodegen::QrCode::Ecc::HIGH));
    } catch (const std::exception& error) {
        LFLOG_ERROR << "Unable to create identity QR code: " << error.what();
        return {};
    }

    constexpr int quietZoneModules = 4;
    constexpr int pixelsPerModule = 8;
    const auto moduleCount = qr->getSize();
    const auto imageSize = (moduleCount + quietZoneModules * 2) * pixelsPerModule;
    QImage image(imageSize, imageSize, QImage::Format_RGB32);
    image.fill(Qt::white);

    for (int y = 0; y < moduleCount; ++y) {
        for (int x = 0; x < moduleCount; ++x) {
            if (!qr->getModule(x, y)) {
                continue;
            }

            const auto left = (x + quietZoneModules) * pixelsPerModule;
            const auto top = (y + quietZoneModules) * pixelsPerModule;
            for (int py = 0; py < pixelsPerModule; ++py) {
                for (int px = 0; px < pixelsPerModule; ++px) {
                    image.setPixel(left + px, top + py, qRgb(0, 0, 0));
                }
            }
        }
    }

    // Overlay a small application mark.  The high error-correction level and
    // the opaque white plate keep the logo from obscuring ambiguous modules.
    QSvgRenderer logoRenderer(QStringLiteral(":/icons/darkspeak.svg"));
    if (logoRenderer.isValid()) {
        constexpr int logoMarginModules = 1;
        const auto logoSize = imageSize / 8;
        const auto plateSize = logoSize + logoMarginModules * 2 * pixelsPerModule;
        const auto plateLeft = (imageSize - plateSize) / 2;
        const auto plateTop = (imageSize - plateSize) / 2;

        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRect(plateLeft, plateTop, plateSize, plateSize);
        logoRenderer.render(&painter,
                            QRectF((imageSize - logoSize) / 2,
                                   (imageSize - logoSize) / 2,
                                   logoSize, logoSize));
    }

    QFont font;
    font.setPixelSize(30);
    font.setBold(true);
    const QFontMetrics metrics(font);
    const auto displayName = identityName.trimmed();
    const auto oneLine = displayName + QStringLiteral(" / DarkSpeak");
    const auto fitsOnOneLine = metrics.horizontalAdvance(oneLine) <= imageSize - 32;
    const auto footerHeight = fitsOnOneLine ? 104 : 144;
    QImage result(imageSize, imageSize + footerHeight, QImage::Format_RGB32);
    result.fill(QColor(QStringLiteral("#e8e8e8")));

    QPainter painter(&result);
    painter.drawImage(0, 0, image);
    painter.setPen(QColor(QStringLiteral("#001f5b")));
    painter.setFont(font);
    const QRect footerRect(16, imageSize, imageSize - 32, footerHeight);
    const auto website = QStringLiteral("https://darkspeak.org");
    if (fitsOnOneLine) {
        painter.drawText(footerRect.adjusted(0, 0, 0, -40), Qt::AlignCenter, oneLine);
        painter.drawText(footerRect.adjusted(0, 40, 0, 0), Qt::AlignCenter, website);
    } else {
        const auto nameLine = metrics.elidedText(displayName, Qt::ElideMiddle,
                                                 footerRect.width());
        const QRect nameRect = footerRect.adjusted(0, 0, 0, -80);
        const QRect productRect = footerRect.adjusted(0, 40, 0, -40);
        const QRect websiteRect = footerRect.adjusted(0, 80, 0, 0);
        painter.drawText(nameRect, Qt::AlignCenter, nameLine);
        painter.drawText(productRect, Qt::AlignCenter, QStringLiteral("DarkSpeak"));
        painter.drawText(websiteRect, Qt::AlignCenter, website);
    }
    return result;
}

logfault::LogLevel levelFromName(const QString& name)
{
    if (name == QStringLiteral("trace")) return logfault::LogLevel::TRACE;
    if (name == QStringLiteral("debug")) return logfault::LogLevel::DEBUGGING;
    if (name == QStringLiteral("info")) return logfault::LogLevel::INFO;
    if (name == QStringLiteral("warn")) return logfault::LogLevel::WARN;
    if (name == QStringLiteral("error")) return logfault::LogLevel::ERROR;
    return logfault::LogLevel::NOTICE;
}

void configureLogging(const QSettings& settings,
                      const QString& logFile,
                      const QString& logLevel,
                      LogModel& logModel)
{
    const auto makePrivate = [](const QString& path) {
        const QFileInfo info(path);
        QDir().mkpath(info.absolutePath());
        QFile::setPermissions(info.absolutePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);
        if (QFile::exists(path)) {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    };

#ifdef QT_DEBUG
    const auto defaultLevel = logfault::LogLevel::DEBUGGING;
#else
    const auto defaultLevel = logfault::LogLevel::NOTICE;
#endif

    auto& logManager = logfault::LogManager::Instance();

    const auto level = logLevel.isEmpty() ? defaultLevel : levelFromName(logLevel);
    const auto appLevel = core::getLogLevel(settings, core::LogSystem::APPLICATION);
    const auto stdoutLevel = core::getLogLevel(settings, core::LogSystem::STDOUT);
    const auto fileLevel = logLevel.isEmpty()
            ? core::getLogLevel(settings, core::LogSystem::LOGFILE) : level;

    if (appLevel != logfault::LogLevel::DISABLED) {
        logModel.installHandler(appLevel);
    }
    if (stdoutLevel != logfault::LogLevel::DISABLED) {
        logManager.AddHandler(std::make_unique<logfault::StreamHandler>(
                                  std::clog, stdoutLevel));
        LFLOG_NOTICE << "Enabled logging to standard output at level "
                     << logfault::Handler::LevelName(stdoutLevel);
    }

    if (!logFile.isEmpty()) {
        makePrivate(logFile);
        logManager.AddHandler(std::make_unique<logfault::StreamHandler>(
                                  logFile.toStdString(), fileLevel, true));
        QFile::setPermissions(logFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        LFLOG_NOTICE << "Enabled logging to \""
                     << logFile.toStdString()
                     << "\" at level "
                     << logfault::Handler::LevelName(fileLevel);
    } else if (core::isEnabled(settings, core::LogSystem::LOGFILE)) {
        const auto path = settings.value("logPath").toString();
        makePrivate(path);
        logManager.AddHandler(std::make_unique<logfault::StreamHandler>(
                                  path.toStdString(),
                                  fileLevel,
                                  true));
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        LFLOG_NOTICE << "Enabled logging to \""
                     << path.toStdString()
                     << "\" at level "
                     << logfault::Handler::LevelName(fileLevel);
    }
}

}

LogModel *Manager::logModel()
{
    return log_.get();
}


ContactsModel *Manager::contactsModel()
{
    return contacts_.get();
}

NotificationsModel *Manager::notificationsModel()
{
    return notifications_.get();
}

ConversationsModel *Manager::conversationsModel()
{
    return conversationsModel_.get();
}

MessagesModel *Manager::messagesModel()
{
    return messagesModel_.get();
}

FilesModel *Manager::filesModel()
{
    return filesModel_.get();
}

void Manager::textToClipboard(const QString& text)
{
    LFLOG_DEBUG << "Copying text to clipboard; DarkSpeak ID: "
                << (text.trimmed().startsWith(QStringLiteral("darkspeak:"))
                        ? "yes" : "no")
                << "; empty: " << (text.isEmpty() ? "yes" : "no");
    auto cb = QGuiApplication::clipboard();
    cb->setText(text);
}

bool Manager::hasValidContactInClipboard() const
{
    ds::protocol::ContactDescriptor descriptor;
    QByteArray identity;
    QString address;
    return descriptorFromText(QGuiApplication::clipboard()->text().trimmed(), descriptor)
            && descriptorToLegacy(descriptor, identity, address);
}

void Manager::createIdentityQrCode(const QString& identityName,
                                   const QString& identity)
{
    ds::protocol::ContactDescriptor descriptor;
    if (!setDescriptorFromLegacy(descriptor, identityName, identity)) {
        setStatusMessage(tr("Unable to create identity QR code"), ERROR);
        return;
    }
    const auto payload = contactText(descriptor);
    if (payload.isEmpty()) {
        setStatusMessage(tr("Unable to create identity QR code"), ERROR);
        return;
    }
    const auto image = makeIdentityQrImage(payload, identityName);
    if (image.isNull()) {
        setStatusMessage(tr("Unable to create identity QR code"), ERROR);
        return;
    }

    QSettings settings;
    auto directory = settings.value(QStringLiteral("identityQr/saveDirectory"))
                             .toString();
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    }
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }

    auto safeName = identityName.trimmed();
    safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                     QStringLiteral("_"));
    if (safeName.isEmpty()) {
        safeName = QStringLiteral("identity");
    }

    auto fileName = QFileDialog::getSaveFileName(
        nullptr, QObject::tr("Save Identity QR Code"),
        QDir(directory).filePath(safeName + QStringLiteral(".png")),
        QObject::tr("PNG image (*.png)"));
    if (fileName.isEmpty()) {
        setStatusMessage(tr("Saving identity QR code was cancelled"), WARNING);
        return;
    }

    if (QFileInfo(fileName).suffix().isEmpty()) {
        fileName += QStringLiteral(".png");
    }

    if (!image.save(fileName, "PNG")) {
        LFLOG_ERROR << "Unable to save identity QR code to "
                    << fileName.toStdString();
        setStatusMessage(tr("Unable to save identity QR code"), ERROR);
        return;
    }

    settings.setValue(QStringLiteral("identityQr/saveDirectory"),
                      QFileInfo(fileName).absolutePath());
    settings.sync();
    LFLOG_INFO << "Saved identity QR code to " << fileName.toStdString();
}

QVariantMap Manager::getIdenityFromClipboard() const
{
    QVariantMap values;
    const auto text = QGuiApplication::clipboard()->text().trimmed();
    ds::protocol::ContactDescriptor descriptor;
    if (!descriptorFromText(text, descriptor)) {
        return values;
    }

    QByteArray identity;
    QString address;
    if (!descriptorToLegacy(descriptor, identity, address)) {
        return {};
    }
    values["nickName"] = descriptor.hasNickname() ? descriptor.nickname() : QString{};
    values["identity"] = QString::fromLatin1(identity);
    values["address"] = address;
    values["handle"] = b58check_enc<QByteArray>(descriptor.userId(), {249, 50});

    return values;
}

QString Manager::getIdentityText(const QString& identityName,
                                 const QString& identity) const
{
    ds::protocol::ContactDescriptor descriptor;
    if (!setDescriptorFromLegacy(descriptor, identityName, identity)) {
        LFLOG_DEBUG << "Unable to create DarkSpeak ID from identity data";
        return {};
    }
    const auto result = QString::fromLatin1(contactText(descriptor));
    LFLOG_DEBUG << "Created DarkSpeak ID from identity data; success: "
                << (!result.isEmpty() ? "yes" : "no");
    return result;
}

QString Manager::urlToPath(const QString& url)
{
    QUrl uurl{url};
    return uurl.toLocalFile();
}

QString Manager::pathToUrl(const QString& path)
{
    auto url = QUrl::fromLocalFile(path);
    return url.url();
}

void Manager::setTmpImageFromPath(const QString& path, const QSize &size)
{
    const QUrl location = QUrl::fromUserInput(path);
    const auto localPath = location.isLocalFile() ? location.toLocalFile() : path;

    QImage image;

    if (!image.load(localPath)) {
        LFLOG_ERROR << "Failed to load file: \"" << localPath << "\".";
        setStatusMessage(tr("Failed to load image"), ERROR);
        return;
    }

    tmpImage_ = make_unique<QImage>(image.scaled(size.width(),
                                                 size.height(),
                                                 Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));

    LFLOG_TRACE << "Loaded tmp image \"" << location.toLocalFile() << "\".";
}

void Manager::setTmpImageFromImage(QImage &image)
{
    tmpImage_ = make_unique<QImage>(image);
}

void Manager::clearTmpImage()
{
    tmpImage_.reset();
}

QImage Manager::getTmpImage() const
{
    if (tmpImage_) {
        return *tmpImage_;
    }

    return {};
}

void Manager::deleteContact(const QUuid &uuid)
{
    try {
        engine_->getContactManager()->deleteContact(uuid);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to delete contact: " << ex.what();
        setStatusMessage(tr("Failed to delete contact: %1")
                         .arg(QString::fromUtf8(ex.what())), ERROR);
    }
}

void Manager::setStatusMessage(const QString& message, const StatusLevel level)
{
    status_message_ = message;
    status_level_ = level;
    emit statusMessageChanged();
}

Manager::Manager(const QString& logFile, const QString& logLevel)
{
    QSettings settings{};
    core::ensureLogSettings(settings);
    log_ = make_unique<LogModel>();
    configureLogging(settings, logFile, logLevel, *log_);

    engine_ = make_unique<DsEngine>();

    connect(engine_.get(), &ds::core::DsEngine::statusMessage,
            this, [this](const QString& message,
                         const ds::core::DsEngine::StatusLevel level) {
        setStatusMessage(message, static_cast<StatusLevel>(level));
    });

    connect(engine_.get(), &ds::core::DsEngine::stateChanged,
            [this] (const AppState /*from*/, const AppState to) {
        app_state_ = to;
        emit appStateChanged(to);
    });

    connect(engine_.get(), &ds::core::DsEngine::onlineStateChanged,
            [this] (const ds::core::ProtocolManager::State /*from*/,
            const ds::core::ProtocolManager::State to) {

        if (to != online_state_) {
            const auto was_online = isOnline();
            online_state_ = to;
            LFLOG_DEBUG << "Manager: Online state changed to " << online_state_;
            emit onlineStateChanged(to);
            emit onlineStatusIconChanged();
            emit torVersionChanged();
            if (was_online != isOnline()) {
                emit onlineChanged();
            }
        }
    });

    connect(engine_.get(), &ds::core::DsEngine::torVersionChanged,
            this, &Manager::torVersionChanged);

    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        const auto clipboardText = QGuiApplication::clipboard()->text();
        //LFLOG_TRACE << "Clipboard text: " << clipboardText;
        const auto hasDarkSpeakPrefix = clipboardText.trimmed().startsWith(
            QStringLiteral("darkspeak:"));
        ds::protocol::ContactDescriptor descriptor;
        QByteArray identity;
        QString address;
        const auto parsesAsContact = hasDarkSpeakPrefix
                && descriptorFromText(clipboardText.trimmed(), descriptor);
        const auto convertsToContact = parsesAsContact
                && descriptorToLegacy(descriptor, identity, address);
        LFLOG_DEBUG << "Clipboard changed; DarkSpeak ID: "
                    << (convertsToContact ? "yes" : "no")
                    << "; darkspeak prefix: "
                    << (hasDarkSpeakPrefix ? "yes" : "no")
                    << "; parses: " << (parsesAsContact ? "yes" : "no")
                    << "; convertible: " << (convertsToContact ? "yes" : "no");
        emit clipboardChanged();
    });

    contacts_ = make_unique<ContactsModel>(*this);
    notifications_ = make_unique<NotificationsModel>(engine_->settings());
    conversationsModel_ = make_unique<ConversationsModel>(*this);
    messagesModel_ = make_unique<MessagesModel>(*this);
    filesModel_ = make_unique<FilesModel>(*this);

    instance_ = this;
}


Manager::AppState Manager::getAppState() const
{
    return app_state_;
}

Manager::OnlineState Manager::getOnlineState() const
{
    return online_state_;
}

bool Manager::isOnline() const
{
    return online_state_ == ProtocolManager::State::ONLINE;
}

QString Manager::getTorVersion() const
{
    return engine_ ? engine_->getTorVersion() : QString{};
}

QString Manager::getQtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString Manager::getCompiler() const
{
    return QStringLiteral(DARKSPEAK_COMPILER);
}

QString Manager::getBuildDate() const
{
    return QStringLiteral(DARKSPEAK_BUILD_DATE);
}

QString Manager::getProgramName() const
{
    return "DarkSpeak";
}

QString Manager::getProgramNameAndVersion() const
{
    return getProgramName() + " " + PROGRAM_VERSION;
}

int Manager::getCurrentPage()
{
    return page_;
}

void Manager::setCurrentPage(int page)
{
    if (page != page_) {
        page_ = page;
        emit currentPageChanged(page);
    }
}

QUrl Manager::getOnlineStatusIcon() const
{
    static const std::array<QUrl, 5> icons = {
       QUrl("qrc:/images/network_offline.svg"),
       QUrl("qrc:/images/network_connecting.svg"),
       QUrl("qrc:/images/network_connected.svg"),
       QUrl("qrc:/images/network_online.svg"),
       QUrl("qrc:/images/network_offline.svg"),
   };

    return icons.at(online_state_);
}

QString Manager::getStatusMessage() const
{
    return status_message_;
}

Manager::StatusLevel Manager::getStatusLevel() const
{
    return status_level_;
}

void Manager::goOnline()
{
    engine_->start();
}

void Manager::goOffline()
{
    engine_->close();
}

void Manager::onQuit()
{
    LFLOG_INFO << "Darkspeak is shutting down.";


    if (engine_) {
        engine_->close();
    }
}

}} // namespaces
