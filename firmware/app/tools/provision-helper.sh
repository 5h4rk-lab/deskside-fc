#!/usr/bin/env bash
# Stores the helper's URL and token in the board's NVS over the serial console
# (:set-url, :set-token), then checks with :show-config that the board shows
# only a masked token. The token is read from a file and written straight to
# the serial port by bash builtins: it never appears on the screen, in a log,
# in a command line (process list) or in this script's output.
#
# Usage: tools/provision-helper.sh [--port /dev/ttyACM0] [--url https://...]
#                                  [--env-file ~/.config/soccer-display/helper.env]
#                                  [--restart]
#   --url       default: $HELPER_URL from the env file if it has one, else
#               https://soccer-display-helper.vercel.app
#   --env-file  a file with a line HELPER_TOKEN=<token> (and optionally HELPER_URL=...)
#   --restart   restart the board afterwards so the new values are used
#               (they are used only from the next start)
# Exit status 0 when the board confirmed both values and showed a masked token.
set -u

port=/dev/ttyACM0
url=""
env_file="$HOME/.config/soccer-display/helper.env"
restart=0
while [ $# -gt 0 ]; do
  case "$1" in
    --port) port="$2"; shift 2 ;;
    --url) url="$2"; shift 2 ;;
    --env-file) env_file="$2"; shift 2 ;;
    --restart) restart=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ ! -r "$env_file" ]; then
  echo "cannot read $env_file" >&2
  exit 2
fi
if [ ! -e "$port" ]; then
  echo "no serial port $port (is the board plugged in?)" >&2
  exit 2
fi

# Read the values with bash only (no grep/cut/sed on the token: their command
# lines would not show it, but keep every copy inside this shell).
token=""
file_url=""
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    HELPER_TOKEN=*) token="${line#HELPER_TOKEN=}" ;;
    HELPER_URL=*) file_url="${line#HELPER_URL=}" ;;
  esac
done < "$env_file"
# Optional surrounding quotes.
token="${token%\"}"; token="${token#\"}"; token="${token%\'}"; token="${token#\'}"
file_url="${file_url%\"}"; file_url="${file_url#\"}"
[ -z "$url" ] && url="${file_url:-https://soccer-display-helper.vercel.app}"

if [ -z "$token" ]; then
  echo "no HELPER_TOKEN line in $env_file" >&2
  exit 2
fi
if [ "${#token}" -lt 16 ] || [ "${#token}" -gt 128 ]; then
  echo "the token in $env_file has ${#token} characters (16 to 128 needed)" >&2
  token=""
  exit 2
fi

stty -F "$port" 115200 raw -echo -hupcl || exit 2

out=$(mktemp)
chmod 600 "$out"
trap 'rm -f "$out"; token=""' EXIT

# Listen while the commands go out (bounded: never a monitor left running).
timeout 8 cat "$port" > "$out" &
reader=$!
sleep 0.5
printf ':set-url %s\n' "$url" > "$port"
sleep 0.5
printf ':set-token %s\n' "$token" > "$port"
sleep 0.8
printf ':show-config\n' > "$port"
wait "$reader" 2>/dev/null

answer=$(tr -d '\r' < "$out")
leaked=0
case "$answer" in *"$token"*) leaked=1 ;; esac
token=""
if [ "$leaked" = 1 ]; then
  echo "FAIL the board printed the token: not showing its output" >&2
  exit 1
fi

# Only the command answers (no secret can be in them: checked above).
printf '%s\n' "$answer" | while IFS= read -r l; do
  case "$l" in "[cmd]"*) echo "board: $l" ;; esac
done

fail=0
case "$answer" in *"[cmd] helper URL saved"*) echo "OK   URL saved" ;; *) echo "FAIL URL not confirmed"; fail=1 ;; esac
case "$answer" in *"[cmd] helper token saved"*) echo "OK   token saved" ;; *) echo "FAIL token not confirmed"; fail=1 ;; esac
case "$answer" in *"[cmd] helper url "*"token "????"... ("*" characters)"*) echo "OK   :show-config shows a masked token" ;;
  *) echo "FAIL :show-config did not show a masked token"; fail=1 ;; esac

if [ "$restart" = 1 ] && [ "$fail" = 0 ]; then
  printf ':restart\n' > "$port"
  echo "restart sent: the board uses the new values from now on"
fi
exit "$fail"
