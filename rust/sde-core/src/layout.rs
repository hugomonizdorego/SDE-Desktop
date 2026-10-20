// SPDX-License-Identifier: GPL-3.0-or-later
// Panel placement is toolkit-independent and shared through the private ABI.
#[repr(C)]
#[derive(Debug, PartialEq, Eq)]
pub struct PanelGeometry {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub reserve: i32,
}

#[allow(clippy::too_many_arguments)]
pub fn panel_geometry(
    mx: i32,
    my: i32,
    mw: i32,
    mh: i32,
    height: i32,
    top: bool,
    style: i32,
    percent: i32,
    gap: i32,
    minimum: i32,
    hidden: bool,
) -> PanelGeometry {
    let mw = mw.max(1);
    let mh = mh.max(1);
    let full = style == 0;
    let gap = if full {
        0
    } else {
        gap.clamp(0, (mw.min(mh) - 1) / 2)
    };
    let percent = if style == 2 { percent.min(72) } else { percent }.clamp(50, 100);
    let available = (mw - 2 * gap).max(1);
    let width = if full {
        mw
    } else {
        ((i64::from(mw) * i64::from(percent) / 100) as i32)
            .max(minimum)
            .min(available)
    };
    let height = if hidden {
        2.min(mh)
    } else {
        height.clamp(1, (mh - 2 * gap).max(1))
    };
    let edge_gap = if hidden { 0 } else { gap };
    PanelGeometry {
        x: mx + (mw - width) / 2,
        y: if top {
            my + edge_gap
        } else {
            my + mh - height - edge_gap
        },
        width,
        height,
        reserve: if hidden { 0 } else { height + edge_gap },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn floating_and_short_panels_respect_edges_and_struts() {
        let g = panel_geometry(120, 90, 1366, 768, 40, false, 1, 96, 8, 600, false);
        assert_eq!(g.width, 1311);
        assert_eq!(g.x, 147);
        assert_eq!(g.y, 810);
        assert_eq!(g.reserve, 48);
        let g = panel_geometry(0, 0, 1366, 768, 40, true, 2, 96, 8, 600, false);
        assert_eq!(g.width, 983);
        assert_eq!(g.y, 8);
    }
    #[test]
    fn full_autohide_and_small_scaled_screens_stay_on_monitor() {
        let g = panel_geometry(-800, 0, 800, 600, 48, false, 0, 70, 8, 600, false);
        assert_eq!(
            g,
            PanelGeometry {
                x: -800,
                y: 552,
                width: 800,
                height: 48,
                reserve: 48
            }
        );
        let g = panel_geometry(0, 0, 640, 480, 80, false, 2, 96, 16, 1200, false);
        assert_eq!(g.width, 608);
        assert_eq!(g.x, 16);
        assert_eq!(g.y + g.height, 464);
        let g = panel_geometry(0, 0, 640, 480, 80, false, 2, 96, 16, 1200, true);
        assert_eq!(g.y, 478);
        assert_eq!(g.reserve, 0);
    }
}
