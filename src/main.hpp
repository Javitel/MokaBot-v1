/*
MIT License

Copyright (c) 2022 Sukesh Ashok Kumar

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef TUX_CONF_H
#define TUX_CONF_H

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
  
#include <inttypes.h>

#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_flash.h>


//using namespace std ;
#include "soc/rtc.h"
//#include "esp_ota_ops.h"
//#include "esp_app_format.h"



// Mount SPIFF partition and print readme.txt content

/********************DEVICE SELECTION ******************/
#if defined(CONFIG_TUX_DEVICE_WT32_SC01)
/* Enable one of the devices from below (shift to bsp selection later) */
#include "conf_WT32SCO1.h"              // WT32-SC01 (ESP32)
#elif defined(CONFIG_TUX_DEVICE_WT32_SC01_PLUS)
// WT32-SC01 Plus (ESP32-S3 + 8Bit Parellel) with SD Card, Audio support
#include "conf_WT32SCO1-Plus.h"         
#elif defined(CONFIG_TUX_DEVICE_ESP32S3SPI35)
// Makerfabs ESP32S335D (ESP32-S3 + SPI) with SD Card, Audio support
#include "conf_Makerfabs_S3_STFT.h" 
#elif defined(CONFIG_TUX_DEVICE_ESP32S335D)
// Makerfabs ESP32S335D (ESP32-S3 + 16Bit Parellel) with SD Card, Audio support
#include "conf_Makerfabs_S3_PTFT.h"    
#else
#endif

/********************************************************/


/* SD Card support */
#if defined(SD_SUPPORTED)
        // Shared SPI Bus with Display
    #if defined(WT32_SC01) //|| defined(MAKERFAB_ESP32S3_SPI)
        #include "helper_storage_shared.hpp"
    #else
        // Display and SD card is not on same SPI bus
        #include "helper_storage.hpp"
    #endif
#endif

// UI design

// Weather update timer - Once per min (60*1000) or maybe once in 10 mins (10*60*1000)
#endif // TUX_CONF_H