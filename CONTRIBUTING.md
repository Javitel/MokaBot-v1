# Contributing to MokaBot

Thanks for your interest in improving MokaBot. A few guidelines keep things
manageable:

- **Be constructive.** Feedback is welcome when it helps the code or the
  people working on it. Explain the why, not just the what.
- **Test on real hardware.** Say which board you used (WT32-SC01 or Waveshare
  ESP32-S3-Touch-LCD-3.5B) and what you tested. Heating hardware is involved:
  never weaken the safety checks (heater off when the sensor is disconnected,
  manual power cut-off) without discussing it first.
- **AI-assisted code is fine when used with care.** Only submit code you have
  read, understood and tested. No large automated refactors nobody can explain.
- **Keep changes focused.** One feature or fix per pull request, with a short
  description of what changed and why.
- **Build both boards** before opening a pull request: `pio run` builds every
  environment (the build workflow does the same on each push).

## Reporting issues

Please include:
- the board and the firmware version (shown in the OTA popup, or
  `custom_firmware_version` in `platformio.ini`);
- what you expected and what happened;
- the serial log, especially around a reboot (the `rst:` line and any
  `Guru Meditation` / backtrace).

## License

By contributing you agree that your contributions are licensed under the
project's license, [CC BY-NC-SA 4.0](LICENSE).
