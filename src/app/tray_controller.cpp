#include "tray_controller.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMenu>
#include <QPainter>
#include <QSystemTrayIcon>
#include <QWindow>

#include "ds/notificationsmodel.h"

TrayController::TrayController(QWindow *window,
                               ds::models::NotificationsModel *notifications,
                               bool startHidden,
                               QObject *parent)
    : QObject(parent)
    , window_(window)
    , notifications_(notifications)
{
    icon_ = QIcon(QStringLiteral(":/icons/darkspeak.svg"));
    if (window_ == nullptr) {
        return;
    }

    if (notifications_ != nullptr) {
        connect(notifications_, &ds::models::NotificationsModel::notificationStateChanged,
                this, &TrayController::updateNotificationIcon);
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        window_->installEventFilter(this);
        updateNotificationIcon();
        return;
    }

    trayIcon_ = new QSystemTrayIcon(icon_, this);
    trayIcon_->setToolTip(QStringLiteral("DarkSpeak"));

    menu_ = new QMenu;
    toggleVisibilityAction_ = menu_->addAction(QStringLiteral("Hide DarkSpeak"));
    quitAction_ = menu_->addAction(QStringLiteral("Quit"));
    trayIcon_->setContextMenu(menu_);

    connect(toggleVisibilityAction_, &QAction::triggered,
            this, &TrayController::toggleWindowVisibility);
    connect(quitAction_, &QAction::triggered,
            this, &TrayController::requestQuit);
    connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    toggleWindowVisibility();
                }
            });
    connect(window_, &QWindow::visibilityChanged, this, [this]() {
        if (toggleVisibilityAction_ != nullptr && window_ != nullptr) {
            toggleVisibilityAction_->setText(window_->isVisible()
                ? QStringLiteral("Hide DarkSpeak")
                : QStringLiteral("Show DarkSpeak"));
        }
    });
    window_->installEventFilter(this);
    updateNotificationIcon();
    trayIcon_->show();
    if (startHidden) {
        hideWindow();
    }
}

void TrayController::updateNotificationIcon()
{
    const auto messages = notifications_ != nullptr && notifications_->hasMessages();
    const auto contactRequests = notifications_ != nullptr
            && notifications_->hasContactRequests();
    if (!messages && !contactRequests) {
        if (trayIcon_ != nullptr) {
            trayIcon_->setIcon(icon_);
        }
        if (window_ != nullptr) {
            window_->setIcon(icon_);
        }
        return;
    }

    auto pixmap = icon_.pixmap(QSize{64, 64});
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto drawDot = [&painter](const QPointF center, const QColor color) {
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(color);
        painter.drawEllipse(center, 8, 8);
    };
    if (contactRequests) {
        drawDot(QPointF{51, 13}, QColor{Qt::red});
    }
    if (messages) {
        drawDot(QPointF{51, 51}, QColor{Qt::green});
    }
    painter.end();
    const QIcon notificationIcon{pixmap};
    if (trayIcon_ != nullptr) {
        trayIcon_->setIcon(notificationIcon);
    }
    if (window_ != nullptr) {
        window_->setIcon(notificationIcon);
    }
}

TrayController::~TrayController()
{
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
    if (trayIcon_ != nullptr) {
        trayIcon_->setContextMenu(nullptr);
    }
    delete menu_;
}

bool TrayController::available() const
{
    return trayIcon_ != nullptr;
}

bool TrayController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == window_ && event != nullptr && event->type() == QEvent::Close
        && !allowWindowClose_ && trayIcon_ != nullptr) {
        static_cast<QCloseEvent *>(event)->ignore();
        hideWindow();
        if (QSystemTrayIcon::supportsMessages()) {
            trayIcon_->showMessage(QStringLiteral("DarkSpeak"),
                                   QStringLiteral("DarkSpeak is still running in the taskbar."),
                                   QSystemTrayIcon::Information, 3000);
        }
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void TrayController::toggleWindowVisibility()
{
    if (window_ == nullptr) {
        return;
    }
    if (window_->isVisible()) {
        hideWindow();
    } else {
        showWindow();
    }
}

void TrayController::showWindow()
{
    if (window_ == nullptr) {
        return;
    }
    window_->show();
    window_->raise();
    window_->requestActivate();
}

void TrayController::hideWindow()
{
    if (window_ != nullptr) {
        window_->hide();
    }
}

void TrayController::requestQuit()
{
    allowWindowClose_ = true;
    QCoreApplication::quit();
}
