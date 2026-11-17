#!/bin/bash
# nvmetrics-setting.sh reposo status|on|off [conf]
#
# NVMetrics' "idle mode" switch. With it on, the driver's Vulkan layer stops asking the GPU for full clocks
# (NVRM_610_BOOST=0, the driver's nvk/nvk-perf-boost-burst.patch), so an idle desktop can drop to the GPU's low-power state; with it
# off the line is taken out again and the shipped default applies. The app runs this with an administrator prompt.
#
# The line lives in the user's own block of /Library/GPUBundles/nvmtl/nvrm610.conf ("# --- my settings, kept on
# update ---" ... "# --- end my settings ---"): the plugin reads the file top to bottom and the last line for a key wins,
# and the driver's package/install.sh keeps that block across updates. The block is created at the end of the file when missing.
# Nothing outside the block is touched. It takes effect for each app when it next starts (log out and in for all).
#
# The file must be a regular file (not a link) owned by root and not writable by others; anything else is refused and
# nothing changes. The new file is written next to the old one and renamed over it, keeping owner and mode.
# status prints {"reposo":"on"|"off"} from the last NVRM_610_BOOST line of the whole file (on = "0").
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
what=${1:-}; cmd=${2:-status}; CONF=${3:-/Library/GPUBundles/nvmtl/nvrm610.conf}
OWNER=${NVMETRICS_CONF_OWNER_UID:-0}            # a test points this at its own uid
START='# --- my settings, kept on update ---'; END='# --- end my settings ---'
KEY=NVRM_610_BOOST
die() { echo "nvmetrics-setting: $*" >&2; exit 2; }
[ "$what" = reposo ] || die "usage: nvmetrics-setting.sh reposo status|on|off [conf]"
case $cmd in status|on|off) ;; *) die "usage: nvmetrics-setting.sh reposo status|on|off [conf]";; esac

fstat() { if [ "$(uname)" = Darwin ]; then stat -f "$1" "$2"; else stat -c "$3" "$2"; fi; }   # GNU stat only for the Linux test
if [ "$cmd" != status ] && [ "$CONF" = /Library/GPUBundles/nvmtl/nvrm610.conf ]; then
    for dir in /Library /Library/GPUBundles /Library/GPUBundles/nvmtl; do
        [ -d "$dir" ] && [ ! -L "$dir" ] || die "unsafe configuration ancestor: $dir"
        [ "$(fstat %u "$dir" %u)" = 0 ] || die "non-root configuration ancestor: $dir"
        mode=$(fstat %Lp "$dir" %a)
        [ $(( 0$mode & 022 )) -eq 0 ] || die "writable configuration ancestor: $dir"
    done
fi
[ -e "$CONF" ] || die "$CONF does not exist (is the driver installed?)"
[ ! -L "$CONF" ] && [ -f "$CONF" ] || die "$CONF is not a regular file; refused"
[ "$(fstat %u "$CONF" %u)" = "$OWNER" ] || die "$CONF is not owned by root; refused"
mode=$(fstat %Lp "$CONF" %a)
[ $(( 0$mode & 022 )) -eq 0 ] || die "$CONF can be written by others; refused"

state() { v=$(grep -E "^[[:space:]]*$KEY[[:space:]]*=" "$CONF" | tail -n 1 | sed 's/^[^=]*=[[:space:]]*//; s/[[:space:]]*$//')
          [ "$v" = 0 ] && echo on || echo off; }
if [ "$cmd" = status ]; then echo "{\"reposo\":\"$(state)\"}"; exit 0; fi

dir=$(dirname "$CONF"); tmp=$(mktemp "$dir/.nvmetrics.XXXXXX") || die "cannot write in $dir"
trap 'rm -f "$tmp"' EXIT
# copy the file without our key inside the user's block; add the key (on) just before the block's end; make a block
# when there is none
awk -v S="$START" -v E="$END" -v K="$KEY" -v on="$([ "$cmd" = on ] && echo 1 || echo 0)" '
  BEGIN { inb = 0; seen = 0 }
  $0 == S { inb = 1; seen = 1; print; next }
  $0 == E && inb { if (on == 1) print K "=0"; inb = 0; print; next }
  inb && $0 ~ "^[[:space:]]*" K "[[:space:]]*=" { next }
  { print }
  END { if (!seen && on == 1) { print S; print K "=0"; print E } }
' "$CONF" > "$tmp" || die "could not rewrite $CONF"
chmod "$mode" "$tmp" && chown "$OWNER" "$tmp" 2>/dev/null || [ "$(id -u)" != 0 ] || die "could not set owner and mode"
mv -f "$tmp" "$CONF" || die "could not replace $CONF"
trap - EXIT
echo "{\"reposo\":\"$(state)\"}"
