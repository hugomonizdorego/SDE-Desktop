// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
extern "C" {
struct SdePanelGeometry {
  int x, y, width, height, reserve;
};
SdePanelGeometry sde_panel_geometry(int mx, int my, int mw, int mh, int height, bool top, int style,
                                    int percent, int gap, int minimum, bool hidden);
}
