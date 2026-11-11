#include "appearance.h"
#include "theme_data.h"
#include "logfault/logfault.h"

#include <QApplication>
#include <QEvent>
#include <QWheelEvent>
#include <QFontInfo>
#include <QHash>
#include <QScreen>
#include <QStyleHints>
#include <QMetaEnum>
#include <algorithm>
#include <cmath>
#ifdef DARKSPEAK_KDE_COLOR_SCHEME
#include <KColorSchemeManager>
#include <QModelIndex>
#endif

Appearance::Appearance(QObject *parent) : QObject(parent)
{
    scale_ = std::clamp(settings_.value("appearance/scale", 100).toInt(), 20, 200);
    mode_ = std::clamp(settings_.value("appearance/mode", 0).toInt(), 0, 2);
    const auto saved_theme = settings_.value("appearance/theme", QStringLiteral("Native")).toString();
    for (std::size_t i = 0; i < ds::ui::themes.size(); ++i) {
        if (saved_theme == QLatin1String(ds::ui::themes[i].id)) {
            theme_index_ = static_cast<int>(i);
            break;
        }
    }
    sidebar_collapsed_ = settings_.value("layout/sidebarCollapsed", false).toBool();
    const auto width = settings_.value("layout/conversationWidth", 280).toDouble();
    if (std::isfinite(width)) conversation_width_ = std::clamp(width, 160.0, 16000.0);
    qApp->installEventFilter(this);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this] { refreshPalette(); emit changed(); });
    applyMode();
    refreshPalette();
}

void Appearance::applyMode()
{
    // Custom themes follow the unmodified system palette in Automatic mode.
    const auto native_mode = theme_index_ == 0 ? mode_ : 0;
    const auto requested = native_mode == 1 ? Qt::ColorScheme::Light
                         : native_mode == 2 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown;
#ifdef DARKSPEAK_KDE_COLOR_SCHEME
    auto *manager = KColorSchemeManager::instance();
    // QSettings owns persistence, including unsaved Settings previews.
    manager->setAutosaveChanges(false);
    const auto scheme_id = native_mode == 1 ? QStringLiteral("BreezeLight")
                         : native_mode == 2 ? QStringLiteral("BreezeDark") : QString();
    const auto index = manager->indexForSchemeId(scheme_id);
    if (index.isValid()) {
        QGuiApplication::styleHints()->setColorScheme(requested);
        manager->activateScheme(index);
        LFLOG_DEBUG << "Native KDE color scheme: "
                    << (scheme_id.isEmpty() ? QStringLiteral("system") : scheme_id);
        return;
    }
    // Clear any previous explicit KDE palette before trying the platform hint.
    manager->activateScheme(QModelIndex());
    LFLOG_DEBUG << "Native KDE color scheme unavailable: " << scheme_id
                << "; using Qt color-scheme hint";
#endif
    QGuiApplication::styleHints()->setColorScheme(requested);
}

QString Appearance::theme() const { return QString::fromLatin1(ds::ui::themes[theme_index_].id); }
QStringList Appearance::themeNames() const
{
    QStringList names;
    for (const auto &entry : ds::ui::themes) names.append(tr(entry.name));
    return names;
}
QString Appearance::themeDescription() const { return tr(ds::ui::themes[theme_index_].description); }
bool Appearance::darkOnly() const { return ds::ui::themes[theme_index_].dark_only; }
qreal Appearance::cornerRadius() const { return ds::ui::themes[theme_index_].corner_radius * factor(); }
qreal Appearance::bubbleRadius() const { return ds::ui::themes[theme_index_].bubble_radius * factor(); }
QColor Appearance::incoming() const { return theme_index_ == 0 ? raised() : incoming_; }
QColor Appearance::outgoing() const { return theme_index_ == 0 ? hover() : outgoing_; }
QPalette Appearance::palette() const { return theme_index_ == 0 ? QApplication::palette() : theme_palette_; }

void Appearance::refreshPalette()
{
    if (theme_index_ != 0) {
        const auto &entry = ds::ui::themes[theme_index_];
        const auto system_palette = QApplication::palette();
        const bool system_dark = system_palette.window().color().lightnessF() < system_palette.text().color().lightnessF();
        const bool use_dark = entry.dark_only || mode_ == 2 || (mode_ == 0 && system_dark);
        const auto &colors = use_dark ? entry.dark : entry.light;
        const auto color = [](std::uint32_t rgb) { return QColor::fromRgb(rgb); };
        const auto text_color = color(colors.text);
        const auto button_color = color(colors.raised);
        // Set every Fusion role rather than inheriting shades from a native variant.
        auto result = QPalette(button_color);
        result.setColor(QPalette::Window, color(colors.window));
        result.setColor(QPalette::WindowText, text_color);
        result.setColor(QPalette::Base, color(colors.surface));
        result.setColor(QPalette::AlternateBase, color(colors.sidebar));
        result.setColor(QPalette::Text, text_color);
        result.setColor(QPalette::Button, button_color);
        result.setColor(QPalette::ButtonText, text_color);
        result.setColor(QPalette::BrightText, color(colors.selected_text));
        result.setColor(QPalette::Highlight, color(colors.accent));
        result.setColor(QPalette::HighlightedText, color(colors.selected_text));
        result.setColor(QPalette::Accent, color(colors.accent));
        result.setColor(QPalette::Link, color(colors.accent));
        result.setColor(QPalette::LinkVisited, color(colors.accent));
        result.setColor(QPalette::PlaceholderText, color(colors.secondary));
        result.setColor(QPalette::ToolTipBase, button_color);
        result.setColor(QPalette::ToolTipText, text_color);
        result.setColor(QPalette::Light, button_color.lighter(120));
        result.setColor(QPalette::Midlight, button_color.lighter(110));
        result.setColor(QPalette::Mid, color(colors.border));
        result.setColor(QPalette::Dark, button_color.darker(130));
        result.setColor(QPalette::Shadow, color(colors.border).darker(150));
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                                QPalette::PlaceholderText, QPalette::HighlightedText})
            result.setColor(QPalette::Disabled, role, color(colors.secondary));
        result.setColor(QPalette::Disabled, QPalette::Highlight, button_color);
        theme_palette_ = result;
        incoming_ = color(colors.incoming);
        outgoing_ = color(colors.outgoing);
    }
    // Public Qt Quick Palette uses ColorGroup objects, not a C++ QPalette.
    // Cache a small, read-only role map for the declarative adapter.
    const auto current = palette();
    const auto roles = QMetaEnum::fromType<QPalette::ColorRole>();
    const std::array groups{QPalette::Active, QPalette::Inactive, QPalette::Disabled};
    const std::array group_names{"active", "inactive", "disabled"};
    QVariantMap exported;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        QVariantMap colors;
        for (int role = 0; role < QPalette::NColorRoles; ++role) {
            if (role == QPalette::NoRole) continue;
            auto name = QString::fromLatin1(roles.valueToKey(role));
            name[0] = name[0].toLower();
            colors.insert(name, current.color(groups[i], static_cast<QPalette::ColorRole>(role)));
        }
        exported.insert(QString::fromLatin1(group_names[i]), colors);
    }
    palette_colors_ = exported;
}
QFont Appearance::font() const
{
    auto result = QApplication::font();
    if (result.pointSizeF() > 0) result.setPointSizeF(result.pointSizeF() * factor());
    else result.setPixelSize(std::max(1, qRound(result.pixelSize() * factor())));
    return result;
}
bool Appearance::dark() const { return window().lightnessF() < text().lightnessF(); }
bool Appearance::variantUnavailable() const { return (mode_ == 1 && dark()) || (mode_ == 2 && !dark()); }
QColor Appearance::hover() const
{
    auto base = surface();
    const auto highlight = accent();
    base.setRgbF(base.redF() * .88 + highlight.redF() * .12,
                 base.greenF() * .88 + highlight.greenF() * .12,
                 base.blueF() * .88 + highlight.blueF() * .12);
    return base;
}
void Appearance::persist()
{
    if (preview_) return;
    settings_.setValue("appearance/scale", scale_);
    settings_.setValue("appearance/mode", mode_);
    settings_.setValue("appearance/theme", theme());
}
void Appearance::setScale(int value)
{
    value = std::clamp(value, 20, 200);
    if (scale_ == value) return;
    scale_ = value;
    LFLOG_DEBUG << "UI scale changed to " << scale_ << "%";
    persist();
    emit changed();
}
void Appearance::setMode(int value)
{
    value = std::clamp(value, 0, 2);
    if (mode_ == value) return;
    mode_ = value;
    LFLOG_DEBUG << "Appearance mode changed to " << mode_ << ", theme=" << theme();
    if (theme_index_ == 0) applyMode();
    refreshPalette();
    persist();
    emit changed();
}
void Appearance::setTheme(const QString &value)
{
    for (std::size_t i = 0; i < ds::ui::themes.size(); ++i) {
        if (value == QLatin1String(ds::ui::themes[i].id)) {
            setThemeIndex(static_cast<int>(i));
            return;
        }
    }
    LFLOG_DEBUG << "Ignoring unknown UI theme";
}
void Appearance::setThemeIndex(int value)
{
    if (value < 0 || value >= static_cast<int>(ds::ui::themes.size()) || value == theme_index_) return;
    const bool was_native = theme_index_ == 0;
    theme_index_ = value;
    LFLOG_DEBUG << "UI theme changed to " << theme();
    if (was_native || theme_index_ == 0) applyMode();
    refreshPalette();
    persist();
    emit changed();
}
void Appearance::zoom(int steps) { setScale(scale_ + std::clamp(steps, -18, 18) * 10); }
void Appearance::beginPreview()
{
    if (preview_) return;
    saved_scale_ = scale_;
    saved_mode_ = mode_;
    saved_theme_index_ = theme_index_;
    preview_ = true;
}
void Appearance::savePreview() { preview_ = false; persist(); }
void Appearance::cancelPreview()
{
    if (!preview_) return;
    setScale(saved_scale_);
    setMode(saved_mode_);
    setThemeIndex(saved_theme_index_);
    preview_ = false;
}
void Appearance::setSidebarCollapsed(bool value)
{
    if (sidebar_collapsed_ == value) return;
    sidebar_collapsed_ = value;
    settings_.setValue("layout/sidebarCollapsed", value);
    emit layoutChanged();
}
void Appearance::setConversationWidth(qreal value)
{
    if (!std::isfinite(value)) return;
    value = std::clamp(value, 160.0, 16000.0);
    if (qFuzzyCompare(conversation_width_, value)) return;
    conversation_width_ = value;
    settings_.setValue("layout/conversationWidth", value);
    emit layoutChanged();
}
void Appearance::resizeWindow(QWindow *window, int width, int height)
{
    if (!window || !window->screen()) return;
    const auto area = window->screen()->availableGeometry();
    if (window->visibility() == QWindow::Maximized || window->visibility() == QWindow::FullScreen) window->showNormal();
    const auto margins = window->frameMargins();
    window->resize(std::clamp(width, 1, std::max(1, area.width() - margins.left() - margins.right())),
                   std::clamp(height, 1, std::max(1, area.height() - margins.top() - margins.bottom())));
    const auto minimum_x = area.left() + margins.left();
    const auto minimum_y = area.top() + margins.top();
    const auto maximum_x = std::max(minimum_x, area.right() - window->width() - margins.right() + 1);
    const auto maximum_y = std::max(minimum_y, area.bottom() - window->height() - margins.bottom() + 1);
    window->setPosition(std::clamp(window->x(), minimum_x, maximum_x),
                        std::clamp(window->y(), minimum_y, maximum_y));
}
void Appearance::restoreWindow(QWindow *window)
{
    if (!window) return;
    const auto geometry = settings_.value("layout/windowGeometry").toRect();
    if (geometry.isValid()) {
        for (auto *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(geometry)) {
                window->setScreen(screen);
                break;
            }
        }
        window->setPosition(geometry.topLeft());
    }
    resizeWindow(window, geometry.isValid() ? geometry.width() : 1366,
                 geometry.isValid() ? geometry.height() : 768);
    if (settings_.value("layout/windowMaximized", false).toBool()) window->showMaximized();
}
void Appearance::saveWindow(QWindow *window)
{
    if (!window || window->visibility() == QWindow::Minimized) return;
    settings_.setValue("layout/windowMaximized", window->visibility() == QWindow::Maximized);
    if (window->visibility() != QWindow::Maximized && window->visibility() != QWindow::FullScreen)
        settings_.setValue("layout/windowGeometry", window->geometry());
}

bool Appearance::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel && qobject_cast<QWindow *>(watched)) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            const auto delta = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y() : wheel->pixelDelta().y() * 3;
            wheel_remainder_ = std::clamp(wheel_remainder_ + std::clamp(delta, -2160, 2160), -2160, 2160);
            const auto steps = wheel_remainder_ / 120;
            if (steps != 0) {
                zoom(steps);
                wheel_remainder_ -= steps * 120;
            }
            wheel->accept();
            return true;
        }
        wheel_remainder_ = 0;
    }
    if (watched == qApp && (event->type() == QEvent::ApplicationPaletteChange
                           || event->type() == QEvent::ApplicationFontChange)) {
        refreshPalette();
        emit changed();
    }
    return QObject::eventFilter(watched, event);
}

qreal Appearance::fontPixels() const { return QFontInfo(font()).pixelSize(); }

QUrl Appearance::asset(const QString &name) const
{
    // Semantic, compiled allowlist. No path concatenation or filesystem/network
    // discovery; unknown names always resolve to a bundled placeholder.
    static const QHash<QString, QUrl> assets{
        {QStringLiteral("avatar"), QUrl(QStringLiteral("qrc:/images/anonymous.svg"))},
        {QStringLiteral("blocked"), QUrl(QStringLiteral("qrc:/images/blocked.svg"))},
        {QStringLiteral("sendFile"), QUrl(QStringLiteral("qrc:/images/FileUpload.svg"))},
        {QStringLiteral("download"), QUrl(QStringLiteral("qrc:/images/FileDownload.svg"))},
        {QStringLiteral("onion"), QUrl(QStringLiteral("qrc:/images/onion-bw.svg"))},
        {QStringLiteral("qr"), QUrl(QStringLiteral("qrc:/images/qr-code.svg"))}
    };
    return assets.value(name, assets.value(QStringLiteral("avatar")));
}

QFont Appearance::captionFont() const
{
    auto result = font();
    result.setPixelSize(std::max(1, qRound(fontPixels() * .9)));
    return result;
}
QFont Appearance::headingFont() const
{
    auto result = font();
    result.setPixelSize(std::max(1, qRound(fontPixels() * 1.5)));
    result.setWeight(QFont::DemiBold);
    return result;
}
