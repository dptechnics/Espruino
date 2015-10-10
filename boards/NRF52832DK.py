#!/bin/false
# This file is part of Espruino, a JavaScript interpreter for Microcontrollers
#
# Copyright (C) 2013 Gordon Williams <gw@pur3.co.uk>
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# ----------------------------------------------------------------------------------------
# This file contains information for a specific board - the available pins, and where LEDs,
# Buttons, and other in-built peripherals are. It is used to build documentation as well
# as various source and header files for Espruino.
# ----------------------------------------------------------------------------------------

import pinutils;

info = {
 'name' : "nRF52 Preview Development Kit",
 'link' :  [ "https://www.nordicsemi.com/Products/Bluetooth-Smart-Bluetooth-low-energy/nRF52832" ],
 'default_console' : "EV_SERIAL1",
 'variables' : 750, # How many variables are allocated for Espruino to use. RAM will be overflowed if this number is too high and code won't compile.
 'binary_name' : 'espruino_%v_nrf52832.bin',
};

chip = {
  'part' : "NRF52832",
  'family' : "NRF52",
  'package' : "QFN48",
  'ram' : 32, # Currently there is a bug with NRF52 preview DK's RAM but this will be fixed next revision.
  'flash' : 512,
  'speed' : 64,
  'usart' : 1, 
  'spi' : 3,
  'i2c' : 2,
  'adc' : 1,
  'dac' : 0,
  'saved_code' : {
    'address' : ((128 - 3) * 4096),
    'page_size' : 4096,
    'pages' : 3,
    'flash_available' : (512 - 124 - 12) # Softdevice uses 31 plages of flash. Each page is 4 kb.
  },
};

# left-right, or top-bottom order
board = {
  'left' : [ 'VDD', 'VDD', 'RESET', 'VDD','5V','GND','GND','PD3','PD4','PD28','PD29','PD30','PD31'],
  'right' : [ 'PD27', 'PD26', 'PD2', 'GND', 'PD25','PD24','PD23', 'PD22','PD20','PD19','PD18','PD17','PD16','PD15','PD14','PD13','PD12','PD11','PD10','PD9','PD8','PD7','PD6','PD5','PD21','PD1','PD0'],
};

devices = {
  'LED_1' : { 'pin' : 'D17' },
  'LED_2' : { 'pin' : 'D18' },
  'LED_3' : { 'pin' : 'D19' },
  'LED_4' : { 'pin' : 'D20' },
  'BUTTON_1' : { 'pin' : 'D13'},
  'BUTTON_2' : { 'pin' : 'D14'},
  'BUTTON_3' : { 'pin' : 'D15'},
  'BUTTON_4' : { 'pin' : 'D16'},
  'RX_PIN_NUMBER' : { 'pin' : 'D8'},
  'TX_PIN_NUMBER' : { 'pin' : 'D6'},
  'CTS_PIN_NUMBER' : { 'pin' : 'D7'},
  'RTS_PIN_NUMBER' : { 'pin' : 'D5'},
};

board_css = """
""";

def get_pins():
  pins = pinutils.generate_pins(0,31) # 32 General Purpose I/O Pins.
  pinutils.findpin(pins, "PD0", True)["functions"]["XL1"]=0;
  pinutils.findpin(pins, "PD1", True)["functions"]["XL2"]=0;
  pinutils.findpin(pins, "PD5", True)["functions"]["RTS"]=0;
  pinutils.findpin(pins, "PD6", True)["functions"]["TXD"]=0;
  pinutils.findpin(pins, "PD7", True)["functions"]["CTS"]=0;
  pinutils.findpin(pins, "PD8", True)["functions"]["RXD"]=0;
  pinutils.findpin(pins, "PD9", True)["functions"]["NFC1"]=0;
  pinutils.findpin(pins, "PD10", True)["functions"]["NFC2"]=0;
  pinutils.findpin(pins, "PD13", True)["functions"]["Button_1"]=0;
  pinutils.findpin(pins, "PD14", True)["functions"]["Button_2"]=0;
  pinutils.findpin(pins, "PD15", True)["functions"]["Button_3"]=0;
  pinutils.findpin(pins, "PD16", True)["functions"]["Button_4"]=0;
  pinutils.findpin(pins, "PD17", True)["functions"]["LED_1"]=0;
  pinutils.findpin(pins, "PD18", True)["functions"]["LED_2"]=0;
  pinutils.findpin(pins, "PD19", True)["functions"]["LED_3"]=0;
  pinutils.findpin(pins, "PD20", True)["functions"]["LED_4"]=0;
  #The boot/reset button will function as a reset button in normal operation. Pin reset on PD21 needs to be enabled on the nRF52832 device for this to work.
  return pins
