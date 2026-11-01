# Readiness gates — complete before claiming an end-to-end working project

- [x] Removed real ThingSpeak keys from candidate source (sample only).
- [x] Replaced topic suffix containing personal nickname/identifier with `CHANGE_TO_A_UNIQUE_NONPERSONAL_ID`.
- [x] Replaced historical TalkBack numeric IDs with `0` placeholders.
- [x] Excluded virtual environment, raw CSV telemetry logs, and obsolete gateway approaches.
- [x] Parsed both Wokwi JSON files and compiled Python source to bytecode for syntax inspection.
- [ ] Rotate/revoke the original ThingSpeak write and TalkBack keys.
- [ ] Choose distinct non-identifying MQTT topic namespaces (or authenticated TLS broker for real telemetry).
- [x] Both ESP32 sketches compiled successfully in [GitHub Actions](https://github.com/SarxanT/esp32-motor-condition-monitoring/actions/runs/41305301047) (1 November 2026).
- [ ] Verify Wokwi diagrams and runtime behavior in each independent project.
- [ ] Verify the gateway against controlled test payloads and cloud upload without leaking keys.
- [ ] Check fallback TalkBack behaviors and acknowledge/report failures honestly.
- [ ] Replace configuration placeholders in private files only, then check `git diff` and history.
- [ ] Add clear demo media (optional) and test results.
- [x] Clearly label the published source as **experimental and unverified**.
- [ ] Update README with any new **actually performed** test results.

**Never publish the original `.venv/`, the source archive's real `config.json`/`secrets.h`, or personal telemetry datasets.**
