# MokaBot-v1
Hello everyone, after a long time, some unexpected paperwork, many refactors and some support from many friends I've found during the last months I decided to publish the code and documents to build your own MokaBot. But fist I need to address some points:

 - I am not a microcontroller expert by any means and I do not pretend to be one, I know basic Arduino enough to do some tricks but this is a whole different level, consider this whenever you comment about the code.

- I know some basic Object Oriented Languages(Java) and C, but never worked seriously with C++, as said early, I would avoid any critiszm that does not have any value for me as a coder or anyone willing to participate.

- No AI was involved and I would request anyone willing to participate to avoid unless used with care and mindfully. I do not want massive slop refactors that nobody knows exactly what is doing.

The goal of this repo, is toshare and improve a concept it has been in my mind and life for more than 3 years now. I know my limits and I believe with the community can bring this concept much furhter that myself alone with limited resources, knowledge, time... And maybe some of you can make improvements on top of it.
## How it works
This brewer is based on the combination of two existing appliances: a traditional moka pot and a temperature controlled ketter with extra fancy math.

- The moka pot is the "cuore", nontheless, is still a moka pot after all, but it means it brews the same way a moka pot always did.
- Takes the concept of a sensor and a PID controlled resistor in order to achieve a target temperature of a smart kettle.
- Using Clausius-Clapeyron equation, knowing boiling point at a certain atmospheric pressure, you can estimate internal pressure

Doing the proper math is possible to keep the pot at a certain pressure. Since pressure and temeprature are related you have to manage resistance of the coffee as your thir chair leg in order to increase or decrease the temperature you need to brew ([James MokaBot ](https://youtu.be/UGf7mtfhOFM?si=DkclQYDJ4awHg2Jr&t=669)that why James took this decision here, to compensate the lower pressure he had to grind coarser to keep the same equivalent output flow rate).

## Features
To function this device the following components were used:

 - WT32-SC01.
 - ADS1115.
 - NTC50k 3950 M6. [NTC 50k](https://es.aliexpress.com/item/1005006189248088.html)
 - 2Kohm resistor.
 - BMP280.
 - 5V2A power supply. [Power supply](https://es.aliexpress.com/item/1005004434305525.html)
 - C13 female connector. [C13 connector](https://es.aliexpress.com/item/1005009947629857.html)
 - Round switch. [Switch](https://es.aliexpress.com/item/1005013351974257.html)

## Points to discuss

 - **Coffee recipe**
	 - There are currently two big opposite trends around "how to brew" moka pots. Let's call James Hoffmann's and Lance Hedrick's recipes, with or without preheating respectively.
	 - As far as I have tested, none of them is exceptionally better, but there is an inherent issue with the second recipe that I found (Thanks to Lance who handed me the corresponding paper).
		>199 It has been already stressed out the absence of thermodynamic equilibrium
		>200 between liquid and vapour phases of water, which results in a temperature 
		>201 difference sensed by probes whether immersed or not. Vapour conditions are 
		>202 driven by both evaporation  from liquid-vapour separation surface and convec
		>203 tive heat transfer with each surrounding surface.

		The problem I need to arrange with the second recipe is the non-stable  water situation inside the boiler, being able to see waterstarting to boil around the heater but the water around the sensor is still relatively cold, caused by the energy transmission limitation of water that reamins steady.

		 Placing the sensor closer to the heating element should give a more accurate reading of the pressure, buch much further from the real water temperature

	 - James recipe, not being better, is far more predictable since the water chamber status is more stable and the readings are far more credible.
		 **Option**: Add a second sensor and add a switch to the base to swtich between two modes, pure pressure or a stable pressure chamber.

 - **Points of improvement**
	
	-	Code Refactor: I am not really familiar with C++ object management, UI libraries and basically platformIO/ESP32 development.
	-	PID Tunning:
	-	Adding support for other screens: I wanted to add support for a board like waveshare ESP32-S3-Touch-LCD-3.5B, to add the option to have an SD card for configuration and a speaker (beep when preheat or brew finished?)
	-	

# Extras
*Also to mention, to disclose any major comment or concern about intellectual property. To this day I am fully owner of the patent around this concept (was a fortunate accident I never considered, like finding a winning lottery ticket) and I was afraid having the source code public and the patent could cause an issue with the concept of ownership in case anyone adds any value to the project. That is why, This is a non-commercial license, getting everything out of any dispute related with intellectual property (if anybody is willing to discuss or ask any question regarding that, please let me know)
# License

This work is licensed under CC BY-NC-SA 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
