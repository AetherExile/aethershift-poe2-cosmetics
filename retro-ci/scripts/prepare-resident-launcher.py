#!/usr/bin/env python3
"""Generate the browser-free resident launcher translation unit.

The checked-in launcher keeps the proven loopback service used by the native
frontend for emulator handoffs. This deterministic transform adds the real
RTPN00001 dashboard lifecycle, stages the title metadata in both /user/app and
/user/appmeta (matching the validated PS5 registration layout), and wraps every
launcher-owned hbldr transition so the watcher cannot confuse internal process
churn with a new user click.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one occurrence, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "core/standalone_fs.h"\n#include "route.h"\n',
        '#include "core/standalone_fs.h"\n#include "route.h"\n#include "dashboard_watch.h"\n',
        "dashboard watcher include",
    )

    text = replace_once(
        text,
        '#define RP_APP_DIR RP_APP_ROOT "/" RP_TITLE_ID\n',
        '#define RP_APP_DIR RP_APP_ROOT "/" RP_TITLE_ID\n'
        '#define RP_APPMETA_ROOT "/user/appmeta"\n'
        '#define RP_APPMETA_DIR RP_APPMETA_ROOT "/" RP_TITLE_ID\n',
        "appmeta path definitions",
    )

    host_block = '''    if (hbldr_prepare_host() != 0) {
        launcher_log("Sony BigApp host preparation failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 13;
    }
'''
    text = replace_once(
        text,
        host_block,
        '''    if (rp_prepare_real_title_registration() != 0) {
        launcher_log("browser-free RTPN00001 registration cleanup failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 13;
    }
    if (hbldr_prepare_host() != 0) {
        launcher_log("RTPN00001 Sony BigApp host preparation failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 14;
    }
''',
        "real-title registration and host preparation",
    )

    icon_write_block = '''    if (update_file_atomic(
            icon_path,
            rp_icon_png,
            (size_t)rp_icon_png_size,
            0644,
            &changed) != 0) {
        launcher_log("dashboard icon write failed");
        goto terminate;
    }
'''
    text = replace_once(
        text,
        icon_write_block,
        icon_write_block
        + '''
    /* Sony's title registration pipeline keeps XMB metadata under
     * /user/appmeta/<TITLE_ID> as well as the source title directory. Keep
     * our real-title registration in the same shape so metadata remains
     * verifiable even if AppInst no longer exposes /user/app/.../sce_sys. */
    if (mkdir_if_needed(RP_APPMETA_ROOT) != 0 ||
        mkdir_if_needed(RP_APPMETA_DIR) != 0) {
        launcher_log("dashboard appmeta directories could not be created");
        goto terminate;
    }
    {
        char appmeta_param[384];
        char appmeta_icon[384];
        snprintf(appmeta_param, sizeof(appmeta_param), "%s/param.json", RP_APPMETA_DIR);
        snprintf(appmeta_icon, sizeof(appmeta_icon), "%s/icon0.png", RP_APPMETA_DIR);
        if (update_file_atomic(
                appmeta_param,
                rp_tile_param,
                (size_t)rp_tile_param_size,
                0644,
                &changed) != 0) {
            launcher_log("dashboard appmeta param write failed");
            goto terminate;
        }
        if (update_file_atomic(
                appmeta_icon,
                rp_icon_png,
                (size_t)rp_icon_png_size,
                0644,
                &changed) != 0) {
            launcher_log("dashboard appmeta icon write failed");
            goto terminate;
        }
    }
''',
        "appmeta staging",
    )

    text = replace_once(
        text,
        '    child = hbldr_launch(RP_ROOT, RP_UI_PATH, stdio_descriptor, argv, envp);\n',
        '    rp_dashboard_internal_begin();\n'
        '    child = hbldr_launch(RP_ROOT, RP_UI_PATH, stdio_descriptor, argv, envp);\n'
        '    rp_dashboard_internal_end();\n',
        "legacy /launch transition guard",
    )

    # sceSystemServiceLaunchApp supplies the BigApp argv[0]. Upstream websrv
    # therefore passes only the user arguments to hbldr_launch(); hbldr later
    # replaces argv[0] with the target ELF path itself. Passing request->path as
    # an additional element makes emulators see their own ELF as argv[1] and the
    # ROM as argv[2], which is fatal for single-ROM frontends such as Mednafen.
    text = replace_once(
        text,
        '''    char *argv[3];
    char *envp[2];
''',
        '''    char *argv[2];
    char *envp[2];
''',
        "homebrew argv capacity",
    )
    text = replace_once(
        text,
        '''    argv[0] = (char *)request->path;
    argv[1] = request->args[0] ? (char *)request->args : NULL;
    argv[2] = NULL;
''',
        '''    argv[0] = request->args[0] ? (char *)request->args : NULL;
    argv[1] = NULL;
''',
        "upstream-compatible homebrew argv",
    )

    homebrew_launch = '''    child = hbldr_launch(
        request->cwd,
        request->path,
        stdio_descriptor,
        argv,
        envp);
'''
    text = replace_once(
        text,
        homebrew_launch,
        '''    rp_dashboard_internal_begin();
    child = hbldr_launch(
        request->cwd,
        request->path,
        stdio_descriptor,
        argv,
        envp);
    rp_dashboard_internal_end();
''',
        "emulator transition guard",
    )

    ready_block = '''    launcher_log(
        "ready address=%s port=%d routes=/launch,/hbldr,/status,/shutdown websrv=unused title=%s",
        RP_SERVICE_ADDRESS,
        RP_SERVICE_PORT,
        RP_TITLE_ID);
'''
    text = replace_once(
        text,
        ready_block,
        '''    if (rp_dashboard_watch_start() != 0) {
        launcher_log("browser-free dashboard watcher could not start errno=%d", errno);
        close(listener);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 16;
    }
    launcher_log(
        "ready address=%s port=%d routes=/hbldr,/status,/shutdown entry=real-title-no-webkit title=%s",
        RP_SERVICE_ADDRESS,
        RP_SERVICE_PORT,
        RP_TITLE_ID);
''',
        "watcher startup",
    )

    required = (
        "rp_prepare_real_title_registration",
        "rp_dashboard_watch_start",
        "rp_dashboard_internal_begin",
        "rp_dashboard_internal_end",
        "entry=real-title-no-webkit",
        "RP_APPMETA_DIR",
        "dashboard appmeta param write failed",
        "argv[0] = request->args[0]",
    )
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"browser-free launcher hook missing: {marker}")

    if 'argv[0] = (char *)request->path;' in text:
        raise RuntimeError("resident launcher still prepends the ELF path to user argv")

    args.destination.write_text(
        "/* Generated by scripts/prepare-resident-launcher.py. Do not edit directly. */\n"
        + text,
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
