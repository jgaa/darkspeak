#pragma once

#include <QObject>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QSettings>
#include <QWindow>
#include <QUrl>
#include <QVariantMap>

// Presentation preferences only. Native desktop palettes are data, never theme code.
class Appearance final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int scale READ scale WRITE setScale NOTIFY changed)
    Q_PROPERTY(qreal factor READ factor NOTIFY changed)
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY changed)
    Q_PROPERTY(int themeIndex READ themeIndex WRITE setThemeIndex NOTIFY changed)
    Q_PROPERTY(QStringList themeNames READ themeNames NOTIFY changed)
    Q_PROPERTY(QString themeDescription READ themeDescription NOTIFY changed)
    Q_PROPERTY(bool darkOnly READ darkOnly NOTIFY changed)
    Q_PROPERTY(QPalette palette READ palette NOTIFY changed)
    Q_PROPERTY(QVariantMap paletteColors READ paletteColors NOTIFY changed)
    Q_PROPERTY(QFont font READ font NOTIFY changed)
    Q_PROPERTY(qreal fontPixels READ fontPixels NOTIFY changed)
    Q_PROPERTY(QFont captionFont READ captionFont NOTIFY changed)
    Q_PROPERTY(QFont headingFont READ headingFont NOTIFY changed)
    Q_PROPERTY(qreal smallSpacing READ smallSpacing NOTIFY changed)
    Q_PROPERTY(qreal spacing READ spacing NOTIFY changed)
    Q_PROPERTY(qreal largeSpacing READ largeSpacing NOTIFY changed)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius NOTIFY changed)
    Q_PROPERTY(qreal bubbleRadius READ bubbleRadius NOTIFY changed)
    Q_PROPERTY(qreal avatarSize READ avatarSize NOTIFY changed)
    Q_PROPERTY(qreal iconSize READ iconSize NOTIFY changed)
    Q_PROPERTY(qreal toolbarHeight READ toolbarHeight NOTIFY changed)
    Q_PROPERTY(qreal headerHeight READ headerHeight NOTIFY changed)
    Q_PROPERTY(qreal navigationHeight READ navigationHeight NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(bool variantUnavailable READ variantUnavailable NOTIFY changed)
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor sidebar READ sidebar NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor raised READ raised NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor secondaryText READ secondaryText NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor selected READ selected NOTIFY changed)
    Q_PROPERTY(QColor selectedText READ selectedText NOTIFY changed)
    Q_PROPERTY(QColor hover READ hover NOTIFY changed)
    Q_PROPERTY(QColor disabled READ disabled NOTIFY changed)
    Q_PROPERTY(QColor incoming READ incoming NOTIFY changed)
    Q_PROPERTY(QColor outgoing READ outgoing NOTIFY changed)
    Q_PROPERTY(QColor success READ success NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    Q_PROPERTY(QColor error READ error NOTIFY changed)
    Q_PROPERTY(bool sidebarCollapsed READ sidebarCollapsed WRITE setSidebarCollapsed NOTIFY layoutChanged)
    Q_PROPERTY(qreal conversationWidth READ conversationWidth WRITE setConversationWidth NOTIFY layoutChanged)
public:
    explicit Appearance(QObject *parent = nullptr);
    int scale() const { return scale_; }
    qreal factor() const { return scale_ / 100.0; }
    int mode() const { return mode_; }
    QString theme() const;
    int themeIndex() const { return theme_index_; }
    QStringList themeNames() const;
    QString themeDescription() const;
    bool darkOnly() const;
    QPalette palette() const;
    QVariantMap paletteColors() const { return palette_colors_; }
    QFont font() const;
    qreal fontPixels() const;
    QFont captionFont() const;
    QFont headingFont() const;
    qreal smallSpacing() const { return 4 * factor(); }
    qreal spacing() const { return 8 * factor(); }
    qreal largeSpacing() const { return 16 * factor(); }
    qreal cornerRadius() const;
    qreal bubbleRadius() const;
    qreal avatarSize() const { return 40 * factor(); }
    qreal iconSize() const { return 24 * factor(); }
    qreal toolbarHeight() const { return 42 * factor(); }
    qreal headerHeight() const { return 56 * factor(); }
    qreal navigationHeight() const { return 44 * factor(); }
    bool dark() const;
    bool variantUnavailable() const;
    QColor window() const { return palette().window().color(); }
    QColor sidebar() const { return palette().alternateBase().color(); }
    QColor surface() const { return palette().base().color(); }
    QColor raised() const { return palette().button().color(); }
    QColor border() const { return palette().mid().color(); }
    QColor text() const { return palette().text().color(); }
    QColor secondaryText() const { return palette().placeholderText().color(); }
    QColor accent() const { return palette().highlight().color(); }
    QColor selected() const { return palette().highlight().color(); }
    QColor selectedText() const { return palette().highlightedText().color(); }
    QColor hover() const;
    QColor disabled() const { return palette().color(QPalette::Disabled, QPalette::Text); }
    QColor incoming() const;
    QColor outgoing() const;
    QColor success() const { return QColor(dark() ? "#8cd6a5" : "#21663b"); }
    QColor warning() const { return QColor(dark() ? "#efc574" : "#805500"); }
    QColor error() const { return QColor(dark() ? "#ffaaa5" : "#a52424"); }
    bool sidebarCollapsed() const { return sidebar_collapsed_; }
    qreal conversationWidth() const { return conversation_width_; }
    void setScale(int value);
    void setMode(int value);
    void setTheme(const QString &value);
    void setThemeIndex(int value);
    void setSidebarCollapsed(bool value);
    void setConversationWidth(qreal value);
    Q_INVOKABLE QUrl asset(const QString &name) const;
    Q_INVOKABLE void zoom(int steps);
    Q_INVOKABLE void beginPreview();
    Q_INVOKABLE void savePreview();
    Q_INVOKABLE void cancelPreview();
    Q_INVOKABLE void restoreWindow(QWindow *window);
    Q_INVOKABLE void saveWindow(QWindow *window);
    Q_INVOKABLE void resizeWindow(QWindow *window, int width, int height);
signals:
    void changed();
    void layoutChanged();
private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void persist();
    void applyMode();
    void refreshPalette();
    QSettings settings_;
    int scale_ = 100;
    int wheel_remainder_ = 0;
    int mode_ = 0;
    int theme_index_ = 0;
    QPalette theme_palette_;
    QVariantMap palette_colors_;
    QColor incoming_;
    QColor outgoing_;
    bool preview_ = false;
    int saved_scale_ = 100;
    int saved_mode_ = 0;
    int saved_theme_index_ = 0;
    bool sidebar_collapsed_ = false;
    qreal conversation_width_ = 280;
};
