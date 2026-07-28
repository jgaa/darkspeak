
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
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QtEndian>
#include <iostream>

#include <qrcodegen.hpp>

#include "ds/manager.h"
#include "ds/base58.h"
#include "ds/base32.h"
#include "ds/bytes.h"
#include "ds/logutil.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

Manager *Manager::instance_;

namespace {

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
    const auto footerHeight = fitsOnOneLine ? 64 : 104;
    QImage result(imageSize, imageSize + footerHeight, QImage::Format_RGB32);
    result.fill(QColor(QStringLiteral("#e8e8e8")));

    QPainter painter(&result);
    painter.drawImage(0, 0, image);
    painter.setPen(QColor(QStringLiteral("#001f5b")));
    painter.setFont(font);
    const QRect footerRect(16, imageSize, imageSize - 32, footerHeight);
    if (fitsOnOneLine) {
        painter.drawText(footerRect, Qt::AlignCenter, oneLine);
    } else {
        const auto nameLine = metrics.elidedText(displayName, Qt::ElideMiddle,
                                                 footerRect.width());
        const QRect nameRect = footerRect.adjusted(0, 0, 0, -footerHeight / 2);
        const QRect productRect = footerRect.adjusted(0, footerHeight / 2, 0, 0);
        painter.drawText(nameRect, Qt::AlignCenter, nameLine);
        painter.drawText(productRect, Qt::AlignCenter, QStringLiteral("DarkSpeak"));
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
#ifdef QT_DEBUG
    const auto defaultLevel = logfault::LogLevel::DEBUGGING;
#else
    const auto defaultLevel = logfault::LogLevel::NOTICE;
#endif

    auto& logManager = logfault::LogManager::Instance();
    logManager.ClearHandlers();

    const auto level = logLevel.isEmpty() ? defaultLevel : levelFromName(logLevel);
    const auto appLevel = logLevel.isEmpty()
            ? core::getLogLevel(settings, core::LogSystem::APPLICATION) : level;
    const auto stdoutLevel = logLevel.isEmpty()
            ? core::getLogLevel(settings, core::LogSystem::STDOUT) : level;

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
        const QFileInfo fileInfo(logFile);
        QDir().mkpath(fileInfo.absolutePath());
        logManager.AddHandler(std::make_unique<logfault::StreamHandler>(
                                  logFile.toStdString(), level, true));
        LFLOG_NOTICE << "Enabled logging to \""
                     << logFile.toStdString()
                     << "\" at level "
                     << logfault::Handler::LevelName(level);
    } else if (core::isEnabled(settings, core::LogSystem::LOGFILE)) {
        const auto path = settings.value("logPath").toString();
        const auto fileLevel = core::getLogLevel(settings, core::LogSystem::LOGFILE);
        logManager.AddHandler(std::make_unique<logfault::StreamHandler>(
                                  path.toStdString(),
                                  fileLevel,
                                  true));
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
    auto cb = QGuiApplication::clipboard();
    cb->setText(text);
}

void Manager::createIdentityQrCode(const QString& identityName,
                                   const QString& identity)
{
    const auto payload = identityName + QStringLiteral(":") + identity;
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
    // name : base58 for v2 identity payload, pubkey, v3 address and port
    static const regex identity_pattern(R"(^(.+)\:([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]+)$)");
    std::smatch match;

    QVariantMap values;

    const auto text =  QGuiApplication::clipboard()->text().toStdString();
    if (!text.empty() && regex_match(text, match, identity_pattern)) {
        if (match.size() == 3) {
            const auto nick = match[1].str();
            const auto blob = match[2].str();
            values["nickName"] = nick.c_str();
            values["identity"] = blob.c_str();

            constexpr auto payload_len = 1 + 32 + 35 + 2;
            QByteArray data;
            try {
                data = b58tobin_check<QByteArray>(blob, payload_len, {11, 176});
            } catch (const std::exception&) {
                return {};
            }
            if (data.size() != payload_len || data.at(0) != 2) { // Version
                LFLOG_NOTICE << "Unsupported version of identity payload: v="
                             << static_cast<int>(data.at(0));
                return {};
            }

            const auto pubkey = data.mid(1, 32);
            const auto host = onionv3encode(data.mid(33, 35));
            const auto port = data.mid(68, 2);
            const auto portVal = bytesToValue<uint16_t>(port);

            values["address"] = QString("onion:") + host + ".onion"
                    + ":" + QString::number(qFromBigEndian(portVal));

            values["handle"] = b58check_enc<QByteArray>(pubkey, {249, 50});
        }
    }

    return values;
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
    log_ = make_unique<LogModel>(settings);
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
       QUrl("qrc:/network_connected.svg"),
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
