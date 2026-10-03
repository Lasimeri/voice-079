#!/bin/bash
# At logout or shutdown: what changes without a command (079's volume) into
# the kept state, so the next login brings it back (see ~/tts079/state079).
exec "$HOME/tts079/state079" snapshot
