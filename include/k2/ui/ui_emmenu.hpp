// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Saved cables page's view flow, pure: no hardware, host-tested. The page is pushed from Connection's Cable tab and
// opens on the list; OK opens a cable's detail, OK there asks to delete, OK on the question deletes. BACK steps one level
// out and leaves the page from the list (the Ui pops it). Spoofing is its own tool.
#pragma once
#include <cstdint>

#include "k2/ui/ui_events.hpp"

namespace k2 {

enum class EmView : uint8_t { List, Detail, Confirm };
enum class EmCmd : uint8_t { None, Delete };

struct EmMenu {
    EmView view = EmView::List;
    int idx = 0;                      // highlighted saved cable (List / Detail / Confirm)
    int count = 0;                    // saved cables, kept current by the page

    // The library changed under the page: keep idx inside it, fall back to the list when the shown cable is gone.
    void set_count(int n) {
        count = n;
        if (idx >= n) idx = n > 0 ? n - 1 : 0;
        if (n == 0 && (view == EmView::Detail || view == EmView::Confirm)) view = EmView::List;
    }

    // Returns true when the event was consumed; *cmd is what the page must now do. BACK in the list is not consumed.
    bool on_event(Event e, EmCmd &cmd) {
        cmd = EmCmd::None;
        if (e.press != Press::Short) return false;
        switch (e.button) {
        case Button::Back:
            if (view == EmView::List) return false;
            view = view == EmView::Detail ? EmView::List : EmView::Detail;
            return true;
        case Button::Plus:
        case Button::Minus:
            if (view != EmView::Confirm && count > 0) idx = (idx + count + (e.button == Button::Plus ? 1 : -1)) % count;
            return true;
        case Button::Ok:
            if (view == EmView::List) {
                if (count > 0) view = EmView::Detail;
            } else if (view == EmView::Detail) {
                view = EmView::Confirm;
            } else {   // Confirm
                cmd = EmCmd::Delete;
                view = EmView::List;
            }
            return true;
        }
        return true;
    }
};

}  // namespace k2
