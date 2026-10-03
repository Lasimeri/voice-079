        # The window: on the desktop whenever the model is loaded and
        # running, for as long as this session lasts (a keeper detached from
        # this shell; PHI_STREAM_WINDOW=0: none).
        if [ "${PHI_STREAM_WINDOW:-1}" != 0 ]; then
            setsid "$0" window --keep < /dev/null > /dev/null 2>&1 &
            echo "a window with the terminal opens on the desktop when the model is running ($0 window)"
        fi
        ;;
    window)
        # A terminal window on the desktop running `attach --follow`: the
        # diagnostics and the input, reconnecting across restarts and
        # reloading onto each build. Alone: open it now unless it is open.
        # `--keep` (what `start` runs): while the service's session lasts,
        # whenever the model is running and no window is open (not yet, or
        # closed), open one; checked every 3 s.
        keep=0
        for a in "$@"; do
            [ "$a" = --keep ] && keep=1
        done
        if [ "$keep" = 0 ]; then
            if alive "$winpid"; then
                echo "the window is already open (pid $(cat "$winpid"))"
                exit 0
            fi
            open_window
            exit
        fi
        alive "$keeppid" && exit 0
        echo $$ > "$keeppid"
        while tmux has-session -t "$session" 2> /dev/null; do
            if ! alive "$winpid" && running; then
                open_window || true
            fi
            sleep 3
        done
        rm -f "$keeppid"
        ;;
    stop)
        # The model stops, and its window with it (a restart by `quit` and
        # `start` keeps the window, which reconnects): the keeper first, so
        # it cannot reopen it while the service is going.
        if alive "$keeppid"; then
            kill "$(cat "$keeppid")" 2> /dev/null || true
        fi
        rm -f "$keeppid"
        "$bin" quit 2>/dev/null || true
        sleep 2
        tmux kill-session -t "$session" 2>/dev/null || true
        if alive "$winpid"; then
            kill "$(cat "$winpid")" 2> /dev/null || true
        fi
        rm -f "$winpid"
        echo "stopped"
        ;;
