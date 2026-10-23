#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif

typedef struct GpGraphics GpGraphics;
typedef struct GpPath GpPath;
typedef struct GpBrush GpBrush;
typedef struct GpPen GpPen;
typedef struct {
	UINT32 GdiplusVersion;
	void* DebugEventCallback;
	BOOL   SuppressBackgroundThread;
	BOOL   SuppressExternalCodecs;
} GdiplusStartupInput;

int  WINAPI GdiplusStartup(ULONG_PTR*, const GdiplusStartupInput*, void*);
void WINAPI GdiplusShutdown(ULONG_PTR);
int  WINAPI GdipCreateFromHDC(HDC, GpGraphics**);
int  WINAPI GdipDeleteGraphics(GpGraphics*);
int  WINAPI GdipSetSmoothingMode(GpGraphics*, int);
int  WINAPI GdipCreateSolidFill(UINT32, GpBrush**);
int  WINAPI GdipDeleteBrush(GpBrush*);
int  WINAPI GdipCreatePen1(UINT32, float, int, GpPen**);
int  WINAPI GdipDeletePen(GpPen*);
int  WINAPI GdipSetPenStartCap(GpPen*, int);
int  WINAPI GdipSetPenEndCap(GpPen*, int);
int  WINAPI GdipCreatePath(int, GpPath**);
int  WINAPI GdipDeletePath(GpPath*);
int  WINAPI GdipAddPathArc(GpPath*, float, float, float, float, float, float);
int  WINAPI GdipClosePathFigure(GpPath*);
int  WINAPI GdipFillPath(GpGraphics*, GpBrush*, GpPath*);
int  WINAPI GdipFillEllipse(GpGraphics*, GpBrush*, float, float, float, float);
int  WINAPI GdipDrawLine(GpGraphics*, GpPen*, float, float, float, float);
typedef struct GpImage GpImage;
typedef struct GpImageAttributes GpImageAttributes;
int  WINAPI GdipCreateBitmapFromStream(IStream*, GpImage**);
int  WINAPI GdipDisposeImage(GpImage*);
int  WINAPI GdipGetImageWidth(GpImage*, UINT*);
int  WINAPI GdipGetImageHeight(GpImage*, UINT*);
int  WINAPI GdipSetInterpolationMode(GpGraphics*, int);
int  WINAPI GdipSetClipPath(GpGraphics*, GpPath*, int);
int  WINAPI GdipCreateImageAttributes(GpImageAttributes**);
int  WINAPI GdipDisposeImageAttributes(GpImageAttributes*);
int  WINAPI GdipSetImageAttributesWrapMode(GpImageAttributes*, int, UINT32, BOOL);
int  WINAPI GdipDrawImageRectRectI(GpGraphics*, GpImage*, int, int, int, int, int, int, int, int,
	int, GpImageAttributes*, void*, void*);

struct DImage { GpImage* img; IStream* st; UINT w, h; };

DImage* g_logo_img;
DImage* g_banner_img;

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif

Theme   g_th;
int     g_light;

int system_prefers_light(void)
{
	DWORD v = 0, sz = sizeof(v);
	if (RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
		return 0;
	return v != 0;
}

int     g_dpi = 96;
HFONT   g_font, g_fontb, g_fontt;
HBRUSH  g_editbrush;
static ULONG_PTR g_gdiptok;

int S(int v) { return MulDiv(v, g_dpi, 96); }

RECT R(int l, int t, int r, int b)
{
	RECT x;
	x.left = S(l); x.top = S(t); x.right = S(r); x.bottom = S(b);
	return x;
}

RECT RAW(int l, int t, int r, int b)
{
	RECT x;
	x.left = l; x.top = t; x.right = r; x.bottom = b;
	return x;
}

WCHAR* dupw(const WCHAR* s)
{
	size_t n = (size_t)lstrlenW(s) + 1;
	WCHAR* c = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
	if (c) CopyMemory(c, s, n * sizeof(WCHAR));
	return c;
}

void fillr(HDC dc, RECT r, COLORREF c)
{
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

static UINT32 argb(COLORREF c)
{
	return 0xFF000000u | ((UINT32)GetRValue(c) << 16) | ((UINT32)GetGValue(c) << 8) | GetBValue(c);
}

static GpGraphics* gfx(HDC dc)
{
	GpGraphics* g = NULL;
	if (GdipCreateFromHDC(dc, &g) != 0) return NULL;
	GdipSetSmoothingMode(g, 4);
	return g;
}

static GpPath* rr_path(float x, float y, float w, float h, float r)
{
	GpPath* p = NULL;
	float d;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r < 0.5f) r = 0.5f;
	d = r * 2;
	if (GdipCreatePath(0, &p) != 0) return NULL;
	GdipAddPathArc(p, x, y, d, d, 180, 90);
	GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
	GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
	GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
	GdipClosePathFigure(p);
	return p;
}

static void fill_rr(GpGraphics* g, COLORREF c, float x, float y, float w, float h, float r)
{
	GpBrush* b = NULL;
	GpPath* p = rr_path(x, y, w, h, r);
	if (p && GdipCreateSolidFill(argb(c), &b) == 0) {
		GdipFillPath(g, b, p);
		GdipDeleteBrush(b);
	}
	if (p) GdipDeletePath(p);
}

void rrect(HDC dc, RECT r, COLORREF fill, COLORREF border, int rad)
{
	GpGraphics* g = gfx(dc);
	float x = (float)r.left, y = (float)r.top;
	float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
	float rr = (float)S(rad);
	if (!g) return;
	if (fill != border) {
		fill_rr(g, border, x, y, w, h, rr);
		fill_rr(g, fill, x + 1, y + 1, w - 2, h - 2, rr - 1);
	}
	else fill_rr(g, fill, x, y, w, h, rr);
	GdipDeleteGraphics(g);
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int wd)
{
	GpGraphics* g;
	GpPen* p = NULL;

	if (wd <= 1 && (x1 == x2 || y1 == y2)) {
		HPEN gp = CreatePen(PS_SOLID, 1, c);
		HGDIOBJ o = SelectObject(dc, gp);
		MoveToEx(dc, x1, y1, NULL);
		LineTo(dc, x2, y2);
		SelectObject(dc, o);
		DeleteObject(gp);
		return;
	}
	g = gfx(dc);
	if (!g) return;
	if (GdipCreatePen1(argb(c), (float)wd, 2, &p) == 0) {
		if (wd > 1) { GdipSetPenStartCap(p, 2); GdipSetPenEndCap(p, 2); }
		GdipDrawLine(g, p, x1 + 0.5f, y1 + 0.5f, x2 + 0.5f, y2 + 0.5f);
		GdipDeletePen(p);
	}
	GdipDeleteGraphics(g);
}

void circle(HDC dc, int cx, int cy, int rad, COLORREF c)
{
	GpGraphics* g = gfx(dc);
	GpBrush* b = NULL;
	if (!g) return;
	if (GdipCreateSolidFill(argb(c), &b) == 0) {
		GdipFillEllipse(g, b, (float)(cx - rad), (float)(cy - rad), (float)(rad * 2), (float)(rad * 2));
		GdipDeleteBrush(b);
	}
	GdipDeleteGraphics(g);
}

void txt(HDC dc, HFONT f, COLORREF c, RECT r, UINT fmt, const WCHAR* t)
{
	SelectObject(dc, f);
	SetTextColor(dc, c);
	SetBkMode(dc, TRANSPARENT);
	DrawTextW(dc, t, -1, &r, fmt);
}

void draw_item(HDC dc, const Item* it, int hov)
{
	RECT r = it->r;
	COLORREF fill, fg;
	hov = hov && it->enabled;

	switch (it->kind) {
	case IK_HEAD: txt(dc, g_fontb, C_TEXT2, r, TXT_L, it->text); break;
	case IK_TEXT: txt(dc, g_font, C_TEXT, r, TXT_L, it->text); break;
	case IK_NOTE: txt(dc, g_font, C_TEXT2, r, TXT_L, it->text); break;

	case IK_CHECK: {
		int on = it->val && *it->val;
		int bx = r.left, by = r.top + ((r.bottom - r.top) - S(16)) / 2;
		RECT tr;
		if (hov)
			rrect(dc, RAW(r.left - S(6), r.top + S(2), r.right, r.bottom - S(2)), C_SEL, C_SEL, 6);
		rrect(dc, RAW(bx, by, bx + S(16), by + S(16)), on ? C_ACCENT : C_BG, on ? C_ACCENT : C_BTNBD, 4);
		if (on) {
			line(dc, bx + S(3), by + S(8), bx + S(6), by + S(11), C_WHITE, S(2));
			line(dc, bx + S(6), by + S(11), bx + S(12), by + S(5), C_WHITE, S(2));
		}
		tr = RAW(r.left + S(26), r.top, r.right - S(6), r.bottom);
		txt(dc, g_font, C_TEXT, tr, TXT_L, it->text);
		break;
	}

	case IK_ACCENT:
		if (!it->enabled) {
			rrect(dc, r, C_HEAD, C_BORDER, 8);
			txt(dc, g_font, C_DIM, r, TXT_C, it->text);
		}
		else {
			fill = hov ? C_ACCHOV : C_ACCENT;
			rrect(dc, r, fill, fill, 8);
			txt(dc, g_font, C_WHITE, r, TXT_C, it->text);
		}
		break;

	case IK_THEME: {
		int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
		fill = hov ? C_BTNHOV : C_SEL;
		rrect(dc, r, fill, hov ? C_BTNBD : fill, 8);
		circle(dc, cx, cy, S(7), C_TEXT);
		circle(dc, cx + S(3), cy - S(3), S(6), fill);
		break;
	}

	case IK_BTN:
		fill = !it->enabled ? C_HEAD : (hov ? C_BTNHOV : C_HEAD);
		fg = it->enabled ? C_TEXT : C_DIM;
		rrect(dc, r, fill, it->enabled ? C_BTNBD : C_BORDER, 8);
		txt(dc, g_font, fg, r, TXT_C, it->text);
		break;
	}
}

DImage* img_load(const void* data, size_t len)
{
	DImage* im;
	HGLOBAL hg;
	void* p;

	if (!data || !len) return NULL;
	im = (DImage*)calloc(1, sizeof(*im));
	hg = GlobalAlloc(GMEM_MOVEABLE, len);
	if (!im || !hg) { free(im); if (hg) GlobalFree(hg); return NULL; }
	p = GlobalLock(hg);
	if (!p) { free(im); GlobalFree(hg); return NULL; }
	memcpy(p, data, len);
	GlobalUnlock(hg);
	if (CreateStreamOnHGlobal(hg, TRUE, &im->st) != S_OK) { free(im); GlobalFree(hg); return NULL; }
	if (GdipCreateBitmapFromStream(im->st, &im->img) != 0 || !im->img ||
		GdipGetImageWidth(im->img, &im->w) != 0 || GdipGetImageHeight(im->img, &im->h) != 0 ||
		!im->w || !im->h) {
		if (im->img) GdipDisposeImage(im->img);
		im->st->lpVtbl->Release(im->st);
		free(im);
		return NULL;
	}
	return im;
}

void img_free(DImage* im)
{
	if (!im) return;
	GdipDisposeImage(im->img);
	im->st->lpVtbl->Release(im->st);
	free(im);
}

void img_draw(HDC dc, DImage* im, RECT d, int cover, int rad)
{
	GpGraphics* g;
	GpImageAttributes* at = NULL;
	GpPath* clip = NULL;
	int dw = d.right - d.left, dh = d.bottom - d.top;
	int sx = 0, sy = 0, sw, sh;
	int x = d.left, y = d.top, w = dw, hh = dh;

	if (!im || dw <= 0 || dh <= 0) return;
	sw = (int)im->w; sh = (int)im->h;
	if (cover) {
		if ((long long)sw * dh > (long long)sh * dw) {
			sw = (int)((long long)sh * dw / dh);
			sx = ((int)im->w - sw) / 2;
		}
		else {
			sh = (int)((long long)sw * dh / dw);
			sy = ((int)im->h - sh) / 2;
		}
	}
	else if ((long long)sw * dh > (long long)sh * dw) {
		hh = (int)((long long)sh * dw / sw);
		y += (dh - hh) / 2;
	}
	else {
		w = (int)((long long)sw * dh / sh);
		x += (dw - w) / 2;
	}
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	g = gfx(dc);
	if (!g) return;
	GdipSetInterpolationMode(g, 7);
	if (rad > 0) {
		clip = rr_path((float)d.left, (float)d.top, (float)dw, (float)dh, (float)S(rad));
		if (clip) GdipSetClipPath(g, clip, 0);
	}
	GdipCreateImageAttributes(&at);
	if (at) GdipSetImageAttributesWrapMode(at, 3, 0, FALSE);
	GdipDrawImageRectRectI(g, im->img, x, y, w, hh, sx, sy, sw, sh, 2, at, NULL, NULL);
	if (at) GdipDisposeImageAttributes(at);
	if (clip) GdipDeletePath(clip);
	GdipDeleteGraphics(g);
}

void draw_logo_at(HDC dc, RECT logo, const WCHAR* name, DImage* img, HFONT f)
{
	WCHAR ini[3];
	const WCHAR* sp;

	if (img) { img_draw(dc, img, logo, 0, 0); return; }
	ini[0] = name && name[0] ? name[0] : L'?';
	ini[1] = 0; ini[2] = 0;
	sp = name ? wcschr(name, L' ') : NULL;
	if (sp && sp[1] && sp[1] != L' ') ini[1] = sp[1];
	else if (name && name[0] && name[1]) ini[1] = name[1];
	CharUpperBuffW(ini, 1);
	rrect(dc, logo, C_ACCENT, C_ACCENT, 14);
	txt(dc, f, C_WHITE, logo, TXT_C, ini);
}

void draw_logo(HDC dc, const WCHAR* name)
{
	draw_logo_at(dc, R(208, 84, 264, 140), name, g_logo_img, g_fontt);
}

void draw_check_badge(HDC dc)
{
	int cx = S(236), cy = S(112), rad = S(28);
	circle(dc, cx, cy, rad, C_ACCENT);
	line(dc, cx - S(14), cy, cx - S(4), cy + S(10), C_WHITE, S(3));
	line(dc, cx - S(4), cy + S(10), cx + S(15), cy - S(11), C_WHITE, S(3));
}

void draw_progress_page(HDC dc, int pct, const WCHAR* status, const WCHAR (*log)[120], int nlog, int busy)
{
	RECT track = R(208, 96, 650, 110);
	RECT bar;
	WCHAR p[16];
	int i, y, w = track.right - track.left;

	rrect(dc, track, C_HEAD, C_BORDER, 7);
	if (pct > 0) {
		bar = track;
		bar.right = bar.left + w * pct / 100;
		if (bar.right - bar.left < S(14)) bar.right = bar.left + S(14);
		rrect(dc, bar, C_ACCENT, C_ACCENT, 7);
	}
	wsprintfW(p, L"%d%%", pct);
	txt(dc, g_font, C_TEXT2, R(208, 118, 580, 140), TXT_L, status);
	txt(dc, g_font, C_TEXT, R(580, 118, 650, 140), TXT_R, p);

	rrect(dc, R(208, 176, 650, 300), C_BG, C_BORDER, 8);
	y = 184;
	for (i = 0; i < nlog; ++i, y += 22) {
		RECT lr = R(220, y, 640, y + 22);
		line(dc, lr.left, lr.top + S(11), lr.left + S(3), lr.top + S(14), C_ACCENT, S(2));
		line(dc, lr.left + S(3), lr.top + S(14), lr.left + S(9), lr.top + S(8), C_ACCENT, S(2));
		lr.left += S(18);
		txt(dc, g_font, C_TEXT, lr, TXT_L, log[i]);
	}
	if (busy) {
		RECT lr = R(220, y, 640, y + 22);
		circle(dc, lr.left + S(3), lr.top + S(11), S(3), C_TEXT2);
		lr.left += S(18);
		txt(dc, g_font, C_TEXT2, lr, TXT_L, status);
	}
}

void dark_title_bar(HWND hwnd)
{
	typedef HRESULT(WINAPI* SetAttr)(HWND, DWORD, LPCVOID, DWORD);
	HMODULE m = LoadLibraryW(L"dwmapi.dll");
	if (m) {
		SetAttr f = (SetAttr)GetProcAddress(m, "DwmSetWindowAttribute");
		BOOL dark = !g_light;
		if (f) f(hwnd, 20, &dark, sizeof(dark));
		FreeLibrary(m);
	}
}

extern HBRUSH g_editbrush;

void set_theme(int light)
{
	g_light = light;
	if (light) {
		g_th.bg = RGB(0xee, 0xf0, 0xf3);
		g_th.panel = RGB(0xf7, 0xf8, 0xfa);
		g_th.card = RGB(0xff, 0xff, 0xff);
		g_th.head = RGB(0xf2, 0xf4, 0xf7);
		g_th.border = RGB(0xdc, 0xdf, 0xe5);
		g_th.sel = RGB(0xe3, 0xe7, 0xee);
		g_th.btnhov = RGB(0xe6, 0xe9, 0xee);
		g_th.btnbd = RGB(0xc4, 0xc8, 0xd0);
		g_th.accent = RGB(0x3b, 0x82, 0xf6);
		g_th.acchov = RGB(0x25, 0x63, 0xeb);
		g_th.text = RGB(0x1c, 0x1e, 0x22);
		g_th.text2 = RGB(0x5c, 0x61, 0x6b);
		g_th.dim = RGB(0xa3, 0xa8, 0xb2);
		g_th.err = RGB(0xd6, 0x45, 0x45);
	}
	else {
		g_th.bg = RGB(0x16, 0x17, 0x1a);
		g_th.panel = RGB(0x1e, 0x20, 0x24);
		g_th.card = RGB(0x1a, 0x1b, 0x1e);
		g_th.head = RGB(0x23, 0x25, 0x2a);
		g_th.border = RGB(0x2e, 0x30, 0x36);
		g_th.sel = RGB(0x2a, 0x2d, 0x33);
		g_th.btnhov = RGB(0x2e, 0x31, 0x38);
		g_th.btnbd = RGB(0x3a, 0x3d, 0x45);
		g_th.accent = RGB(0x3b, 0x82, 0xf6);
		g_th.acchov = RGB(0x5a, 0x97, 0xf8);
		g_th.text = RGB(0xe6, 0xe6, 0xe6);
		g_th.text2 = RGB(0x8b, 0x8f, 0x98);
		g_th.dim = RGB(0x5a, 0x5d, 0x66);
		g_th.err = RGB(0xf0, 0x7a, 0x7a);
	}
	if (g_editbrush) DeleteObject(g_editbrush);
	g_editbrush = CreateSolidBrush(g_th.head);
}

void apply_theme(HWND hwnd, int light)
{
	set_theme(light);
	dark_title_bar(hwnd);
	RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void draw_init(void)
{
	GdiplusStartupInput gi;
	HDC sd;

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	ZeroMemory(&gi, sizeof(gi));
	gi.GdiplusVersion = 1;
	GdiplusStartup(&g_gdiptok, &gi, NULL);

	sd = GetDC(NULL);
	g_dpi = GetDeviceCaps(sd, LOGPIXELSX);
	ReleaseDC(NULL, sd);

	g_font = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
		CLEARTYPE_QUALITY, 0, L"Segoe UI");
	g_fontb = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
		CLEARTYPE_QUALITY, 0, L"Segoe UI");
	g_fontt = CreateFontW(-S(18), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
		CLEARTYPE_QUALITY, 0, L"Segoe UI");
	set_theme(system_prefers_light());
}

void draw_shutdown(void)
{
	if (g_font)      DeleteObject(g_font);
	if (g_fontb)     DeleteObject(g_fontb);
	if (g_fontt)     DeleteObject(g_fontt);
	if (g_editbrush) DeleteObject(g_editbrush);
	g_font = g_fontb = g_fontt = NULL;
	g_editbrush = NULL;
	GdiplusShutdown(g_gdiptok);
}
