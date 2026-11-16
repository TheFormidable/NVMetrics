# Source from the developer build scripts. Works with macOS Bash 3.2.
# Select standard CPython 3.11–3.15 (PyInstaller 6.22.3 support range).
python_is_compatible() {
    [[ -n "$1" && -x "$1" ]] || return 1
    "$1" -I -c 'import sys, sysconfig, platform, venv, ensurepip; sys.exit(0 if (3,11) <= sys.version_info[:2] < (3,16) and platform.python_implementation() == "CPython" and not sysconfig.get_config_var("Py_GIL_DISABLED") else 1)' 2>/dev/null
}

find_build_python() {
    local version candidate
    # An explicit override is never silently ignored.
    if [[ -n "${NVMETRICS_BUILD_PYTHON:-}" ]]; then
        candidate="$(command -v "$NVMETRICS_BUILD_PYTHON" 2>/dev/null || true)"
        python_is_compatible "$candidate" && printf '%s\n' "$candidate"
        return
    fi
    for version in 3.13 3.14 3.12 3.11 3.15; do
        for candidate in \
            "/Library/Frameworks/Python.framework/Versions/$version/bin/python$version" \
            "/usr/local/opt/python@$version/bin/python$version" \
            "/usr/local/bin/python$version" \
            "$(command -v "python$version" 2>/dev/null || true)"; do
            if python_is_compatible "$candidate"; then printf '%s\n' "$candidate"; return 0; fi
        done
    done
    candidate="$(command -v python3 2>/dev/null || true)"
    if python_is_compatible "$candidate"; then printf '%s\n' "$candidate"; return 0; fi
    return 1
}
