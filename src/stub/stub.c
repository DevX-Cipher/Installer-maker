#include "common.h"
#include "install.h"
#include "payload.h"
#include <shellapi.h>
#include <wchar.h>

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
	int argc = 0, i, silent = 0, existing = 0, allusers = 0, rc;
	WCHAR** argv;
	InstallOpts o;
	HANDLE mutex;
	WCHAR dir0[MAX_PATH], space[96], mname[160];
	unsigned long long total;

	(void)hi; (void)hprev; (void)cmd; (void)show;
	if (is_uninstall_mode()) return uninstall_main();

	if (!payload_load_config(&g_cfg)) {
		MessageBoxW(NULL,
			L"This is the setup template, not a finished installer.\n\nUse Installer Maker to create one.",
			L"Installer Maker", MB_OK | MB_ICONINFORMATION);
		return 1;
	}

	ZeroMemory(&o, sizeof(o));
	o.desktop = g_cfg.desktop;
	o.context = g_cfg.ctx_offer;
	o.allusers = g_cfg.allusers;
	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (i = 1; argv && i < argc; ++i) {
		if (!lstrcmpiW(argv[i], L"/S")) silent = 1;
		else if (!lstrcmpiW(argv[i], L"/allusers")) o.allusers = 1;
		else if (!lstrcmpiW(argv[i], L"/user")) o.allusers = 0;
		else if (!lstrcmpiW(argv[i], L"/nodesktop")) o.desktop = 0;
		else if (!lstrcmpiW(argv[i], L"/noctx")) o.context = 0;
		else if (!lstrcmpiW(argv[i], L"/child")) o.child = 1;
		else if (!_wcsnicmp(argv[i], L"/dir=", 5)) lstrcpynW(o.dir, argv[i] + 5, MAX_PATH);
	}
	if (argv) LocalFree(argv);

	if (silent) {
		WCHAR err[256];
		if (!o.dir[0]) default_install_dir(o.allusers, o.dir, MAX_PATH);
		return run_install_flow(&o, NULL, NULL, err, 256) ? 0 : 1;
	}

	wsprintfW(mname, L"Local\\imk-setup-%s", g_cfg.id);
	mutex = CreateMutexW(NULL, TRUE, mname);
	if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

	if (find_existing_install(dir0, MAX_PATH, &allusers)) {
		WCHAR u[MAX_PATH];
		path_join(u, MAX_PATH, dir0, UNINSTALL_EXE);
		existing = (GetFileAttributesW(u) != INVALID_FILE_ATTRIBUTES);
	}
	else { allusers = g_cfg.allusers; default_install_dir(allusers, dir0, MAX_PATH); }

	total = payload_peek_total();
	if (g_cfg.source_link)
		lstrcpyW(space, L"Setup downloads the app files, so an internet connection is needed.");
	else if (total) wsprintfW(space, L"About %u MB of disk space required.",
		(unsigned)(total / (1024 * 1024)) + 1);
	else lstrcpyW(space, L"Warning: the installer data is missing.");

	rc = ui_setup_run(dir0, allusers, existing, space);

	if (mutex) CloseHandle(mutex);
	return rc;
}