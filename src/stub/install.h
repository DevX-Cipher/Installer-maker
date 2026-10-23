#ifndef INSTALL_H
#define INSTALL_H

#include "common.h"

int do_install(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap);
int run_install_flow(const InstallOpts* o, ProgressCb cb, void* ud, WCHAR* err, size_t errcap);

int is_uninstall_mode(void);
int uninstall_main(void);

int  ui_uninstall_confirm(const WCHAR* dir);
void ui_uninstall_progress(const WCHAR* dir, int allusers);
int  ui_setup_run(const WCHAR* dir, int allusers, int existing, const WCHAR* space);

void uninstall_remove(const WCHAR* dir, int allusers, ProgressCb cb, void* ud);

#endif