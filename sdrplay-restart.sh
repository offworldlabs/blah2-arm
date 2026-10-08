#!/bin/bash
# Runs before blah2 on every container start (see the compose `command`).
#
# It no longer kills anything. Its two `pkill -x` lines never matched in the
# container: Linux truncates process names to 15 characters, so
# sdrplay_apiService is "sdrplay_apiServ", and the only blah2 it could find was
# one in another container. Resetting the SDRplay service belongs to the host
# side, which holds /data/retina-gui/restart.lock: retina-gui's apply and mode
# switches, and the owl-os watchdog. A kill here would add a second, unsettled
# service restart to each of those.
#
# The pause stays: several of those paths start blah2 straight after restarting
# the service, and this is their only gap. The file stays because the compose
# files call it by path.

sleep 2
