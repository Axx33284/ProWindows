// ProWindows - the Clock page.
//
// Same contract as every other page: Load() pulls the live Config into the
// controls, Save() pushes them back, and the preview draws whatever the
// controls currently say through the clock's own painter.
#include "settings_internal.h"
#include "clock.h"
#include "clocktheme.h"
#include "theme.h"

namespace awa {

namespace {

int SelectedSkin(HWND page) {
    const int sel = (int)SendDlgItemMessageW(page, IDC_CLK_THEME, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel < ClockSkinCount()) ? sel : 0;
}

int SelectedStyle(HWND page) {
    const int sel = (int)SendDlgItemMessageW(page, IDC_CLK_STYLE, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel < ClockStyleCount()) ? sel : 0;
}

ClockPreview PreviewFromPage(HWND page) {
    ClockPreview look;
    look.theme   = SelectedSkin(page);
    look.style   = SelectedStyle(page);
    look.opacity = (int)SendDlgItemMessageW(page, IDC_CLK_OPACITY, TBM_GETPOS, 0, 0);
    look.hours24 = GetCheck(page, IDC_CLK_24H);
    look.seconds = GetCheck(page, IDC_CLK_SECONDS);
    look.date    = GetCheck(page, IDC_CLK_DATE);
    look.weekday = GetCheck(page, IDC_CLK_WEEKDAY);
    return look;
}

void UpdateEnabling(HWND page) {
    const bool on = GetCheck(page, IDC_CLK_ENABLED);
    const int gated[] = { IDC_CLK_PINNED, IDC_CLK_DESKTOP, IDC_CLK_RESET_POS,
                          IDC_CLK_STYLE, IDC_CLK_THEME, IDC_CLK_24H, IDC_CLK_SECONDS,
                          IDC_CLK_DATE, IDC_CLK_WEEKDAY, IDC_CLK_OPACITY,
                          IDC_CLK_SCALE, IDC_CLK_PREVIEW };
    for (int id : gated) EnableWindow(GetDlgItem(page, id), on);
}

void UpdateLook(HWND page) {
    SetDlgItemTextW(page, IDC_CLK_THEME_DESC, ClockSkinAt(SelectedSkin(page)).blurb);
    SetDlgItemTextW(page, IDC_CLK_STYLE_DESC, ClockStyleAt(SelectedStyle(page)).blurb);
    InvalidateRect(GetDlgItem(page, IDC_CLK_PREVIEW), nullptr, TRUE);
}

// The sample sits on a scrap of "desktop" so a translucent panel reads as
// translucent, exactly as the monitor's preview does.
void DrawPreview(const DRAWITEMSTRUCT* dis, HWND page) {
    RECT r = dis->rcItem;
    FillRect(dis->hDC, &r, theme::BrushPanel());
    theme::Chamfer(dis->hDC, r, 8, theme::Bg, 255, theme::Border, 255);
    RECT inner = { r.left + 6, r.top + 5, r.right - 6, r.bottom - 5 };
    ClockDrawPreview(dis->hDC, inner, PreviewFromPage(page));
}

void ShowPercent(HWND page, int slider, int label) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d%%", (int)SendDlgItemMessageW(page, slider, TBM_GETPOS, 0, 0));
    SetDlgItemTextW(page, label, buf);
}

} // namespace

void PageClockLoad(HWND page) {
    const Config& cfg = AppConfig();
    SetCheck(page, IDC_CLK_ENABLED, cfg.clockEnabled);
    SetCheck(page, IDC_CLK_PINNED,  cfg.clockPinned);
    SetCheck(page, IDC_CLK_DESKTOP, cfg.clockOnDesktop);
    SetCheck(page, IDC_CLK_24H,     cfg.clockHours24);
    SetCheck(page, IDC_CLK_SECONDS, cfg.clockSeconds);
    SetCheck(page, IDC_CLK_DATE,    cfg.clockDate);
    SetCheck(page, IDC_CLK_WEEKDAY, cfg.clockWeekday);
    SendDlgItemMessageW(page, IDC_CLK_THEME, CB_SETCURSEL, (WPARAM)cfg.clockTheme, 0);
    SendDlgItemMessageW(page, IDC_CLK_STYLE, CB_SETCURSEL, (WPARAM)cfg.clockStyle, 0);
    SendDlgItemMessageW(page, IDC_CLK_OPACITY, TBM_SETPOS, TRUE, cfg.clockOpacity);
    SendDlgItemMessageW(page, IDC_CLK_SCALE, TBM_SETPOS, TRUE, cfg.clockScale);
    ShowPercent(page, IDC_CLK_OPACITY, IDC_CLK_OPACITY_VAL);
    ShowPercent(page, IDC_CLK_SCALE, IDC_CLK_SCALE_VAL);
    UpdateLook(page);
    UpdateEnabling(page);
}

void PageClockSave(HWND page) {
    Config& cfg = AppConfig();
    cfg.clockEnabled   = GetCheck(page, IDC_CLK_ENABLED);
    cfg.clockPinned    = GetCheck(page, IDC_CLK_PINNED);
    cfg.clockOnDesktop = GetCheck(page, IDC_CLK_DESKTOP);
    cfg.clockHours24   = GetCheck(page, IDC_CLK_24H);
    cfg.clockSeconds   = GetCheck(page, IDC_CLK_SECONDS);
    cfg.clockDate      = GetCheck(page, IDC_CLK_DATE);
    cfg.clockWeekday   = GetCheck(page, IDC_CLK_WEEKDAY);
    cfg.clockTheme     = SelectedSkin(page);
    cfg.clockStyle     = SelectedStyle(page);
    cfg.clockOpacity   = (int)SendDlgItemMessageW(page, IDC_CLK_OPACITY, TBM_GETPOS, 0, 0);
    cfg.clockScale     = (int)SendDlgItemMessageW(page, IDC_CLK_SCALE, TBM_GETPOS, 0, 0);
}

INT_PTR CALLBACK PageClockProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            theme::PrepareDialog(page);
            for (int i = 0; i < ClockStyleCount(); ++i)
                SendDlgItemMessageW(page, IDC_CLK_STYLE, CB_ADDSTRING, 0,
                                    (LPARAM)ClockStyleAt(i).name);
            for (int i = 0; i < ClockSkinCount(); ++i)
                SendDlgItemMessageW(page, IDC_CLK_THEME, CB_ADDSTRING, 0,
                                    (LPARAM)ClockSkinAt(i).name);
            SendDlgItemMessageW(page, IDC_CLK_OPACITY, TBM_SETRANGE, TRUE,
                                MAKELPARAM(20, 100));    // matches config.cpp
            SendDlgItemMessageW(page, IDC_CLK_SCALE, TBM_SETRANGE, TRUE,
                                MAKELPARAM(50, 250));
            return TRUE;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_CLK_PREVIEW) { DrawPreview(dis, page); return TRUE; }
            return FALSE;
        }

        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(page, IDC_CLK_OPACITY)) {
                ShowPercent(page, IDC_CLK_OPACITY, IDC_CLK_OPACITY_VAL);
                InvalidateRect(GetDlgItem(page, IDC_CLK_PREVIEW), nullptr, TRUE);
            } else if ((HWND)lp == GetDlgItem(page, IDC_CLK_SCALE)) {
                ShowPercent(page, IDC_CLK_SCALE, IDC_CLK_SCALE_VAL);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_CLK_ENABLED:
                    UpdateEnabling(page);
                    return TRUE;
                case IDC_CLK_24H:
                case IDC_CLK_SECONDS:
                case IDC_CLK_DATE:
                case IDC_CLK_WEEKDAY:
                    UpdateLook(page);
                    return TRUE;
                case IDC_CLK_THEME:
                case IDC_CLK_STYLE:
                    if (HIWORD(wp) == CBN_SELCHANGE) UpdateLook(page);
                    return TRUE;
                case IDC_CLK_RESET_POS: {
                    Config& cfg = AppConfig();
                    cfg.clockX = INT_MIN;
                    cfg.clockY = INT_MIN;
                    ClockApplyConfig();
                    AppSaveConfig();
                    return TRUE;
                }
            }
            return FALSE;
    }
    return FALSE;
}

} // namespace awa
