// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure geometry of the content area and the four-slot button-label panel.
#pragma once
#include <cstdint>

namespace k2 {

constexpr int16_t kScreenW = 240;
constexpr int16_t kScreenH = 240;
constexpr int16_t kOppRailThickness = 16;
constexpr int16_t kTwinContentW = kScreenW - 2 * kOppRailThickness;   // 208 px (Portrait twin-rail body)
constexpr int16_t kTwinContentH = kScreenH;                           // 240 px (full vertical span, no TopBar/Footer)

enum class Button : uint8_t { Back, Minus, Ok, Plus };
enum class PanelEdge : uint8_t { Right, Left, Top, Bottom };

constexpr PanelEdge opposite_edge(PanelEdge edge) {
    switch (edge) {
    case PanelEdge::Right:  return PanelEdge::Left;
    case PanelEdge::Left:   return PanelEdge::Right;
    case PanelEdge::Top:    return PanelEdge::Bottom;
    case PanelEdge::Bottom: return PanelEdge::Top;
    }
    return PanelEdge::Left;
}

struct Rect { int16_t x, y, w, h; };

// Which edge carries the panel and which ST7789 MADCTL rotation (0..3) the screen runs in.
struct Orientation { PanelEdge edge; uint8_t rotation; };
// The button-label strip is always attached to the physical button edge (the RIGHT edge of the
// board, verified on hardware at MADCTL 0x00), so the boot default must pair rotation 0 with the
// Right edge — exactly orientation_for(0). The bottom-strip 240x220 canvas used by the host
// mockups is rotation 3; the mockup harness selects it explicitly.
constexpr Orientation kDefaultOrientation{PanelEdge::Right, 0};
constexpr Orientation orientation_for(uint8_t rotation) {
    constexpr PanelEdge edges[4] = {PanelEdge::Right, PanelEdge::Top, PanelEdge::Left, PanelEdge::Bottom};
    return {edges[rotation & 3], static_cast<uint8_t>(rotation & 3)};
}

// Where the physical upright top male plug lies in the rotated screen's coordinates.
enum class PortDirection : uint8_t { Up, Left, Down, Right };
constexpr PortDirection top_port_direction(uint8_t rotation) {
    return static_cast<PortDirection>(rotation & 3);
}

// MADCTL (command 0x36) value per rotation. Only rotation 0 is verified on hardware.
constexpr uint8_t kMadctl[4] = {0x00, 0x60, 0xC0, 0xA0};

struct Layout {
    Rect panel;
    Rect content;       // single-strip content (224x240 Portrait / 240x224 Landscape): Home & full-panel pages
    Rect opp_rail;      // 16 px opposite telemetry rail (opposite_edge(edge))
    Rect twin_content;  // twin-rail content (208x240 Portrait / 240x208 Landscape): all standard pages != Home
    Rect slot[4];       // in on-screen order along the panel (top to bottom, or left to right)
};

// centers: slot centre along the panel axis (pixels); slot_len: slot size along the axis;
// thickness: panel size across the axis. Slots are clipped to the screen.
Layout compute_layout(PanelEdge edge, const int16_t (&centers)[4], int16_t slot_len, int16_t thickness);
// Same with one length per slot.
Layout compute_layout(PanelEdge edge, const int16_t (&centers)[4], const int16_t (&slot_lens)[4], int16_t thickness);

// Reading direction of text drawn along a vertical panel (a 90 degree rotation of the glyphs).
//   Ccw: reads bottom-to-top, letter tops face left. Right-edge panel: turn the device a quarter
//        clockwise (right edge to the bottom) and the labels read left-to-right.
//   Cw:  reads top-to-bottom, letter tops face right. Left-edge panel, device turned counter-
//        clockwise.
enum class TextDir : uint8_t { Ccw, Cw };

// Direction for the labels of a panel on `edge`; vertical edges only (a row panel keeps
// horizontal text, returned as Ccw but not used).
constexpr TextDir panel_text_dir(PanelEdge edge) { return edge == PanelEdge::Left ? TextDir::Cw : TextDir::Ccw; }
constexpr bool panel_is_column(PanelEdge edge) { return edge == PanelEdge::Right || edge == PanelEdge::Left; }
// The telemetry rail's cells and PC/outlet icon stay anchored to the physical K2 bezel in every rotation: Slot 0
// (title) sits at the physical female-port end, Slots 1..3 run toward the physical male-plug end, and the PC/outlet
// icon stays at the physical male-plug corner. Side rails follow panel_text_dir so Rot 0 reads bottom-up and Rot 2 reads
// top-down (matching physical bottom->top in both); horizontal rails draw left-to-right (Rot 3) or right-to-left (Rot 1).
constexpr TextDir rail_text_dir(PanelEdge edge) { return panel_text_dir(edge); }

// The telemetry rail's four cells in physical bezel order (title at female-port end, readouts toward male-plug end);
// lengths match across every rotation.
constexpr int16_t kOppSlotLen[4] = {54, 62, 62, 62};
static_assert(kOppSlotLen[0] + kOppSlotLen[1] + kOppSlotLen[2] + kOppSlotLen[3] == kScreenH);   // the rail spans the whole edge
constexpr bool opp_reversed(PanelEdge opp_edge, TextDir dir) {
    return panel_is_column(opp_edge) ? (dir == TextDir::Ccw) : (opp_edge == PanelEdge::Bottom);
}
Rect opp_slot_rect(const Rect &rail, PanelEdge opp_edge, TextDir dir, unsigned slot);

// Which physical button the i-th slot shows. Right edge, rotation 0: BACK, MINUS, OK, PLUS.
Button slot_button(const Orientation &o, unsigned slot);

// Map a physical directional key to its on-screen action; Back and OK never move.
constexpr Button oriented_button(Button physical, uint8_t rotation, bool enabled) {
    if (!enabled || (rotation & 3) < 2) return physical;
    if (physical == Button::Plus) return Button::Minus;
    if (physical == Button::Minus) return Button::Plus;
    return physical;
}

}  // namespace k2
