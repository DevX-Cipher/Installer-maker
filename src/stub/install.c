#include "install.h"
#include "payload.h"
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "unzip.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

static void report(ProgressCb cb, void* ud, int pct, const WCHAR* text, int step)
{
  if (cb) cb(ud, pct, text, step);
}

static void set_err(WCHAR* err, size_t cap, const WCHAR* msg)
{
  if (err && cap) lstrcpynW(err, msg, (int)cap);
}

static int finish_install(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap,
  unsigned long long total);

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

/* ---------- download from a link ---------- */

typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Sha;

static int sha_begin(Sha* s)
{
  memset(s, 0, sizeof(*s));
  if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
  if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(s->alg, 0);
    s->alg = NULL;
    return 0;
  }
  return 1;
}

static void sha_free(Sha* s)
{
  if (s->h) BCryptDestroyHash(s->h);
  if (s->alg) BCryptCloseAlgorithmProvider(s->alg, 0);
  s->h = NULL; s->alg = NULL;
}

static void sha_finish(Sha* s, WCHAR* hex)
{
  static const WCHAR hx[] = L"0123456789abcdef";
  UCHAR d[32];
  int i;
  hex[0] = 0;
  if (s->h && BCryptFinishHash(s->h, d, 32, 0) == 0) {
    for (i = 0; i < 32; ++i) { hex[i * 2] = hx[d[i] >> 4]; hex[i * 2 + 1] = hx[d[i] & 15]; }
    hex[64] = 0;
  }
  sha_free(s);
}

static int download_file(const WCHAR* url, const WCHAR* dest, WCHAR* hex, ProgressCb cb, void* ud,
  int pct0, int pct1, WCHAR* err, size_t errcap)
{
  URL_COMPONENTSW uc;
  WCHAR host[256], path[2048], extra[1024], tmp[300];
  HINTERNET ses = NULL, con = NULL, req = NULL;
  HANDLE f = INVALID_HANDLE_VALUE;
  Sha sha;
  unsigned char* buf = NULL;
  DWORD status = 0, sz, clen = 0, got = 0, wr = 0, t0, last = 0, code;
  unsigned long long done = 0;
  int ok = 0;

  memset(&sha, 0, sizeof(sha));
  hex[0] = 0;
  host[0] = path[0] = extra[0] = 0;

  ZeroMemory(&uc, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;    uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;     uc.dwUrlPathLength = 2048;
  uc.lpszExtraInfo = extra;  uc.dwExtraInfoLength = 1024;
  if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    set_err(err, errcap, L"The download link is not valid.");
    goto cleanup;
  }
  if (!path[0]) lstrcpyW(path, L"/");
  if (lstrlenW(path) + lstrlenW(extra) < 2048) lstrcatW(path, extra);

  if (!sha_begin(&sha)) {
    set_err(err, errcap, L"Could not start the checksum check.");
    goto cleanup;
  }

  ses = WinHttpOpen(L"Setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) ses = WinHttpOpen(L"Setup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) goto netfail;
  {
    DWORD tls = 0x2800;               /* TLS 1.2 + 1.3 */
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls))) {
      tls = 0x0800;                   /* older Windows: TLS 1.2 only */
      WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls));
    }
  }
  WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);

  con = WinHttpConnect(ses, host, uc.nPort, 0);
  if (!con) goto netfail;
  req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!req) goto netfail;
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
    !WinHttpReceiveResponse(req, NULL)) goto netfail;

  sz = sizeof(status);
  if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX) || status != 200) {
    wsprintfW(tmp, L"The server returned error %lu. Check the download link.", status);
    set_err(err, errcap, tmp);
    goto cleanup;
  }
  sz = sizeof(clen);
  if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
    WINHTTP_HEADER_NAME_BY_INDEX, &clen, &sz, WINHTTP_NO_HEADER_INDEX)) clen = 0;

  f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  buf = (unsigned char*)malloc(65536);
  if (f == INVALID_HANDLE_VALUE || !buf) {
    set_err(err, errcap, L"Could not save the download to a temporary file.");
    goto cleanup;
  }

  t0 = GetTickCount();
  for (;;) {
    if (!WinHttpReadData(req, buf, 65536, &got)) goto netfail;
    if (!got) break;
    if (!WriteFile(f, buf, got, &wr, NULL) || wr != got) {
      set_err(err, errcap, L"Could not save the download. Is the disk full?");
      goto cleanup;
    }
    BCryptHashData(sha.h, buf, got, 0);
    done += got;
    if (cb) {
      DWORD now = GetTickCount();
      if (now - last >= 150) {
        unsigned d10 = (unsigned)(done * 10 / 1048576ULL);
        unsigned s10 = (unsigned)(done * 10000ULL / (1048576ULL * (now - t0 ? now - t0 : 1)));
        int pct = clen ? pct0 + (int)((unsigned long long)(pct1 - pct0) * done / clen) : pct0;
        last = now;
        if (clen) {
          unsigned c10 = (unsigned)((unsigned long long)clen * 10 / 1048576ULL);
          wsprintfW(tmp, L"Downloading... %u.%u of %u.%u MB (%u.%u MB/s)",
            d10 / 10, d10 % 10, c10 / 10, c10 % 10, s10 / 10, s10 % 10);
        }
        else wsprintfW(tmp, L"Downloading... %u.%u MB", d10 / 10, d10 % 10);
        cb(ud, pct, tmp, 0);
      }
    }
  }
  if (clen && done != clen) {
    set_err(err, errcap, L"The download was interrupted. Check your internet connection and try again.");
    goto cleanup;
  }
  sha_finish(&sha, hex);
  ok = 1;
  goto cleanup;

netfail:
  code = GetLastError();
  wsprintfW(tmp, L"Couldn't reach the server. Check your internet connection and try again. (code %lu)", code);
  set_err(err, errcap, tmp);

cleanup:
  sha_free(&sha);
  free(buf);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  if (ses) WinHttpCloseHandle(ses);
  return ok;
}

static int do_install_link(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap)
{
  WCHAR lst[MAX_PATH], tdir[MAX_PATH], tmpf[MAX_PATH], hex[65];
  WCHAR rel[MAX_PATH], full[MAX_PATH], par[MAX_PATH], tmp[700];
  unsigned char magic[4];
  unsigned long long total = 0;
  LARGE_INTEGER fsz;
  HANDLE hl, hf;
  DWORD got = 0, wr;
  char u8[600];
  WCHAR* p;

  memset(magic, 0, sizeof(magic));
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
  if (!GetTempPathW(MAX_PATH, tdir) || !GetTempFileNameW(tdir, L"imk", 0, tmpf)) {
    CloseHandle(hl);
    set_err(err, errcap, L"Could not create a temporary file for the download.");
    return 0;
  }

  report(cb, ud, 0, L"Connecting...", 0);
  if (!download_file(g_cfg.url, tmpf, hex, cb, ud, 0, 60, err, errcap)) goto fail;
  report(cb, ud, 60, L"Downloaded files", 1);

  if (g_cfg.sha256[0]) {
    if (lstrcmpiW(hex, g_cfg.sha256) != 0) {
      set_err(err, errcap, L"The download doesn't match the expected checksum, so it was not installed.");
      goto fail;
    }
    report(cb, ud, 62, L"Checked the download", 1);
  }

  hf = CreateFileW(tmpf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (hf != INVALID_HANDLE_VALUE) { ReadFile(hf, magic, 4, &got, NULL); CloseHandle(hf); }

  if (got == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4) {
    if (!unzip_extract(tmpf, o->dir, cb, ud, 62, 90, hl, err, errcap, &total)) goto fail;
  }
  else {
    lstrcpynW(rel, g_cfg.exe, MAX_PATH);
    for (p = rel; *p; ++p) if (*p == L'/') *p = L'\\';
    path_join(full, MAX_PATH, o->dir, rel);
    lstrcpynW(par, full, MAX_PATH);
    p = wcsrchr(par, L'\\');
    if (p) { *p = 0; make_dirs(par); }
    report(cb, ud, 80, L"Copying files...", 0);
    if (!CopyFileW(tmpf, full, FALSE)) {
      wsprintfW(tmp, L"Could not write %s (error %lu). If %s is running, close it and try again.",
        g_cfg.exe, GetLastError(), g_cfg.name);
      set_err(err, errcap, tmp);
      goto fail;
    }
    hf = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
      if (GetFileSizeEx(hf, &fsz)) total = (unsigned long long)fsz.QuadPart;
      CloseHandle(hf);
    }
    if (WideCharToMultiByte(CP_UTF8, 0, g_cfg.exe, -1, u8, sizeof(u8), NULL, NULL)) {
      char* q;
      for (q = u8; *q; ++q) if (*q == '\\') *q = '/';
      WriteFile(hl, u8, (DWORD)strlen(u8), &wr, NULL);
      WriteFile(hl, "\n", 1, &wr, NULL);
    }
  }
  CloseHandle(hl);
  DeleteFileW(tmpf);
  return finish_install(o, cb, ud, err, errcap, total ? total : 1);

fail:
  CloseHandle(hl);
  DeleteFileW(tmpf);
  return 0;
}

int do_install(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap)
{
  Payload pl;
  PayloadEntry e;
  HANDLE hl;
  unsigned long long done = 0, total;
  int lastpct = -1, rc;
  DWORD werr = 0, wr;
  WCHAR lst[MAX_PATH];
  WCHAR rel[MAX_PATH], full[MAX_PATH], par[MAX_PATH], tmp[700];
  WCHAR* p;

  if (g_cfg.source_link) return do_install_link(o, cb, ud, err, errcap);

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

  return finish_install(o, cb, ud, err, errcap, total);
}

static int finish_install(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap,
  unsigned long long total)
{
  WCHAR exe[MAX_PATH], unx[MAX_PATH], sm[MAX_PATH], lnk[MAX_PATH], tmp[700];
  int comok;

  path_join(exe, MAX_PATH, o->dir, g_cfg.exe);
  path_join(unx, MAX_PATH, o->dir, UNINSTALL_EXE);
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
    wsprintfW(tmp, g_cfg.source_link ? L"%s was not found in the downloaded files."
      : L"%s is missing from the installer.", g_cfg.exe);
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