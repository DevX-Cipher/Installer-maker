#include "install.h"
#include "payload.h"
#include <shellapi.h>
#include <string.h>
#include <wchar.h>

static void report(ProgressCb cb, void* ud, int pct, const WCHAR* text, int step)
{
  if (cb) cb(ud, pct, text, step);
}

static void set_err(WCHAR* err, size_t cap, const WCHAR* msg)
{
  if (err && cap) lstrcpynW(err, msg, (int)cap);
}

static int write_entry(const WCHAR* full, const PayloadEntry* e,
  unsigned long long* done, unsigned long long total,
  ProgressCb cb, void* ud, int* lastpct, DWORD* werr)
{
  HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  unsigned long long off = 0;

  if (h == INVALID_HANDLE_VALUE) { *werr = GetLastError(); return 0; }
  while (off < e->size) {
    DWORD chunk = (e->size - off > (1u << 20)) ? (1u << 20) : (DWORD)(e->size - off);
    DWORD wr = 0;
    int pct;
    if (!WriteFile(h, e->data + off, chunk, &wr, NULL) || wr != chunk) {
      *werr = GetLastError();
      CloseHandle(h);
      return 0;
    }
    off += chunk;
    *done += chunk;
    pct = (int)(*done * 90 / total);
    if (pct != *lastpct) { *lastpct = pct; report(cb, ud, pct, NULL, 0); }
  }
  CloseHandle(h);
  return 1;
}

static int make_uninstaller(const WCHAR* dst)
{
  WCHAR zone[MAX_PATH + 20];
  if (!payload_write_uninstaller(dst)) return 0;
  wsprintfW(zone, L"%s:Zone.Identifier", dst);
  DeleteFileW(zone);
  return 1;
}

int do_install(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap)
{
  Payload pl;
  PayloadEntry e;
  HANDLE hl;
  unsigned long long done = 0, total;
  int lastpct = -1, rc, comok;
  DWORD werr = 0, wr;
  WCHAR lst[MAX_PATH], exe[MAX_PATH], unx[MAX_PATH], sm[MAX_PATH], lnk[MAX_PATH];
  WCHAR rel[MAX_PATH], full[MAX_PATH], par[MAX_PATH], tmp[700];
  WCHAR* p;

  report(cb, ud, 0, L"Unpacking installer data...", 0);
  if (!payload_open(&pl)) {
    set_err(err, errcap, L"The installer data is missing or damaged. Download the installer again.");
    return 0;
  }
  total = pl.total ? pl.total : 1;

  report(cb, ud, 0, L"Creating the install folder...", 0);
  if (!make_dirs(o->dir)) {
    set_err(err, errcap, L"Could not create the install folder. Choose a different folder.");
    return 0;
  }

  path_join(lst, MAX_PATH, o->dir, LIST_FILE);
  hl = CreateFileW(lst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hl == INVALID_HANDLE_VALUE) {
    set_err(err, errcap, L"Could not write to the install folder. Choose a different folder.");
    return 0;
  }

  while ((rc = payload_next(&pl, &e)) > 0) {
    if (!rel_path_is_safe(e.path)) {
      CloseHandle(hl);
      set_err(err, errcap, L"The installer data contains an invalid path.");
      return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, e.path, -1, rel, MAX_PATH);
    for (p = rel; *p; ++p) if (*p == L'/') *p = L'\\';
    path_join(full, MAX_PATH, o->dir, rel);

    lstrcpynW(par, full, MAX_PATH);
    p = wcsrchr(par, L'\\');
    if (p) { *p = 0; make_dirs(par); }

    wsprintfW(tmp, L"Copying %s", rel);
    report(cb, ud, lastpct < 0 ? 0 : lastpct, tmp, 0);

    if (!write_entry(full, &e, &done, total, cb, ud, &lastpct, &werr)) {
      CloseHandle(hl);
      wsprintfW(tmp, L"Could not write %s (error %lu). If %s is running, close it and try again.",
        rel, werr, g_cfg.name);
      set_err(err, errcap, tmp);
      return 0;
    }
    WriteFile(hl, e.path, (DWORD)strlen(e.path), &wr, NULL);
    WriteFile(hl, "\n", 1, &wr, NULL);
  }
  CloseHandle(hl);
  if (rc < 0) {
    set_err(err, errcap, L"The installer data is damaged. Download the installer again.");
    return 0;
  }

  path_join(exe, MAX_PATH, o->dir, g_cfg.exe);
  path_join(unx, MAX_PATH, o->dir, UNINSTALL_EXE);
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
    wsprintfW(tmp, L"%s is missing from the installer.", g_cfg.exe);
    set_err(err, errcap, tmp);
    return 0;
  }
  if (!make_uninstaller(unx)) {
    set_err(err, errcap, L"Could not create the uninstaller.");
    return 0;
  }
  report(cb, ud, 90, L"Copied program files", 1);

  report(cb, ud, 92, L"Creating shortcuts...", 0);
  comok = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
  start_menu_dir(o->allusers, sm, MAX_PATH);
  if (sm[0]) {
    make_dirs(sm);
    wsprintfW(tmp, L"%s.lnk", g_cfg.name);
    path_join(lnk, MAX_PATH, sm, tmp);
    create_shortcut(lnk, exe, o->dir, g_cfg.name);
    wsprintfW(tmp, L"Uninstall %s.lnk", g_cfg.name);
    path_join(lnk, MAX_PATH, sm, tmp);
    create_shortcut(lnk, unx, o->dir, L"Uninstall");
  }
  if (o->desktop) {
    desktop_link(o->allusers, lnk, MAX_PATH);
    if (lnk[0]) create_shortcut(lnk, exe, o->dir, g_cfg.name);
  }
  if (comok) CoUninitialize();
  report(cb, ud, 95, L"Created shortcuts", 1);

  if (o->context) set_context_menu(o->allusers, exe, 1);
  register_uninstall(o->allusers, o->dir, (DWORD)(total / 1024));
  report(cb, ud, 100, L"Registered the uninstaller", 1);
  return 1;
}

static int run_elevated(const InstallOpts* o, WCHAR* err, size_t cap)
{
  WCHAR exe[MAX_PATH], params[1000];
  SHELLEXECUTEINFOW sei;
  DWORD code = 1;

  GetModuleFileNameW(NULL, exe, MAX_PATH);
  wsprintfW(params, L"/S /child /allusers /dir=\"%s\"%s%s",
    o->dir, o->desktop ? L"" : L" /nodesktop", o->context ? L"" : L" /noctx");

  ZeroMemory(&sei, sizeof(sei));
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = exe;
  sei.lpParameters = params;
  sei.nShow = SW_HIDE;

  if (!ShellExecuteExW(&sei)) {
    set_err(err, cap, GetLastError() == ERROR_CANCELLED
      ? L"Administrator permission was declined."
      : L"Could not start the administrator install.");
    return 0;
  }
  WaitForSingleObject(sei.hProcess, INFINITE);
  GetExitCodeProcess(sei.hProcess, &code);
  CloseHandle(sei.hProcess);
  if (code != 0) {
    set_err(err, cap, L"The administrator install failed. Try a different folder, or install for the current user only.");
    return 0;
  }
  return 1;
}

int run_install_flow(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap)
{
  if (o->allusers && !is_elevated()) {
    report(cb, ud, 0, L"Waiting for administrator approval...", 0);
    if (!run_elevated(o, err, errcap)) return 0;
    report(cb, ud, 100, L"Installed for all users", 1);
    return 1;
  }
  return do_install(o, cb, ud, err, errcap);
}