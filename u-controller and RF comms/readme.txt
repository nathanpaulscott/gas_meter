seems like I have this one:

ESP32-DevKitC V4 (WIFI using usb C)
This is a clone board running that esp32 chip
The closest official reference is:
ESP32-DevKitC V4
Getting Started and Hardware Reference
https://docs.espressif.com/projects/esp-idf/en/latest/esp32/hw-reference/esp32/get-started-devkitc.html


GPT starter prompt:
use this to get gpt up to speed on the board you are coding for:
------------------------------------------------------------------------------------
I’m working with an ESP32-DevKitC V4 board using the ESP32-WROOM-32U module (with external u.FL antenna). It’s a generic AliExpress clone but fully pin-compatible with the official Espressif DevKitC V4. It uses a CH340 or CP2102 USB-UART chip, and I’m programming it using the Arduino IDE. Assume standard pin mapping and power setup.
------------------------------------------------------------------------------------




-----------------------------------------------------------------------------



The arduino IDE has this expressive esp32 board type and you can download the drivers for it, nothing too complicated

driver link points to here, this has all the documentation and bug reports etc as well as drivers....
https://github.com/espressif/arduino-esp32

my aliexpress order:
https://www.aliexpress.com/item/1005008851115917.html?spm=a2g0o.order_list.order_list_main.4.73bd1802EKW2Mv#nav-description
what I got desc:
---------------------
ESP32 DevKitC is an entry-level ESP32 development board. 
The ESP32 pin integrated on the board has been led out for easy connection and use. It can be used to evaluate all ESP32 modules and chips, and can be easily inserted into the test board.
ESP32-DevKitC V4 Getting Started Guide
https://github.com/espressif/esp-idf/blob/5f8de19/docs/en/hw-reference/esp32/get-started-devkitc.rst
This guide shows how to start using the ESP32-DevKitC V4 development board. For description of other versions of ESP32-DevKitC check ESP32 Hardware Reference.
What You Need
ESP32-DevKitC V4 board
USB A / micro USB B cable
Computer running Windows, Linux, or macOS
You can skip the introduction sections and go directly to Section Start Application Development.

Overview
ESP32-DevKitC V4 is a small-sized ESP32-based development board produced by Espres-sif. Most of the I/O pins are broken out to the pin headers on both sides for easy interfacing. Developers can either connect peripherals with jumper wires or mount ESP32-DevKitC V4 on a breadboard.

To cover a wide range of user requirements, the following versions of ESP32-DevKitC V4 are available:

For details please refer to Espre-ssif Product Ordering Information.
-------------------------------
 
 
