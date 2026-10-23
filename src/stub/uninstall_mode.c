#include "common.h"
#include "install.h"
#include "payload.h"
#include <shellapi.h>
#include <string.h>
#include <wchar.h>

static int s_silent;

static void msgbox(const WCHAR* text, UINT icon)
{
  WCHAR title[100];
  wsprintfW(title, L"%s uninstall", g_cfg.name);
  if (!s_silent) MessageBoxW(NULL, text, title, MB_OK | icon);
}

static void delete_retry(const WCHAR* path)
{
  int i;
  SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
  for (i = 0; i < 20; ++i) {
    DWORD e;
    if (DeleteFileW(path)) return;
    e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
    Sleep(250);                     
  }
}

static void prune_dirs(const WCHAR* root, const WCHAR* file)
{
  WCHAR p[MAX_PATH];
  WCHAR* s;
  lstrcpynW(p, file, MAX_PATH);
  for (;;) {
    s = wcsrchr(p, L'\\');
    if (!s) break;
    *s = 0;
    if (lstrlenW(p) <= lstrlenW(root)) break;
    if (!RemoveDirectoryW(p)) break;
  }
}

static char* read_all(const WCHAR* path)
{
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  DWORD sz, rd = 0;
  char* buf;
  if (h == INVALID_HANDLE_VALUE) return NULL;
  sz = GetFileSize(h, NULL);
  if (sz == INVALID_FILE_SIZE || sz > 64u * 1024 * 1024) { CloseHandle(h); return NULL; }
  buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz + 1);
  if (!buf) { CloseHandle(h); return NULL; }
  if (!ReadFile(h, buf, sz, &rd, NULL)) rd = 0;
  buf[rd] = 0;
  CloseHandle(h);
  return buf;
}

static void rep(ProgressCb cb, void* ud, int pct, const WCHAR* t, int step)
{
  if (cb) cb(ud, pct, t, step);
}

void uninstall_remove(const WCHAR* dir, int allusers, ProgressCb cb, void* ud)
{
  WCHAR lst[MAX_PATH], full[MAX_PATH], rel[MAX_PATH], sm[MAX_PATH], lnk[MAX_PATH], tmp[MAX_PATH];
  char* buf;
  char* p;
  size_t total;
  int last = -1;

  rep(cb, ud, 0, L"Removing program files...", 0);

  path_join(lst, MAX_PATH, dir, LIST_FILE);
  buf = read_all(lst);
  if (buf) {
    total = strlen(buf);
    p = buf;
    while (*p) {
      char line[512];
      char* nl = strchr(p, '\n');
      size_t n = nl ? (size_t)(nl - p) : strlen(p);
      WCHAR* w;
      if (n >= sizeof(line)) n = sizeof(line) - 1;
      memcpy(line, p, n);
      while (n && line[n - 1] == '\r') --n;
      line[n] = 0;
      p += (nl ? (size_t)(nl - p) + 1 : strlen(p));

      {
        int pct = (int)((size_t)(p - buf) * 80 / total);
        if (pct != last) { last = pct; rep(cb, ud, pct, L"Removing files...", 0); }
      }

      if (!line[0] || !rel_path_is_safe(line)) continue;
      if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, rel, MAX_PATH)) continue;
      for (w = rel; *w; ++w) if (*w == L'/') *w = L'\\';
      path_join(full, MAX_PATH, dir, rel);
      delete_retry(full);
      prune_dirs(dir, full);
    }
    HeapFree(GetProcessHeap(), 0, buf);
  }
  rep(cb, ud, 80, L"Removed program files", 1);

  delete_retry(lst);
  path_join(full, MAX_PATH, dir, UNINSTALL_EXE);
  delete_retry(full);
  RemoveDirectoryW(dir);              

  desktop_link(allusers, lnk, MAX_PATH);
  if (lnk[0]) delete_retry(lnk);
  start_menu_dir(allusers, sm, MAX_PATH);
  if (sm[0]) {
    wsprintfW(tmp, L"%s.lnk", g_cfg.name);
    path_join(lnk, MAX_PATH, sm, tmp);
    delete_retry(lnk);
    wsprintfW(tmp, L"Uninstall %s.lnk", g_cfg.name);
    path_join(lnk, MAX_PATH, sm, tmp);
    delete_retry(lnk);
    RemoveDirectoryW(sm);
  }
  rep(cb, ud, 90, L"Removed shortcuts", 1);

  set_context_menu(allusers, NULL, 0);
  unregister_uninstall(allusers);
  rep(cb, ud, 100, L"Removed registry entries", 1);
}

static void self_delete(void)
{
  WCHAR self[MAX_PATH], cmd[2 * MAX_PATH];
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;

  GetModuleFileNameW(NULL, self, MAX_PATH);
  wsprintfW(cmd, L"cmd.exe /C ping 127.0.0.1 -n 3 >nul & del /F /Q \"%s\"", self);
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static int scope_of(const WCHAR* dir)
{
  WCHAR loc[MAX_PATH];
  if (read_install_location(HKEY_CURRENT_USER, loc, MAX_PATH) && !lstrcmpiW(loc, dir)) return 0;
  if (read_install_location(HKEY_LOCAL_MACHINE, loc, MAX_PATH) && !lstrcmpiW(loc, dir)) return 1;
  if (read_install_location(HKEY_LOCAL_MACHINE, loc, MAX_PATH)) return 1;
  return 0;
}

static int stage1(void)
{
  WCHAR self[MAX_PATH], dir[MAX_PATH], tmpdir[MAX_PATH], tmpexe[MAX_PATH], cmd[1000];
  WCHAR* s;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  int all;

  GetModuleFileNameW(NULL, self, MAX_PATH);
  lstrcpynW(dir, self, MAX_PATH);
  s = wcsrchr(dir, L'\\');
  if (!s) return 1;
  *s = 0;

  all = scope_of(dir);
  if (all && !is_elevated()) {
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = s_silent ? L"/S" : L"";
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    return 0;
  }

  if (!s_silent && !ui_uninstall_confirm(dir)) return 0;

  GetTempPathW(MAX_PATH, tmpdir);
  wsprintfW(tmpexe, L"%s" UNINSTALL_TMP L"%lu.exe", tmpdir, GetTickCount());
  if (!CopyFileW(self, tmpexe, FALSE)) {
    msgbox(L"Could not start the uninstaller (temp copy failed).", MB_ICONERROR);
    return 1;
  }

  wsprintfW(cmd, L"\"%s\" /tmp /dir=\"%s\" %s%s", tmpexe, dir,
    all ? L"/allusers" : L"/user", s_silent ? L" /S" : L"");
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    msgbox(L"Could not start the uninstaller.", MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}

static int stage2(const WCHAR* dir, int allusers)
{
  if (!dir[0]) return 1;
  if (s_silent) {
    Sleep(800);                       
    uninstall_remove(dir, allusers, NULL, NULL);
  }
  else {
    ui_uninstall_progress(dir, allusers);
  }
  self_delete();
  return 0;
}

int is_uninstall_mode(void)
{
  WCHAR self[MAX_PATH];
  const WCHAR* base;
  GetModuleFileNameW(NULL, self, MAX_PATH);
  base = wcsrchr(self, L'\\');
  base = base ? base + 1 : self;
  return !lstrcmpiW(base, UNINSTALL_EXE) || !_wcsnicmp(base, UNINSTALL_TMP, lstrlenW(UNINSTALL_TMP));
}

int uninstall_main(void)
{
  int argc = 0, i, tmpmode = 0, allusers = 0;
  WCHAR** argv;
  WCHAR dir[MAX_PATH];

  dir[0] = 0;
  if (!payload_load_config(&g_cfg)) return 1;

  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (i = 1; argv && i < argc; ++i) {
    if (!lstrcmpiW(argv[i], L"/S")) s_silent = 1;
    else if (!lstrcmpiW(argv[i], L"/tmp")) tmpmode = 1;
    else if (!lstrcmpiW(argv[i], L"/allusers")) allusers = 1;
    else if (!lstrcmpiW(argv[i], L"/user")) allusers = 0;
    else if (!_wcsnicmp(argv[i], L"/dir=", 5)) lstrcpynW(dir, argv[i] + 5, MAX_PATH);
  }
  if (argv) LocalFree(argv);

  return tmpmode ? stage2(dir, allusers) : stage1();
}