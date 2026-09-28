## Version History

### Changes in v1.1.0-rc3

* Added sudden drop suspicion: a fall steeper than sudden\_drop.slope\_mgl\_per\_10min (default 3 mg/L per 10 min, measured over 2 min) is faster than tank water can lose oxygen. Until the reading is back above every entry threshold, every level waits sudden\_drop.extend\_s (default 10 min) longer than its dwell. Logged, shown in the header and shaded on the chart
* Added alarm handling by day (night.day\_alarm): outside the night hours an alarm can sound as at night, acknowledge itself after 10 s, or stay silent. The level, its devices and the pushes are unchanged, the night always sounds
* Added the 2 minute slope to the status (probe.slope\_2min) and the console

### Changes in v1.1.0-rc2

* Removed effective level vs readable level hidden logic. Previously any device test failures would silently raise effective level above the current readable level, leading to confusion and unnecessary alarms. Now there is only one alert level, which is readable from the probe and shown on screen, no hidden states anymore


### Changes in v1.1.0-rc1

* Added a device registry of eight devices, each with one output slot: a relay (NO or NC wired), a Tuya plug or nothing. No two devices can share an output, and a device with a trigger, test, boost or window needs one
* Added Tuya Wi-Fi plugs as outputs, switched over the local network without the Tuya cloud (protocol 3.3, 3.4, 3.5). A plug that stops answering is logged and pushed, its local key never leaves the board
* Added output type descriptions as JSON Schema (GET /api/slots). The Config page builds each device's output editor from it, replacing the separate Tuya plug table and wired column
* Added always-on windows, up to four daily windows per device
* Added manual on / off / auto control for every device, holding until the device's next scheduled or triggered change
* Added device maintenance: a device can be taken out of order for 5-120 minutes, it stays off even in an alert
* Added exclusive test mode (on by default): during a test run only the tested device runs, and only such runs are judged
* Added a DO limit (test.no\_fail\_above\_mgl, default 6.1 mg/L) above which a missed test response is logged as unchecked instead of FAIL
* Added the reasons why a device is on to the device status and cards
* Added events for device maintenance, manual switching, lost and recovered plugs and the end of maintenance mode
* Renamed Service to Test everywhere (page, config keys, API, console), configurations with the old names still load
* Replaced the maintenance relay test with the manual switch: the device test on the Maintenance page switches a device on for 5 s through its relay or plug
* Split the Maintenance page into ReefDO maintenance and device maintenance
* Removed the effective level and the escalation of a failed device's level (escalate\_if\_failed): the level is only what the readings support, and a failed test verdict changes nothing that runs or sounds. FAULT still shows as FAULT and runs the devices of fault.level
* Added a manual clear for a failed test verdict (Test page, device card, console test clear, {"clear":{"device":n}})
* Updated the status buzzer field to name the pattern actually playing, removed the status led field
* Fixed maintenance mode lasting until switched off, it now ends by itself after 30 minutes as documented, with a countdown in the header
* Fixed probe settings (stuck time, sample period, zero calibration) taking effect only after a reboot
* Fixed an applied but unsaved configuration reporting success
* Fixed the factory defaults warning not showing after a configuration reset
* Fixed the empty console status json
* Fixed the Wi-Fi status occasionally being read half-updated
* Fixed the test failure push naming the wrong device

Upgrade notes:

* Configurations, test history and logs of 1.0 carry over; saving the configuration stores the new format
* Do not downgrade to 1.0 with a configuration saved by 1.1: 1.0 would treat every device as NO wired. Load a 1.0 configuration right after a downgrade

