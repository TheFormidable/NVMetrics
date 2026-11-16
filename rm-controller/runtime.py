#!/usr/bin/env python3
"""Frozen entry point. onedir only: independent rescue must retain its PID/FDs."""
import os
import sys


def main():
    if len(sys.argv) < 2:
        return 2
    role = sys.argv.pop(1)
    if role == 'client':
        from nvmetrics_ipc_v08 import main as client_main
        return client_main()
    if role not in ('server', 'rescue', 'install') or os.geteuid() != 0:
        print('privileged role requires root', file=sys.stderr)
        return 2
    if role == 'server':
        from server import main as server_main
        return server_main()
    if role == 'rescue':
        from rescue import main as rescue_main
        return rescue_main()
    from package_install import main as install_main
    return install_main()


if __name__ == '__main__':
    raise SystemExit(main())
