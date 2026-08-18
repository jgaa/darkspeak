#pragma once

#include <QObject>
#include <QIcon>

class QAction;
class QMenu;
class QSystemTrayIcon;
class QWindow;
namespace ds::models { class NotificationsModel; }

class TrayController final : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QWindow *window,
                            ds::models::NotificationsModel *notifications,
                            bool startHidden = false,
                            QObject *parent = nullptr);
    ~TrayController() override;

    [[nodiscard]] bool available() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void toggleWindowVisibility();
    void showWindow();
    void hideWindow();
    void requestQuit();
    void updateNotificationIcon();

    QWindow *window_{};
    QSystemTrayIcon *trayIcon_{};
    QMenu *menu_{};
    QAction *toggleVisibilityAction_{};
    QAction *quitAction_{};
    QIcon icon_{};
    ds::models::NotificationsModel *notifications_{};
    bool allowWindowClose_{};
};
