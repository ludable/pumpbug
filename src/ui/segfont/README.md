# segfont

Header-only seven-segment text for embedded C++. Glyphs are synthesized from
integer geometry and emitted as filled rectangles; there are no bitmap assets,
heap allocations, or display dependencies in the core.

## Model

`segfont` separates shape from layout:

1. `Style`: integer geometry for one synthesized font size.
2. `GlyphMetrics`: per-glyph ink bounds and advance.
3. `TextOptions`: string layout policy such as tabular/proportional spacing and
   between-glyph tracking.

The core sink contract is the whole rendering interface:

```cpp
struct Sink {
  void fill(int x, int y, int w, int h);
};
```

Color, clipping, buffering, and bus transactions live outside the core.

## Sizing

Low-level sizing is still available when you know the exact stroke:

```cpp
auto exact = segfont::style_for_height(48, /*stroke=*/5);
```

For most UI code, use semantic weight. The weight chooses a natural stroke from
the requested height:

```cpp
auto thin = segfont::style_for_height(32, segfont::Weight::Thin);     // ~2 px
auto regular = segfont::style_for_height(32, segfont::Weight::Regular); // ~3 px
auto bold = segfont::style_for_height(32, segfont::Weight::Bold);     // ~4 px
```

The geometry is:

```text
digit width  = core_x + 2 * stroke
digit height = 2 * core_y + 3 * stroke
baseline     = bottom
```

`style_for_height()` creates a natural symmetric style where `core_x == core_y`.
Use `style_for_size(width, height, stroke)` when the requested ink box is
intentionally anisotropic; the stroke remains an integer segment thickness while
`core_x` and `core_y` stretch independently.

`style_for_cell_height()` reserves roughly one stroke of vertical breathing room
outside the ink box before deriving the style.

## Text Layout

Default text is tabular, which is what live readouts usually need:

```cpp
segfont::draw_text(style, "12:34", x, y, sink);
int w = segfont::text_width(style, "12:34");
```

For display strings where proportional digits are desirable, opt in through
`TextOptions`:

```cpp
segfont::TextOptions opts;
opts.spacing = segfont::SpacingMode::Proportional;
opts.tracking = 2;  // applied only between characters

segfont::draw_text(style, "CAFE", x, y, sink, opts);
```

In proportional mode, segment glyphs are advanced by their ink bounds and a
space advances by about half of a digit cell. Tabular mode keeps space at the
full digit advance.

Tracking is intentionally string-level. It is added between glyphs, not after
the final glyph.

## LovyanGFX / M5GFX String Renderer

Use this path when exact string-level tracking matters:

```cpp
#include <M5GFX.h>
#include "segfont_lgfx.h"

auto style = segfont::style_for_height(48, segfont::Weight::Regular);
segfont::drawString(gfx, style, "12:34", x, y, TFT_WHITE);
```

The adapter is templated on the display type and works with M5GFX,
M5Unified-backed displays, and LovyanGFX objects that provide `startWrite()`,
`writeFillRect()`, and `endWrite()`.

## LGFX IFont Adapter

Use this path when you want LGFX-native text APIs:

```cpp
#include <M5GFX.h>
#include "segfont_ifont.h"

static segfont::IFontAdapter segFont(
    segfont::style_for_height(48, segfont::Weight::Regular));

gfx.setFont(&segFont);
gfx.setTextDatum(middle_center);
gfx.drawString("12.3", x, y);
```

The IFont adapter is glyph-oriented, like other LGFX fonts:

- `x_advance` includes the glyph advance plus segfont tracking as a normal
  right side bearing.
- `textWidth()` includes the final glyph advance/bearing, as normal fonts do.
- `setTextSize()` resynthesizes a nearby integer `Style`, preserving consistent
  segment thickness instead of bitmap-scaling rectangles.
- non-uniform `setTextSize(x, y)` derives independent `core_x` and `core_y`
  values, so width and height changes do not stretch individual rectangles.

If you need tracking only between glyphs, use `draw_text()` or
`segfont::drawString()` instead of IFont.

## Character Set

Supported glyphs: `0-9`, `A-F`/`a-f`, `H L P U` plus lowercase, `- _ =`,
space, `:` and `.`. Unknown characters render as blank.

## Files

```text
segfont.h        core renderer, sizing, glyph metrics, text layout
segfont_lgfx.h   optional LovyanGFX/M5GFX string renderer
segfont_ifont.h  optional lgfx::v1::IFont adapter
tools/           host-side utilities
README.md        this file
```

## Contact Sheet

Generate a visual specimen sheet with the compiled C++ renderer:

```sh
python3 tools/make_contact_sheet.py
```

The Python wrapper only compiles and runs `tools/segfont_contact_sheet.cpp.in`.
Rendering is done by C++ including `segfont.h`, and the default output is
`segfont_contact_sheet.svg`.
