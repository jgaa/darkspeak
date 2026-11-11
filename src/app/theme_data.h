#pragma once

#include <array>
#include <cstdint>
#include <QtCore/qttranslation.h>

// Compiled presentation data only. IDs are stable settings values, not paths.
namespace ds::ui {
struct ThemeColors {
    std::uint32_t window, sidebar, surface, raised, border;
    std::uint32_t text, secondary, accent, selected_text, incoming, outgoing;
};

struct ThemeData {
    const char *id;
    const char *name;
    const char *description;
    bool dark_only;
    int corner_radius;
    int bubble_radius;
    ThemeColors light;
    ThemeColors dark;
};

inline constexpr std::array themes{
    ThemeData{"Native", QT_TRANSLATE_NOOP("Appearance", "Native"),
              QT_TRANSLATE_NOOP("Appearance", "Your desktop palette and system font."), false, 4, 8, {}, {}},
    ThemeData{"Coffee", QT_TRANSLATE_NOOP("Appearance", "Coffee"),
              QT_TRANSLATE_NOOP("Appearance", "Cream, espresso, and muted gold."), false, 6, 10,
              {0xf3ede3, 0xe8dfd0, 0xfffaf2, 0xeee4d5, 0xa08d74,
               0x35291f, 0x6c5947, 0x805923, 0xffffff, 0xeee4d5, 0xe8dbc3},
              {0x241e19, 0x1d1814, 0x2c251e, 0x382e24, 0x826e55,
               0xf4ead9, 0xc7b79e, 0xd5b16c, 0x241e19, 0x382e24, 0x453923}},
    ThemeData{"Late Night", QT_TRANSLATE_NOOP("Appearance", "Late Night"),
              QT_TRANSLATE_NOOP("Appearance", "Deep navy and charcoal with cool blue accents."), true, 4, 8, {},
              {0x171d29, 0x111722, 0x1d2533, 0x283345, 0x64748b,
               0xe9eef8, 0xb2bfd3, 0x9dbcfb, 0x141b28, 0x283345, 0x2b3c57}},
    ThemeData{"Rosewood", QT_TRANSLATE_NOOP("Appearance", "Rosewood"),
              QT_TRANSLATE_NOOP("Appearance", "Rose, burgundy, and muted mauve."), false, 6, 10,
              {0xf5eeee, 0xeadde1, 0xfff9fa, 0xf0e2e7, 0xa88794,
               0x3e2832, 0x725762, 0x91435f, 0xffffff, 0xf0e2e7, 0xe9d9e2},
              {0x291e25, 0x21181e, 0x32252e, 0x402e39, 0x946b80,
               0xf6e8ee, 0xd0b4c2, 0xe5a2bd, 0x291e25, 0x402e39, 0x503244}},
    ThemeData{"Windows", QT_TRANSLATE_NOOP("Appearance", "Windows"),
              QT_TRANSLATE_NOOP("Appearance", "Familiar neutral surfaces and blue accents."), false, 2, 4,
              {0xf3f3f3, 0xe8e8e8, 0xffffff, 0xededed, 0x8a8a8a,
               0x202020, 0x595959, 0x0067b8, 0xffffff, 0xededed, 0xddebf8},
              {0x202020, 0x191919, 0x272727, 0x333333, 0x777777,
               0xf3f3f3, 0xbfbfbf, 0x78bfff, 0x102238, 0x333333, 0x253b50}},
    ThemeData{"Matrix", QT_TRANSLATE_NOOP("Appearance", "Matrix"),
              QT_TRANSLATE_NOOP("Appearance", "Near-black surfaces, restrained green, and square corners."), true, 0, 0, {},
              {0x0d1410, 0x080e0a, 0x121c15, 0x1c2a20, 0x52755b,
               0xd7e9dc, 0xabc4b2, 0x8aca9c, 0x0d1410, 0x1c2a20, 0x213a29}},
    ThemeData{"Inferno", QT_TRANSLATE_NOOP("Appearance", "Inferno"),
              QT_TRANSLATE_NOOP("Appearance", "Burnt orange, warm red, and charcoal."), false, 3, 6,
              {0xf9efe7, 0xf0dfd1, 0xfffaf5, 0xf4e5d8, 0xb08468,
               0x3c261d, 0x785240, 0xa34418, 0xffffff, 0xf4e5d8, 0xf3d9c4},
              {0x261e1b, 0x1d1715, 0x302420, 0x3d2d25, 0x9b745c,
               0xfaeade, 0xd5b7a3, 0xf3a36b, 0x291b13, 0x3d2d25, 0x4b3022}}
};
} // namespace ds::ui
