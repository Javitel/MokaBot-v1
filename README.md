# MokaBot-v1
Hello everyone, after a long time, some unexpected paperwork, many refactors and some support from many friends I've found during the last months I decided to publish the code and documents to build your own MokaBot. But first I need to address some points:

 - I am not a microcontroller expert by any means and I do not pretend to be one, I know basic Arduino enough to do some tricks but this is a whole different level, consider this whenever you comment about the code.

- I know some basic object-oriented languages (Java) and C, but never worked seriously with C++, as said earlier, I would avoid any criticism that does not have any value for me as a coder or anyone willing to participate.

- Questions about AI, I ended up using Claude in order to implement functionalities that are out of my proficiency area (html, program OTA updates, advanced C++ libraries, ESP32 specific functions...). I can not say I am against AI, but avoid any kind of slop with no purpose or criteria about what is being sent. I tried to prompt specific pieces of code, review it and test it on a real device before pushing. Can not guarantee the code is bulletproof, but it is still somehow understandable and approachable.

The goal of this repo, is to share and improve a concept that has been in my mind and life for more than 3 years now. I know my limits and I believe the community can bring this concept much further than myself alone with limited resources, knowledge, time... And maybe some of you can make improvements on top of it.
## How it works
This brewer is based on the combination of two existing appliances: a traditional moka pot and a temperature-controlled kettle with extra fancy math.

- The moka pot is the "cuore", nonetheless, is still a moka pot after all, but it means it brews the same way a moka pot always did.
- Takes the concept of a sensor and a PID controlled resistor in order to achieve a target temperature of a smart kettle.
- Using Clausius-Clapeyron equation, knowing boiling point at a certain atmospheric pressure, you can estimate internal pressure

Doing the proper math is possible to keep the pot at a certain pressure. Since pressure and temperature are related you have to manage resistance of the coffee as your third chair leg in order to increase or decrease the temperature you need to brew ([James MokaBot ](https://youtu.be/UGf7mtfhOFM?si=DkclQYDJ4awHg2Jr&t=669): that's why James took this decision here, to compensate the lower pressure he had to grind coarser to keep the same equivalent output flow rate).

## Features
List of features
- Bloom. Based on boiling point (with an offset) is supposed to create enough pressure to soak the coffee bed but not enough to make it flow through the siphon completely.
- Boiling point calibration (long press PREHEAT: takes the maximum temperature over 3 seconds), or set it from the settings page.
- Save preheat and target temperature (long press OFF).
- Three modes, selected by swiping the main screen:
  - Extraction: PID controlled preheat, bloom and brew.
  - Manual power: the heater runs at a fixed 50-100% of the time window, and switches off automatically at the boiling point plus a configurable margin.
  - Profiles: select one of the saved brews (A, B, C).
- Brew page (Chart.js) with temperature, pressure and target temperature, with customizable line colours. The brew history is recorded on the device, so the whole curve is shown even if the page is opened late.
- Save finished brews in three slots (A, B, C) and delete them from the settings page.
- Settings page to adjust details: PID values, fixed resistor and Steinhart-Hart coefficients of the thermistor, bloom time and offset, boiling temperature, manual power cut-off, °C/°F and decimals, screen colour.
- Firmware updates over the air: from a URL (version check, confirmation, reinstall) or uploading the .bin from the settings page.
- Reachable at http://mokabot.local once connected to WiFi.

Components used:

 - WT32-SC01 (**NO WT32-SC01 Plus**), or Waveshare ESP32-S3-Touch-LCD-3.5B (adds a speaker for button beeps).
 - ADS1115.
 - NTC50k 3950 M6. [NTC 50k](https://es.aliexpress.com/item/1005006189248088.html)
 - 2Kohm resistor, 1% or better precision.
 - BMP280.
 - 5V2A power supply. [Power supply](https://es.aliexpress.com/item/1005004434305525.html)
 - C13 female connector. [C13 connector](https://es.aliexpress.com/item/1005009947629857.html)
 - Round switch. [Switch](https://es.aliexpress.com/item/1005013351974257.html)
 - Kettle Connector [Connector](https://es.aliexpress.com/item/1005012049626600.html?)

TO DO add STL files for the different screens

TO DO add wiring diagrams for both screens

## Points to discuss

 - **Coffee recipe**
	 - There are currently two big opposite trends around "how to brew" moka pots. Let's call James Hoffmann's and Lance Hedrick's recipes, with or without preheating respectively.
	 - As far as I have tested, none of them is exceptionally better, but there is an inherent issue with the second recipe that I found (Thanks to Lance who handed me the corresponding paper).
		>It has been already stressed out the absence of thermodynamic equilibrium between liquid and vapour phases of water, which results in a temperature difference sensed by probes whether immersed or not. Vapour conditions are driven by both evaporation from liquid-vapour separation surface and convective heat transfer with each surrounding surface.

		The problem I need to arrange with the second recipe is the non-stable water situation inside the boiler, being able to see water starting to boil around the heater but the water around the sensor is still relatively cold, caused by the energy transmission limitation of water that remains steady.

		 Placing the sensor closer to the heating element should give a more accurate reading of the pressure, but much further from the real water temperature

	 - James recipe, not being better, is far more predictable since the water chamber status is more stable and the readings are far more credible.
		 **Option**: Add a second sensor and add a switch to the base to switch between two modes, pure pressure or a stable pressure chamber.

 - **Points of improvement**
	
	-	Code Refactor: I am not really familiar with C++ object management, UI libraries and basically platformIO/ESP32 development.
	-	PID Tuning: I have basic PID knowledge and added a basic Integral compensation based on delta temperature between ambient and target (Ki = 0.01 per degree, ~0.7 for a 70°C delta), it is a basic scalar value but I suppose it is not as easy as that.
	-	Adding support for other screens: the Waveshare ESP32-S3-Touch-LCD-3.5B is now supported (with button beeps); the SD card for configuration is still pending.
	-	Share profiles (saving them on the device is already possible)
	-	Better boiling point calibration process. Still room for improvement
  

## Usage

**First setup**
1. Flash the firmware over USB: `pio run -e WT32-SC01-MOKA -t upload` (or `-e WAVESHARE-S3-TOUCH-LCD-3_5B-MOKA`).
2. Tap the IP button (top left). Without saved WiFi credentials the device opens a setup access point called **"ESP32 Wi-Fi Provisioning"**: connect to it with a phone and choose your network. Tapping the IP button again while the setup portal is open cancels it.
3. Once connected, the IP button shows the address. The web pages are at `http://mokabot.local/` (brew chart), `/settings` and `/debug`.

**Buttons** (tap / long press)
- **OFF**: switch off / save the current target temperature.
- **PREHEAT**: preheat / calibrate the boiling point (maximum temperature over 3 seconds).
- **EXTRACT**: on the extraction page start brewing (tap again to skip the bloom) / brew with a manual bloom phase. On the manual power page start manual power / save the percentage. Nothing on the profile page.
- **+ / -**: target temperature, power percentage, or saved profile, depending on the page.
- **IP**: connect or disconnect WiFi / forget the saved WiFi network.
- **Colour (pencil, top right)**: change the button colour (tap outside the panel to cancel) / check for a firmware update.
- **°C / °F**: switch units / show or hide decimals.
- **Swipe left or right**: change page (extraction, manual power, profiles). Changing page switches the moka off.

**Firmware updates (OTA)**
- Set the "Firmware folder URL" in the settings page and publish there the `.bin` and `.version` files that `pio run` leaves in `build/`. A long press on the colour button checks the version and asks for confirmation before updating.
- Or upload the `.bin` directly from the settings page.

## Building

Built with [PlatformIO](https://platformio.org/). `pio run` builds both boards (`WT32-SC01-MOKA` and `WAVESHARE-S3-TOUCH-LCD-3_5B-MOKA`) and copies the firmware images for OTA updates to `build/`, together with a `.version` file per board. The version is set with `custom_firmware_version` in `platformio.ini`. The web pages are compiled into the firmware, so there is no filesystem image to upload.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Extras
this project is free to anyone willing to do implement by themselves, but I leave a link for donations [Buy Me a Coffee](buymeacoffee.com/javitel)

Also to mention, to disclose any major comment or concern about intellectual property. To this day I am the full owner of the patent around this concept (was a fortunate accident I never considered, like finding a winning lottery ticket) and I was afraid having the source code public and the patent could cause an issue with the concept of ownership in case anyone adds any value to the project. That is why this is a non-commercial license, getting everything out of any dispute related with intellectual property (if anybody is willing to discuss or ask any question regarding that, please let me know)
## License

This work is licensed under CC BY-NC-SA 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
