// ProWindows - the drop indicator shown while a tiled window is dragged.
//
// A translucent rectangle over the space the window is about to take. It exists
// because the placement rule is invisible otherwise: dropping a window near a
// corner is a genuinely ambiguous gesture, and without a preview the only way
// to find out which side you were closer to is to let go and see. That is
// exactly the "sometimes it goes right, sometimes it goes down" complaint - the
// rule was not really wrong so much as unknowable until too late.
//
// A layered, click-through, never-activated window, so it cannot steal the drag
// or take the foreground away from the window being moved.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void DragGuideInit(HINSTANCE inst, Config* cfg);
void DragGuideShutdown();

// Move the indicator to `r` and show it, creating it on first use. Repeated
// calls with the same rectangle are free, which matters because this is driven
// off a timer that fires many times a second during a drag.
void DragGuideShow(const Rect& r);
void DragGuideHide();

} // namespace awa
