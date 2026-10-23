#include "common.h"
#include <string.h>
#include <stdlib.h>

AppConfig g_cfg;

void cfg_defaults(AppConfig* c)
{
  ZeroMemory(c, sizeof(*c));
  lstrcpyW(c->version, L"1.0.0");
  lstrcpyW(c->ctxlabel, L"Open with app");
  c->desktop = 1;
}

int cfg_name_is_valid(const WCHAR* name)
{
  const WCHAR* p;
  int n = lstrlenW(name);
  if (n == 0 || name[0] == L' ' || name[n - 1] == L' ' || name[n - 1] == L'.') return 0;
  for (p = name; *p; ++p)
    if (*p < 32 || wcschr(L"\\/:*?\"<>|", *p)) return 0;
  return 1;
}

void cfg_finish(AppConfig* c)
{
  int i, n = 0;
  for (i = 0; c->name[i] && n < 63; ++i) {
    WCHAR ch = c->name[i];
    int ok = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
    c->id[n++] = ok ? ch : L'_';
  }
  c->id[n] = 0;
  wsprintfW(c->uninstall_key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\imk_%s", c->id);
  wsprintfW(c->context_key, L"Software\\Classes\\*\\shell\\imk_%s", c->id);
}

static int put(char* out, size_t cap, size_t* pos, const char* key, const WCHAR* wval, int ival, int isint)
{
  char val[1600];
  int n;
  if (isint) wsprintfA(val, "%d", ival);
  else {
    WCHAR clean[520];
    int i, k = 0;
    for (i = 0; wval[i] && k < 519; ++i) if (wval[i] >= 32) clean[k++] = wval[i];
    clean[k] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, clean, -1, val, sizeof(val), NULL, NULL)) val[0] = 0;
  }
  n = (int)(strlen(key) + 1 + strlen(val) + 1);
  if (*pos + (size_t)n + 1 > cap) return 0;
  memcpy(out + *pos, key, strlen(key)); *pos += strlen(key);
  out[(*pos)++] = '=';
  memcpy(out + *pos, val, strlen(val)); *pos += strlen(val);
  out[(*pos)++] = '\n';
  out[*pos] = 0;
  return 1;
}

int cfg_serialize(const AppConfig* c, char* out, size_t cap)
{
  size_t pos = 0;
  if (!cap) return 0;
  out[0] = 0;
  if (!(put(out, cap, &pos, "name", c->name, 0, 0) &&
    put(out, cap, &pos, "version", c->version, 0, 0) &&
    put(out, cap, &pos, "publisher", c->publisher, 0, 0) &&
    put(out, cap, &pos, "description", c->description, 0, 0) &&
    put(out, cap, &pos, "exe", c->exe, 0, 0) &&
    put(out, cap, &pos, "ctxlabel", c->ctxlabel, 0, 0) &&
    put(out, cap, &pos, "ctx", NULL, c->ctx_offer, 1) &&
    put(out, cap, &pos, "desktop", NULL, c->desktop, 1) &&
    put(out, cap, &pos, "allusers", NULL, c->allusers, 1) &&
    put(out, cap, &pos, "launch", NULL, c->launch, 1) &&
    put(out, cap, &pos, "logo_len", NULL, (int)c->logo_len, 1) &&
    put(out, cap, &pos, "banner_len", NULL, (int)c->banner_len, 1) &&
    put(out, cap, &pos, "source_link", NULL, c->source_link, 1) &&
    put(out, cap, &pos, "url", c->url, 0, 0) &&
    put(out, cap, &pos, "sha256", c->sha256, 0, 0))) return 0;
  return (int)pos;
}

static void set_w(WCHAR* dst, int cap, const char* utf8)
{
  if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, dst, cap)) dst[0] = 0;
}

int cfg_parse(AppConfig* c, const char* text)
{
  const char* p = text;
  cfg_defaults(c);
  c->desktop = 0;
  while (*p) {
    char line[1300];
    const char* nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    char* eq;
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, p, n);
    while (n && line[n - 1] == '\r') --n;
    line[n] = 0;
    p += nl ? (size_t)(nl - p) + 1 : strlen(p);

    eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    if (!strcmp(line, "name")) set_w(c->name, 64, eq);
    else if (!strcmp(line, "version")) set_w(c->version, 32, eq);
    else if (!strcmp(line, "publisher")) set_w(c->publisher, 64, eq);
    else if (!strcmp(line, "description")) set_w(c->description, 160, eq);
    else if (!strcmp(line, "exe")) set_w(c->exe, 260, eq);
    else if (!strcmp(line, "ctxlabel")) set_w(c->ctxlabel, 64, eq);
    else if (!strcmp(line, "ctx")) c->ctx_offer = atoi(eq) != 0;
    else if (!strcmp(line, "desktop")) c->desktop = atoi(eq) != 0;
    else if (!strcmp(line, "allusers")) c->allusers = atoi(eq) != 0;
    else if (!strcmp(line, "launch")) c->launch = atoi(eq) != 0;
    else if (!strcmp(line, "logo_len")) c->logo_len = (unsigned)strtoul(eq, NULL, 10);
    else if (!strcmp(line, "banner_len")) c->banner_len = (unsigned)strtoul(eq, NULL, 10);
    else if (!strcmp(line, "banner_len")) c->banner_len = (unsigned)strtoul(eq, NULL, 10);
    else if (!strcmp(line, "source_link")) c->source_link = atoi(eq) != 0;
    else if (!strcmp(line, "url")) set_w(c->url, 512, eq);
    else if (!strcmp(line, "sha256")) set_w(c->sha256, 72, eq);
  }
  if (!c->name[0] || !c->exe[0]) return 0;
  cfg_finish(c);
  return 1;
}
