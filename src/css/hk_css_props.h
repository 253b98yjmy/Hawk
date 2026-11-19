/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CSS properties Hawk understands. Unknown properties are dropped at parse time. */
#ifndef HK_CSS_PROPS_H
#define HK_CSS_PROPS_H

#include "base/hk_str.h"

#define HK_CSS_PROPERTIES(X)                                                                       \
    X(DISPLAY, "display")                                                                          \
    X(POSITION, "position")                                                                        \
    X(FLOAT, "float")                                                                              \
    X(VISIBILITY, "visibility")                                                                    \
    X(BOX_SIZING, "box-sizing")                                                                    \
    X(MARGIN, "margin")                                                                            \
    X(MARGIN_TOP, "margin-top")                                                                    \
    X(MARGIN_RIGHT, "margin-right")                                                                \
    X(MARGIN_BOTTOM, "margin-bottom")                                                              \
    X(MARGIN_LEFT, "margin-left")                                                                  \
    X(MARGIN_BLOCK, "margin-block")                                                                \
    X(MARGIN_BLOCK_START, "margin-block-start")                                                    \
    X(MARGIN_BLOCK_END, "margin-block-end")                                                        \
    X(MARGIN_INLINE, "margin-inline")                                                              \
    X(MARGIN_INLINE_START, "margin-inline-start")                                                  \
    X(MARGIN_INLINE_END, "margin-inline-end")                                                      \
    X(PADDING, "padding")                                                                          \
    X(PADDING_TOP, "padding-top")                                                                  \
    X(PADDING_RIGHT, "padding-right")                                                              \
    X(PADDING_BOTTOM, "padding-bottom")                                                            \
    X(PADDING_LEFT, "padding-left")                                                                \
    X(PADDING_BLOCK, "padding-block")                                                              \
    X(PADDING_INLINE, "padding-inline")                                                            \
    X(PADDING_INLINE_START, "padding-inline-start")                                                \
    X(PADDING_INLINE_END, "padding-inline-end")                                                    \
    X(BORDER, "border")                                                                            \
    X(BORDER_TOP, "border-top")                                                                    \
    X(BORDER_RIGHT, "border-right")                                                                \
    X(BORDER_BOTTOM, "border-bottom")                                                              \
    X(BORDER_LEFT, "border-left")                                                                  \
    X(BORDER_WIDTH, "border-width")                                                                \
    X(BORDER_STYLE, "border-style")                                                                \
    X(BORDER_COLOR, "border-color")                                                                \
    X(BORDER_TOP_WIDTH, "border-top-width")                                                        \
    X(BORDER_RIGHT_WIDTH, "border-right-width")                                                    \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width")                                                  \
    X(BORDER_LEFT_WIDTH, "border-left-width")                                                      \
    X(BORDER_TOP_STYLE, "border-top-style")                                                        \
    X(BORDER_RIGHT_STYLE, "border-right-style")                                                    \
    X(BORDER_BOTTOM_STYLE, "border-bottom-style")                                                  \
    X(BORDER_LEFT_STYLE, "border-left-style")                                                      \
    X(BORDER_TOP_COLOR, "border-top-color")                                                        \
    X(BORDER_RIGHT_COLOR, "border-right-color")                                                    \
    X(BORDER_BOTTOM_COLOR, "border-bottom-color")                                                  \
    X(BORDER_LEFT_COLOR, "border-left-color")                                                      \
    X(WIDTH, "width")                                                                              \
    X(HEIGHT, "height")                                                                            \
    X(MIN_WIDTH, "min-width")                                                                      \
    X(MAX_WIDTH, "max-width")                                                                      \
    X(MIN_HEIGHT, "min-height")                                                                    \
    X(MAX_HEIGHT, "max-height")                                                                    \
    X(INLINE_SIZE, "inline-size")                                                                  \
    X(MAX_INLINE_SIZE, "max-inline-size")                                                          \
    X(COLOR, "color")                                                                              \
    X(BACKGROUND, "background")                                                                    \
    X(BACKGROUND_COLOR, "background-color")                                                        \
    X(FONT, "font")                                                                                \
    X(FONT_FAMILY, "font-family")                                                                  \
    X(FONT_SIZE, "font-size")                                                                      \
    X(FONT_WEIGHT, "font-weight")                                                                  \
    X(FONT_STYLE, "font-style")                                                                    \
    X(LINE_HEIGHT, "line-height")                                                                  \
    X(TEXT_ALIGN, "text-align")                                                                    \
    X(TEXT_DECORATION, "text-decoration")                                                          \
    X(TEXT_DECORATION_LINE, "text-decoration-line")                                                \
    X(TEXT_INDENT, "text-indent")                                                                  \
    X(TEXT_TRANSFORM, "text-transform")                                                            \
    X(WHITE_SPACE, "white-space")                                                                  \
    X(LIST_STYLE, "list-style")                                                                    \
    X(LIST_STYLE_TYPE, "list-style-type")                                                          \
    X(OVERFLOW, "overflow")                                                                        \
    X(CLIP, "clip")                                                                                \
    X(CLIP_PATH, "clip-path")                                                                      \
    X(OPACITY, "opacity")                                                                          \
    X(VERTICAL_ALIGN, "vertical-align")                                                            \
    X(BORDER_RADIUS, "border-radius")                                                              \
    X(BORDER_TOP_LEFT_RADIUS, "border-top-left-radius")                                            \
    X(BORDER_TOP_RIGHT_RADIUS, "border-top-right-radius")                                          \
    X(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius")                                    \
    X(BORDER_BOTTOM_LEFT_RADIUS, "border-bottom-left-radius")                                      \
    X(BORDER_START_START_RADIUS, "border-start-start-radius")                                      \
    X(BORDER_START_END_RADIUS, "border-start-end-radius")                                          \
    X(BORDER_END_END_RADIUS, "border-end-end-radius")                                              \
    X(BORDER_END_START_RADIUS, "border-end-start-radius")                                          \
    X(CONTENT, "content")                                                                          \
    X(TOP, "top")                                                                                  \
    X(RIGHT, "right")                                                                              \
    X(BOTTOM, "bottom")                                                                            \
    X(LEFT, "left")                                                                                \
    X(INSET, "inset")                                                                              \
    X(CLEAR, "clear")                                                                              \
    X(Z_INDEX, "z-index")                                                                          \
    X(BORDER_COLLAPSE, "border-collapse")                                                          \
    X(BORDER_SPACING, "border-spacing")                                                            \
    X(OVERFLOW_X, "overflow-x")                                                                    \
    X(OVERFLOW_Y, "overflow-y")

typedef enum hk_css_property {
    HK_PROP_NONE_ = 0,
#define HK_PROP_ENUM(id, name) HK_PROP_##id,
    HK_CSS_PROPERTIES(HK_PROP_ENUM)
#undef HK_PROP_ENUM
    HK_PROP_CUSTOM,
    HK_PROP_COUNT_
} hk_css_property;

/* ASCII case-insensitive lookup; returns HK_PROP_NONE_ for unknown names. */
hk_css_property hk_css_property_from_name(hk_str name);
const char *hk_css_property_name(hk_css_property p);

#endif
