#include "common.h"
#include "draw.h"
#include "packer.h"
#include "resource.h"
#include <shellapi.h>
#include <commdlg.h>
#include <wchar.h>

#define WM_APP_PROGRESS  (WM_APP + 1)
#define WM_APP_DONE      (WM_APP + 2)
#define WM_APP_ACTIVATE  (WM_APP + 3)

#define IDC_FIELD  1000
#define MAXIT      24
#define LOG_MAX    5

enum { PG_WELCOME, PG_DETAILS, PG_FILES, PG_OPTIONS, PG_DESIGN, PG_OUTPUT, PG_BUILD, PG_DONE, PG_COUNT };
enum { NAV_COUNT = 7 };
enum {
	F_NAME, F_VERSION, F_PUB, F_DESC, F_SRC, F_EXE, F_CTX, F_OUT, F_URL, F_SHA, F_COUNT
};
enum {
	ID_NONE, ID_CANCEL, ID_BACK, ID_NEXT, ID_FINISH, ID_BROWSE_SRC, ID_BROWSE_EXE, ID_BROWSE_OUT,
	ID_CHK_ALL, ID_CHK_DESK, ID_CHK_LAUNCH, ID_CHK_CTX, ID_SHOW, ID_AGAIN,
	ID_PICK_ICON, ID_PICK_LOGO, ID_PICK_BANNER, ID_CLR_ICON, ID_CLR_LOGO, ID_CLR_BANNER,
	ID_THEME, ID_SRC_FILES, ID_SRC_LINK
};
enum { SLOT_ICON, SLOT_LOGO, SLOT_BANNER, SLOT_COUNT };
#define IK_HOT 100   /* invisible click target; drawn by the design preview */

typedef struct { HWND h; int page; int x1, y1, x2, y2; int maxlen; } Field;

static const WCHAR* s_nav[NAV_COUNT] = { L"Welcome", L"Details", L"Source", L"Options", L"Design", L"Build", L"Finish" };
static const WCHAR* s_title[PG_COUNT] = { L"Welcome", L"App details", L"App source", L"Options", L"Design", L"Output", L"Building", L"Finish" };
static const WCHAR* s_sub[PG_COUNT] = {
	L"Create a Windows installer",
	L"Name your app",
	L"Choose where your app's files come from",
	L"Choose how setup behaves",
	L"Add your icon and images (optional)",
	L"Choose where to save it",
	L"Packing and compressing your files",
	L"Your installer is ready"
};
static const WCHAR* s_slot_name[SLOT_COUNT] = { L"ICON", L"LOGO", L"BANNER" };
static const WCHAR* s_slot_hint[SLOT_COUNT] = { L".ico file", L"Square image", L"Wide image" };

static HWND    g_hwnd;
static WNDPROC g_editproc;
static Field   g_f[F_COUNT];
static Item    g_it[MAXIT];
static int     g_nit, g_page, g_hov = -1, g_tracking;
static int     g_allusers, g_desktop = 1, g_launch = 1, g_ctx;
static int     g_link;
static int     g_building, g_ok, g_pct, g_nlog;
static WCHAR   g_msg[200], g_status[200], g_err[300], g_summary[120], g_note[120];
static WCHAR   g_log[LOG_MAX][120];
static AppConfig g_build_cfg;
static WCHAR   g_build_src[MAX_PATH], g_build_out[MAX_PATH];
static WCHAR   g_asset[SLOT_COUNT][MAX_PATH], g_slot_note[SLOT_COUNT][80];
static PackAssets g_pa;
static DImage* g_pv_logo, * g_pv_banner;
static HICON   g_pv_icon;

static void update_slot_notes(void);

static void ftext(int f, WCHAR* out, int cap)
{
	int n, a = 0;
	GetWindowTextW(g_f[f].h, out, cap);
	while (out[a] == L' ') ++a;
	if (a) MoveMemory(out, out + a, (size_t)(lstrlenW(out + a) + 1) * sizeof(WCHAR));
	n = lstrlenW(out);
	while (n > 0 && out[n - 1] == L' ') out[--n] = 0;
}

static int* chk_ptr(int id)
{
	switch (id) {
	case ID_CHK_ALL:    return &g_allusers;
	case ID_CHK_DESK:   return &g_desktop;
	case ID_CHK_LAUNCH: return &g_launch;
	case ID_CHK_CTX:    return &g_ctx;
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
	it->sel = 0;
}

static void build_items(void)
{
	g_nit = 0;
	switch (g_page) {
	case PG_WELCOME:
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Start", 1, 580, 330, 660, 364);
		break;

	case PG_DETAILS:
		add(IK_HEAD, 0, L"PRODUCT NAME", 1, 208, 72, 478, 92);
		add(IK_HEAD, 0, L"VERSION", 1, 490, 72, 650, 92);
		add(IK_HEAD, 0, L"PUBLISHER (OPTIONAL)", 1, 208, 134, 650, 154);
		add(IK_HEAD, 0, L"SHORT DESCRIPTION (OPTIONAL)", 1, 208, 196, 650, 216);
		add(IK_NOTE, 0, L"The name is used for the install folder, shortcuts and Add/Remove Programs.", 1, 208, 256, 650, 276);
		break;

	case PG_FILES:
		add(IK_HEAD, 0, L"WHERE DO THE FILES COME FROM?", 1, 208, 72, 650, 92);
		add(IK_SEG, ID_SRC_FILES, L"Add files", 1, 208, 94, 427, 120);
		g_it[g_nit - 1].sel = !g_link;
		add(IK_SEG, ID_SRC_LINK, L"Download from a link", 1, 431, 94, 650, 120);
		g_it[g_nit - 1].sel = g_link;
		if (!g_link) {
			add(IK_HEAD, 0, L"APP FOLDER (EVERYTHING INSIDE IS PACKED)", 1, 208, 128, 650, 146);
			add(IK_BTN, ID_BROWSE_SRC, L"Browse", 1, 554, 148, 650, 174);
			add(IK_NOTE, 0, g_summary, 1, 208, 176, 650, 196);
			add(IK_HEAD, 0, L"MAIN PROGRAM (RELATIVE PATH, E.G. MYAPP.EXE)", 1, 208, 200, 650, 218);
			add(IK_BTN, ID_BROWSE_EXE, L"Browse", 1, 554, 220, 650, 246);
			add(IK_NOTE, 0, L"Shortcuts and \"Launch\" start this file.", 1, 208, 250, 650, 270);
		}
		else {
			add(IK_HEAD, 0, L"DOWNLOAD LINK (HTTPS)", 1, 208, 128, 650, 146);
			add(IK_NOTE, 0, L"A zip is unpacked. Any other file becomes the main program.", 1, 208, 176, 650, 196);
			add(IK_HEAD, 0, L"MAIN PROGRAM (RELATIVE PATH, E.G. MYAPP.EXE)", 1, 208, 200, 650, 218);
			add(IK_HEAD, 0, L"SHA-256 CHECKSUM (OPTIONAL, RECOMMENDED)", 1, 208, 254, 650, 272);
		}
		break;

	case PG_OPTIONS:
		add(IK_HEAD, 0, L"SETUP OPTIONS", 1, 208, 72, 650, 92);
		add(IK_CHECK, ID_CHK_ALL, L"Install for all users by default (needs administrator)", 1, 208, 96, 650, 128);
		add(IK_CHECK, ID_CHK_DESK, L"Create a desktop shortcut by default", 1, 208, 128, 650, 160);
		add(IK_CHECK, ID_CHK_LAUNCH, L"Offer to launch the app when setup finishes", 1, 208, 160, 650, 192);
		add(IK_CHECK, ID_CHK_CTX, L"Add an Explorer right-click menu entry (opens a file with the app)", 1, 208, 192, 650, 224);
		if (g_ctx) add(IK_HEAD, 0, L"MENU TEXT", 1, 208, 232, 650, 252);
		break;

	case PG_DESIGN: {
		int i, y;
		for (i = 0; i < SLOT_COUNT; ++i) {
			y = 70 + i * 76;
			add(IK_HEAD, 0, s_slot_name[i], 1, 556, y, 650, y + 18);
			add(IK_NOTE, 0, g_slot_note[i], 1, 556, y + 18, 650, y + 38);
			add(IK_BTN, ID_PICK_ICON + i, L"Choose", 1, 556, y + 42, 608, y + 68);
			add(IK_BTN, ID_CLR_ICON + i, L"Clear", g_asset[i][0] != 0, 612, y + 42, 650, y + 68);
		}
		add(IK_HOT, ID_PICK_ICON, NULL, 1, 211, 73, 225, 87);
		add(IK_HOT, ID_PICK_LOGO, NULL, 1, 312, 132, 340, 160);
		add(IK_HOT, ID_PICK_BANNER, NULL, 1, 218, 188, 290, 243);
		break;
	}

	case PG_OUTPUT:
		add(IK_HEAD, 0, L"SAVE THE INSTALLER AS", 1, 208, 72, 650, 92);
		add(IK_BTN, ID_BROWSE_OUT, L"Browse", 1, 554, 94, 650, 124);
		add(IK_NOTE, 0, g_note, 1, 208, 128, 650, 148);
		break;

	case PG_BUILD:
		add(IK_HEAD, 0, L"PROGRESS", 1, 208, 72, 650, 92);
		add(IK_HEAD, 0, L"LOG", 1, 208, 152, 650, 172);
		break;

	case PG_DONE:
		if (g_ok) add(IK_BTN, ID_SHOW, L"Show in folder", 1, 208, 176, 328, 210);
		break;
	}

	switch (g_page) {
	case PG_WELCOME: break;
	case PG_BUILD:
		add(IK_BTN, ID_CANCEL, L"Cancel", 0, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, L"Next", 0, 580, 330, 660, 364);
		break;
	case PG_DONE:
		add(IK_BTN, ID_AGAIN, g_ok ? L"Make another" : L"Back", 1, 492, 330, 572, 364);
		add(IK_ACCENT, ID_FINISH, g_ok ? L"Finish" : L"Close", 1, 580, 330, 660, 364);
		break;
	default:
		add(IK_BTN, ID_CANCEL, L"Cancel", 1, 404, 330, 484, 364);
		add(IK_BTN, ID_BACK, L"Back", g_page > PG_WELCOME, 492, 330, 572, 364);
		add(IK_ACCENT, ID_NEXT, g_page == PG_OUTPUT ? L"Build" : L"Next", 1, 580, 330, 660, 364);
		break;
	}
	add(IK_THEME, ID_THEME, L"", 1, 626, 21, 654, 49);
}

static int field_visible(int i)
{
	if (g_f[i].page != g_page) return 0;
	if (i == F_CTX) return g_ctx;
	if (i == F_SRC) return !g_link;
	if (i == F_URL || i == F_SHA) return g_link;
	return 1;
}

static void position_fields(void)
{
	int i, h = S(18);
	g_f[F_EXE].x2 = g_link ? 650 : 544;
	for (i = 0; i < F_COUNT; ++i)
		MoveWindow(g_f[i].h, S(g_f[i].x1 + 10), S(g_f[i].y1) + (S(g_f[i].y2 - g_f[i].y1) - h) / 2,
			S(g_f[i].x2 - g_f[i].x1 - 20), h, TRUE);
}

static void update_fields(void)
{
	int i;
	for (i = 0; i < F_COUNT; ++i) ShowWindow(g_f[i].h, field_visible(i) ? SW_SHOW : SW_HIDE);
}

static void first_focus(void)
{
	int i;
	for (i = 0; i < F_COUNT; ++i)
		if (field_visible(i)) { SetFocus(g_f[i].h); return; }
	SetFocus(g_hwnd);
}

static void sanitize_file_name(const WCHAR* in, WCHAR* out, int cap)
{
	int i, n = 0;
	for (i = 0; in[i] && n < cap - 1; ++i) out[n++] = in[i] == L' ' ? L'-' : in[i];
	out[n] = 0;
}

static void default_output(void)
{
	WCHAR src[MAX_PATH], par[MAX_PATH], name[100], file[140];
	WCHAR* s;
	ftext(F_SRC, src, MAX_PATH);
	if (g_link || !src[0]) {
		if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, par) != S_OK)
			lstrcpyW(par, L"C:\\");
	}
	else {
		lstrcpynW(par, src, MAX_PATH);
		while (lstrlenW(par) > 3 && par[lstrlenW(par) - 1] == L'\\') par[lstrlenW(par) - 1] = 0;
		s = wcsrchr(par, L'\\');
		if (s) { if (s - par <= 2) s[1] = 0; else *s = 0; }
	}
	sanitize_file_name(g_build_cfg.name, name, 100);
	wsprintfW(file, L"%s-setup.exe", name);
	path_join(src, MAX_PATH, par, file);
	SetWindowTextW(g_f[F_OUT].h, src);
}

static void update_note(void)
{
	WCHAR out[MAX_PATH];
	ftext(F_OUT, out, MAX_PATH);
	g_note[0] = 0;
	if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
		lstrcpyW(g_note, L"This file exists and will be replaced.");
}

static void set_page(int p)
{
	g_page = p;
	g_hov = -1;
	g_msg[0] = 0;
	update_slot_notes();
	if (p == PG_OUTPUT) {
		WCHAR o[MAX_PATH];
		ftext(F_OUT, o, MAX_PATH);
		if (!o[0]) default_output();
		update_note();
	}
	build_items();
	update_fields();
	first_focus();
	InvalidateRect(g_hwnd, NULL, FALSE);
}

static void refresh_summary(void)
{
	WCHAR src[MAX_PATH];
	unsigned files = 0;
	unsigned long long bytes = 0;
	DWORD a;

	ftext(F_SRC, src, MAX_PATH);
	g_summary[0] = 0;
	if (!src[0]) return;
	a = GetFileAttributesW(src);
	if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
		lstrcpyW(g_summary, L"That folder does not exist.");
		return;
	}
	pack_scan(src, &files, &bytes);
	wsprintfW(g_summary, L"%u files, %u MB", files, (unsigned)(bytes / (1024 * 1024)));
}

static void guess_exe(void)
{
	WCHAR src[MAX_PATH], pat[MAX_PATH], cur[260];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	ftext(F_EXE, cur, 260);
	if (cur[0]) return;
	ftext(F_SRC, src, MAX_PATH);
	path_join(pat, MAX_PATH, src, L"*.exe");
	h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && _wcsnicmp(fd.cFileName, L"unins", 5) != 0) {
			SetWindowTextW(g_f[F_EXE].h, fd.cFileName);
			break;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static void browse_src(void)
{
	BROWSEINFOW bi;
	LPITEMIDLIST pidl;
	WCHAR path[MAX_PATH];

	ZeroMemory(&bi, sizeof(bi));
	bi.hwndOwner = g_hwnd;
	bi.lpszTitle = L"Select the folder that contains your app";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	pidl = SHBrowseForFolderW(&bi);
	if (!pidl) return;
	if (SHGetPathFromIDListW(pidl, path)) {
		SetWindowTextW(g_f[F_SRC].h, path);
		SetWindowTextW(g_f[F_EXE].h, L"");
		guess_exe();
		refresh_summary();
	}
	CoTaskMemFree(pidl);
}

static void browse_exe(void)
{
	OPENFILENAMEW of;
	WCHAR src[MAX_PATH], file[MAX_PATH];
	int n;

	ftext(F_SRC, src, MAX_PATH);
	if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
		lstrcpyW(g_msg, L"Choose the app folder first.");
		return;
	}
	file[0] = 0;
	ZeroMemory(&of, sizeof(of));
	of.lStructSize = sizeof(of);
	of.hwndOwner = g_hwnd;
	of.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
	of.lpstrFile = file;
	of.nMaxFile = MAX_PATH;
	of.lpstrInitialDir = src;
	of.lpstrTitle = L"Select the main program";
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&of)) return;

	while (lstrlenW(src) > 3 && src[lstrlenW(src) - 1] == L'\\') src[lstrlenW(src) - 1] = 0;
	n = lstrlenW(src);
	if (_wcsnicmp(file, src, n) != 0 || file[n] != L'\\') {
		lstrcpyW(g_msg, L"The program must be inside the app folder.");
		return;
	}
	SetWindowTextW(g_f[F_EXE].h, file + n + 1);
}

static void browse_out(void)
{
	OPENFILENAMEW of;
	WCHAR file[MAX_PATH];

	ftext(F_OUT, file, MAX_PATH);
	ZeroMemory(&of, sizeof(of));
	of.lStructSize = sizeof(of);
	of.hwndOwner = g_hwnd;
	of.lpstrFilter = L"Installer (*.exe)\0*.exe\0";
	of.lpstrFile = file;
	of.nMaxFile = MAX_PATH;
	of.lpstrDefExt = L"exe";
	of.lpstrTitle = L"Save installer as";
	of.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&of)) return;
	SetWindowTextW(g_f[F_OUT].h, file);
	update_note();
}

static void update_slot_notes(void)
{
	int i;
	for (i = 0; i < SLOT_COUNT; ++i) {
		const WCHAR* n = g_asset[i][0] ? wcsrchr(g_asset[i], L'\\') : NULL;
		lstrcpynW(g_slot_note[i], g_asset[i][0] ? (n ? n + 1 : g_asset[i]) : s_slot_hint[i], 80);
	}
}

static void clear_asset(int s)
{
	g_asset[s][0] = 0;
	if (s == SLOT_ICON && g_pv_icon) { DestroyIcon(g_pv_icon); g_pv_icon = NULL; }
	if (s == SLOT_LOGO) { img_free(g_pv_logo); g_pv_logo = NULL; }
	if (s == SLOT_BANNER) { img_free(g_pv_banner); g_pv_banner = NULL; }
	update_slot_notes();
	build_items();
}

static void pick_asset(int s)
{
	OPENFILENAMEW of;
	WCHAR file[MAX_PATH];
	unsigned char* d;
	size_t n = 0;

	file[0] = 0;
	ZeroMemory(&of, sizeof(of));
	of.lStructSize = sizeof(of);
	of.hwndOwner = g_hwnd;
	of.lpstrFilter = s == SLOT_ICON ? L"Icon (*.ico)\0*.ico\0" :
		L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0";
	of.lpstrFile = file;
	of.nMaxFile = MAX_PATH;
	of.lpstrTitle = s == SLOT_ICON ? L"Choose the installer icon" : (s == SLOT_LOGO ? L"Choose the logo" : L"Choose the sidebar banner");
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&of)) return;

	d = pack_slurp(file, s == SLOT_ICON ? (1u << 20) : ASSET_MAX_BYTES, &n);
	if (!d) { lstrcpyW(g_msg, L"That file is empty, unreadable, or too large (max 3 MB)."); return; }
	if (s == SLOT_ICON) {
		HICON h = pack_icon_valid(d, n) ?
			(HICON)LoadImageW(NULL, file, IMAGE_ICON, S(14), S(14), LR_LOADFROMFILE) : NULL;
		if (!h) { lstrcpyW(g_msg, L"That is not a valid .ico file."); free(d); return; }
		if (g_pv_icon) DestroyIcon(g_pv_icon);
		g_pv_icon = h;
	}
	else {
		DImage* im = img_load(d, n);
		if (!im) { lstrcpyW(g_msg, L"That image could not be read."); free(d); return; }
		if (s == SLOT_LOGO) { img_free(g_pv_logo); g_pv_logo = im; }
		else { img_free(g_pv_banner); g_pv_banner = im; }
	}
	free(d);
	lstrcpynW(g_asset[s], file, MAX_PATH);
	g_msg[0] = 0;
	update_slot_notes();
	build_items();
}

static int is_dir(const WCHAR* p)
{
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int sha_ok(const WCHAR* s)
{
	int i, n = lstrlenW(s);
	if (n == 0) return 1;
	if (n != 64) return 0;
	for (i = 0; i < n; ++i)
		if (!((s[i] >= L'0' && s[i] <= L'9') || (s[i] >= L'a' && s[i] <= L'f') || (s[i] >= L'A' && s[i] <= L'F')))
			return 0;
	return 1;
}

static int validate_page(void)
{
	WCHAR t[MAX_PATH], full[MAX_PATH], url[512];
	char u8[600];

	switch (g_page) {
	case PG_DETAILS:
		ftext(F_NAME, t, 64);
		if (!cfg_name_is_valid(t)) {
			lstrcpyW(g_msg, t[0] ? L"Name can't contain \\ / : * ? \" < > |" : L"Enter a product name.");
			return 0;
		}
		return 1;

	case PG_FILES:
		if (g_link) {
			ftext(F_URL, url, 512);
			if (_wcsnicmp(url, L"https://", 8) != 0 || lstrlenW(url) < 12 || wcschr(url, L' ')) {
				lstrcpyW(g_msg, L"Enter a link that starts with https://");
				return 0;
			}
			ftext(F_EXE, t, 260);
			if (!t[0]) { lstrcpyW(g_msg, L"Enter the main program, for example MyApp.exe."); return 0; }
			if (!WideCharToMultiByte(CP_UTF8, 0, t, -1, u8, sizeof(u8), NULL, NULL) ||
				wcschr(t, L'/') || !rel_path_is_safe(u8) || t[0] == L'\\') {
				lstrcpyW(g_msg, L"The main program must be a relative path inside the download.");
				return 0;
			}
			ftext(F_SHA, t, 80);
			if (!sha_ok(t)) { lstrcpyW(g_msg, L"The checksum must be 64 hex characters, or empty."); return 0; }
			return 1;
		}
		ftext(F_SRC, t, MAX_PATH);
		if (!is_dir(t)) { lstrcpyW(g_msg, L"Choose an existing app folder."); return 0; }
		refresh_summary();
		ftext(F_EXE, t, 260);
		if (!t[0]) { lstrcpyW(g_msg, L"Enter the main program, for example MyApp.exe."); return 0; }
		if (!WideCharToMultiByte(CP_UTF8, 0, t, -1, u8, sizeof(u8), NULL, NULL) ||
			wcschr(t, L'/') || !rel_path_is_safe(u8) || t[0] == L'\\') {
			lstrcpyW(g_msg, L"The main program must be a relative path inside the folder.");
			return 0;
		}
		ftext(F_SRC, full, MAX_PATH);
		path_join(full, MAX_PATH, full, t);
		if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES || is_dir(full)) {
			lstrcpyW(g_msg, L"That program was not found in the folder.");
			return 0;
		}
		return 1;

	case PG_OPTIONS:
		ftext(F_CTX, t, 64);
		if (g_ctx && !t[0]) { lstrcpyW(g_msg, L"Enter the right-click menu text."); return 0; }
		return 1;

	case PG_OUTPUT:
		ftext(F_OUT, t, MAX_PATH);
		if (!t[0]) { lstrcpyW(g_msg, L"Choose where to save the installer."); return 0; }
		if (lstrlenW(t) < 5 || lstrcmpiW(t + lstrlenW(t) - 4, L".exe") != 0) {
			if (lstrlenW(t) + 4 >= MAX_PATH) { lstrcpyW(g_msg, L"That path is too long."); return 0; }
			lstrcatW(t, L".exe");
			SetWindowTextW(g_f[F_OUT].h, t);
		}
		{
			WCHAR par[MAX_PATH];
			WCHAR* s;
			lstrcpynW(par, t, MAX_PATH);
			s = wcsrchr(par, L'\\');
			if (s && s - par == 2) s[1] = 0;
			else if (s && s - par > 2) *s = 0;
			if (!s || !is_dir(par)) {
				lstrcpyW(g_msg, L"Enter a full path to a folder that exists.");
				return 0;
			}
		}
		return 1;
	}
	return 1;
}

static void gather_config(void)
{
	WCHAR t[260];
	cfg_defaults(&g_build_cfg);
	ftext(F_NAME, g_build_cfg.name, 64);
	ftext(F_VERSION, g_build_cfg.version, 32);
	if (!g_build_cfg.version[0]) lstrcpyW(g_build_cfg.version, L"1.0.0");
	ftext(F_PUB, g_build_cfg.publisher, 64);
	ftext(F_DESC, g_build_cfg.description, 160);
	ftext(F_EXE, t, 260);
	{
		WCHAR* p;
		for (p = t; *p; ++p) if (*p == L'/') *p = L'\\';
	}
	lstrcpynW(g_build_cfg.exe, t, 260);
	ftext(F_CTX, g_build_cfg.ctxlabel, 64);
	g_build_cfg.source_link = g_link;
	ftext(F_URL, g_build_cfg.url, 512);
	ftext(F_SHA, g_build_cfg.sha256, 72);
	g_build_cfg.ctx_offer = g_ctx;
	g_build_cfg.desktop = g_desktop;
	g_build_cfg.allusers = g_allusers;
	g_build_cfg.launch = g_launch;
	cfg_finish(&g_build_cfg);
	ftext(F_SRC, g_build_src, MAX_PATH);
	ftext(F_OUT, g_build_out, MAX_PATH);
	g_pa.icon = g_asset[SLOT_ICON][0] ? g_asset[SLOT_ICON] : NULL;
	g_pa.logo = g_asset[SLOT_LOGO][0] ? g_asset[SLOT_LOGO] : NULL;
	g_pa.banner = g_asset[SLOT_BANNER][0] ? g_asset[SLOT_BANNER] : NULL;
}

static void on_progress(void* ud, int pct, const WCHAR* text, int step)
{
	(void)ud;
	PostMessageW(g_hwnd, WM_APP_PROGRESS,
		(WPARAM)((pct & 0xFFFF) | ((step ? 1 : 0) << 16)),
		(LPARAM)(text ? dupw(text) : NULL));
}

static DWORD WINAPI build_thread(LPVOID param)
{
	WCHAR err[300];
	HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_STUB), RT_RCDATA);
	HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
	const unsigned char* stub = g ? (const unsigned char*)LockResource(g) : NULL;
	int ok;
	(void)param;
	err[0] = 0;
	if (!stub) {
		ok = 0;
		lstrcpyW(err, L"The setup template is missing from this program. Rebuild Installer Maker.");
	}
	else {
		ok = pack_installer(&g_build_cfg, g_build_src, g_build_out, stub, (size_t)SizeofResource(NULL, r), &g_pa,
			on_progress, NULL, err, 300);
	}
	PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)ok,
		(LPARAM)dupw(ok ? L"" : (err[0] ? err : L"Could not build the installer.")));
	return 0;
}

static void start_build(void)
{
	HANDLE t;
	WCHAR out[MAX_PATH];

	gather_config();
	if (GetFileAttributesW(g_build_out) != INVALID_FILE_ATTRIBUTES) {
		wsprintfW(out, L"%s already exists. Replace it?", g_build_out);
		if (MessageBoxW(g_hwnd, out, L"Installer Maker", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
	}
	g_nlog = 0; g_pct = 0; g_ok = 0; g_err[0] = 0;
	lstrcpyW(g_status, L"Preparing...");
	g_building = 1;
	set_page(PG_BUILD);
	t = CreateThread(NULL, 0, build_thread, NULL, 0, NULL);
	if (t) CloseHandle(t);
	else PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)dupw(L"Could not start the build thread."));
}

static int confirm_exit(void)
{
	if (g_building) {
		MessageBoxW(g_hwnd, L"The installer is still being built. Please wait for it to finish.",
			L"Installer Maker", MB_OK | MB_ICONINFORMATION);
		return 0;
	}
	if (g_page == PG_WELCOME || g_page == PG_DONE) return 1;
	return MessageBoxW(g_hwnd, L"Exit Installer Maker? Your installer has not been built yet.",
		L"Installer Maker", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static void show_in_folder(void)
{
	WCHAR args[MAX_PATH + 20];
	wsprintfW(args, L"/select,\"%s\"", g_build_out);
	ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static int g_manual;  

static void toggle_theme(void)
{
	g_manual = 1;
	apply_theme(g_hwnd, !g_light);
}

static void activate(int id)
{
	int* v;
	switch (id) {
	case ID_CANCEL: if (confirm_exit()) DestroyWindow(g_hwnd); break;
	case ID_BACK: if (g_page > PG_WELCOME) set_page(g_page - 1); break;
	case ID_NEXT:
		if (g_page == PG_OUTPUT) {
			if (validate_page()) start_build();
		}
		else if (g_page < PG_OUTPUT) {
			if (validate_page()) {
				if (g_page == PG_DETAILS) {
					WCHAR n[64];
					ftext(F_NAME, n, 64);
					lstrcpynW(g_build_cfg.name, n, 64);
					if (!GetWindowTextLengthW(g_f[F_CTX].h)) {
						WCHAR l[100];
						wsprintfW(l, L"Open with %s", n);
						SetWindowTextW(g_f[F_CTX].h, l);
					}
				}
				set_page(g_page + 1);
			}
		}
		break;
	case ID_BROWSE_SRC: browse_src(); break;
	case ID_BROWSE_EXE: browse_exe(); break;
	case ID_BROWSE_OUT: browse_out(); break;
	case ID_SHOW: show_in_folder(); break;
	case ID_SRC_FILES: case ID_SRC_LINK:
		g_link = (id == ID_SRC_LINK);
		g_msg[0] = 0;
		build_items(); position_fields(); update_fields(); first_focus();
		break;
	case ID_THEME: toggle_theme(); break;
	case ID_PICK_ICON: case ID_PICK_LOGO: case ID_PICK_BANNER: pick_asset(id - ID_PICK_ICON); break;
	case ID_CLR_ICON: case ID_CLR_LOGO: case ID_CLR_BANNER: clear_asset(id - ID_CLR_ICON); break;
	case ID_AGAIN: set_page(g_ok ? PG_DETAILS : PG_OUTPUT); break;
	case ID_FINISH: DestroyWindow(g_hwnd); break;
	case ID_CHK_CTX:
		g_ctx = !g_ctx;
		build_items(); update_fields();
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

static void tab_field(HWND from, int back)
{
	int i, cur = -1, n;
	for (i = 0; i < F_COUNT; ++i) if (g_f[i].h == from) cur = i;
	for (n = 1; n <= F_COUNT; ++n) {
		i = ((cur + (back ? -n : n)) % F_COUNT + F_COUNT) % F_COUNT;
		if (field_visible(i)) { SetFocus(g_f[i].h); return; }
	}
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_KEYDOWN && w == VK_RETURN) { PostMessageW(g_hwnd, WM_APP_ACTIVATE, ID_NEXT, 0); return 0; }
	if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostMessageW(g_hwnd, WM_APP_ACTIVATE, ID_CANCEL, 0); return 0; }
	if (m == WM_KEYDOWN && w == VK_TAB) { tab_field(h, GetKeyState(VK_SHIFT) < 0); return 0; }
	if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
	return CallWindowProcW(g_editproc, h, m, w, l);
}

static int nav_of(int pg)
{
	if (pg <= PG_DESIGN) return pg;
	return pg <= PG_BUILD ? PG_DESIGN + 1 : NAV_COUNT - 1;
}

static void hbar(HDC dc, int x1, int x2, int y, COLORREF c, int wd)
{
	RECT a = R(208 + x1 / 2, 90 + y / 2, 208 + x2 / 2, 90 + y / 2);
	line(dc, a.left, a.top, a.right, a.top, c, wd);
}

static void draw_design(HDC dc)
{
#define MR(l, t, r, b) R(208 + (l) / 2, 90 + (t) / 2, 208 + (r) / 2, 90 + (b) / 2)
	WCHAR nm[64], title[100];
	RECT ic = R(211, 73, 225, 87);
	int i;

	ftext(F_NAME, nm, 64);
	if (!nm[0]) lstrcpyW(nm, L"Your App");

	rrect(dc, R(208, 70, 548, 276), C_BG, C_BORDER, 6);
	rrect(dc, R(209, 71, 547, 89), C_HEAD, C_HEAD, 5);
	if (g_pv_icon) DrawIconEx(dc, ic.left, ic.top, g_pv_icon, S(14), S(14), 0, NULL, DI_NORMAL);
	else rrect(dc, ic, C_BTNBD, C_BTNBD, 3);
	wsprintfW(title, L"%s setup", nm);
	txt(dc, g_font, C_TEXT2, R(232, 70, 540, 90), TXT_L, title);

	rrect(dc, MR(12, 12, 172, 314), C_PANEL, C_BORDER, 4);
	rrect(dc, MR(20, 24, 164, 58), C_SEL, C_SEL, 3);
	for (i = 0; i < 4; ++i) hbar(dc, 34, i ? 100 : 110, 24 + i * 38 + 17, i ? C_DIM : C_TEXT, 3);
	if (g_pv_banner) img_draw(dc, g_pv_banner, MR(20, 196, 164, 306), 1, 4);
	else {
		RECT b = MR(20, 196, 164, 306);
		rrect(dc, b, C_HEAD, C_BORDER, 4);
		txt(dc, g_font, C_DIM, b, TXT_C, L"Banner");
	}

	rrect(dc, MR(184, 12, 668, 314), C_CARD, C_BORDER, 5);
	rrect(dc, MR(185, 13, 667, 68), C_HEAD, C_HEAD, 4);
	fillr(dc, MR(185, 56, 667, 68), C_CARD);
	hbar(dc, 204, 260, 34, C_TEXT, 3);
	draw_logo_at(dc, MR(208, 84, 264, 140), nm, g_pv_logo, g_fontb);
	hbar(dc, 280, 440, 101, C_TEXT, 3);
	hbar(dc, 280, 400, 125, C_TEXT2, 2);
	hbar(dc, 208, 600, 179, C_TEXT2, 2);
	hbar(dc, 208, 540, 201, C_TEXT2, 2);
	hbar(dc, 0, 680, 320, C_BORDER, 1);
	rrect(dc, MR(492, 330, 572, 364), C_HEAD, C_BTNBD, 4);
	rrect(dc, MR(580, 330, 660, 364), C_ACCENT, C_ACCENT, 4);

	for (i = 0; i < g_nit; ++i) {
		RECT r;
		COLORREF c;
		if (g_it[i].kind != IK_HOT) continue;
		r = g_it[i].r;
		c = g_hov == i ? C_ACCHOV : C_ACCENT;
		InflateRect(&r, S(2), S(2));
		line(dc, r.left, r.top, r.right, r.top, c, 2);
		line(dc, r.left, r.bottom, r.right, r.bottom, c, 2);
		line(dc, r.left, r.top, r.left, r.bottom, c, 2);
		line(dc, r.right, r.top, r.right, r.bottom, c, 2);
	}
	txt(dc, g_font, C_TEXT2, R(208, 278, 548, 312), TXT_W,
		L"Click a highlighted spot, or use Choose. Every image is optional.");
#undef MR
}

static void draw_welcome(HDC dc)
{
	draw_logo(dc, L"Installer Maker");
	txt(dc, g_fontt, C_TEXT, R(280, 88, 650, 114), TXT_L, L"Installer Maker");
	txt(dc, g_font, C_TEXT2, R(280, 114, 650, 136), TXT_L, L"Package your app into a single setup.exe");
	txt(dc, g_font, C_TEXT, R(208, 168, 650, 190), TXT_L, L"Pick your app's folder, name it, and get a ready-to-share installer");
	txt(dc, g_font, C_TEXT, R(208, 190, 650, 212), TXT_L, L"with a setup wizard, shortcuts and an uninstaller.");
	txt(dc, g_font, C_TEXT2, R(208, 236, 650, 258), TXT_L, L"It takes about a minute.");
}

static void draw_fields(HDC dc)
{
	int i;
	for (i = 0; i < F_COUNT; ++i) {
		RECT b;
		if (!field_visible(i)) continue;
		b = R(g_f[i].x1, g_f[i].y1, g_f[i].x2, g_f[i].y2);
		rrect(dc, b, C_HEAD, GetFocus() == g_f[i].h ? C_ACCENT : C_BORDER, 8);
	}
}

static void draw_done(HDC dc)
{
	int cx = S(236), cy = S(112), rad = S(28);
	if (g_ok) {
		draw_check_badge(dc);
		txt(dc, g_fontt, C_TEXT, R(280, 90, 650, 116), TXT_L, L"Installer created");
		txt(dc, g_font, C_TEXT2, R(280, 116, 650, 138), TXT_L, g_build_out);
		txt(dc, g_font, C_TEXT2, R(208, 144, 650, 166), TXT_L, L"Share this one file. Double-click it to run the setup wizard.");
	}
	else {
		circle(dc, cx, cy, rad, RGB(0xe2, 0x4b, 0x4a));
		txt(dc, g_fontt, C_WHITE, RAW(cx - rad, cy - rad, cx + rad, cy + rad), TXT_C, L"!");
		txt(dc, g_fontt, C_TEXT, R(280, 90, 650, 116), TXT_L, L"Couldn't build the installer");
		txt(dc, g_font, C_TEXT2, R(280, 124, 650, 280), TXT_W, g_err);
	}
}

static void draw_all(HDC dc, int w, int h)
{
	int i, nav = nav_of(g_page);
	WCHAR buf[32];

	fillr(dc, RAW(0, 0, w, h), C_BG);
	if (g_light) line(dc, 0, 0, w, 0, C_BTNBD, 1);

	rrect(dc, R(12, 12, 172, 314), C_PANEL, C_BORDER, 12);
	for (i = 0; i < NAV_COUNT; ++i) {
		int y = 24 + i * 38;
		if (i == nav) rrect(dc, R(20, y, 164, y + 34), C_SEL, C_SEL, 6);
		txt(dc, g_font, i == nav ? C_TEXT : C_TEXT2, R(34, y, 160, y + 34), TXT_L, s_nav[i]);
	}

	rrect(dc, R(184, 12, 668, 314), C_CARD, C_BORDER, 12);
	rrect(dc, R(185, 13, 667, 68), C_HEAD, C_HEAD, 11);
	fillr(dc, R(185, 56, 667, 68), C_CARD);
	line(dc, S(185), S(56), S(667), S(56), C_BORDER, 1);
	txt(dc, g_fontb, C_TEXT, R(204, 12, 360, 56), TXT_L, s_title[g_page]);
	txt(dc, g_font, C_TEXT2, R(360, 12, 618, 56), TXT_L, s_sub[g_page]);

	line(dc, 0, S(320), w, S(320), C_BORDER, 1);
	wsprintfW(buf, L"Step %d of %d", nav + 1, NAV_COUNT);
	txt(dc, g_font, C_TEXT2, R(16, 326, 110, 368), TXT_L, buf);
	if (g_msg[0]) txt(dc, g_font, C_ERR, R(112, 326, 396, 368), TXT_L, g_msg);

	switch (g_page) {
	case PG_WELCOME: draw_welcome(dc); break;
	case PG_BUILD:   draw_progress_page(dc, g_pct, g_status, g_log, g_nlog, g_building); break;
	case PG_DONE:    draw_done(dc); break;
	case PG_DESIGN:  draw_design(dc); break;
	default:         draw_fields(dc); break;
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

static void init_fields(HWND hwnd, HINSTANCE hi)
{
	static const struct { int page, x1, y1, x2, y2, maxlen; } d[F_COUNT] = {
		{ PG_DETAILS, 208, 94, 478, 124, 63 },
		{ PG_DETAILS, 490, 94, 650, 124, 31 },
		{ PG_DETAILS, 208, 156, 650, 186, 63 },
		{ PG_DETAILS, 208, 218, 650, 248, 159 },
		{ PG_FILES, 208, 148, 544, 174, MAX_PATH - 1 },
		{ PG_FILES, 208, 220, 544, 246, 259 },
		{ PG_OPTIONS, 208, 254, 650, 284, 63 },
		{ PG_OUTPUT, 208, 94, 544, 124, MAX_PATH - 1 },
		{ PG_FILES, 208, 148, 650, 174, 511 },
		{ PG_FILES, 208, 274, 650, 300, 64 }
	};
	int i;
	for (i = 0; i < F_COUNT; ++i) {
		g_f[i].page = d[i].page;
		g_f[i].x1 = d[i].x1; g_f[i].y1 = d[i].y1; g_f[i].x2 = d[i].x2; g_f[i].y2 = d[i].y2;
		g_f[i].maxlen = d[i].maxlen;
		g_f[i].h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
			0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(IDC_FIELD + i), hi, NULL);
		SendMessageW(g_f[i].h, WM_SETFONT, (WPARAM)g_font, FALSE);
		SendMessageW(g_f[i].h, EM_LIMITTEXT, (WPARAM)d[i].maxlen, 0);
		g_editproc = (WNDPROC)SetWindowLongPtrW(g_f[i].h, GWLP_WNDPROC, (LONG_PTR)edit_proc);
	}
	SetWindowTextW(g_f[F_VERSION].h, L"1.0.0");
	position_fields();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		g_hwnd = hwnd;
		init_fields(hwnd, ((CREATESTRUCTW*)lp)->hInstance);
		build_items();
		update_fields();
		return 0;

	case WM_PAINT:      on_paint(hwnd); return 0;
	case WM_ERASEBKGND: return 1;

	case WM_CTLCOLOREDIT: {
		HDC dc = (HDC)wp;
		SetTextColor(dc, C_TEXT);
		SetBkColor(dc, C_HEAD);
		return (LRESULT)g_editbrush;
	}

	case WM_COMMAND:
		if (LOWORD(wp) >= IDC_FIELD && LOWORD(wp) < IDC_FIELD + F_COUNT) {
			if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)
				InvalidateRect(hwnd, NULL, FALSE);
			else if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_FIELD + F_OUT) {
				update_note();
				InvalidateRect(hwnd, NULL, FALSE);
			}
			else if (HIWORD(wp) == EN_CHANGE && g_msg[0]) {
				g_msg[0] = 0;
				InvalidateRect(hwnd, NULL, FALSE);
			}
		}
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
			if (g_page == PG_DONE) activate(ID_FINISH);
			else if (g_page != PG_BUILD) activate(ID_NEXT);
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
		g_building = 0;
		g_ok = (int)wp;
		if (g_ok) g_pct = 100;
		lstrcpynW(g_err, t ? t : L"", 300);
		if (t) HeapFree(GetProcessHeap(), 0, t);
		set_page(PG_DONE);
		return 0;
	}

	case WM_SETTINGCHANGE:
		if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0 &&
			system_prefers_light() != g_light)
			apply_theme(hwnd, system_prefers_light());
		break;

	case WM_CLOSE:
		if (confirm_exit()) DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
	WNDCLASSW wc;
	RECT rc;
	MSG m;
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

	(void)hprev; (void)cmd; (void)show;
	cfg_defaults(&g_build_cfg);
	draw_init();

	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.lpszClassName = L"InstallerMakerWnd";
	RegisterClassW(&wc);

	rc = R(0, 0, 680, 372);
	AdjustWindowRect(&rc, style, FALSE);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Installer Maker", style,
		(GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
		(GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
		rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
	if (!g_hwnd) return 1;

	dark_title_bar(g_hwnd);
	set_page(PG_WELCOME);
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	m.wParam = 0;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	draw_shutdown();
	return (int)m.wParam;
}
