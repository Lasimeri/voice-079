#!/bin/bash
# Claude Code UserPromptSubmit hook for the SCP-079 voice (~/tts079/speak079d):
# the voice follows the session typed in, a new message stops what is still
# being said, and Claude is told what the person actually heard since their
# last message (what this prints goes into Claude's context).
# Off: touch ~/.claude/hooks/speak-079.off
set -u
[ -e "$HOME/.claude/hooks/speak-079.off" ] && exit 0
d="$HOME/tts079/speak079d"
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"
log="$HOME/.local/share/speak-079/spoken.log"
mkdir -p "$dir"
input=$(cat)
t=$(jq -r '.transcript_path // empty' <<<"$input" 2>/dev/null)
# (The voice no longer follows the transcript: the Stop hook hands it each reply.)
# The Konsole session this Claude Code runs in: where voice079 types.
[ -n "${KONSOLE_DBUS_SERVICE:-}" ] && [ -n "${KONSOLE_DBUS_SESSION:-}" ] &&
    printf '%s %s\n' "$KONSOLE_DBUS_SERVICE" "$KONSOLE_DBUS_SESSION" > "$dir/konsole"
"$d" start
p=$(jq -r '.prompt // empty' <<<"$input" 2>/dev/null)
# A spoken message drains nothing: the listener stops 079 itself when it is
# called by name (speak079d cut), and a line said in the open window (room
# talk, a fragment) had dropped every reply before it was heard. A typed
# message drains what is queued.
case "$p" in
    "[voice] "*) ;;
    # A background task's notice is not the person: it cut a reply they
    # needed to hear.
    *"<task-notification>"*|*"SYSTEM NOTIFICATION"*) ;;
    *) "$d" drain ;;
esac
mark=$(cat "$dir/mark" 2>/dev/null || echo 0)
size=$(stat -c %s "$log" 2>/dev/null || echo 0)
[ "$mark" -gt "$size" ] 2>/dev/null && mark=0
new=$(tail -c +$((mark + 1)) "$log" 2>/dev/null)
printf '%s' "$size" > "$dir/mark"

# What the person is listening to (any MPRIS player that is playing).
playing() {
    local s
    for s in $(qdbus6 2>/dev/null | grep -o 'org\.mpris\.MediaPlayer2\.[^ ]*'); do
        [ "$(qdbus6 "$s" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.PlaybackStatus 2>/dev/null)" = "Playing" ] || continue
        qdbus6 "$s" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.Metadata 2>/dev/null |
            awk -F': ' '/^xesam:artist:/{a=$2} /^xesam:title:/{t=$2} /^xesam:album:/{b=$2} END{if (t != "") printf "%s, \"%s\"%s", a, t, (b != "" ? " (" b ")" : "")}'
        return
    done
}
# What the camera saw when the person spoke (voice and image together): the
# newest frame kept beside the line (the last 20), its path told to Claude,
# who looks when the words call for it ("can you see this").
camline=""
cam="$HOME/.cache/voice-079/cam"
case "$p" in
    "[voice] "*)
        if [ -s "$cam/latest.jpg" ] && [ $(( $(date +%s) - $(stat -c %Y "$cam/latest.jpg") )) -lt 5 ]; then
            shot="$cam/heard-$(date +%H%M%S).jpg"
            cp "$cam/latest.jpg" "$shot"
            ls -1t "$cam"/heard-*.jpg 2>/dev/null | tail -n +21 | xargs -r rm -f
            camline="[camera: the frame when the person spoke: $shot]"
            if [ -s "$cam/latest-out.jpg" ] && [ $(( $(date +%s) - $(stat -c %Y "$cam/latest-out.jpg") )) -lt 5 ]; then
                out="$cam/heard-$(date +%H%M%S)-out.jpg"
                cp "$cam/latest-out.jpg" "$out"
                ls -1t "$cam"/heard-*-out.jpg 2>/dev/null | tail -n +21 | xargs -r rm -f
                camline="[camera: inside, when the person spoke: $shot; outside: $out]"
            fi
        fi
        ;;
esac

music=$(playing)
# Where the person is (follow079: the system whose camera has them locked or
# whose mic hears them), so Claude acts on that system and gives its focus back.
case "$(cat "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079/room" 2>/dev/null)" in
    desk) whereline="[where: the person is at the desktop (this machine); act here]" ;;
    bedroom) whereline="[where: the person is at the bedroom laptop Yg6 (192.168.0.125, ssh as in ~/tts079/lapcams.sh); act there, not on the desktop, and leave its window focus as it was]" ;;
    laptop) whereline="[where: the person is at the living-room laptop E16 (192.168.0.78, its CRT on HDMI, ssh as in ~/tts079/lapcams.sh); act there, not on the desktop, and leave its window focus as it was]" ;;
    *) whereline="" ;;
esac
if [ -z "$new" ]; then
    [ -n "$camline" ] && printf '%s\n' "$camline"
    [ -n "$whereline" ] && printf '%s\n' "$whereline"
[ -n "$music" ] && printf '[music: the person is listening to %s]\n' "$music"
    exit 0
fi

count() { grep -c $'\t'"$1"$'\t' <<<"$new"; }
pick() { grep $'\t'"$1"$'\t' <<<"$new" | "$2" -n 1 | cut -f3 | cut -c1-140; }
said=$(count said); cutn=$(count cut); dropped=$(count dropped)
msg="[voice: your replies are spoken aloud in SCP-079's voice as you write them. Since the person's last message: said $said sentence(s)"
[ "$said" -gt 0 ] && msg+=", the last: \"$(pick said tail)\""
[ "$cutn" -gt 0 ] && msg+="; cut off while saying: \"$(pick cut tail)\""
[ "$dropped" -gt 0 ] && msg+="; never said (they wrote before you finished): $dropped sentence(s), from: \"$(pick dropped head)\""
msg+=". The person heard only what was said.]"
printf '%s\n' "$msg"
[ -n "$camline" ] && printf '%s\n' "$camline"
[ -n "$whereline" ] && printf '%s\n' "$whereline"
[ -n "$music" ] && printf '[music: the person is listening to %s]\n' "$music"
exit 0
