#!/bin/sh
# Regression test for the Aether line editor (ae/readline.ae), driven
# through the REPL (`mqjs -i`) with scripted keystrokes — control bytes
# stand in for the keys a terminal would send. Each case pipes an input
# byte sequence and asserts the evaluated result appears in the output.
#
# Usage: tests/test_readline.sh path/to/mqjs
set -e

MQJS="${1:-./mqjs}"
if [ ! -x "$MQJS" ]; then
    echo "usage: $0 path/to/mqjs" >&2
    exit 2
fi

fails=0

# strip terminal escape/control noise, leaving the logical text
strip() {
    sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b//g' | tr -d '\r\b'
}

# check NAME INPUT EXPECT — pipe INPUT to the REPL, assert EXPECT is present.
# INPUT is a printf format string with \xNN / \n escapes interpreted here.
check() {
    name="$1"; input="$2"; expect="$3"
    out=$(printf "%b" "$input" | "$MQJS" -i 2>&1 | strip)
    if printf '%s' "$out" | grep -qF "$expect"; then
        echo "ok   $name"
    else
        echo "FAIL $name (want '$expect')"
        printf '%s\n' "$out" | sed 's/^/     /'
        fails=$((fails + 1))
    fi
}

# Control bytes use POSIX octal escapes (\0NNN) so `printf %b` interprets
# them under any /bin/sh (dash's %b does not accept \xNN).
#   ^A=\001  ^D=\004  ^K=\013  ^U=\025  ^W=\027  ^Y=\031  DEL=\0177  ESC=\033

# basic evaluation
check "plain"        '1+2\n'                   '3'
# backspace (DEL) removes the bad char: 12X<bs>+3 => 15
check "backspace"    '12X\0177+3\n'            '15'
# kill-line ^U clears the line: junk^U (6*7) => 42. A space after ^U keeps
# the octal escape from absorbing the following digit.
check "kill_line"    'junk\025 6*7\n'          '42'
# delete-char ^D after ^A home: X12 -> delete X -> 12
check "delete_char"  'X12\001\004\n'           '12'
# arrow-left (ESC [ D) then insert: 13 <left> 2 => 123
check "arrow_insert" '13\033[D2\n'             '123'
# yank ^Y restores killed text: 2*4 ^U ^Y evaluates the yanked expr
check "yank"         '2*4\025\031\n'           '8'
# UTF-8 (café: c3 a9) round trips through the buffer
check "utf8_2byte"   '"caf\0303\0251".length\n' '4'
# history recall via up-arrow (ESC [ A) re-evaluates the prior line
check "history_up"   '7*7\n\033[A\n'           '49'

echo
if [ "$fails" -eq 0 ]; then
    echo "readline: all editing scenarios passed"
    exit 0
else
    echo "readline: $fails failure(s)"
    exit 1
fi
