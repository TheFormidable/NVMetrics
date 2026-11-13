#!/bin/bash
# test_nvmetrics_setting.sh: the idle-mode switch edits only the user's block of a copy of nvrm610.conf. Runs anywhere.
set -u
H=$(cd "$(dirname "$0")" && pwd); S="$H/nvmetrics-setting.sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export NVMETRICS_CONF_OWNER_UID=$(id -u)
fails=0; arm() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails+1)); fi; }
C=$T/nvrm610.conf
printf 'NVMTL_A=1\nNVRM_610_BOOST=1\nNVK_B=2\n' > $C; chmod 644 $C
arm "status reads the shipped line (boost on = reposo off)" '[ "$($S reposo status $C)" = "{\"reposo\":\"off\"}" ]'
arm "on prints the new state" '[ "$($S reposo on $C)" = "{\"reposo\":\"on\"}" ]'
arm "on adds a user block at the end with the key" '[ "$(tail -n 3 $C | tr "\n" "|")" = "# --- my settings, kept on update ---|NVRM_610_BOOST=0|# --- end my settings ---|" ]'
arm "the shipped lines are untouched" '[ "$(head -n 3 $C | tr "\n" "|")" = "NVMTL_A=1|NVRM_610_BOOST=1|NVK_B=2|" ]'
arm "mode is kept" '[ "$( [ "$(uname)" = Darwin ] && stat -f %Lp $C || stat -c %a $C)" = 644 ]'
$S reposo on $C >/dev/null
arm "on twice keeps one line" '[ $(grep -c "^NVRM_610_BOOST=0" $C) = 1 ]'
arm "off takes our line out and status follows the shipped value" '[ "$($S reposo off $C)" = "{\"reposo\":\"off\"}" ] && ! grep -q "^NVRM_610_BOOST=0" $C'
arm "off leaves the (now empty) block and the other lines" '[ $(wc -l < $C) = 5 ] && grep -q "^NVK_B=2" $C'
printf 'X=1\n# --- my settings, kept on update ---\nNVMTL_FPS_LIMIT=Game=60,*=0\nNVRM_610_BOOST=1\n# --- end my settings ---\n' > $C
$S reposo on $C >/dev/null
arm "an existing block keeps the user's other settings and loses their old boost line" 'grep -q "^NVMTL_FPS_LIMIT=Game=60,\*=0" $C && ! grep -q "^NVRM_610_BOOST=1" $C && [ "$($S reposo status $C)" = "{\"reposo\":\"on\"}" ]'
chmod 666 $C
arm "a file others can write is refused and not changed" '! $S reposo off $C 2>/dev/null && grep -q "^NVRM_610_BOOST=0" $C'
chmod 644 $C; ln -s $C $T/link
arm "a symbolic link is refused" '! $S reposo on $T/link 2>/dev/null'
arm "a missing file is refused" '! $S reposo on $T/nope 2>/dev/null'
arm "a wrong owner is refused" '! NVMETRICS_CONF_OWNER_UID=4242 $S reposo on $C 2>/dev/null'
arm "bad arguments are refused" '! $S reposo max $C 2>/dev/null && ! $S boost on $C 2>/dev/null'
arm "no temporary file is left behind" '[ -z "$(ls -A $T | grep nvmetrics)" ]'
echo "$fails failed"; [ $fails = 0 ]
