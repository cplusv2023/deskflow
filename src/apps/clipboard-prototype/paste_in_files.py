#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Invoke Nautilus's real clipboard paste action in one dedicated test window.

Requires the system Python packages python3-gi and gir1.2-atspi-2.0, an active
desktop session, and a running clipboard publisher. This helper deliberately
does not call Nautilus FileOperations2.CopyURIs: that would bypass clipboard
negotiation and would not exercise the M2 integration.
"""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time
import warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path, help="Dedicated, existing destination directory")
    parser.add_argument("--open", action="store_true", help="Open a new Files window first")
    parser.add_argument("--timeout", type=float, default=15.0, help="Window wait timeout, in seconds")
    args = parser.parse_args()
    result = {"status": "error", "action": "view.paste", "opened": False}

    try:
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise RuntimeError("--timeout must be finite and positive")
        target = args.target.expanduser().resolve(strict=True)
        if not target.is_dir():
            raise RuntimeError("The target must be an existing directory")
        if not target.name:
            raise RuntimeError("Use a dedicated destination directory, not a filesystem root")
        result["target"] = str(target)
        result["window"] = target.name

        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        # get_action_name is the available GI API on Ubuntu, despite its warning.
        warnings.filterwarnings("ignore", category=DeprecationWarning, module=__name__)

        def matching_windows():
            matches = []
            desktop = Atspi.get_desktop(0)
            if desktop is None:
                raise RuntimeError("The AT-SPI desktop is unavailable")
            for application in desktop:
                if application.get_name() != "org.gnome.Nautilus":
                    continue
                for window in application:
                    if window.get_name() == target.name:
                        matches.append(window)
            return matches

        if args.open:
            if matching_windows():
                raise RuntimeError("A Files window has this basename already; omit --open to use it")
            executable = shutil.which("nautilus")
            if executable is None:
                raise RuntimeError("nautilus is not installed")
            subprocess.Popen(
                [executable, "--new-window", str(target)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            result["opened"] = True

        started = time.monotonic()
        while True:
            matches = matching_windows()
            if len(matches) > 1:
                raise RuntimeError("Multiple Files windows have this basename; refusing to choose")
            if matches:
                window = matches[0]
                for index in range(window.get_n_actions()):
                    if window.get_action_name(index) == "view.paste":
                        if not window.do_action(index):
                            raise RuntimeError("Files rejected the view.paste accessibility action")
                        result.update(
                            status="invoked",
                            action_index=index,
                            elapsed_seconds=round(time.monotonic() - started, 3),
                        )
                        print(json.dumps(result, ensure_ascii=False))
                        return 0
            if time.monotonic() - started >= args.timeout:
                raise RuntimeError("No unique Files window exposing view.paste appeared before timeout")
            time.sleep(0.2)
    except Exception as error:
        result["error"] = str(error)
        print(json.dumps(result, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
