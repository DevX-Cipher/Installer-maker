#include "common.h"
#include "draw.h"
#include "install.h"
#include "payload.h"
#include "appicon.h"
#include <shellapi.h>
#include <wchar.h>

#define WM_APP_PROGRESS  (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_ACTIVATE  (WM_APP + 3)

#define IDC_PATH   1000
#define MAXIT      16
#define LOG_MAX    5

static HWND    g_hwnd, g_edit;
static WNDPROC g_editproc;
static int     g_page, g_hov = -1, g_tracking;
static Item    g_it[MAXIT];
static int     g_nit;
static int     g_allusers, g_desktop, g_ctx, g_launch;
static int     g_installing, g_ok, g_pct;
static WCHAR   g_msg[160], g_status[200], g_err[256], g_space[96], g_dir0[MAX_PATH];
static WCHAR   g_log[LOG_MAX][120];
static WCHAR   g_lbl_ctx[128], g_lbl_launch[100], g_wintitle[100];
static int     g_nlog;
static InstallOpts g_opts;
static int     g_mode, g_confirmed, g_un_all, g_existing;
static WCHAR   g_un_dir[MAX_PATH];

enum {
	PG_WELCOME, PG_LOCATION, PG_INSTALL, PG_FINISH, PG_COUNT,
	PG_UCONFIRM = PG_COUNT, PG_UREMOVE, PG_UDONE, PG_EXISTING, PG_ALL
};

enum {
	ID_NONE, ID_CANCEL, ID_BACK, ID_NEXT, ID_FINISH, ID_BROWSE,
	ID_CHK_ALL, ID_CHK_DESK, ID_CHK_CTX, ID_CHK_LAUNCH, ID_UNINSTALL, ID_RUN_UNINST
};

static const WCHAR* s_nav[PG_ALL] = { L"Welcome", L"Location", L"Install", L"Finish",
																				L"Confirm", L"Remove", L"Finish", L"Welcome" };
static const WCHAR* s_title[PG_ALL] = { L"Welcome", L"Install location", L"Installing", L"Finish",
																				L"Uninstall", L"Removing", L"Finish", L"Already installed" };

static void page_sub(int pg, WCHAR* out, size_t cap)
{
	WCHAR t[160];
	switch (pg) {
	case PG_WELCOME:  wsprintfW(t, L"Set up %s on this computer", g_cfg.name); break;
	case PG_LOCATION: wsprintfW(t, L"Choose where %s is installed", g_cfg.name); break;
	case PG_INSTALL:  lstrcpyW(t, L"Copying files and creating shortcuts"); break;
	case PG_FINISH:   lstrcpyW(t, L"Setup is complete"); break;
	case PG_UCONFIRM: wsprintfW(t, L"Remove %s from this computer", g_cfg.name); break;
	case PG_UREMOVE:  lstrcpyW(t, L"Deleting files and shortcuts"); break;
	case PG_UDONE:    lstrcpyW(t, L"Uninstall is complete"); break;
	default:          lstrcpyW(t, L"Update or remove the current install"); break;
	}
	lstrcpynW(out, t, (int)cap);
}

static int* chk_ptr(int id)
{
	switch (id) {
	case ID_CHK_ALL:    return &g_allusers;
	case ID_CHK_DESK:   return &g_desktop;
	case ID_CHK_CTX:    return &g_ctx;
	case ID_CHK_LAUNCH: return &g_launch;
	default:            return NULL;
	}
}

static void add(int kind, int id, const WCHAR* text, int enabled, int l, int t, int r, int b)
{
	Item* it;
	if (g_nit >= MAXIT) return;
	it = &g_it[g_nit++];
	it->kind = kind; it->id = id; it->text = text; it->enabled = enabled;
	it->r = R(l, t, r, b);
	it->val = chk_ptr(id);
}

static void build_items(void)
{
	g_nit = 0;
	switch (g_page) {
	case PG_WELCOME:
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Next", 1, 580, 330, 660, 364);
		break;

	case PG_LOCATION:
		add(IK_HEAD, 0, L"INSTALL FOLDER", 1, 208, 72, 650, 92);
		add(IK_BTN, ID_BROWSE, L"Browse", 1, 554, 96, 650, 126);
		add(IK_NOTE, 0, g_space, 1, 208, 134, 650, 154);
		add(IK_HEAD, 0, L"OPTIONS", 1, 208, 172, 650, 192);
		add(IK_CHECK, ID_CHK_ALL, L"Install for all users (needs administrator)", 1, 208, 196, 650, 230);
		add(IK_CHECK, ID_CHK_DESK, L"Create a desktop shortcut", 1, 208, 230, 650, 264);
		if (g_cfg.ctx_offer)
			add(IK_CHECK, ID_CHK_CTX, g_lbl_ctx, 1, 208, 264, 650, 298);
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 404, 330, 484, 364);
		add(IK_BTN, ID_BACK, L"Back", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Install", 1, 580, 330, 660, 364);
		break;

	case PG_INSTALL:
		add(IK_HEAD, 0, L"PROGRESS", 1, 208, 72, 650, 92);
		add(IK_HEAD, 0, L"LOG", 1, 208, 152, 650, 172);
		add(IK_BTN, ID_CANCEL, L"Cancel", 0, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Next", 0, 580, 330, 660, 364);
		break;

	case PG_FINISH:
		if (g_ok) {
			wsprintfW(g_lbl_launch, L"Launch %s", g_cfg.name);
			add(IK_CHECK, ID_CHK_LAUNCH, g_lbl_launch, 1, 208, 176, 650, 210);
		}
		add(IK_ACCENT, ID_FINISH, g_ok ? L"Finish" : L"Close", 1, 580, 330, 660, 364);
		break;

	case PG_UCONFIRM:
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_UNINSTALL, L"Uninstall", 1, 580, 330, 660, 364);
		break;

	case PG_UREMOVE:
		add(IK_HEAD, 0, L"PROGRESS", 1, 208, 72, 650, 92);
		add(IK_HEAD, 0, L"LOG", 1, 208, 152, 650, 172);
		add(IK_BTN, ID_CANCEL, L"Cancel", 0, 492, 330, 572, 364);
		add(IK_ACCENT, ID_FINISH, L"Close", 0, 580, 330, 660, 364);
		break;

	case PG_UDONE:
		add(IK_ACCENT, ID_FINISH, L"Close", 1, 580, 330, 660, 364);
		break;

	case PG_EXISTING:
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 404, 330, 484, 364);
		add(IK_BTN, ID_RUN_UNINST, L"Uninstall", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Update", 1, 580, 330, 660, 364);
		break;
	}
}

static void position_edit(void)
{
	int h = S(18);
	MoveWindow(g_edit, S(208 + 10), S(96) + (S(30) - h) / 2, S(336 - 20), h, TRUE);
}

static void set_page(int p)
{
	g_page = p;
	g_hov = -1;
	g_msg[0] = 0;
	ShowWindow(g_edit, p == PG_LOCATION ? SW_SHOW : SW_HIDE);
	build_items();
	if (p == PG_LOCATION) SetFocus(g_edit);
	InvalidateRect(g_hwnd, NULL, FALSE);
}

static void line_fmt(HDC dc, HFONT f, COLORREF c, int y, const WCHAR* fmt)
{
	WCHAR t[300];
	wsprintfW(t, fmt, g_cfg.name);
	txt(dc, f, c, R(208, y, 650, y + 22), TXT_L, t);
}

static void draw_header(HDC dc, const WCHAR* title, const WCHAR* sub)
{
	txt(dc, g_fontt, C_TEXT, R(280, 88, 650, 114), TXT_L, title);
	txt(dc, g_font, C_TEXT2, R(280, 114, 650, 136), TXT_L, sub);
}

static void draw_welcome(HDC dc)
{
	WCHAR ver[64], pub[100];
	draw_logo(dc, g_cfg.name);
	wsprintfW(ver, L"Version %s", g_cfg.version);
	if (g_cfg.publisher[0]) wsprintfW(pub, L"%s  |  by %s", ver, g_cfg.publisher);
	else lstrcpyW(pub, ver);
	draw_header(dc, g_cfg.name, pub);
	if (g_cfg.description[0])
		txt(dc, g_font, C_TEXT, R(208, 168, 650, 190), TXT_L, g_cfg.description);
	else
		line_fmt(dc, g_font, C_TEXT, 168, L"This installer copies %s to your computer.");
	line_fmt(dc, g_font, C_TEXT2, 236, L"Close any running copy of %s before you continue.");
}

static void draw_location(HDC dc)
{
	RECT box = R(208, 96, 544, 126);
	int focus = (GetFocus() == g_edit);
	rrect(dc, box, C_HEAD, focus ? C_ACCENT : C_BORDER, 8);
}

static void draw_finish(HDC dc)
{
	int cx = S(236), cy = S(112), rad = S(28);
	WCHAR t[200];
	if (g_ok) {
		draw_check_badge(dc);
		wsprintfW(t, L"%s is installed", g_cfg.name);
		txt(dc, g_fontt, C_TEXT, R(280, 90, 650, 116), TXT_L, t);
		txt(dc, g_font, C_TEXT2, R(280, 116, 650, 138), TXT_L, g_opts.dir);
	}
	else {
		circle(dc, cx, cy, rad, RGB(0xe2, 0x4b, 0x4a));
		txt(dc, g_fontt, C_WHITE, RAW(cx - rad, cy - rad, cx + rad, cy + rad), TXT_C, L"!");
		txt(dc, g_fontt, C_TEXT, R(280, 90, 650, 116), TXT_L, L"Setup couldn't finish");
		txt(dc, g_font, C_TEXT2, R(280, 124, 650, 280), TXT_W, g_err);
	}
}

static void draw_uconfirm(HDC dc)
{
	WCHAR t[200];
	draw_logo(dc, g_cfg.name);
	wsprintfW(t, L"Remove %s?", g_cfg.name);
	draw_header(dc, t, g_un_dir);
	line_fmt(dc, g_font, C_TEXT, 168, L"This removes %s and its files from this computer.");
	line_fmt(dc, g_font, C_TEXT2, 220, L"Close any running copy of %s before you continue.");
}

static void draw_udone(HDC dc)
{
	WCHAR t[200];
	draw_check_badge(dc);
	wsprintfW(t, L"%s was removed", g_cfg.name);
	txt(dc, g_fontt, C_TEXT, R(280, 90, 650, 116), TXT_L, t);
}

static void draw_existing(HDC dc)
{
	WCHAR t[200];
	draw_logo(dc, g_cfg.name);
	wsprintfW(t, L"%s is already installed", g_cfg.name);
	draw_header(dc, t, g_dir0);
	txt(dc, g_font, C_TEXT, R(208, 168, 650, 190), TXT_L,
		L"Update installs this version over the current one.");
	line_fmt(dc, g_font, C_TEXT, 190, L"Uninstall removes %s from this computer.");
	line_fmt(dc, g_font, C_TEXT2, 236, L"Close any running copy of %s before you continue.");
}

static void draw_all(HDC dc, int w, int h)
{
	int i;
	WCHAR buf[32], sub[200];

	fillr(dc, RAW(0, 0, w, h), C_BG);
	if (g_light) line(dc, 0, 0, w, 0, C_BTNBD, 1);

	rrect(dc, R(12, 12, 172, 314), C_PANEL, C_BORDER, 12);
	{
		int first = g_mode ? PG_UCONFIRM : 0;
		int cnt = g_mode ? 3 : PG_COUNT;
		int cur = (g_page == PG_EXISTING) ? PG_WELCOME : g_page;
		for (i = 0; i < cnt; ++i) {
			int y = 24 + i * 38;
			int pg = first + i;
			if (pg == cur) rrect(dc, R(20, y, 164, y + 34), C_SEL, C_SEL, 6);
			txt(dc, g_font, pg == cur ? C_TEXT : C_TEXT2, R(34, y, 160, y + 34), TXT_L, s_nav[pg]);
		}
	}
	if (g_banner_img) img_draw(dc, g_banner_img, R(20, 196, 164, 306), 1, 8);

	rrect(dc, R(184, 12, 668, 314), C_CARD, C_BORDER, 12);
	rrect(dc, R(185, 13, 667, 68), C_HEAD, C_HEAD, 11);
	fillr(dc, R(185, 56, 667, 68), C_CARD);
	line(dc, S(185), S(56), S(667), S(56), C_BORDER, 1);
	page_sub(g_page, sub, 200);
	txt(dc, g_fontb, C_TEXT, R(204, 12, 360, 56), TXT_L, s_title[g_page]);
	txt(dc, g_font, C_TEXT2, R(360, 12, 656, 56), TXT_L, sub);

	line(dc, 0, S(320), w, S(320), C_BORDER, 1);
	wsprintfW(buf, L"Step %d of %d", g_mode ? g_page - PG_UCONFIRM + 1 : (g_page == PG_EXISTING ? 1 : g_page + 1), g_mode ? 3 : PG_COUNT);
	txt(dc, g_font, C_TEXT2, R(16, 326, 110, 368), TXT_L, buf);
	if (g_msg[0]) txt(dc, g_font, C_ERR, R(112, 326, 396, 368), TXT_L, g_msg);

	switch (g_page) {
	case PG_WELCOME:  draw_welcome(dc); break;
	case PG_LOCATION: draw_location(dc); break;
	case PG_INSTALL:  draw_progress_page(dc, g_pct, g_status, g_log, g_nlog, g_installing); break;
	case PG_FINISH:   draw_finish(dc); break;
	case PG_UCONFIRM: draw_uconfirm(dc); break;
	case PG_UREMOVE:  draw_progress_page(dc, g_pct, g_status, g_log, g_nlog, g_installing); break;
	case PG_UDONE:    draw_udone(dc); break;
	case PG_EXISTING: draw_existing(dc); break;
	}
	for (i = 0; i < g_nit; ++i) draw_item(dc, &g_it[i], g_hov == i);
}

static void on_paint(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);
	RECT cr;
	HDC mdc;
	HBITMAP bmp, old;

	GetClientRect(hwnd, &cr);
	mdc = CreateCompatibleDC(dc);
	bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
	old = (HBITMAP)SelectObject(mdc, bmp);
	draw_all(mdc, cr.right, cr.bottom);
	BitBlt(dc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
	SelectObject(mdc, old);
	DeleteObject(bmp);
	DeleteDC(mdc);
	EndPaint(hwnd, &ps);
}

static void on_progress(void* ud, int pct, const WCHAR* text, int step)
{
	(void)ud;
	PostMessageW(g_hwnd, WM_APP_PROGRESS,
		(WPARAM)((pct & 0xFFFF) | ((step ? 1 : 0) << 16)),
		(LPARAM)(text ? dupw(text) : NULL));
}

static DWORD WINAPI install_thread(LPVOID param)
{
	WCHAR err[256];
	int ok;
	(void)param;
	err[0] = 0;
	ok = run_install_flow(&g_opts, on_progress, NULL, err, 256);
	PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)ok,
		(LPARAM)dupw(ok ? L"" : (err[0] ? err : L"Setup could not finish.")));
	return 0;
}

static void start_install(void)
{
	WCHAR d[MAX_PATH];
	int n, a, b;
	HANDLE t;

	GetWindowTextW(g_edit, d, MAX_PATH);
	a = 0;
	while (d[a] == L' ') ++a;
	if (a) MoveMemory(d, d + a, (size_t)(lstrlenW(d + a) + 1) * sizeof(WCHAR));
	n = lstrlenW(d);
	while (n > 3 && (d[n - 1] == L'\\' || d[n - 1] == L' ')) d[--n] = 0;
	b = (n >= 3 && d[1] == L':' && d[2] == L'\\') || (n >= 3 && d[0] == L'\\' && d[1] == L'\\');
	if (!b) {
		lstrcpyW(g_msg, L"Enter a full folder path, such as C:\\Apps\\MyApp.");
		InvalidateRect(g_hwnd, NULL, FALSE);
		return;
	}

	ZeroMemory(&g_opts, sizeof(g_opts));
	lstrcpynW(g_opts.dir, d, MAX_PATH);
	g_opts.allusers = g_allusers;
	g_opts.desktop = g_desktop;
	g_opts.context = g_cfg.ctx_offer && g_ctx;

	g_nlog = 0; g_pct = 0; g_ok = 0; g_err[0] = 0;
	lstrcpyW(g_status, L"Preparing...");
	g_installing = 1;
	set_page(PG_INSTALL);

	t = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
	if (t) CloseHandle(t);
	else PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)dupw(L"Could not start the install thread."));
}

static DWORD WINAPI remove_thread(LPVOID param)
{
	(void)param;
	Sleep(800);
	uninstall_remove(g_un_dir, g_un_all, on_progress, NULL);
	PostMessageW(g_hwnd, WM_APP_DONE, 1, (LPARAM)dupw(L""));
	return 0;
}

static void start_remove(void)
{
	HANDLE t;
	g_nlog = 0; g_pct = 0; g_ok = 0; g_err[0] = 0;
	lstrcpyW(g_status, L"Preparing...");
	g_installing = 1;
	t = CreateThread(NULL, 0, remove_thread, NULL, 0, NULL);
	if (t) CloseHandle(t);
	else PostMessageW(g_hwnd, WM_APP_DONE, 1, (LPARAM)dupw(L""));
}

static int confirm_exit(void)
{
	if (g_installing) {
		MessageBoxW(g_hwnd, g_mode ? L"Uninstall is still running. Please wait for it to finish."
			: L"Setup is still installing. Please wait for it to finish.",
			g_wintitle, MB_OK | MB_ICONINFORMATION);
		return 0;
	}
	if (g_page == PG_FINISH || g_page == PG_UCONFIRM || g_page == PG_UDONE || g_page == PG_EXISTING) return 1;
	{
		WCHAR q[200];
		wsprintfW(q, L"Exit setup? %s has not been installed yet.", g_cfg.name);
		return MessageBoxW(g_hwnd, q, g_wintitle, MB_YESNO | MB_ICONQUESTION) == IDYES;
	}
}

static void browse_folder(void)
{
	BROWSEINFOW bi;
	LPITEMIDLIST pidl;
	WCHAR path[MAX_PATH], suffix[80];

	ZeroMemory(&bi, sizeof(bi));
	bi.hwndOwner = g_hwnd;
	bi.lpszTitle = L"Select the folder to install into";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	pidl = SHBrowseForFolderW(&bi);
	if (!pidl) return;
	if (SHGetPathFromIDListW(pidl, path)) {
		int n = lstrlenW(path), sl;
		wsprintfW(suffix, L"\\%s", g_cfg.name);
		sl = lstrlenW(suffix);
		while (n > 3 && path[n - 1] == L'\\') path[--n] = 0;
		if (n < sl || lstrcmpiW(path + n - sl, suffix) != 0) {
			if (n + sl < MAX_PATH) lstrcatW(path, n > 3 ? suffix : suffix + 1);
		}
		SetWindowTextW(g_edit, path);
	}
	CoTaskMemFree(pidl);
}

static void toggle_allusers(void)
{
	WCHAR cur[MAX_PATH], olddef[MAX_PATH], newdef[MAX_PATH];
	GetWindowTextW(g_edit, cur, MAX_PATH);
	default_install_dir(g_allusers, olddef, MAX_PATH);
	g_allusers = !g_allusers;
	default_install_dir(g_allusers, newdef, MAX_PATH);
	if (!lstrcmpiW(cur, olddef)) SetWindowTextW(g_edit, newdef);
}

static void activate(int id)
{
	int* v;
	switch (id) {
	case ID_CANCEL: if (confirm_exit()) DestroyWindow(g_hwnd); break;
	case ID_BACK:
		if (g_page == PG_LOCATION && g_existing) set_page(PG_EXISTING);
		else if (g_page > PG_WELCOME) set_page(g_page - 1);
		break;
	case ID_NEXT:
		if (g_page == PG_WELCOME || g_page == PG_EXISTING) set_page(PG_LOCATION);
		else if (g_page == PG_LOCATION) start_install();
		break;
	case ID_RUN_UNINST: {
		WCHAR u[MAX_PATH];
		path_join(u, MAX_PATH, g_dir0, UNINSTALL_EXE);
		ShellExecuteW(NULL, L"open", u, NULL, g_dir0, SW_SHOWNORMAL);
		DestroyWindow(g_hwnd);
		break;
	}
	case ID_BROWSE: browse_folder(); break;
	case ID_CHK_ALL: toggle_allusers(); break;
	case ID_UNINSTALL:
		g_confirmed = 1;
		DestroyWindow(g_hwnd);
		break;
	case ID_FINISH:
		if (!g_mode && g_ok && g_launch) {
			WCHAR exe[MAX_PATH];
			path_join(exe, MAX_PATH, g_opts.dir, g_cfg.exe);
			ShellExecuteW(NULL, L"open", exe, NULL, g_opts.dir, SW_SHOWNORMAL);
		}
		DestroyWindow(g_hwnd);
		break;
	default:
		v = chk_ptr(id);
		if (v) *v = !*v;
		break;
	}
}

static int hit_item(int x, int y)
{
	int i;
	POINT pt;
	pt.x = x; pt.y = y;
	for (i = 0; i < g_nit; ++i)
		if (g_it[i].kind >= IK_CHECK && PtInRect(&g_it[i].r, pt)) return i;
	return -1;
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_KEYDOWN && w == VK_RETURN) { PostMessageW(g_hwnd, WM_APP_ACTIVATE, ID_NEXT, 0); return 0; }
	if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostMessageW(g_hwnd, WM_APP_ACTIVATE, ID_CANCEL, 0); return 0; }
	if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
	return CallWindowProcW(g_editproc, h, m, w, l);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE: {
		HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
		g_hwnd = hwnd;
		g_edit = CreateWindowExW(0, L"EDIT", g_dir0, WS_CHILD | ES_AUTOHSCROLL,
			0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_PATH, hi, NULL);
		SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
		g_editproc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
		position_edit();
		build_items();
		ShowWindow(g_edit, SW_HIDE);
		return 0;
	}

	case WM_PAINT:      on_paint(hwnd); return 0;
	case WM_ERASEBKGND: return 1;

	case WM_CTLCOLOREDIT: {
		HDC dc = (HDC)wp;
		SetTextColor(dc, C_TEXT);
		SetBkColor(dc, C_HEAD);
		return (LRESULT)g_editbrush;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDC_PATH &&
			(HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS))
			InvalidateRect(hwnd, NULL, FALSE);
		return 0;

	case WM_MOUSEMOVE: {
		int h = hit_item((short)LOWORD(lp), (short)HIWORD(lp));
		if (!g_tracking) {
			TRACKMOUSEEVENT te;
			te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = hwnd; te.dwHoverTime = 0;
			TrackMouseEvent(&te);
			g_tracking = 1;
		}
		if (h != g_hov) { g_hov = h; InvalidateRect(hwnd, NULL, FALSE); }
		return 0;
	}

	case WM_MOUSELEAVE:
		g_tracking = 0;
		if (g_hov != -1) { g_hov = -1; InvalidateRect(hwnd, NULL, FALSE); }
		return 0;

	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) {
			POINT pt;
			int h;
			GetCursorPos(&pt);
			ScreenToClient(hwnd, &pt);
			h = hit_item(pt.x, pt.y);
			if (h >= 0 && g_it[h].enabled) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
		}
		break;

	case WM_LBUTTONDOWN: {
		int h = hit_item((short)LOWORD(lp), (short)HIWORD(lp));
		SetFocus(hwnd);
		if (h >= 0 && g_it[h].enabled) {
			activate(g_it[h].id);
			if (IsWindow(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
		}
		return 0;
	}

	case WM_KEYDOWN:
		if (wp == VK_ESCAPE) activate(ID_CANCEL);
		else if (wp == VK_RETURN) {
			if (g_page == PG_FINISH || g_page == PG_UDONE) activate(ID_FINISH);
			else if (g_page != PG_INSTALL && g_page != PG_UREMOVE && g_page != PG_UCONFIRM) activate(ID_NEXT);
		}
		return 0;

	case WM_APP_ACTIVATE:
		activate((int)wp);
		if (IsWindow(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
		return 0;

	case WM_APP_PROGRESS: {
		int pct = (int)(wp & 0xFFFF), step = (int)((wp >> 16) & 1);
		WCHAR* t = (WCHAR*)lp;
		if (pct > 100) pct = 100;
		if (pct > g_pct) g_pct = pct;
		if (t) {
			if (step) {
				if (g_nlog == LOG_MAX) {
					int i;
					for (i = 1; i < LOG_MAX; ++i) lstrcpyW(g_log[i - 1], g_log[i]);
					--g_nlog;
				}
				lstrcpynW(g_log[g_nlog++], t, 120);
			}
			else lstrcpynW(g_status, t, 200);
			HeapFree(GetProcessHeap(), 0, t);
		}
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}

	case WM_APP_DONE: {
		WCHAR* t = (WCHAR*)lp;
		g_installing = 0;
		g_ok = (int)wp;
		if (g_ok) g_pct = 100;
		lstrcpynW(g_err, t ? t : L"", 256);
		if (t) HeapFree(GetProcessHeap(), 0, t);
		set_page(g_mode ? PG_UDONE : PG_FINISH);
		return 0;
	}

	case WM_CLOSE:
		if (confirm_exit()) DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static int ui_open(const WCHAR* suffix)
{
	WNDCLASSW wc;
	RECT rc;
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	HINSTANCE hi = GetModuleHandleW(NULL);

	draw_init();
	{
		size_t n;
		const unsigned char* b = payload_logo(&n);
		if (b) g_logo_img = img_load(b, n);
		b = payload_banner(&n);
		if (b) g_banner_img = img_load(b, n);
	}
	wsprintfW(g_wintitle, L"%s %s", g_cfg.name, suffix);
	wsprintfW(g_lbl_ctx, L"Add \"%s\" to the Explorer menu", g_cfg.ctxlabel);

	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(APP_ICON_ID));
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.lpszClassName = L"ImkSetupWnd";
	RegisterClassW(&wc);

	rc = R(0, 0, 680, 372);
	AdjustWindowRect(&rc, style, FALSE);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, g_wintitle, style,
		(GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
		(GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
		rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
	if (!g_hwnd) return 0;

	{
		HICON sm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(APP_ICON_ID), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
		if (sm) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
	}

	dark_title_bar(g_hwnd);
	return 1;
}

static int ui_loop(void)
{
	MSG m;
	m.wParam = 0;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	return (int)m.wParam;
}

int ui_setup_run(const WCHAR* dir, int allusers, int existing, const WCHAR* space)
{
	int rc;

	g_mode = 0;
	g_allusers = allusers;
	g_desktop = g_cfg.desktop;
	g_ctx = g_cfg.ctx_offer;
	g_launch = g_cfg.launch;
	g_existing = existing;
	lstrcpynW(g_dir0, dir, MAX_PATH);
	lstrcpynW(g_space, space ? space : L"", 96);

	if (!ui_open(L"setup")) return 1;
	set_page(existing ? PG_EXISTING : PG_WELCOME);
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	rc = ui_loop();
	draw_shutdown();
	return rc;
}

static void ui_run_uninstall(int page, const WCHAR* dir, int allusers)
{
	g_mode = 1;
	lstrcpynW(g_un_dir, dir, MAX_PATH);
	g_un_all = allusers;
	g_dir0[0] = 0;

	if (!ui_open(L"uninstall")) return;
	set_page(page);
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);
	if (page == PG_UREMOVE) start_remove();

	ui_loop();
	draw_shutdown();
}

int ui_uninstall_confirm(const WCHAR* dir)
{
	g_confirmed = 0;
	ui_run_uninstall(PG_UCONFIRM, dir, 0);
	return g_confirmed;
}

void ui_uninstall_progress(const WCHAR* dir, int allusers)
{
	ui_run_uninstall(PG_UREMOVE, dir, allusers);
}