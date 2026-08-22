# Release Notes

## v0.2.0

Settings will now survive subsequent updates. Until now, every M5Burner
installation wiped Wi-Fi details, browser pairing, target weight, and shot
counter. Pump Bug now keeps them in a part of the device that updates don't
touch.

### New

- Guided backflush: a timer that paces five 10-second pump bursts with
  10-second rests, following the pump so the steps stay in sync.
- The live screen shows when the pump is winding down, before it is confirmed
  off, and saved shot charts mark the same moment.

### Improved reliability

- Saved shots are protected if device settings are cleared or lost, with the
  option to retry, erase, or carry on without recording.
- Shot weight is recovered if the scale tares mid-extraction, instead of
  reporting a yield that looks too low.

### Updating from 0.1.x

Install without erasing the whole flash: press Start without Erase, or clear
"Erase whole flash before burning" in web M5Burner. Saved shots carry over,
but settings reset once on this update — set your Wi-Fi, pairing, and target
again afterwards. From here on, updates keep both. A full-chip erase still
deletes everything, so download any shots you want to keep first.

