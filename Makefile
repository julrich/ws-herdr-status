# ws-herdr-status — the two halves of the desk companion.
#
#   bridge-*      PC side: reads herdr over its Unix socket, serves /state on the LAN
#   fw-*          device side: build/flash/monitor the ESP32-C6 firmware
#   ui-test       render the companion UI on the host and assert on pixels
#
# The device firmware needs `. ~/esp/esp-idf/export.sh` in its shell; IDF_EXPORT
# is where that script lives. Recipes do not echo themselves, so
# `make bridge-once | jq ...` works.

IDF_EXPORT ?= $(HOME)/esp/esp-idf/export.sh

.PHONY: bridge bridge-once bridge-service-install bridge-service-status \
        fw-build fw-flash fw-monitor ui-test rotation-test gesture-test

bridge:
	@python3 bridge/herdr_status_bridge.py

bridge-once:
	@python3 bridge/herdr_status_bridge.py --once

bridge-service-install:
	@mkdir -p ~/.config/systemd/user && cp bridge/herdr-status-bridge.service ~/.config/systemd/user/ && systemctl --user daemon-reload && systemctl --user enable --now herdr-status-bridge.service

bridge-service-status:
	@systemctl --user status herdr-status-bridge.service --no-pager

fw-build:
	@bash -lc '. $(IDF_EXPORT) && idf.py build'

fw-flash:
	@bash -lc '. $(IDF_EXPORT) && idf.py -p /dev/ttyACM0 flash'

fw-monitor:
	@bash -lc '. $(IDF_EXPORT) && idf.py -p /dev/ttyACM0 monitor'

ui-test:
	@bash tools/ui_host_test/run.sh

rotation-test:
	@bash tools/rotation_test/run.sh

gesture-test:
	@bash tools/gesture_test/run.sh