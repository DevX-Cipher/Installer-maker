#ifndef COMMON_H
#define COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <shlobj.h>
#include <stddef.h>

#define UNINSTALL_EXE  L"uninstall.exe"
#define UNINSTALL_TMP  L"imk-uninstall-"
#define LIST_FILE      L"uninstall.lst"
#define REGVIEW        KEY_WOW64_64KEY

typedef struct {
  WCHAR name[64];          /* product name: title, folder, shortcuts */
  WCHAR version[32];
  WCHAR publisher[64];
  WCHAR description[160];  /* one line shown on the welcome page */
  WCHAR exe[260];          /* main executable, relative to the install folder */
  WCHAR ctxlabel[64];      /* Explorer context menu text */
  int   ctx_offer;         /* show the context menu option in setup */
  int   desktop;           /* desktop shortcut checked by default */
  int   allusers;          /* all users checked by default */
  int   launch;            /* "launch" checked on the finish page */
  unsigned logo_len;       /* bytes of the logo image stored after the config text */
  unsigned banner_len;     /* bytes of the sidebar banner image stored after the logo */
  WCHAR id[64];            /* derived: registry-safe name */
  WCHAR uninstall_key[200];
  WCHAR context_key[200];
} AppConfig;

extern AppConfig g_cfg;

void cfg_defaults(AppConfig* c);
void cfg_finish(AppConfig* c);                       /* derive id and registry keys */
int  cfg_serialize(const AppConfig* c, char* out, size_t cap);
int  cfg_parse(AppConfig* c, const char* text);
int  cfg_name_is_valid(const WCHAR* name);

typedef struct {
  WCHAR dir[MAX_PATH];
  int allusers;   /* install for all users (Program Files, HKLM) */
  int desktop;    /* create desktop shortcut */
  int context;    /* add Explorer context menu entry */
  int child;      /* running as the elevated helper of another installer */
} InstallOpts;

typedef void (*ProgressCb)(void* ud, int percent, const WCHAR* text, int step);

void path_join(WCHAR* out, size_t cap, const WCHAR* a, const WCHAR* b);
int  is_elevated(void);
int  make_dirs(const WCHAR* path);
int  rel_path_is_safe(const char* utf8_rel);

void default_install_dir(int allusers, WCHAR* out, size_t cap);
int  read_install_location(HKEY root, WCHAR* out, size_t cap);
int  find_existing_install(WCHAR* dir, size_t cap, int* allusers);

int  create_shortcut(const WCHAR* lnk, const WCHAR* target, const WCHAR* workdir, const WCHAR* desc);
void start_menu_dir(int allusers, WCHAR* out, size_t cap);
void desktop_link(int allusers, WCHAR* out, size_t cap);

int  register_uninstall(int allusers, const WCHAR* dir, DWORD size_kb);
void unregister_uninstall(int allusers);
int  set_context_menu(int allusers, const WCHAR* exe, int enable);

#endif