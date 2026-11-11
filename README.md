# JoyOff Lite

A simple Nintendo Switch homebrew app that puts original Joy-Con and Pro Controllers into shipment low-power mode and disconnects them, ready for long-term shelf storage.

## Features

- **Minimalist & Fast UI:** Uses standard Switch console output and touch support.
- **Automatic detection:** The app detects connected wireless Joy-Con L/R controllers and Pro Controllers.
- **Single & Bulk Management:** Turn off controllers individually or shut them all down at once with a single press.
- **Dual Input Modes:** Full navigation using physical buttons or touchscreen.


## Requirements

- A Nintendo Switch running custom firmware and firmware 13.0.0 or newer.
- Original Joy-Con L/R controllers or Pro Controllers connected wirelessly.
- Homebrew Menu in full application mode (title override).

MissionControl is not required. Joy-Con 2, Switch 2 Pro Controllers, NSO controllers (NES/SNES/N64/Genesis), and third-party controllers are outside the scope of this app.

## Usage

1. Extract the release ZIP to the SD card root and launch JoyOff Lite through the Homebrew Menu.
2. Detach your Joy-Cons and connect them wirelessly. Connect Pro Controllers over Bluetooth with the USB cable unplugged (or with *Pro Controller Wired Communication* off). Detection refreshes automatically.
3. Select a controller and press **A** or press **Minus (-)** to turn off all.

> [!NOTE]  
> You can use the touchscreen to exit if all controllers disconnect.

## Controls

### Physical Buttons

| Control  | Action |
| --- | --- |
| **Up / Down** | Navigate between available controllers |
| **A** | Turn off the selected controller (or Refresh if failed) |
| **Minus (-)** | Turn off all connected controllers |
| **X** | Refresh controller list |
| **Plus (+)** | Exit application |

### Touchscreen Controls

- **Controller Cards:** Tap any controller card to select it, turn it off, or retry a failed controller.
- **Bottom Buttons:**
  - **[A] TURN OFF SELECTED:** Turns off the currently selected controller.
  - **[-] TURN OFF ALL:** Initiates shutdown for all ready controllers.
  - **[X] REFRESH LIST:** Forces a refresh of connected Bluetooth devices.
  - **[+] EXIT:** Closes the application.

## How It Works & Shipment Mode

When a controller receives the shipment command:
1. Saved Bluetooth pairing state is reset.
2. The controller enters a low-power shipment mode and disconnects immediately.
3. Standard button presses will **not** wake the controller while in shipment mode.

**To wake & re-pair:**
- **All**: Press and hold the **SYNC** button
- **Joy-Cons:** Attach them physically to the Switch console rails.
- **Pro Controllers:** Connect them to the Switch console/dock via a USB cable.


## Build

Requires `devkitPro`, `devkitA64`, and `libnx`.

```sh
make -j4
```

## Credits and license

The Switch application is distributed under [LGPLv3](COPYING), with the accompanying [GPLv3 terms](COPYING.GPL).

Protocol and implementation references include [jc_toolkit](https://github.com/CTCaer/jc_toolkit), [joycon-turnoff](https://github.com/Sopsy/joycon-turnoff) (Pro Controller command pacing), [dekuNukem's reverse-engineering notes](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering), [libnx](https://github.com/switchbrew/libnx), and [MissionControl](https://github.com/ndeadly/MissionControl).
.

## Support My Work

If you find this project useful, please consider supporting me by buying me a coffee!

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/J2T723F0R2)