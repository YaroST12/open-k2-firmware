// The page names on the telemetry rail: at most 4 characters, one table. A page reads the same slot in every rotation,
// so its name never stretches or squeezes the rail. The consteval constructor rejects a longer literal at compile time.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace k2::rail {

inline constexpr size_t kTitleMax = 4;

struct Title {
    char s[kTitleMax + 1]{};
    constexpr Title() = default;
    template <size_t N>
        requires(N <= kTitleMax + 1)
    consteval Title(const char (&a)[N]) {
        for (size_t i = 0; i < N; i++) s[i] = a[i];
    }
    constexpr operator std::string_view() const { return s; }
    constexpr bool operator==(const Title &o) const { return std::string_view(s) == std::string_view(o.s); }
};

inline constexpr Title kNone{""};   // Home and a page that has no rail text
// Rotation pages.
inline constexpr Title kHome{"HOME"}, kNum{"NUM"}, kTest{"TEST"}, kLine{"LINE"}, kK2{"K2"}, kGraph{"GRPH"}, kRipple{"RIPL"};
// Charger test stages (the strip's and the footer's names for them).
inline constexpr Title kWait{"WAIT"}, kResult{"RSLT"}, kSteps{"STEP"}, kDetail{"DET"};
// Bench places (the Who's attached tabs name themselves with these).
inline constexpr Title kMeter{"METR"}, kCharger{"CHRG"}, kCable{"CABL"}, kDevice{"DEV"}, kBattery{"BATT"}, kLog{"LOG"}, kLink{"LINK"};
// Menus and tool pages.
inline constexpr Title kSys{"SYS"}, kDiagnosis{"DIAG"}, kFcTrig{"FC"}, kPdTrig{"PD"}, kCableR{"CBLR"}, kBlackBox{"BBOX"}, kSpoof{"SPOF"},
    kUsb{"USB"}, kRotate{"ROT"}, kCards{"CARD"}, kShare{"SHAR"}, kTwin{"TWIN"}, kTwins{"TWNS"}, kProbe{"PROB"}, kStress{"STRS"},
    kMemory{"MEM"}, kDisplay{"DISP"}, kHomeBoxes{"BOX"}, kDim{"DIM"}, kOff{"OFF"}, kTools{"TOOL"}, kLimit{"LIM"}, kQr{"QR"}, kEmarker{"CBLS"};

// The Bench places in Place order (ui_tools.hpp; src/ui/ui_tools.cpp pins the count).
inline constexpr size_t kPlaceCount = 7;
inline constexpr Title kPlace[kPlaceCount] = {kMeter, kCharger, kCable, kDevice, kBattery, kLog, kK2};
constexpr const Title &place(unsigned i) { return kPlace[i < kPlaceCount ? i : 0]; }

inline constexpr std::array kAll = {kHome, kNum, kTest, kLine, kK2, kGraph, kRipple, kWait, kResult, kSteps, kDetail, kMeter, kCharger, kCable,
                                    kDevice, kBattery, kLog, kLink, kSys, kDiagnosis, kFcTrig, kPdTrig, kCableR, kBlackBox, kSpoof, kUsb,
                                    kRotate, kCards, kShare, kTwin, kTwins, kProbe, kStress, kMemory, kDisplay, kHomeBoxes, kDim, kOff, kTools, kLimit,
                                    kQr, kEmarker};

// Every name is present and unique: two pages never share a name.
constexpr bool all_distinct() {
    for (size_t i = 0; i < kAll.size(); i++) {
        if (std::string_view(kAll[i]).empty()) return false;
        for (size_t j = i + 1; j < kAll.size(); j++)
            if (kAll[i] == kAll[j]) return false;
    }
    return true;
}
static_assert(all_distinct(), "rail names: non-empty and unique");

}  // namespace k2::rail
