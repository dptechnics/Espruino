/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2013 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Interactive Shell implementation
 * ----------------------------------------------------------------------------
 */
#include "jsutils.h"
#include "jsinteractive.h"
#include "jshardware.h"
#include "jstimer.h"
#include "jswrapper.h"
#include "jswrap_json.h"
#include "jswrap_io.h"
#include "jswrap_stream.h"
#include "jswrap_flash.h" // load and save to flash

#ifdef ARM
#define CHAR_DELETE_SEND 0x08
#else
#define CHAR_DELETE_SEND '\b'
#endif

#define CTRL_C_TIME_FOR_BREAK jshGetTimeFromMilliseconds(100)

// ----------------------------------------------------------------------------
typedef enum {
  IS_NONE,
  IS_HAD_R,
  IS_HAD_27,
  IS_HAD_27_79,
  IS_HAD_27_91,
  IS_HAD_27_91_49,
  IS_HAD_27_91_50,
  IS_HAD_27_91_51,
  IS_HAD_27_91_52,
  IS_HAD_27_91_53,
  IS_HAD_27_91_54,
} PACKED_FLAGS InputState;

JsVar *events = 0; // Array of events to execute
JsVarRef timerArray = 0; // Linked List of timers to check and run
JsVarRef watchArray = 0; // Linked List of input watches to check and run
// ----------------------------------------------------------------------------
IOEventFlags consoleDevice = DEFAULT_CONSOLE_DEVICE; ///< The console device for user interaction
Pin pinBusyIndicator = DEFAULT_BUSY_PIN_INDICATOR;
Pin pinSleepIndicator = DEFAULT_SLEEP_PIN_INDICATOR;
JsiStatus jsiStatus;
JsSysTime jsiLastIdleTime;  ///< The last time we went around the idle loop - use this for timers
uint32_t jsiTimeSinceCtrlC;
// ----------------------------------------------------------------------------
JsVar *inputLine = 0; ///< The current input line
JsvStringIterator inputLineIterator; ///< Iterator that points to the end of the input line
int inputLineLength = -1;
bool inputLineRemoved = false;
size_t inputCursorPos = 0; ///< The position of the cursor in the input line
InputState inputState = 0; ///< state for dealing with cursor keys
bool hasUsedHistory = false; ///< Used to speed up - if we were cycling through history and then edit, we need to copy the string
unsigned char loopsIdling; ///< How many times around the loop have we been entirely idle?
bool interruptedDuringEvent; ///< Were we interrupted while executing an event? If so may want to clear timers
// ----------------------------------------------------------------------------

void jsiDebuggerLine(JsVar *line);

// ----------------------------------------------------------------------------

IOEventFlags jsiGetDeviceFromClass(JsVar *class) {
  // Devices have their Object data set up to something special
  // See jspNewObject
  if (class->varData.str[0]=='D' &&
      class->varData.str[1]=='E' &&
      class->varData.str[2]=='V')
    return (IOEventFlags)class->varData.str[3];

  return EV_NONE;
}

JsVar *jsiGetClassNameFromDevice(IOEventFlags device) {
  const char *deviceName = jshGetDeviceString(device);
  return jsvFindChildFromString(execInfo.root, deviceName, false);
}

NO_INLINE bool jsiEcho() {
  return ((jsiStatus&JSIS_ECHO_OFF_MASK)==0);
}

static bool jsiShowInputLine() {
  return jsiEcho() && !inputLineRemoved;
}

/** Called when the input line/cursor is modified *and its iterator should be reset
 * Because JsvStringIterator doesn't lock the string, it's REALLY IMPORTANT
 * that we call this BEFORE we do jsvUnLock(inputLine) */
static NO_INLINE void jsiInputLineCursorMoved() {
  // free string iterator
  if (inputLineIterator.var) {
    jsvStringIteratorFree(&inputLineIterator);
    inputLineIterator.var = 0;
  }
  inputLineLength = -1;
}

/// Called to append to the input line
static NO_INLINE void jsiAppendToInputLine(const char *str) {
  // recreate string iterator if needed
  if (!inputLineIterator.var) {
    jsvStringIteratorNew(&inputLineIterator, inputLine, 0);
    jsvStringIteratorGotoEnd(&inputLineIterator);
  }
  while (*str) {
    jsvStringIteratorAppend(&inputLineIterator, *(str++));
    inputLineLength++;
  }
}


/// Change the console to a new location
void jsiSetConsoleDevice(IOEventFlags device) {
  if (device == consoleDevice) return;

  if (!jshIsDeviceInitialised(device)) {
    JshUSARTInfo inf;
    jshUSARTInitInfo(&inf);
    jshUSARTSetup(device, &inf);
  }

  jsiConsoleRemoveInputLine();
  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    jsiConsolePrint("Console Moved to ");
    jsiConsolePrint(jshGetDeviceString(device));
    jsiConsolePrint("\n");
  }
  IOEventFlags oldDevice = consoleDevice;
  consoleDevice = device;
  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    jsiConsolePrint("Console Moved from ");
    jsiConsolePrint(jshGetDeviceString(oldDevice));
    jsiConsolePrint("\n");
  }
}

/// Get the device that the console is currently on
IOEventFlags jsiGetConsoleDevice() {
  return consoleDevice;
}

NO_INLINE void jsiConsolePrintChar(char data) {
  jshTransmit(consoleDevice, (unsigned char)data);
}

NO_INLINE void jsiConsolePrint(const char *str) {
  while (*str) {
    if (*str == '\n') jsiConsolePrintChar('\r');
    jsiConsolePrintChar(*(str++));
  }
}

void jsiConsolePrintf(const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrint,0, fmt, argp);
  va_end(argp);
}

/// Print the contents of a string var from a character position until end of line (adding an extra ' ' to delete a character if there was one)
void jsiConsolePrintStringVarUntilEOL(JsVar *v, size_t fromCharacter, size_t maxChars, bool andBackup) {
  size_t chars = 0;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, fromCharacter);
  while (jsvStringIteratorHasChar(&it) && chars<maxChars) {
    char ch = jsvStringIteratorGetChar(&it);
    if (ch == '\n') break;
    jsiConsolePrintChar(ch);
    chars++;
    jsvStringIteratorNext(&it);
  }
  jsvStringIteratorFree(&it);
  if (andBackup) {
    jsiConsolePrintChar(' ');chars++;
    while (chars--) jsiConsolePrintChar(0x08); //delete
  }
}

/** Print the contents of a string var - directly - starting from the given character, and
 * using newLineCh to prefix new lines (if it is not 0). */
void jsiConsolePrintStringVarWithNewLineChar(JsVar *v, size_t fromCharacter, char newLineCh) {
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, fromCharacter);
  while (jsvStringIteratorHasChar(&it)) {
    char ch = jsvStringIteratorGetChar(&it);
    if (ch == '\n') jsiConsolePrintChar('\r');
    jsiConsolePrintChar(ch);
    if (ch == '\n' && newLineCh) jsiConsolePrintChar(newLineCh);
    jsvStringIteratorNext(&it);
  }
  jsvStringIteratorFree(&it);
}

/// Print the contents of a string var - directly
void jsiConsolePrintStringVar(JsVar *v) {
  jsiConsolePrintStringVarWithNewLineChar(v,0,0);
}

/** Assuming that we are at the end of the string, this backs up
 * and deletes it */
void jsiConsoleEraseStringVarBackwards(JsVar *v) {
  assert(jsvHasCharacterData(v));

  size_t line, lines = jsvGetLinesInString(v);
  for (line=lines;line>0;line--) {
    size_t i,chars = jsvGetCharsOnLine(v, line);
    if (line==lines) {
      for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    }
    for (i=0;i<chars;i++) jsiConsolePrintChar(' '); // move cursor forwards and wipe out
    for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    if (line>1) { 
      // clear the character before - this would have had a colon
      jsiConsolePrint("\x08 ");
      // move cursor up      
      jsiConsolePrint("\x1B[A"); // 27,91,65 - up
    }
  }
}

/** Assuming that we are at fromCharacter position in the string var,
 * erase everything that comes AFTER and return the cursor to 'fromCharacter'
 * On newlines, if erasePrevCharacter, we remove the character before too. */
void jsiConsoleEraseStringVarFrom(JsVar *v, size_t fromCharacter, bool erasePrevCharacter) {
  assert(jsvHasCharacterData(v));
  size_t cursorLine, cursorCol;
  jsvGetLineAndCol(v, fromCharacter, &cursorLine, &cursorCol);
  // delete contents of current line
  size_t i, chars = jsvGetCharsOnLine(v, cursorLine);
  for (i=cursorCol;i<=chars;i++) jsiConsolePrintChar(' ');
  for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back

  size_t line, lines = jsvGetLinesInString(v);
  for (line=cursorLine+1;line<=lines;line++) {
    jsiConsolePrint("\x1B[B"); // move down
    chars = jsvGetCharsOnLine(v, line);
    for (i=0;i<chars;i++) jsiConsolePrintChar(' '); // move cursor forwards and wipe out
    for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    if (erasePrevCharacter) {
      jsiConsolePrint("\x08 "); // move cursor back and insert space
    }
  }
  // move the cursor back up
  for (line=cursorLine+1;line<=lines;line++)
    jsiConsolePrint("\x1B[A"); // 27,91,65 - up
  // move the cursor forwards
  for (i=1;i<cursorCol;i++)
    jsiConsolePrint("\x1B[C"); // 27,91,67 - right
}

void jsiMoveCursor(size_t oldX, size_t oldY, size_t newX, size_t newY) {
  // see http://www.termsys.demon.co.uk/vtansi.htm - we could do this better
  // move cursor
  while (oldX < newX) {
    jsiConsolePrint("\x1B[C"); // 27,91,67 - right
    oldX++;
  }
  while (oldX > newX) {
    jsiConsolePrint("\x1B[D"); // 27,91,68 - left
    oldX--;
  }
  while (oldY < newY) {
    jsiConsolePrint("\x1B[B"); // 27,91,66 - down
    oldY++;
  }
  while (oldY > newY) {
    jsiConsolePrint("\x1B[A"); // 27,91,65 - up
    oldY--;
  }
}

void jsiMoveCursorChar(JsVar *v, size_t fromCharacter, size_t toCharacter) {
  if (fromCharacter==toCharacter) return;
  size_t oldX, oldY;
  jsvGetLineAndCol(v, fromCharacter, &oldY, &oldX);
  size_t newX, newY;
  jsvGetLineAndCol(v, toCharacter, &newY, &newX);
  jsiMoveCursor(oldX, oldY, newX, newY);
}

/// If the input line was shown in the console, remove it
void jsiConsoleRemoveInputLine() {
  if (!inputLineRemoved) {
    inputLineRemoved = true;
    if (jsiEcho() && inputLine) { // intentionally not using jsiShowInputLine()
      jsiMoveCursorChar(inputLine, inputCursorPos, 0);
      jsiConsoleEraseStringVarFrom(inputLine, 0, true);
      jsiConsolePrintChar(0x08); // go back to start of line
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER) {
        jsiConsolePrintChar(0x08); // d
        jsiConsolePrintChar(0x08); // e
        jsiConsolePrintChar(0x08); // b
        jsiConsolePrintChar(0x08); // u
        jsiConsolePrintChar(0x08); // g
      }
#endif
    }
  }
}

/// If the input line has been removed, return it
void jsiReturnInputLine() {
  if (inputLineRemoved) {
    inputLineRemoved = false;
    if (jsiEcho()) { // intentionally not using jsiShowInputLine()
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER)
        jsiConsolePrint("debug");
#endif
      jsiConsolePrintChar('>'); // show the prompt
      jsiConsolePrintStringVarWithNewLineChar(inputLine, 0, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos);
    }
  }
}
void jsiConsolePrintPosition(struct JsLex *lex, size_t tokenPos) {
  jslPrintPosition((vcbprintf_callback)jsiConsolePrint, 0, lex, tokenPos);
}

void jsiClearInputLine() {
  jsiConsoleRemoveInputLine();
  // clear input line
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine = jsvNewFromEmptyString();
}

void jsiSetBusy(JsiBusyDevice device, bool isBusy) {
  static JsiBusyDevice business = 0;

  if (isBusy)
    business |= device;
  else
    business &= (JsiBusyDevice)~device;

  if (pinBusyIndicator != PIN_UNDEFINED)
    jshPinOutput(pinBusyIndicator, business!=0);
}

void jsiSetSleep(JsiSleepType isSleep) {
  if (pinSleepIndicator != PIN_UNDEFINED)
    jshPinOutput(pinSleepIndicator, isSleep == JSI_SLEEP_AWAKE);
}

static JsVarRef _jsiInitNamedArray(const char *name) {
  JsVar *array = jsvObjectGetChild(execInfo.hiddenRoot, name, JSV_ARRAY);
  JsVarRef arrayRef = 0;
  if (array) arrayRef = jsvGetRef(jsvRef(array));
  jsvUnLock(array);
  return arrayRef;
}

// Used when recovering after being flashed
// 'claim' anything we are using
void jsiSoftInit() {
  jswInit();

  jsErrorFlags = 0;
  events = jsvNewWithFlags(JSV_ARRAY);
  inputLine = jsvNewFromEmptyString();
  inputCursorPos = 0;
  jsiInputLineCursorMoved();
  inputLineIterator.var = 0;

  jsiStatus &= ~JSIS_ALLOW_DEEP_SLEEP;

  // Load timer/watch arrays
  timerArray = _jsiInitNamedArray(JSI_TIMERS_NAME);
  watchArray = _jsiInitNamedArray(JSI_WATCHES_NAME);

  // Now run initialisation code
  JsVar *initCode = jsvObjectGetChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME, 0);
  if (initCode) {
    jsvUnLock2(jspEvaluateVar(initCode, 0, false), initCode);
    jsvRemoveNamedChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME);
  }

  // Check any existing watches and set up interrupts for them
  if (watchArray) {
    JsVar *watchArrayPtr = jsvLock(watchArray);
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, watchArrayPtr);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *watch = jsvObjectIteratorGetValue(&it);
      JsVar *watchPin = jsvObjectGetChild(watch, "pin", 0);
      jshPinWatch(jshGetPinFromVar(watchPin), true);
      jsvUnLock2(watchPin, watch);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
    jsvUnLock(watchArrayPtr);
  }

  // Timers are stored by time in the future now, so no need
  // to fiddle with them.

  // Make sure we set up lastIdleTime, as this could be used
  // when adding an interval from onInit (called below)
  jsiLastIdleTime = jshGetSystemTime();
  jsiTimeSinceCtrlC = 0xFFFFFFFF;

  // And look for onInit function
  JsVar *onInit = jsvObjectGetChild(execInfo.root, JSI_ONINIT_NAME, 0);
  if (onInit) {
    if (jsiEcho()) jsiConsolePrint("Running onInit()...\n");
    if (jsvIsFunction(onInit))
      jsvUnLock(jspExecuteFunction(onInit, 0, 0, (JsVar**)0));
    else if (jsvIsString(onInit))
      jsvUnLock(jspEvaluateVar(onInit, 0, false));
    else
      jsError("onInit is not a Function or a String");
    jsvUnLock(onInit);
  }
}

/** Output the given variable as JSON, or if it exists
 * in the root scope (and it's not 'existing') then just
 * the name is dumped.  */
void jsiDumpJSON(vcbprintf_callback user_callback, void *user_data, JsVar *data, JsVar *existing) {
  // Check if it exists in the root scope
  JsVar *name = jsvGetArrayIndexOf(execInfo.root,  data, true);
  if (name && jsvIsString(name) && name!=existing) {
    // if it does, print the name
    cbprintf(user_callback, user_data, "%v", name);
  } else {
    // if it doesn't, print JSON
    jsfGetJSONWithCallback(data, JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES, user_callback, user_data);
  }
}

/** Dump the code required to initialise a serial port to this string */
void jsiDumpSerialInitialisation(vcbprintf_callback user_callback, void *user_data, const char *serialName, bool addCallbacks) {
  JsVar *serialVar = jsvObjectGetChild(execInfo.root, serialName, 0);
  if (serialVar) {
    if (addCallbacks) {
      JsVar *onData = jsvObjectGetChild(serialVar, USART_CALLBACK_NAME, 0);
      if (onData) {
        cbprintf(user_callback, user_data, "%s.on('data', ", serialName);
        jsiDumpJSON(user_callback, user_data, onData, 0);
        user_callback(");\n", user_data);
      }
    }
    JsVar *baud = jsvObjectGetChild(serialVar, USART_BAUDRATE_NAME, 0);
    JsVar *options = jsvObjectGetChild(serialVar, DEVICE_OPTIONS_NAME, 0);
    if (baud || options) {
      int baudrate = (int)jsvGetInteger(baud);
      if (baudrate <= 0) baudrate = DEFAULT_BAUD_RATE;
      cbprintf(user_callback, user_data, "%s.setup(%d", serialName, baudrate);
      if (jsvIsObject(options)) {
        user_callback(", ", user_data);
        jsfGetJSONWithCallback(options, JSON_SHOW_DEVICES, user_callback, user_data);
      }
      user_callback(");\n", user_data);
    }
    jsvUnLock3(baud, options, serialVar);
  }
}

/** Dump the code required to initialise a SPI port to this string */
void jsiDumpDeviceInitialisation(vcbprintf_callback user_callback, void *user_data, const char *deviceName) {
  JsVar *deviceVar = jsvObjectGetChild(execInfo.root, deviceName, 0);
  if (deviceVar) {
    JsVar *options = jsvObjectGetChild(deviceVar, DEVICE_OPTIONS_NAME, 0);
    if (options) {
      cbprintf(user_callback, user_data, "%s.setup(", deviceName);
      if (jsvIsObject(options))
        jsfGetJSONWithCallback(options, JSON_SHOW_DEVICES, user_callback, user_data);
      user_callback(");\n", user_data);
    }
    jsvUnLock2(options, deviceVar);
  }
}

/** Dump all the code required to initialise hardware to this string */
void jsiDumpHardwareInitialisation(vcbprintf_callback user_callback, void *user_data, bool addCallbacks) {
  if (jsiStatus&JSIS_ECHO_OFF) user_callback("echo(0);", user_data);
  if (pinBusyIndicator != DEFAULT_BUSY_PIN_INDICATOR) {
    cbprintf(user_callback, user_data, "setBusyIndicator(%p);\n", pinBusyIndicator);
  }
  if (pinSleepIndicator != DEFAULT_BUSY_PIN_INDICATOR) {
    cbprintf(user_callback, user_data, "setSleepIndicator(%p);\n", pinSleepIndicator);
  }
  if (jsiStatus&JSIS_ALLOW_DEEP_SLEEP) {
    user_callback("setDeepSleep(1);\n", user_data);
  }

  jsiDumpSerialInitialisation(user_callback, user_data, "USB", addCallbacks);
  int i;
  for (i=0;i<USART_COUNT;i++)
    jsiDumpSerialInitialisation(user_callback, user_data, jshGetDeviceString(EV_SERIAL1+i), addCallbacks);
  for (i=0;i<SPI_COUNT;i++)
    jsiDumpDeviceInitialisation(user_callback, user_data, jshGetDeviceString(EV_SPI1+i));
  for (i=0;i<I2C_COUNT;i++)
    jsiDumpDeviceInitialisation(user_callback, user_data, jshGetDeviceString(EV_I2C1+i));
  // pins
  Pin pin;
  for (pin=0;jshIsPinValid(pin) && pin<255;pin++) {
    if (IS_PIN_USED_INTERNALLY(pin)) continue;
    JshPinState state = jshPinGetState(pin);
    JshPinState statem = state&JSHPINSTATE_MASK;
    if (statem == JSHPINSTATE_GPIO_OUT || statem == JSHPINSTATE_GPIO_OUT_OPENDRAIN) {
      bool isOn = (state&JSHPINSTATE_PIN_IS_ON)!=0;
      if (!isOn && IS_PIN_A_LED(pin)) continue;
      cbprintf(user_callback, user_data, "digitalWrite(%p,%d);\n",pin,isOn?1:0);
    } else if (/*statem == JSHPINSTATE_GPIO_IN ||*/statem == JSHPINSTATE_GPIO_IN_PULLUP || statem == JSHPINSTATE_GPIO_IN_PULLDOWN) {
#ifdef DEFAULT_CONSOLE_RX_PIN
      // the console input pin is always a pullup now - which is expected
      if (pin == DEFAULT_CONSOLE_RX_PIN &&
          statem == JSHPINSTATE_GPIO_IN_PULLUP) continue;
#endif
      // don't bother with normal inputs, as they come up in this state (ish) anyway
      const char *s = "";
      if (statem == JSHPINSTATE_GPIO_IN_PULLUP) s="_pullup";
      if (statem == JSHPINSTATE_GPIO_IN_PULLDOWN) s="_pulldown";
      cbprintf(user_callback, user_data, "pinMode(%p,\"input%s\");\n",pin,s);
    }

    if (statem == JSHPINSTATE_GPIO_OUT_OPENDRAIN)
      cbprintf(user_callback, user_data, "pinMode(%p,\"opendrain\");\n",pin);
  }
}

// Used when shutting down before flashing
// 'release' anything we are using, but ensure that it doesn't get freed
void jsiSoftKill() {
  inputCursorPos = 0;
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine=0;

  // kill any wrapped stuff
  jswKill();
  // Stop all active timer tasks
  jstReset();
  // Unref Watches/etc
  if (events) {
    jsvUnLock(events);
    events=0;
  }
  if (timerArray) {
    jsvUnRefRef(timerArray);
    timerArray=0;
  }
  if (watchArray) {
    // Check any existing watches and disable interrupts for them
    JsVar *watchArrayPtr = jsvLock(watchArray);
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, watchArrayPtr);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
      JsVar *watchPin = jsvObjectGetChild(watchPtr, "pin", 0);
      jshPinWatch(jshGetPinFromVar(watchPin), false);
      jsvUnLock2(watchPin, watchPtr);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
    jsvUnRef(watchArrayPtr);
    jsvUnLock(watchArrayPtr);
    watchArray=0;
  }
  // Save initialisation information
  JsVar *initCode = jsvNewFromEmptyString();
  if (initCode) { // out of memory
    JsvStringIterator it;
    jsvStringIteratorNew(&it, initCode, 0);
    jsiDumpHardwareInitialisation((vcbprintf_callback)&jsvStringIteratorPrintfCallback, &it, false);
    jsvStringIteratorFree(&it);
    jsvObjectSetChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME, initCode);
    jsvUnLock(initCode);
  }
}

void jsiSemiInit(bool autoLoad, bool consolePrint) {
  jspInit();

  // Set state
  interruptedDuringEvent = false;
  // Set defaults
  jsiStatus = JSIS_NONE;
  pinBusyIndicator = DEFAULT_BUSY_PIN_INDICATOR;

  /* If flash contains any code, then we should
     Try and load from it... */
  bool loadFlash = autoLoad && jsfFlashContainsCode();
  if (loadFlash) {
    jspSoftKill();
    jsvSoftKill();
    jsfLoadFromFlash();
    jsvSoftInit();
    jspSoftInit();
  }

  // Softinit may run initialisation code that will overwrite defaults
  jsiSoftInit();

  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    if (!loadFlash && consolePrint) {
      jsiConsolePrint(
#ifndef LINUX
          // set up terminal to avoid word wrap
          "\e[?7l"
#endif
          // rectangles @ http://www.network-science.de/ascii/
          "\n"
          " _____                 _ \n"
          "|   __|___ ___ ___ _ _|_|___ ___ \n"
          "|   __|_ -| . |  _| | | |   | . |\n"
          "|_____|___|  _|_| |___|_|_|_|___|\n"
          "          |_| http://espruino.com\n"
          " "JS_VERSION" Copyright 2015 G.Williams\n");
    }
    jsiConsolePrint("\n"); // output new line
    inputLineRemoved = true; // we need to put the input line back...
  }
}

// The 'proper' init function - this should be called only once at bootup
void jsiInit(bool autoLoad) {
#if defined(LINUX) || !defined(USB)
  consoleDevice = DEFAULT_CONSOLE_DEVICE;
#else
  consoleDevice = EV_LIMBO;
#endif

  jsiSemiInit(autoLoad, true);
}

/*
 * The 'proper' init function for embedded evaluation use. No output will be printed
 * unless asked by the evaluated code. 
 */
void jsiInitForEval(bool autoLoad)
{
#if defined(LINUX) || !defined(USB)
  consoleDevice = DEFAULT_CONSOLE_DEVICE;
#else
  consoleDevice = EV_LIMBO;
#endif

  jsiSemiInit(autoLoad, false);
}

#ifndef LINUX
// This should get jsiOneSecondAfterStartupcalled from jshardware.c one second after startup,
// it does initialisation tasks like setting the right console device
void jsiOneSecondAfterStartup() {
  /* When we start up, we put all console output into 'Limbo' (EV_LIMBO),
     because we want to get started immediately, but we don't know where
     to send any console output (USB takes a while to initialise). Not only
     that but if we start transmitting on Serial right away, the first
     char or two can get corrupted.
   */
#ifdef USB
  if (consoleDevice == EV_LIMBO) {
    consoleDevice = DEFAULT_CONSOLE_DEVICE;
    if (jshIsUSBSERIALConnected())
      consoleDevice = EV_USBSERIAL;
    // now move any output that was made to Limbo to the given device
    jshTransmitMove(EV_LIMBO, consoleDevice);
    // finally, kick output - just in case
    jshUSARTKick(consoleDevice);
  } else {
    // the console has already been moved
    jshTransmitClearDevice(EV_LIMBO);
  }
#endif
}
#endif

void jsiKill() {
  jsiSoftKill();

  jspKill();
}

int jsiCountBracketsInInput() {
  int brackets = 0;

  JsLex lex;
  jslInit(&lex, inputLine);
  while (lex.tk!=LEX_EOF && lex.tk!=LEX_UNFINISHED_COMMENT) {
    if (lex.tk=='{' || lex.tk=='[' || lex.tk=='(') brackets++;
    if (lex.tk=='}' || lex.tk==']' || lex.tk==')') brackets--;
    if (brackets<0) break; // closing bracket before opening!
    jslGetNextToken(&lex);
  }
  if (lex.tk==LEX_UNFINISHED_COMMENT)
    brackets=1000; // if there's an unfinished comment, we're in the middle of something
  jslKill(&lex);

  return brackets;
} 

/// Tries to get rid of some memory (by clearing command history). Returns true if it got rid of something, false if it didn't.
bool jsiFreeMoreMemory() {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  if (!history) return 0;
  JsVar *item = jsvArrayPopFirst(history);
  bool freed = item!=0;
  jsvUnLock2(item, history);
  // TODO: could also free the array structure?
  // TODO: could look at all streams (Serial1/HTTP/etc) and see if their buffers contain data that could be removed

  return freed;
}

// Add a new line to the command history
void jsiHistoryAddLine(JsVar *newLine) {
  if (!newLine || jsvGetStringLength(newLine)==0) return;
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, JSV_ARRAY);
  if (!history) return; // out of memory
  // if it was already in history, remove it - we'll put it back in front
  JsVar *alreadyInHistory = jsvGetArrayIndexOf(history, newLine, false/*not exact*/);
  if (alreadyInHistory) {
    jsvRemoveChild(history, alreadyInHistory);
    jsvUnLock(alreadyInHistory);
  }
  // put it back in front
  jsvArrayPush(history, newLine);
  jsvUnLock(history);
}

JsVar *jsiGetHistoryLine(bool previous /* next if false */) {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  JsVar *historyLine = 0;
  if (history) {
    JsVar *idx = jsvGetArrayIndexOf(history, inputLine, true/*exact*/); // get index of current line
    if (idx) {
      if (previous && jsvGetPrevSibling(idx)) {
        historyLine = jsvSkipNameAndUnLock(jsvLock(jsvGetPrevSibling(idx)));
      } else if (!previous && jsvGetNextSibling(idx)) {
        historyLine = jsvSkipNameAndUnLock(jsvLock(jsvGetNextSibling(idx)));
      }
      jsvUnLock(idx);
    } else {
      if (previous) historyLine = jsvSkipNameAndUnLock(jsvGetArrayItem(history, jsvGetArrayLength(history)-1));
      // if next, we weren't using history so couldn't go forwards
    }

    jsvUnLock(history);
  }
  return historyLine;
}

bool jsiIsInHistory(JsVar *line) {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  if (!history) return false;
  JsVar *historyFound = jsvGetArrayIndexOf(history, line, true/*exact*/);
  bool inHistory = historyFound!=0;
  jsvUnLock2(historyFound, history);
  return inHistory;
}

void jsiReplaceInputLine(JsVar *newLine) {
  if (jsiShowInputLine()) {
    size_t oldLen =  jsvGetStringLength(inputLine);
    jsiMoveCursorChar(inputLine, inputCursorPos, oldLen); // move cursor to end
    jsiConsoleEraseStringVarBackwards(inputLine);
    jsiConsolePrintStringVarWithNewLineChar(newLine,0,':');
  }
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine = jsvLockAgain(newLine);
  inputCursorPos = jsvGetStringLength(inputLine);
}

void jsiChangeToHistory(bool previous) {
#ifdef USE_DEBUGGER
  if (jsiStatus & JSIS_IN_DEBUGGER) return;
#endif
  JsVar *nextHistory = jsiGetHistoryLine(previous);
  if (nextHistory) {
    jsiReplaceInputLine(nextHistory);
    jsvUnLock(nextHistory);
    hasUsedHistory = true;
  } else if (!previous) { // if next, but we have something, just clear the line
    if (jsiShowInputLine()) {
      jsiConsoleEraseStringVarBackwards(inputLine);
    }
    jsiInputLineCursorMoved();
    jsvUnLock(inputLine);
    inputLine = jsvNewFromEmptyString();
    inputCursorPos = 0;
  }
}

void jsiIsAboutToEditInputLine() {
  // we probably plan to do something with the line now - check it wasn't in history
  // and if it was, duplicate it
  if (hasUsedHistory) {
    hasUsedHistory = false;
    if (jsiIsInHistory(inputLine)) {
      JsVar *newLine = jsvCopy(inputLine);
      if (newLine) { // could have been out of memory!
        jsiInputLineCursorMoved();
        jsvUnLock(inputLine);
        inputLine = newLine;
      }
    }
  }
}

void jsiHandleDelete(bool isBackspace) {
  size_t l = jsvGetStringLength(inputLine);
  if (isBackspace && inputCursorPos==0) return; // at beginning of line
  if (!isBackspace && inputCursorPos>=l) return; // at end of line
  // work out if we are deleting a newline
  bool deleteNewline = (isBackspace && jsvGetCharInString(inputLine,inputCursorPos-1)=='\n') ||
      (!isBackspace && jsvGetCharInString(inputLine,inputCursorPos)=='\n');
  // If we mod this to keep the string, use jsiIsAboutToEditInputLine
  if (deleteNewline && jsiShowInputLine()) {
    jsiConsoleEraseStringVarFrom(inputLine, inputCursorPos, true/*before newline*/); // erase all in front
    if (isBackspace) {
      // delete newline char
      jsiConsolePrint("\x08 "); // delete and then send space
      jsiMoveCursorChar(inputLine, inputCursorPos, inputCursorPos-1); // move cursor back
      jsiInputLineCursorMoved();
    }
  }

  JsVar *v = jsvNewFromEmptyString();
  size_t p = inputCursorPos;
  if (isBackspace) p--;
  if (p>0) jsvAppendStringVar(v, inputLine, 0, p); // add before cursor (delete)
  if (p+1<l) jsvAppendStringVar(v, inputLine, p+1, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine=v;
  if (isBackspace)
    inputCursorPos--; // move cursor back

  // update the console
  if (jsiShowInputLine()) {
    if (deleteNewline) {
      // we already removed everything, so just put it back
      jsiConsolePrintStringVarWithNewLineChar(inputLine, inputCursorPos, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos); // move cursor back
    } else {
      // clear the character and move line back
      if (isBackspace) jsiConsolePrintChar(0x08);
      jsiConsolePrintStringVarUntilEOL(inputLine, inputCursorPos, 0xFFFFFFFF, true/*and backup*/);
    }
  }
}

void jsiHandleHome() {
  while (inputCursorPos>0 && jsvGetCharInString(inputLine,inputCursorPos-1)!='\n') {
    if (jsiShowInputLine()) jsiConsolePrintChar(0x08);
    inputCursorPos--;
  }
}

void jsiHandleEnd() {
  size_t l = jsvGetStringLength(inputLine);
  while (inputCursorPos<l && jsvGetCharInString(inputLine,inputCursorPos)!='\n') {
    if (jsiShowInputLine())
      jsiConsolePrintChar(jsvGetCharInString(inputLine,inputCursorPos));
    inputCursorPos++;
  }
}

/** Page up/down move cursor to beginnint or end */
void jsiHandlePageUpDown(bool isDown) {
  size_t x,y;
  jsvGetLineAndCol(inputLine, inputCursorPos, &y, &x);
  if (!isDown) { // up
    inputCursorPos = 0;
  } else { // down
    inputCursorPos = jsvGetStringLength(inputLine);
  }
  size_t newX=x,newY=y;
  jsvGetLineAndCol(inputLine, inputCursorPos, &newY, &newX);
  jsiMoveCursor(x,y,newX,newY);
}

void jsiHandleMoveUpDown(int direction) {
  size_t x,y, lines=jsvGetLinesInString(inputLine);
  jsvGetLineAndCol(inputLine, inputCursorPos, &y, &x);
  size_t newX=x,newY=y;
  newY = (size_t)((int)newY + direction);
  if (newY<1) newY=1;
  if (newY>lines) newY=lines;
  // work out cursor pos and feed back through - we might not be able to get right to the same place
  // if we move up
  inputCursorPos = jsvGetIndexFromLineAndCol(inputLine, newY, newX);
  jsvGetLineAndCol(inputLine, inputCursorPos, &newY, &newX);
  if (jsiShowInputLine()) {
    jsiMoveCursor(x,y,newX,newY);
  }
}

bool jsiAtEndOfInputLine() {
  size_t i = inputCursorPos, l = jsvGetStringLength(inputLine);
  while (i < l) {
    if (!isWhitespace(jsvGetCharInString(inputLine, i)))
      return false;
    i++;
  }
  return true;
}

void jsiHandleNewLine(bool execute) {
  if (jsiAtEndOfInputLine()) { // at EOL so we need to figure out if we can execute or not
    if (execute && jsiCountBracketsInInput()<=0) { // actually execute!
      if (jsiShowInputLine()) {
        jsiConsolePrint("\n");
      }
      if (!(jsiStatus & JSIS_ECHO_OFF_FOR_LINE))
        inputLineRemoved = true;

      // Get line to execute, and reset inputLine
      JsVar *lineToExecute = jsvStringTrimRight(inputLine);
      jsiInputLineCursorMoved();
      jsvUnLock(inputLine);
      inputLine = jsvNewFromEmptyString();
      inputCursorPos = 0;
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER) {
        jsiDebuggerLine(lineToExecute);
        jsvUnLock(lineToExecute);
      } else
#endif
      {
        // execute!
        JsVar *v = jspEvaluateVar(lineToExecute, 0, false);
        // add input line to history
        jsiHistoryAddLine(lineToExecute);
        jsvUnLock(lineToExecute);
        // print result (but NOT if we had an error)
        if (jsiEcho() && !jspHasError()) {
          jsiConsolePrintChar('=');
          jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
          jsiConsolePrint("\n");
        }
        jsvUnLock(v);
      }
      // console will be returned next time around the input loop
      // if we had echo off just for this line, reinstate it!
      jsiStatus &= ~JSIS_ECHO_OFF_FOR_LINE;
    } else {
      // Brackets aren't all closed, so we're going to append a newline
      // without executing
      if (jsiShowInputLine()) jsiConsolePrint("\n:");
      jsiIsAboutToEditInputLine();
      jsiAppendToInputLine("\n");
      inputCursorPos++;
    }
  } else { // new line - but not at end of line!
    jsiIsAboutToEditInputLine();
    if (jsiShowInputLine()) jsiConsoleEraseStringVarFrom(inputLine, inputCursorPos, false/*no need to erase the char before*/); // erase all in front
    JsVar *v = jsvNewFromEmptyString();
    if (inputCursorPos>0) jsvAppendStringVar(v, inputLine, 0, inputCursorPos);
    jsvAppendCharacter(v, '\n');
    jsvAppendStringVar(v, inputLine, inputCursorPos, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
    jsiInputLineCursorMoved();
    jsvUnLock(inputLine);
    inputLine=v;
    if (jsiShowInputLine()) { // now print the rest
      jsiConsolePrintStringVarWithNewLineChar(inputLine, inputCursorPos, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos+1); // move cursor back
    }
    inputCursorPos++;
  }
}

void jsiHandleChar(char ch) {
  // jsiConsolePrintf("[%d:%d]\n", inputState, ch);
  //
  // special stuff
  // 1 - Ctrl-a - beginning of line
  // 4 - Ctrl-d - backwards delete
  // 5 - Ctrl-e - end of line
  // 21 - Ctrl-u - delete line
  // 23 - Ctrl-w - delete word (currently just does the same as Ctrl-u)
  //
  // 27 then 91 then 68 - left
  // 27 then 91 then 67 - right
  // 27 then 91 then 65 - up
  // 27 then 91 then 66 - down
  // 27 then 91 then 50 then 75 - Erases the entire current line.
  // 27 then 91 then 51 then 126 - backwards delete
  // 27 then 91 then 52 then 126 - numpad end
  // 27 then 91 then 49 then 126 - numpad home
  // 27 then 91 then 53 then 126 - pgup
  // 27 then 91 then 54 then 126 - pgdn
  // 27 then 79 then 70 - home
  // 27 then 79 then 72 - end
  // 27 then 10 - alt enter


  if (ch == 0) {
    inputState = IS_NONE; // ignore 0 - it's scary
  } else if (ch == 1) { // Ctrl-a
    jsiHandleHome();
  } else if (ch == 4) { // Ctrl-d
    jsiHandleDelete(false/*not backspace*/);
  } else if (ch == 5) { // Ctrl-e
    jsiHandleEnd();
  } else if (ch == 21 || ch == 23) { // Ctrl-u or Ctrl-w
    jsiClearInputLine();
  } else if (ch == 27) {
    inputState = IS_HAD_27;
  } else if (inputState==IS_HAD_27) {
    inputState = IS_NONE;
    if (ch == 79)
      inputState = IS_HAD_27_79;
    else if (ch == 91)
      inputState = IS_HAD_27_91;
    else if (ch == 10)
      jsiHandleNewLine(false);
  } else if (inputState==IS_HAD_27_79) { // Numpad
    inputState = IS_NONE;
    if (ch == 70) jsiHandleEnd();
    else if (ch == 72) jsiHandleHome();
    else if (ch == 111) jsiHandleChar('/');
    else if (ch == 106) jsiHandleChar('*');
    else if (ch == 109) jsiHandleChar('-');
    else if (ch == 107) jsiHandleChar('+');
    else if (ch == 77) jsiHandleChar('\r');
  } else if (inputState==IS_HAD_27_91) {
    inputState = IS_NONE;
    if (ch==68) { // left
      if (inputCursorPos>0 && jsvGetCharInString(inputLine,inputCursorPos-1)!='\n') {
        inputCursorPos--;
        if (jsiShowInputLine()) {
          jsiConsolePrint("\x1B[D"); // 27,91,68 - left
        }
      }
    } else if (ch==67) { // right
      if (inputCursorPos<jsvGetStringLength(inputLine) && jsvGetCharInString(inputLine,inputCursorPos)!='\n') {
        inputCursorPos++;
        if (jsiShowInputLine()) {
          jsiConsolePrint("\x1B[C"); // 27,91,67 - right
        }
      }
    } else if (ch==65) { // up
      size_t l = jsvGetStringLength(inputLine);
      if ((l==0 || jsiIsInHistory(inputLine)) && inputCursorPos==l)
        jsiChangeToHistory(true); // if at end of line
      else
        jsiHandleMoveUpDown(-1);
    } else if (ch==66) { // down
      size_t l = jsvGetStringLength(inputLine);
      if ((l==0 || jsiIsInHistory(inputLine)) && inputCursorPos==l)
        jsiChangeToHistory(false); // if at end of line
      else
        jsiHandleMoveUpDown(1);
    } else if (ch==49) {
      inputState=IS_HAD_27_91_49;
    } else if (ch==50) {
      inputState=IS_HAD_27_91_50;
    } else if (ch==51) {
      inputState=IS_HAD_27_91_51;
    } else if (ch==52) {
      inputState=IS_HAD_27_91_52;
    } else if (ch==53) {
      inputState=IS_HAD_27_91_53;
    } else if (ch==54) {
      inputState=IS_HAD_27_91_54;
    }
  } else if (inputState==IS_HAD_27_91_49) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad Home
      jsiHandleHome();
    }
  } else if (inputState==IS_HAD_27_91_50) {
    inputState = IS_NONE;
    if (ch==75) { // Erase current line
      jsiClearInputLine();
    }
  } else if (inputState==IS_HAD_27_91_51) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad (forwards) Delete
      jsiHandleDelete(false/*not backspace*/);
    }
  } else if (inputState==IS_HAD_27_91_52) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad End
      jsiHandleEnd();
    }
  } else if (inputState==IS_HAD_27_91_53) {
    inputState = IS_NONE;
    if (ch==126) { // Page Up
      jsiHandlePageUpDown(0);
    }
  } else if (inputState==IS_HAD_27_91_54) {
    inputState = IS_NONE;
    if (ch==126) { // Page Down
      jsiHandlePageUpDown(1);
    }
  } else if (ch==16 && jsvGetStringLength(inputLine)==0) {
    /* DLE - Data Link Escape
    Espruino uses DLE on the start of a line to signal that just the line in
    question should be executed without echo */
    jsiStatus  |= JSIS_ECHO_OFF_FOR_LINE;
  } else {  
    inputState = IS_NONE;
    if (ch == 0x08 || ch == 0x7F /*delete*/) {
      jsiHandleDelete(true /*backspace*/);
    } else if (ch == '\n' && inputState == IS_HAD_R) {
      inputState = IS_NONE; //  ignore \ r\n - we already handled it all on \r
    } else if (ch == '\r' || ch == '\n') { 
      if (ch == '\r') inputState = IS_HAD_R;
      jsiHandleNewLine(true);
    } else if (ch>=32 || ch=='\t') {
      // Add the character to our input line
      jsiIsAboutToEditInputLine();
      char buf[2] = {ch,0};
      const char *strToAppend = (ch=='\t') ? "    " : buf;
      size_t strSize = (ch=='\t') ? 4 : 1;

      if (inputLineLength < 0)
        inputLineLength = (int)jsvGetStringLength(inputLine);

      if ((int)inputCursorPos>=inputLineLength) { // append to the end
        jsiAppendToInputLine(strToAppend);
      } else { // add in halfway through
        JsVar *v = jsvNewFromEmptyString();
        if (inputCursorPos>0) jsvAppendStringVar(v, inputLine, 0, inputCursorPos);
        jsvAppendString(v, strToAppend);
        jsvAppendStringVar(v, inputLine, inputCursorPos, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
        jsiInputLineCursorMoved();
        jsvUnLock(inputLine);
        inputLine=v;
        if (jsiShowInputLine()) jsiConsolePrintStringVarUntilEOL(inputLine, inputCursorPos, 0xFFFFFFFF, true/*and backup*/);
      }
      inputCursorPos += strSize; // no need for jsiInputLineCursorMoved(); as we just appended
      if (jsiShowInputLine()) {
        jsiConsolePrint(strToAppend);
      }
    }
  }
}

/// Queue a function, string, or array (of funcs/strings) to be executed next time around the idle loop
void jsiQueueEvents(JsVar *object, JsVar *callback, JsVar **args, int argCount) { // an array of functions, a string, or a single function
  assert(argCount<10);

  JsVar *event = jsvNewWithFlags(JSV_OBJECT);
  if (event) { // Could be out of memory error!
    jsvUnLock(jsvAddNamedChild(event, callback, "func"));

    if (argCount) {
      JsVar *arr = jsvNewArray(args, argCount);
      if (arr) {
        jsvUnLock2(jsvAddNamedChild(event, arr, "args"), arr);
      }
    }
    if (object) jsvUnLock(jsvAddNamedChild(event, object, "this"));

    jsvArrayPushAndUnLock(events, event);
  }
}

bool jsiObjectHasCallbacks(JsVar *object, const char *callbackName) {
  JsVar *callback = jsvObjectGetChild(object, callbackName, 0);
  bool hasCallbacks = !jsvIsUndefined(callback);
  jsvUnLock(callback);
  return hasCallbacks;
}

void jsiQueueObjectCallbacks(JsVar *object, const char *callbackName, JsVar **args, int argCount) {
  JsVar *callback = jsvObjectGetChild(object, callbackName, 0);
  if (!callback) return;
  jsiQueueEvents(object, callback, args, argCount);
  jsvUnLock(callback);
}

void jsiExecuteEvents() {
  bool hasEvents = !jsvArrayIsEmpty(events);
  if (hasEvents) jsiSetBusy(BUSY_INTERACTIVE, true);
  while (!jsvArrayIsEmpty(events)) {
    JsVar *event = jsvSkipNameAndUnLock(jsvArrayPopFirst(events));
    // Get function to execute
    JsVar *func = jsvObjectGetChild(event, "func", 0);
    JsVar *thisVar = jsvObjectGetChild(event, "this", 0);
    JsVar *argsArray = jsvObjectGetChild(event, "args", 0);
    // free actual event
    jsvUnLock(event);
    // now run..
    jsiExecuteEventCallbackArgsArray(thisVar, func, argsArray);
    jsvUnLock(argsArray);
    //jsPrint("Event Done\n");
    jsvUnLock2(func, thisVar);
  }
  if (hasEvents) {
    jsiSetBusy(BUSY_INTERACTIVE, false);
    if (jspIsInterrupted() || jsiTimeSinceCtrlC<CTRL_C_TIME_FOR_BREAK)
      interruptedDuringEvent = true;
  }
}

NO_INLINE bool jsiExecuteEventCallbackArgsArray(JsVar *thisVar, JsVar *callbackVar, JsVar *argsArray) { // array of functions or single function
  unsigned int l = (unsigned int)jsvGetArrayLength(argsArray);
  JsVar **args = 0;
  if (l) {
    args = alloca(sizeof(JsVar*) * l);
    if (!args) return false;
    jsvGetArrayItems(argsArray, l, args); // not very fast
  }
  bool r = jsiExecuteEventCallback(thisVar, callbackVar, l, args);
  jsvUnLockMany(l, args);
  return r;
}

NO_INLINE bool jsiExecuteEventCallback(JsVar *thisVar, JsVar *callbackVar, unsigned int argCount, JsVar **argPtr) { // array of functions or single function
  JsVar *callbackNoNames = jsvSkipName(callbackVar);

  bool ok = true;
  if (callbackNoNames) {
    if (jsvIsArray(callbackNoNames)) {
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, callbackNoNames);
      while (ok && jsvObjectIteratorHasValue(&it)) {
        JsVar *child = jsvObjectIteratorGetValue(&it);
        ok &= jsiExecuteEventCallback(thisVar, child, argCount, argPtr);
        jsvUnLock(child);
        jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
    } else if (jsvIsFunction(callbackNoNames)) {
      jsvUnLock(jspExecuteFunction(callbackNoNames, thisVar, (int)argCount, argPtr));
    } else if (jsvIsString(callbackNoNames)) {
      jsvUnLock(jspEvaluateVar(callbackNoNames, 0, false));
    } else
      jsError("Unknown type of callback in Event Queue");
    jsvUnLock(callbackNoNames);
  }
  if (!ok || jspIsInterrupted() || jsiTimeSinceCtrlC<CTRL_C_TIME_FOR_BREAK) {
    interruptedDuringEvent = true;
    return false;
  }
  return true;
}

bool jsiHasTimers() {
  if (!timerArray) return false;
  JsVar *timerArrayPtr = jsvLock(timerArray);
  bool hasTimers = !jsvArrayIsEmpty(timerArrayPtr);
  jsvUnLock(timerArrayPtr);
  return hasTimers;
}

/// Is the given watch object meant to be executed when the current value of the pin is pinIsHigh
bool jsiShouldExecuteWatch(JsVar *watchPtr, bool pinIsHigh) {
  int watchEdge = (int)jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "edge", 0));
  return watchEdge==0 || // any edge
      (pinIsHigh && watchEdge>0) || // rising edge
      (!pinIsHigh && watchEdge<0); // falling edge
}

bool jsiIsWatchingPin(Pin pin) {
  bool isWatched = false;
  JsVar *watchArrayPtr = jsvLock(watchArray);
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, watchArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
    JsVar *pinVar = jsvObjectGetChild(watchPtr, "pin", 0);
    if (jshGetPinFromVar(pinVar) == pin)
      isWatched = true;
    jsvUnLock2(pinVar, watchPtr);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  jsvUnLock(watchArrayPtr);
  return isWatched;
}

void jsiHandleIOEventForUSART(JsVar *usartClass, IOEvent *event) {
  /* work out byteSize. On STM32 we fake 7 bit, and it's easier to
   * check the options and work out the masking here than it is to
   * do it in the IRQ */
  unsigned char bytesize = 8;
  JsVar *options = jsvObjectGetChild(usartClass, DEVICE_OPTIONS_NAME, 0);
  if(jsvIsObject(options)) {
    unsigned char c = (unsigned char)jsvGetIntegerAndUnLock(jsvObjectGetChild(options, "bytesize", 0));
    if (c>=7 && c<10) bytesize = c;
  }
  jsvUnLock(options);

  JsVar *stringData = jsvNewFromEmptyString();
  if (stringData) {
    JsvStringIterator it;
    jsvStringIteratorNew(&it, stringData, 0);

    int i, chars = IOEVENTFLAGS_GETCHARS(event->flags);
    while (chars) {
      for (i=0;i<chars;i++) {
        char ch = (char)(event->data.chars[i] & ((1<<bytesize)-1)); // mask
        jsvStringIteratorAppend(&it, ch);
      }
      // look down the stack and see if there is more data
      if (jshIsTopEvent(IOEVENTFLAGS_GETTYPE(event->flags))) {
        jshPopIOEvent(event);
        chars = IOEVENTFLAGS_GETCHARS(event->flags);
      } else
        chars = 0;
    }
    jsvStringIteratorFree(&it);

    // Now run the handler
    jswrap_stream_pushData(usartClass, stringData, true);
    jsvUnLock(stringData);
  }
}

void jsiHandleIOEventForConsole(IOEvent *event) {
  int i, c = IOEVENTFLAGS_GETCHARS(event->flags);
  jsiSetBusy(BUSY_INTERACTIVE, true);
  for (i=0;i<c;i++) jsiHandleChar(event->data.chars[i]);
  jsiSetBusy(BUSY_INTERACTIVE, false);
}

void jsiIdle() {
  // This is how many times we have been here and not done anything.
  // It will be zeroed if we do stuff later
  if (loopsIdling<255) loopsIdling++;

  // Handle hardware-related idle stuff (like checking for pin events)
  bool wasBusy = false;
  IOEvent event;
  int maxEvents = IOBUFFERMASK+1; // ensure we can't get totally swamped by having more events than we can process
  while (maxEvents-- && jshPopIOEvent(&event)) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    wasBusy = true;

    IOEventFlags eventType = IOEVENTFLAGS_GETTYPE(event.flags);

    loopsIdling = 0; // because we're not idling
    if (eventType == consoleDevice) {
      jsiHandleIOEventForConsole(&event);
      /** don't allow us to read data when the device is our
       console device. It slows us down and just causes pain. */
    } else if (DEVICE_IS_USART(eventType)) {
      // ------------------------------------------------------------------------ SERIAL CALLBACK
      JsVar *usartClass = jsvSkipNameAndUnLock(jsiGetClassNameFromDevice(IOEVENTFLAGS_GETTYPE(event.flags)));
      if (jsvIsObject(usartClass)) {
        jsiHandleIOEventForUSART(usartClass, &event);
      }
      jsvUnLock(usartClass);
    } else if (DEVICE_IS_EXTI(eventType)) { // ---------------------------------------------------------------- PIN WATCH
      // we have an event... find out what it was for...
      // Check everything in our Watch array
      JsVar *watchArrayPtr = jsvLock(watchArray);
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, watchArrayPtr);
      while (jsvObjectIteratorHasValue(&it)) {
        bool hasDeletedWatch = false;
        JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
        Pin pin = jshGetPinFromVarAndUnLock(jsvObjectGetChild(watchPtr, "pin", 0));

        if (jshIsEventForPin(&event, pin)) {
          /** Work out event time. Events time is only stored in 32 bits, so we need to
           * use the correct 'high' 32 bits from the current time.
           *
           * We know that the current time is always newer than the event time, so
           * if the bottom 32 bits of the current time is less than the bottom
           * 32 bits of the event time, we need to subtract a full 32 bits worth
           * from the current time.
           */
          JsSysTime time = jshGetSystemTime();
          if (((unsigned int)time) < (unsigned int)event.data.time)
            time = time - 0x100000000LL;
          // finally, mask in the event's time
          JsSysTime eventTime = (time & ~0xFFFFFFFFLL) | (JsSysTime)event.data.time;

          // Now actually process the event
          bool pinIsHigh = (event.flags&EV_EXTI_IS_HIGH)!=0;

          bool executeNow = false;
          JsVarInt debounce = jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "debounce", 0));
          if (debounce<=0) {
            executeNow = true;
          } else { // Debouncing - use timeouts to ensure we only fire at the right time
            // store the current state of the pin
            bool oldWatchState = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr, "state",0));
            jsvObjectSetChildAndUnLock(watchPtr, "state", jsvNewFromBool(pinIsHigh));

            JsVar *timeout = jsvObjectGetChild(watchPtr, "timeout", 0);
            if (timeout) { // if we had a timeout, update the callback time
              JsSysTime timeoutTime = jsiLastIdleTime + (JsSysTime)jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timeout, "time", 0));
              jsvUnLock(jsvObjectSetChild(timeout, "time", jsvNewFromLongInteger((JsSysTime)(eventTime - jsiLastIdleTime) + debounce)));
              if (eventTime > timeoutTime) {
                // timeout should have fired, but we didn't get around to executing it!
                // Do it now (with the old timeout time)
                executeNow = true;
                eventTime = timeoutTime - debounce;
                pinIsHigh = oldWatchState;
              }
            } else { // else create a new timeout
              timeout = jsvNewWithFlags(JSV_OBJECT);
              if (timeout) {
                jsvObjectSetChild(timeout, "watch", watchPtr); // no unlock
                jsvObjectSetChildAndUnLock(timeout, "time", jsvNewFromLongInteger((JsSysTime)(eventTime - jsiLastIdleTime) + debounce));
                jsvObjectSetChildAndUnLock(timeout, "callback", jsvObjectGetChild(watchPtr, "callback", 0));
                jsvObjectSetChildAndUnLock(timeout, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
                jsvObjectSetChildAndUnLock(timeout, "pin", jsvNewFromPin(pin));
                // Add to timer array
                jsiTimerAdd(timeout);
                // Add to our watch
                jsvObjectSetChild(watchPtr, "timeout", timeout); // no unlock
              }
            }
            jsvUnLock(timeout);
          }

          // If we want to execute this watch right now...
          if (executeNow) {
            JsVar *timePtr = jsvNewFromFloat(jshGetMillisecondsFromTime(eventTime)/1000);
            if (jsiShouldExecuteWatch(watchPtr, pinIsHigh)) { // edge triggering
              JsVar *watchCallback = jsvObjectGetChild(watchPtr, "callback", 0);
              bool watchRecurring = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr,  "recur", 0));
              JsVar *data = jsvNewWithFlags(JSV_OBJECT);
              if (data) {
                jsvObjectSetChildAndUnLock(data, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
                // set both data.time, and watch.lastTime in one go
                jsvObjectSetChild(data, "time", timePtr); // no unlock
                jsvObjectSetChildAndUnLock(data, "pin", jsvNewFromPin(pin));
                jsvObjectSetChildAndUnLock(data, "state", jsvNewFromBool(pinIsHigh));
              }
              if (!jsiExecuteEventCallback(0, watchCallback, 1, &data) && watchRecurring) {
                jsError("Ctrl-C while processing watch - removing it.");
                jsErrorFlags |= JSERR_CALLBACK;
                watchRecurring = false;
              }
              jsvUnLock(data);
              if (!watchRecurring) {
                // free all
                jsvObjectIteratorRemoveAndGotoNext(&it, watchArrayPtr);
                hasDeletedWatch = true;
                if (!jsiIsWatchingPin(pin))
                  jshPinWatch(pin, false);
              }
              jsvUnLock(watchCallback);
            }
            jsvObjectSetChildAndUnLock(watchPtr, "lastTime", timePtr);
          }
        }

        jsvUnLock(watchPtr);
        if (!hasDeletedWatch)
          jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
      jsvUnLock(watchArrayPtr);
    }
  }

  // Reset Flow control if it was set...
  if (jshGetEventsUsed() < IOBUFFER_XON) { 
    jshSetFlowControlXON(EV_USBSERIAL, true);
    int i;
    for (i=0;i<USART_COUNT;i++)
      jshSetFlowControlXON(EV_SERIAL1+i, true);
  }

  // Check timers
  JsSysTime minTimeUntilNext = JSSYSTIME_MAX;
  JsSysTime time = jshGetSystemTime();
  JsSysTime timePassed = time - jsiLastIdleTime;
  jsiLastIdleTime = time;
  // add time to Ctrl-C counter, checking for overflow
  uint32_t oldTimeSinceCtrlC = jsiTimeSinceCtrlC;
  jsiTimeSinceCtrlC += (uint32_t)timePassed;
  if (oldTimeSinceCtrlC > jsiTimeSinceCtrlC)
    jsiTimeSinceCtrlC = 0xFFFFFFFF;

  jsiStatus = jsiStatus & ~JSIS_TIMERS_CHANGED;
  JsVar *timerArrayPtr = jsvLock(timerArray);
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, timerArrayPtr);
  while (jsvObjectIteratorHasValue(&it) && !(jsiStatus & JSIS_TIMERS_CHANGED)) {
    bool hasDeletedTimer = false;
    JsVar *timerPtr = jsvObjectIteratorGetValue(&it);
    JsSysTime timerTime = (JsSysTime)jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timerPtr, "time", 0));
    JsSysTime timeUntilNext = timerTime - timePassed;

    if (timeUntilNext<=0) {
      // we're now doing work
      jsiSetBusy(BUSY_INTERACTIVE, true);
      wasBusy = true;
      JsVar *timerCallback = jsvObjectGetChild(timerPtr, "callback", 0);
      JsVar *watchPtr = jsvObjectGetChild(timerPtr, "watch", 0); // for debounce - may be undefined
      bool exec = true;
      JsVar *data = 0;
      if (watchPtr) {
        data = jsvNewWithFlags(JSV_OBJECT);
        // if we were from a watch then we were delayed by the debounce time...
        if (data) {
          JsVarInt delay = jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "debounce", 0));
          // Create the 'time' variable that will be passed to the user
          JsVar *timePtr = jsvNewFromFloat(jshGetMillisecondsFromTime(jsiLastIdleTime+timeUntilNext-delay)/1000);
          // if it was a watch, set the last state up
          bool state = jsvGetBoolAndUnLock(jsvObjectSetChild(data, "state", jsvObjectGetChild(watchPtr, "state", 0)));
          exec = jsiShouldExecuteWatch(watchPtr, state);
          // set up the lastTime variable of data to what was in the watch
          jsvObjectSetChildAndUnLock(data, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
          // set up the watches lastTime to this one
          jsvObjectSetChild(watchPtr, "lastTime", timePtr); // don't unlock
          jsvObjectSetChildAndUnLock(data, "time", timePtr);
        }
      }
      JsVar *interval = jsvObjectGetChild(timerPtr, "interval", 0);
      if (exec) {
        bool execResult;
        if (data) {
          execResult = jsiExecuteEventCallback(0, timerCallback, 1, &data);
        } else {
          JsVar *argsArray = jsvObjectGetChild(timerPtr, "args", 0);
          execResult = jsiExecuteEventCallbackArgsArray(0, timerCallback, argsArray);
          jsvUnLock(argsArray);
        }
        if (!execResult && interval) {
          jsError("Ctrl-C while processing interval - removing it.");
          jsErrorFlags |= JSERR_CALLBACK;
          // by setting interval to 0, we now think we've for a Timeout,
          // which will get removed.
          jsvUnLock(interval);
          interval = 0;
        }
      }
      jsvUnLock(data);
      if (watchPtr) { // if we had a watch pointer, be sure to remove us from it
        jsvObjectSetChild(watchPtr, "timeout", 0);
        // Deal with non-recurring watches
        if (exec) {
          bool watchRecurring = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr,  "recur", 0));
          if (!watchRecurring) {
            JsVar *watchArrayPtr = jsvLock(watchArray);
            JsVar *watchNamePtr = jsvGetArrayIndexOf(watchArrayPtr, watchPtr, true);
            if (watchNamePtr) {
              jsvRemoveChild(watchArrayPtr, watchNamePtr);
              jsvUnLock(watchNamePtr);
            }
            jsvUnLock(watchArrayPtr);
            Pin pin = jshGetPinFromVarAndUnLock(jsvObjectGetChild(watchPtr, "pin", 0));
            if (!jsiIsWatchingPin(pin))
              jshPinWatch(pin, false);
          }
        }
        jsvUnLock(watchPtr);
      }

      if (interval) {
        timeUntilNext = timeUntilNext + jsvGetLongIntegerAndUnLock(interval);
      } else {
        // free
        // Beware... may have already been removed!
        jsvObjectIteratorRemoveAndGotoNext(&it, timerArrayPtr);
        hasDeletedTimer = true;
        timeUntilNext = -1;
      }
      jsvUnLock(timerCallback);

    }
    // update the time until the next timer
    if (timeUntilNext>=0 && timeUntilNext < minTimeUntilNext)
      minTimeUntilNext = timeUntilNext;
    // update the timer's time
    if (!hasDeletedTimer) {
      jsvObjectSetChildAndUnLock(timerPtr, "time", jsvNewFromLongInteger(timeUntilNext));
      jsvObjectIteratorNext(&it);
    }
    jsvUnLock(timerPtr);
  }
  jsvObjectIteratorFree(&it);
  jsvUnLock(timerArrayPtr);
  /* We might have left the timers loop with stuff to do because the contents of it
   * changed. It's not a big deal because it could only have changed because a timer
   * got executed - so `wasBusy` got set and we know we're going to go around the
   * loop again before sleeping.
   */ 

  // Check for events that might need to be processed from other libraries
  if (jswIdle()) wasBusy = true;

  // Just in case we got any events to do and didn't clear loopsIdling before
  if (wasBusy || !jsvArrayIsEmpty(events) )
    loopsIdling = 0;

  if (wasBusy)
    jsiSetBusy(BUSY_INTERACTIVE, false);

  // execute any outstanding events
  if (!jspIsInterrupted()) {
    jsiExecuteEvents();
  }
  if (interruptedDuringEvent) {
    jspSetInterrupted(false);
    interruptedDuringEvent = false;
    jsiConsoleRemoveInputLine();
    jsiConsolePrint("Execution Interrupted during event processing.\n");
  }

  // check for TODOs
  if (jsiStatus&JSIS_TODO_MASK) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_RESET) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;
      // shut down everything and start up again
      jsiKill();
      jsvKill();
      jshReset();
      jsvInit();
      jsiSemiInit(false, false); // don't autoload
    }
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_FLASH_SAVE) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;

      jsvGarbageCollect(); // nice to have everything all tidy!
      jsiSoftKill();
      jspSoftKill();
      jsvSoftKill();
      jsfSaveToFlash();
      jshReset();
      jsvSoftInit();
      jspSoftInit();
      jsiSoftInit();
    }
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_FLASH_LOAD) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;

      jsiSoftKill();
      jspSoftKill();
      jsvSoftKill();
      jshReset();
      jsfLoadFromFlash();
      jsvSoftInit();
      jspSoftInit();
      jsiSoftInit();
    }
    jsiSetBusy(BUSY_INTERACTIVE, false);
  }

  /* if we've been around this loop, there is nothing to do, and
   * we have a spare 10ms then let's do some Garbage Collection
   * just in case. */
  if (loopsIdling==1 &&
      minTimeUntilNext > jshGetTimeFromMilliseconds(10)) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    jsvGarbageCollect();
    jsiSetBusy(BUSY_INTERACTIVE, false);
  }

  // Go to sleep!
  if (loopsIdling>1 && // once around the idle loop without having done any work already (just in case)
#ifdef USB
      !jshIsUSBSERIALConnected() && // if USB is on, no point sleeping (later, sleep might be more drastic)
#endif
      !jshHasEvents() && //no events have arrived in the mean time
      !jshHasTransmitData()/* && //nothing left to send over serial?
      minTimeUntilNext > SYSTICK_RANGE*5/4*/) { // we are sure we won't miss anything - leave a little leeway (SysTick will wake us up!)
    jshSleep(minTimeUntilNext);
  }
}

bool jsiLoop() {
  // idle stuff for hardware
  jshIdle();
  // Do general idle stuff
  jsiIdle();

  JsVar *exception = jspGetException();
  if (exception) {
    jsiConsolePrintf("Uncaught %v\n", exception);
    jsvUnLock(exception);
  }

  if (jspIsInterrupted()) {
    jsiConsoleRemoveInputLine();
    jsiConsolePrint("Execution Interrupted\n");
    jspSetInterrupted(false);
  }
  JsVar *stackTrace = jspGetStackTrace();
  if (stackTrace) {
    jsiConsolePrintStringVar(stackTrace);
    jsvUnLock(stackTrace);
  }

  // If Ctrl-C was pressed, clear the line
  if (execInfo.execute & EXEC_CTRL_C_MASK) {
    execInfo.execute = execInfo.execute & (JsExecFlags)~EXEC_CTRL_C_MASK;
    if (jsvIsEmptyString(inputLine)) {
#ifndef EMBEDDED
      if (jsiTimeSinceCtrlC < jshGetTimeFromMilliseconds(5000))
        exit(0); // exit if ctrl-c on empty input line
      else
        jsiConsolePrintf("Press Ctrl-C again to exit\n");
#endif
      jsiTimeSinceCtrlC = 0;
    }
    jsiClearInputLine();
  }

  // return console (if it was gone!)
  jsiReturnInputLine();

  return loopsIdling==0;
}

/** Output extra functions defined in an object such that they can be copied to a new device */
NO_INLINE void jsiDumpObjectState(vcbprintf_callback user_callback, void *user_data, JsVar *parentName, JsVar *parent) {
  JsvIsInternalChecker checker = jsvGetInternalFunctionCheckerFor(parent);
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, parent);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *child = jsvObjectIteratorGetKey(&it);
    JsVar *data = jsvObjectIteratorGetValue(&it);

    if (!checker || !checker(child)) {
      if (jsvIsStringEqual(child, JSPARSE_PROTOTYPE_VAR)) {
        // recurse to print prototypes
        JsVar *name = jsvNewFromStringVar(parentName,0,JSVAPPENDSTRINGVAR_MAXLENGTH);
        if (name) {
          jsvAppendString(name, ".prototype");
          jsiDumpObjectState(user_callback, user_data, name, data);
          jsvUnLock(name);
        }
      } else {
        if (!jsvIsNative(data)) {

          cbprintf(user_callback, user_data, "%v.%v = ", parentName, child);
          jsiDumpJSON(user_callback, user_data, data, 0);
          user_callback(";\n", user_data);
        }
      }
    }
    jsvUnLock2(data, child);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
}

/** Output current interpreter state such that it can be copied to a new device */
void jsiDumpState(vcbprintf_callback user_callback, void *user_data) {
  JsvObjectIterator it;

  jsvObjectIteratorNew(&it, execInfo.root);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *child = jsvObjectIteratorGetKey(&it);
    JsVar *data = jsvObjectIteratorGetValue(&it);
    char childName[JSLEX_MAX_TOKEN_LENGTH];
    jsvGetString(child, childName, JSLEX_MAX_TOKEN_LENGTH);

    if (jswIsBuiltInObject(childName)) {
      jsiDumpObjectState(user_callback, user_data, child, data);
    } else if (jsvIsStringEqual(child, JSI_TIMERS_NAME)) {
      // skip - done later
    } else if (jsvIsStringEqual(child, JSI_WATCHES_NAME)) {
      // skip - done later
    } else if (child->varData.str[0]==JS_HIDDEN_CHAR ||
        jshFromDeviceString(childName)!=EV_NONE) {
      // skip - don't care about this stuff
    } else if (!jsvIsNative(data)) { // just a variable/function!
      if (jsvIsFunction(data)) {
        // function-specific output
        cbprintf(user_callback, user_data, "function %v", child);
        jsfGetJSONForFunctionWithCallback(data, JSON_SHOW_DEVICES, user_callback, user_data);
        user_callback("\n", user_data);
        // print any prototypes we had
        jsiDumpObjectState(user_callback, user_data, child, data);
      } else {
        // normal variable definition
        cbprintf(user_callback, user_data, "var %v = ", child);
        bool hasProto = false;
        if (jsvIsObject(data)) {
          JsVar *proto = jsvObjectGetChild(data, JSPARSE_INHERITS_VAR, 0);
          if (proto) {
            JsVar *protoName = jsvGetPathTo(execInfo.root, proto, 4, data);
            if (protoName) {
              cbprintf(user_callback, user_data, "Object.create(%v);\n", protoName);
              jsiDumpObjectState(user_callback, user_data, child, data);
              hasProto = true;
            }
          }
        }
        if (!hasProto) {
          jsiDumpJSON(user_callback, user_data, data, child);
          user_callback(";\n", user_data);
        }
      }
    }
    jsvUnLock2(data, child);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  // Now do timers
  JsVar *timerArrayPtr = jsvLock(timerArray);
  jsvObjectIteratorNew(&it, timerArrayPtr);
  jsvUnLock(timerArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *timer = jsvObjectIteratorGetValue(&it);
    JsVar *timerCallback = jsvSkipOneNameAndUnLock(jsvFindChildFromString(timer, "callback", false));
    JsVar *timerInterval = jsvObjectGetChild(timer, "interval", 0);
    user_callback(timerInterval ? "setInterval(" : "setTimeout(", user_data);
    jsiDumpJSON(user_callback, user_data, timerCallback, 0);
    cbprintf(user_callback, user_data, ", %f);\n", jshGetMillisecondsFromTime(timerInterval ? jsvGetLongInteger(timerInterval) : jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timer, "time", 0))));
    jsvUnLock2(timerInterval, timerCallback);
    // next
    jsvUnLock(timer);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  // Now do watches
  JsVar *watchArrayPtr = jsvLock(watchArray);
  jsvObjectIteratorNew(&it, watchArrayPtr);
  jsvUnLock(watchArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *watch = jsvObjectIteratorGetValue(&it);
    JsVar *watchCallback = jsvSkipOneNameAndUnLock(jsvFindChildFromString(watch, "callback", false));
    bool watchRecur = jsvGetBoolAndUnLock(jsvObjectGetChild(watch, "recur", 0));
    int watchEdge = (int)jsvGetIntegerAndUnLock(jsvObjectGetChild(watch, "edge", 0));
    JsVar *watchPin = jsvObjectGetChild(watch, "pin", 0);
    JsVarInt watchDebounce = jsvGetIntegerAndUnLock(jsvObjectGetChild(watch, "debounce", 0));
    user_callback("setWatch(", user_data);
    jsiDumpJSON(user_callback, user_data, watchCallback, 0);
    cbprintf(user_callback, user_data, ", %j, { repeat:%s, edge:'%s'",
        watchPin,
        watchRecur?"true":"false",
            (watchEdge<0)?"falling":((watchEdge>0)?"rising":"both"));
    if (watchDebounce>0)
      cbprintf(user_callback, user_data, ", debounce : %f", jshGetMillisecondsFromTime(watchDebounce));
    user_callback(" });\n", user_data);
    jsvUnLock2(watchPin, watchCallback);
    // next
    jsvUnLock(watch);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  // and now the actual hardware
  jsiDumpHardwareInitialisation(user_callback, user_data, true);
}

JsVarInt jsiTimerAdd(JsVar *timerPtr) {
  JsVar *timerArrayPtr = jsvLock(timerArray);
  JsVarInt itemIndex = jsvArrayAddToEnd(timerArrayPtr, timerPtr, 1) - 1;
  jsvUnLock(timerArrayPtr);
  return itemIndex;
}

void jsiTimersChanged() {
  jsiStatus |= JSIS_TIMERS_CHANGED;
}

#ifdef USE_DEBUGGER
void jsiDebuggerLoop() {
  if ((jsiStatus & JSIS_IN_DEBUGGER) ||
      (execInfo.execute & EXEC_PARSE_FUNCTION_DECL)) return;
  execInfo.execute &= (JsExecFlags)~(
      EXEC_CTRL_C_MASK |
      EXEC_DEBUGGER_NEXT_LINE |
      EXEC_DEBUGGER_STEP_INTO |
      EXEC_DEBUGGER_FINISH_FUNCTION);
  jsiClearInputLine();
  jsiConsoleRemoveInputLine();
  jsiStatus = (jsiStatus & ~JSIS_ECHO_OFF_MASK) | JSIS_IN_DEBUGGER;

  if (execInfo.lex)
    jslPrintTokenLineMarker((vcbprintf_callback)jsiConsolePrint, 0, execInfo.lex, execInfo.lex->tokenLastStart);

  while (!(jsiStatus & JSIS_EXIT_DEBUGGER) &&
         !(execInfo.execute & EXEC_CTRL_C_MASK)) {
    jsiReturnInputLine();
    // idle stuff for hardware
    jshIdle();
    // Idle just for debug (much stuff removed) -------------------------------
    IOEvent event;
    // If we have too many events (> half full) drain the queue
    while (jshGetEventsUsed()>IOBUFFERMASK*1/2) {
      if (jshPopIOEvent(&event) && IOEVENTFLAGS_GETTYPE(event.flags)==consoleDevice)
        jsiHandleIOEventForConsole(&event);
    }
    // otherwise grab the remaining console events
    while (jshPopIOEventOfType(consoleDevice, &event)) {
      jsiHandleIOEventForConsole(&event);
    }
    // -----------------------------------------------------------------------
  }
  jsiConsoleRemoveInputLine();
  if (execInfo.execute & EXEC_CTRL_C_MASK)
    execInfo.execute |= EXEC_INTERRUPTED;
  jsiStatus &= ~(JSIS_IN_DEBUGGER|JSIS_EXIT_DEBUGGER);
}

void jsiDebuggerPrintScope(JsVar *scope) {
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, scope);
  bool found = false;
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *k = jsvObjectIteratorGetKey(&it);
    JsVar *ks = jsvAsString(k, false);
    JsVar *v = jsvObjectIteratorGetValue(&it);
    size_t l = jsvGetStringLength(ks);

    if (!jsvIsStringEqual(ks, JSPARSE_RETURN_VAR)) {
      found = true;
      jsiConsolePrintChar(' ');
      if (jsvIsFunctionParameter(k)) {
        jsiConsolePrint("param ");
        l+=6;
      }
      jsiConsolePrintStringVar(ks);
      while (l<20) {
        jsiConsolePrintChar(' ');
        l++;
      }
      jsiConsolePrint(" : ");
      jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
      jsiConsolePrint("\n");
    }

    jsvUnLock3(k, ks, v);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  if (!found) {
    jsiConsolePrint(" [No variables]\n");
  }
}

/// Interpret a line of input in the debugger
void jsiDebuggerLine(JsVar *line) {
  assert(jsvIsString(line));
  JsLex lex;
  jslInit(&lex, line);
  bool handled = false;
  if (lex.tk == LEX_ID) {
    handled = true;
    char *id = jslGetTokenValueAsString(&lex);

    if (!strcmp(id,"help") || !strcmp(id,"h")) {
      jsiConsolePrint("Commands:\n"
                      "help / h           - this information\n"
                      "quit / q / Ctrl-C  - Quit debug mode, break execution\n"
                      "reset              - Soft-reset Espruino\n"
                      "continue / c       - Continue execution\n"
                      "next / n           - execute to next line\n"
                      "step / s           - execute to next line, or step into function call\n"
                      "finish / f         - finish execution of the function call\n"
                      "print ... / p ...  - evaluate and print the next argument\n"
                      "info ... / i ...   - print information. Type 'info' for help \n");
    } else if (!strcmp(id,"quit") || !strcmp(id,"q")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_INTERRUPTED;
    } else if (!strcmp(id,"reset")) {
      jsiStatus = (JsiStatus)(jsiStatus & ~JSIS_TODO_MASK) | JSIS_EXIT_DEBUGGER | JSIS_TODO_RESET;
      execInfo.execute |= EXEC_INTERRUPTED;
    } else if (!strcmp(id,"continue") || !strcmp(id,"c")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
    } else if (!strcmp(id,"next") || !strcmp(id,"n")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_NEXT_LINE;
    } else if (!strcmp(id,"step") || !strcmp(id,"s")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_NEXT_LINE|EXEC_DEBUGGER_STEP_INTO;
    } else if (!strcmp(id,"finish") || !strcmp(id,"f")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_FINISH_FUNCTION;
    } else if (!strcmp(id,"print") || !strcmp(id,"p")) {
      jslGetNextToken(&lex);
      JsExecInfo oldExecInfo = execInfo;
      execInfo.lex = &lex; // execute with the remainder of the line
      execInfo.execute = EXEC_YES;
      JsVar *v = jsvSkipNameAndUnLock(jspParse());
      execInfo = oldExecInfo;
      jsiConsolePrintChar('=');
      jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
      jsiConsolePrint("\n");
      jsvUnLock(v);
    } else if (!strcmp(id,"info") || !strcmp(id,"i")) {
       jslGetNextToken(&lex);
       id = jslGetTokenValueAsString(&lex);
       if (!strcmp(id,"locals") || !strcmp(id,"l")) {
         if (execInfo.scopeCount==0)
           jsiConsolePrint("No locals found\n");
         else {
           jsiConsolePrintf("Locals:\n--------------------------------\n");
           jsiDebuggerPrintScope(execInfo.scopes[execInfo.scopeCount-1]);
           jsiConsolePrint("\n\n");
         }
       } else if (!strcmp(id,"scopechain") || !strcmp(id,"s")) {
         if (execInfo.scopeCount==0) jsiConsolePrint("No scopes found\n");
         int i;
         for (i=0;i<execInfo.scopeCount;i++) {
           jsiConsolePrintf("Scope %d:\n--------------------------------\n", i);
           jsiDebuggerPrintScope(execInfo.scopes[i]);
           jsiConsolePrint("\n\n");
         }
       } else {
         jsiConsolePrint("Unknown command:\n"
                         "info locals     (l) - output local variables\n"
                         "info scopechain (s) - output all variables in all scopes\n");
       }
    } else
      handled = false;
  }
  if (!handled) {
    jsiConsolePrint("In debug mode: Expected a simple ID, type 'help' for more info.\n");
  }

  jslKill(&lex);
}
#endif // USE_DEBUGGER
=======
/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2013 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Interactive Shell implementation
 * ----------------------------------------------------------------------------
 */
#include "jsutils.h"
#include "jsinteractive.h"
#include "jshardware.h"
#include "jstimer.h"
#include "jswrapper.h"
#include "jswrap_json.h"
#include "jswrap_io.h"
#include "jswrap_stream.h"
#include "jswrap_flash.h" // load and save to flash

#ifdef ARM
#define CHAR_DELETE_SEND 0x08
#else
#define CHAR_DELETE_SEND '\b'
#endif

#define CTRL_C_TIME_FOR_BREAK jshGetTimeFromMilliseconds(100)

// ----------------------------------------------------------------------------
typedef enum {
  IS_NONE,
  IS_HAD_R,
  IS_HAD_27,
  IS_HAD_27_79,
  IS_HAD_27_91,
  IS_HAD_27_91_49,
  IS_HAD_27_91_50,
  IS_HAD_27_91_51,
  IS_HAD_27_91_52,
  IS_HAD_27_91_53,
  IS_HAD_27_91_54,
} PACKED_FLAGS InputState;

JsVar *events = 0; // Array of events to execute
JsVarRef timerArray = 0; // Linked List of timers to check and run
JsVarRef watchArray = 0; // Linked List of input watches to check and run
// ----------------------------------------------------------------------------
IOEventFlags consoleDevice = DEFAULT_CONSOLE_DEVICE; ///< The console device for user interaction
Pin pinBusyIndicator = DEFAULT_BUSY_PIN_INDICATOR;
Pin pinSleepIndicator = DEFAULT_SLEEP_PIN_INDICATOR;
JsiStatus jsiStatus;
JsSysTime jsiLastIdleTime;  ///< The last time we went around the idle loop - use this for timers
uint32_t jsiTimeSinceCtrlC;
// ----------------------------------------------------------------------------
JsVar *inputLine = 0; ///< The current input line
JsvStringIterator inputLineIterator; ///< Iterator that points to the end of the input line
int inputLineLength = -1;
bool inputLineRemoved = false;
size_t inputCursorPos = 0; ///< The position of the cursor in the input line
InputState inputState = 0; ///< state for dealing with cursor keys
bool hasUsedHistory = false; ///< Used to speed up - if we were cycling through history and then edit, we need to copy the string
unsigned char loopsIdling; ///< How many times around the loop have we been entirely idle?
bool interruptedDuringEvent; ///< Were we interrupted while executing an event? If so may want to clear timers
// ----------------------------------------------------------------------------

void jsiDebuggerLine(JsVar *line);

// ----------------------------------------------------------------------------

/**
 * Get the device from the class variable.
 */
IOEventFlags jsiGetDeviceFromClass(JsVar *class) {
  // Devices have their Object data set up to something special
  // See jspNewObject
  if (class->varData.str[0]=='D' &&
      class->varData.str[1]=='E' &&
      class->varData.str[2]=='V')
    return (IOEventFlags)class->varData.str[3];

  return EV_NONE;
}


JsVar *jsiGetClassNameFromDevice(IOEventFlags device) {
  const char *deviceName = jshGetDeviceString(device);
  return jsvFindChildFromString(execInfo.root, deviceName, false);
}

NO_INLINE bool jsiEcho() {
  return ((jsiStatus&JSIS_ECHO_OFF_MASK)==0);
}

static bool jsiShowInputLine() {
  return jsiEcho() && !inputLineRemoved;
}

/** Called when the input line/cursor is modified *and its iterator should be reset
 * Because JsvStringIterator doesn't lock the string, it's REALLY IMPORTANT
 * that we call this BEFORE we do jsvUnLock(inputLine) */
static NO_INLINE void jsiInputLineCursorMoved() {
  // free string iterator
  if (inputLineIterator.var) {
    jsvStringIteratorFree(&inputLineIterator);
    inputLineIterator.var = 0;
  }
  inputLineLength = -1;
}

/// Called to append to the input line
static NO_INLINE void jsiAppendToInputLine(const char *str) {
  // recreate string iterator if needed
  if (!inputLineIterator.var) {
    jsvStringIteratorNew(&inputLineIterator, inputLine, 0);
    jsvStringIteratorGotoEnd(&inputLineIterator);
  }
  while (*str) {
    jsvStringIteratorAppend(&inputLineIterator, *(str++));
    inputLineLength++;
  }
}

/**
 * Change the console to a new location.
 */
void jsiSetConsoleDevice(
    IOEventFlags device //!< The device to use as a console.
  ) {
  // The `consoleDevice` is the global used to indicate which device we are using as the
  // the console.
  if (device == consoleDevice) return;

  if (!jshIsDeviceInitialised(device)) {
    JshUSARTInfo inf;
    jshUSARTInitInfo(&inf);
    jshUSARTSetup(device, &inf);
  }

  // Log to the old console that we are moving consoles and then, once we have moved
  // the console, log to the new console that we have moved consoles.
  jsiConsoleRemoveInputLine();
  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    jsiConsolePrint("Console Moved to ");
    jsiConsolePrint(jshGetDeviceString(device));
    jsiConsolePrint("\n");
  }
  IOEventFlags oldDevice = consoleDevice;
  consoleDevice = device;
  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    jsiConsolePrint("Console Moved from ");
    jsiConsolePrint(jshGetDeviceString(oldDevice));
    jsiConsolePrint("\n");
  }
}

/**
 * Retrieve the device being used as the console.
 */
IOEventFlags jsiGetConsoleDevice() {
  // The `consoleDevice` is the global used to hold the current console.  This function
  // encapsulates access.
  return consoleDevice;
}

/**
 * Send a character to the console.
 */
NO_INLINE void jsiConsolePrintChar(char data) {
  jshTransmit(consoleDevice, (unsigned char)data);
}

/**
 * \breif Send a NULL terminated string to the console.
 */
NO_INLINE void jsiConsolePrint(const char *str) {
  while (*str) {
    if (*str == '\n') jsiConsolePrintChar('\r');
    jsiConsolePrintChar(*(str++));
  }
}

/**
 * Perform a printf to the console.
 * Execute a printf command to the current JS console.
 */
void jsiConsolePrintf(const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrint,0, fmt, argp);
  va_end(argp);
}

/// Print the contents of a string var from a character position until end of line (adding an extra ' ' to delete a character if there was one)
void jsiConsolePrintStringVarUntilEOL(JsVar *v, size_t fromCharacter, size_t maxChars, bool andBackup) {
  size_t chars = 0;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, fromCharacter);
  while (jsvStringIteratorHasChar(&it) && chars<maxChars) {
    char ch = jsvStringIteratorGetChar(&it);
    if (ch == '\n') break;
    jsiConsolePrintChar(ch);
    chars++;
    jsvStringIteratorNext(&it);
  }
  jsvStringIteratorFree(&it);
  if (andBackup) {
    jsiConsolePrintChar(' ');chars++;
    while (chars--) jsiConsolePrintChar(0x08); //delete
  }
}

/** Print the contents of a string var - directly - starting from the given character, and
 * using newLineCh to prefix new lines (if it is not 0). */
void jsiConsolePrintStringVarWithNewLineChar(JsVar *v, size_t fromCharacter, char newLineCh) {
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, fromCharacter);
  while (jsvStringIteratorHasChar(&it)) {
    char ch = jsvStringIteratorGetChar(&it);
    if (ch == '\n') jsiConsolePrintChar('\r');
    jsiConsolePrintChar(ch);
    if (ch == '\n' && newLineCh) jsiConsolePrintChar(newLineCh);
    jsvStringIteratorNext(&it);
  }
  jsvStringIteratorFree(&it);
}

/**
 * Print the contents of a string var - directly.
 */
void jsiConsolePrintStringVar(JsVar *v) {
  jsiConsolePrintStringVarWithNewLineChar(v,0,0);
}

/** Assuming that we are at the end of the string, this backs up
 * and deletes it */
void jsiConsoleEraseStringVarBackwards(JsVar *v) {
  assert(jsvHasCharacterData(v));

  size_t line, lines = jsvGetLinesInString(v);
  for (line=lines;line>0;line--) {
    size_t i,chars = jsvGetCharsOnLine(v, line);
    if (line==lines) {
      for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    }
    for (i=0;i<chars;i++) jsiConsolePrintChar(' '); // move cursor forwards and wipe out
    for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    if (line>1) { 
      // clear the character before - this would have had a colon
      jsiConsolePrint("\x08 ");
      // move cursor up      
      jsiConsolePrint("\x1B[A"); // 27,91,65 - up
    }
  }
}

/** Assuming that we are at fromCharacter position in the string var,
 * erase everything that comes AFTER and return the cursor to 'fromCharacter'
 * On newlines, if erasePrevCharacter, we remove the character before too. */
void jsiConsoleEraseStringVarFrom(JsVar *v, size_t fromCharacter, bool erasePrevCharacter) {
  assert(jsvHasCharacterData(v));
  size_t cursorLine, cursorCol;
  jsvGetLineAndCol(v, fromCharacter, &cursorLine, &cursorCol);
  // delete contents of current line
  size_t i, chars = jsvGetCharsOnLine(v, cursorLine);
  for (i=cursorCol;i<=chars;i++) jsiConsolePrintChar(' ');
  for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back

  size_t line, lines = jsvGetLinesInString(v);
  for (line=cursorLine+1;line<=lines;line++) {
    jsiConsolePrint("\x1B[B"); // move down
    chars = jsvGetCharsOnLine(v, line);
    for (i=0;i<chars;i++) jsiConsolePrintChar(' '); // move cursor forwards and wipe out
    for (i=0;i<chars;i++) jsiConsolePrintChar(0x08); // move cursor back
    if (erasePrevCharacter) {
      jsiConsolePrint("\x08 "); // move cursor back and insert space
    }
  }
  // move the cursor back up
  for (line=cursorLine+1;line<=lines;line++)
    jsiConsolePrint("\x1B[A"); // 27,91,65 - up
  // move the cursor forwards
  for (i=1;i<cursorCol;i++)
    jsiConsolePrint("\x1B[C"); // 27,91,67 - right
}

void jsiMoveCursor(size_t oldX, size_t oldY, size_t newX, size_t newY) {
  // see http://www.termsys.demon.co.uk/vtansi.htm - we could do this better
  // move cursor
  while (oldX < newX) {
    jsiConsolePrint("\x1B[C"); // 27,91,67 - right
    oldX++;
  }
  while (oldX > newX) {
    jsiConsolePrint("\x1B[D"); // 27,91,68 - left
    oldX--;
  }
  while (oldY < newY) {
    jsiConsolePrint("\x1B[B"); // 27,91,66 - down
    oldY++;
  }
  while (oldY > newY) {
    jsiConsolePrint("\x1B[A"); // 27,91,65 - up
    oldY--;
  }
}

void jsiMoveCursorChar(JsVar *v, size_t fromCharacter, size_t toCharacter) {
  if (fromCharacter==toCharacter) return;
  size_t oldX, oldY;
  jsvGetLineAndCol(v, fromCharacter, &oldY, &oldX);
  size_t newX, newY;
  jsvGetLineAndCol(v, toCharacter, &newY, &newX);
  jsiMoveCursor(oldX, oldY, newX, newY);
}

/// If the input line was shown in the console, remove it
void jsiConsoleRemoveInputLine() {
  if (!inputLineRemoved) {
    inputLineRemoved = true;
    if (jsiEcho() && inputLine) { // intentionally not using jsiShowInputLine()
      jsiMoveCursorChar(inputLine, inputCursorPos, 0);
      jsiConsoleEraseStringVarFrom(inputLine, 0, true);
      jsiConsolePrintChar(0x08); // go back to start of line
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER) {
        jsiConsolePrintChar(0x08); // d
        jsiConsolePrintChar(0x08); // e
        jsiConsolePrintChar(0x08); // b
        jsiConsolePrintChar(0x08); // u
        jsiConsolePrintChar(0x08); // g
      }
#endif
    }
  }
}

/// If the input line has been removed, return it
void jsiReturnInputLine() {
  if (inputLineRemoved) {
    inputLineRemoved = false;
    if (jsiEcho()) { // intentionally not using jsiShowInputLine()
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER)
        jsiConsolePrint("debug");
#endif
      jsiConsolePrintChar('>'); // show the prompt
      jsiConsolePrintStringVarWithNewLineChar(inputLine, 0, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos);
    }
  }
}
void jsiConsolePrintPosition(struct JsLex *lex, size_t tokenPos) {
  jslPrintPosition((vcbprintf_callback)jsiConsolePrint, 0, lex, tokenPos);
}

/**
 * Clear the input line of data.
 */
void jsiClearInputLine() {
  jsiConsoleRemoveInputLine();
  // clear input line
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine = jsvNewFromEmptyString();
}

/**
 * ??? What does this do ???.
 */
void jsiSetBusy(
    JsiBusyDevice device, //!< ???
    bool isBusy           //!< ???
  ) {
  static JsiBusyDevice business = 0;

  if (isBusy)
    business |= device;
  else
    business &= (JsiBusyDevice)~device;

  if (pinBusyIndicator != PIN_UNDEFINED)
    jshPinOutput(pinBusyIndicator, business!=0);
}

/**
 * Set the status of a pin as a function of whether we are asleep.
 * When called, if a pin is set for a sleep indicator, we set the pin to be true
 * if the sleep type is awake and false otherwise.
 */
void jsiSetSleep(JsiSleepType isSleep) {
  if (pinSleepIndicator != PIN_UNDEFINED)
    jshPinOutput(pinSleepIndicator, isSleep == JSI_SLEEP_AWAKE);
}

static JsVarRef _jsiInitNamedArray(const char *name) {
  JsVar *array = jsvObjectGetChild(execInfo.hiddenRoot, name, JSV_ARRAY);
  JsVarRef arrayRef = 0;
  if (array) arrayRef = jsvGetRef(jsvRef(array));
  jsvUnLock(array);
  return arrayRef;
}

// Used when recovering after being flashed
// 'claim' anything we are using
void jsiSoftInit() {
  jswInit();

  jsErrorFlags = 0;
  events = jsvNewWithFlags(JSV_ARRAY);
  inputLine = jsvNewFromEmptyString();
  inputCursorPos = 0;
  jsiInputLineCursorMoved();
  inputLineIterator.var = 0;

  jsiStatus &= ~JSIS_ALLOW_DEEP_SLEEP;

  // Load timer/watch arrays
  timerArray = _jsiInitNamedArray(JSI_TIMERS_NAME);
  watchArray = _jsiInitNamedArray(JSI_WATCHES_NAME);

  // Now run initialisation code
  JsVar *initCode = jsvObjectGetChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME, 0);
  if (initCode) {
    jsvUnLock2(jspEvaluateVar(initCode, 0, false), initCode);
    jsvRemoveNamedChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME);
  }

  // Check any existing watches and set up interrupts for them
  if (watchArray) {
    JsVar *watchArrayPtr = jsvLock(watchArray);
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, watchArrayPtr);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *watch = jsvObjectIteratorGetValue(&it);
      JsVar *watchPin = jsvObjectGetChild(watch, "pin", 0);
      jshPinWatch(jshGetPinFromVar(watchPin), true);
      jsvUnLock2(watchPin, watch);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
    jsvUnLock(watchArrayPtr);
  }

  // Timers are stored by time in the future now, so no need
  // to fiddle with them.

  // Make sure we set up lastIdleTime, as this could be used
  // when adding an interval from onInit (called below)
  jsiLastIdleTime = jshGetSystemTime();
  jsiTimeSinceCtrlC = 0xFFFFFFFF;

  // And look for onInit function
  JsVar *onInit = jsvObjectGetChild(execInfo.root, JSI_ONINIT_NAME, 0);
  if (onInit) {
    if (jsiEcho()) jsiConsolePrint("Running onInit()...\n");
    jsiExecuteEventCallback(0, onInit, 0, 0);
    jsvUnLock(onInit);
  }
  // Now look for `init` events on `E`
  JsVar *E = jsvObjectGetChild(execInfo.root, "E", 0);
  if (E) {
    JsVar *callback = jsvObjectGetChild(E, INIT_CALLBACK_NAME, 0);
    if (callback) {
      jsiExecuteEventCallback(0, callback, 0, 0);
      jsvUnLock(callback);
    }
    jsvUnLock(E);
  }
}

/** Output the given variable as JSON, or if it exists
 * in the root scope (and it's not 'existing') then just
 * the name is dumped.  */
void jsiDumpJSON(vcbprintf_callback user_callback, void *user_data, JsVar *data, JsVar *existing) {
  // Check if it exists in the root scope
  JsVar *name = jsvGetArrayIndexOf(execInfo.root,  data, true);
  if (name && jsvIsString(name) && name!=existing) {
    // if it does, print the name
    cbprintf(user_callback, user_data, "%v", name);
  } else {
    // if it doesn't, print JSON
    jsfGetJSONWithCallback(data, JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES, user_callback, user_data);
  }
}

NO_INLINE static void jsiDumpEvent(vcbprintf_callback user_callback, void *user_data, JsVar *parentName, JsVar *eventKeyName, JsVar *eventFn) {
  JsVar *eventName = jsvNewFromStringVar(eventKeyName, strlen(JS_EVENT_PREFIX), JSVAPPENDSTRINGVAR_MAXLENGTH);
  cbprintf(user_callback, user_data, "%v.on(%q, ", parentName, eventName);
  jsvUnLock(eventName);
  jsiDumpJSON(user_callback, user_data, eventFn, 0);
  user_callback(");\n", user_data);
}

/** Output extra functions defined in an object such that they can be copied to a new device */
NO_INLINE void jsiDumpObjectState(vcbprintf_callback user_callback, void *user_data, JsVar *parentName, JsVar *parent) {
  JsvIsInternalChecker checker = jsvGetInternalFunctionCheckerFor(parent);

  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, parent);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *child = jsvObjectIteratorGetKey(&it);
    JsVar *data = jsvObjectIteratorGetValue(&it);

    if (!checker || !checker(child)) {
      if (jsvIsStringEqual(child, JSPARSE_PROTOTYPE_VAR)) {
        // recurse to print prototypes
        JsVar *name = jsvNewFromStringVar(parentName,0,JSVAPPENDSTRINGVAR_MAXLENGTH);
        if (name) {
          jsvAppendString(name, ".prototype");
          jsiDumpObjectState(user_callback, user_data, name, data);
          jsvUnLock(name);
        }
      } else if (jsvIsStringEqualOrStartsWith(child, JS_EVENT_PREFIX, true)) {
        // Handle the case that this is an event
        if (jsvIsArray(data)) {
          JsvObjectIterator ait;
          jsvObjectIteratorNew(&ait, data);
          while (jsvObjectIteratorHasValue(&ait)) {
            JsVar *v = jsvObjectIteratorGetValue(&ait);
            jsiDumpEvent(user_callback, user_data, parentName, child, v);
            jsvUnLock(v);
            jsvObjectIteratorNext(&ait);
          }
          jsvObjectIteratorFree(&ait);
        } else {
          jsiDumpEvent(user_callback, user_data, parentName, child, data);
        }
      } else {
        // It's a normal function
        if (!jsvIsNative(data)) {
          cbprintf(user_callback, user_data, "%v.%v = ", parentName, child);
          jsiDumpJSON(user_callback, user_data, data, 0);
          user_callback(";\n", user_data);
        }
      }
    }
    jsvUnLock2(data, child);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
}

/** Dump the code required to initialise a serial port to this string */
void jsiDumpSerialInitialisation(vcbprintf_callback user_callback, void *user_data, const char *serialName, bool addObjectProperties) {
  JsVar *serialVarName = jsvFindChildFromString(execInfo.root, serialName, false);
  JsVar *serialVar = jsvSkipName(serialVarName);

  if (serialVar) {
    if (addObjectProperties)
      jsiDumpObjectState(user_callback, user_data, serialVarName, serialVar);

    JsVar *baud = jsvObjectGetChild(serialVar, USART_BAUDRATE_NAME, 0);
    JsVar *options = jsvObjectGetChild(serialVar, DEVICE_OPTIONS_NAME, 0);
    if (baud || options) {
      int baudrate = (int)jsvGetInteger(baud);
      if (baudrate <= 0) baudrate = DEFAULT_BAUD_RATE;
      cbprintf(user_callback, user_data, "%s.setup(%d", serialName, baudrate);
      if (jsvIsObject(options)) {
        user_callback(", ", user_data);
        jsfGetJSONWithCallback(options, JSON_SHOW_DEVICES, user_callback, user_data);
      }
      user_callback(");\n", user_data);
    }
    jsvUnLock3(baud, options, serialVar);
  }
  jsvUnLock(serialVarName);
}

/** Dump the code required to initialise a SPI port to this string */
void jsiDumpDeviceInitialisation(vcbprintf_callback user_callback, void *user_data, const char *deviceName) {
  JsVar *deviceVar = jsvObjectGetChild(execInfo.root, deviceName, 0);
  if (deviceVar) {
    JsVar *options = jsvObjectGetChild(deviceVar, DEVICE_OPTIONS_NAME, 0);
    if (options) {
      cbprintf(user_callback, user_data, "%s.setup(", deviceName);
      if (jsvIsObject(options))
        jsfGetJSONWithCallback(options, JSON_SHOW_DEVICES, user_callback, user_data);
      user_callback(");\n", user_data);
    }
    jsvUnLock2(options, deviceVar);
  }
}

/** Dump all the code required to initialise hardware to this string */
void jsiDumpHardwareInitialisation(vcbprintf_callback user_callback, void *user_data, bool addObjectProperties) {
  if (jsiStatus&JSIS_ECHO_OFF) user_callback("echo(0);", user_data);
  if (pinBusyIndicator != DEFAULT_BUSY_PIN_INDICATOR) {
    cbprintf(user_callback, user_data, "setBusyIndicator(%p);\n", pinBusyIndicator);
  }
  if (pinSleepIndicator != DEFAULT_BUSY_PIN_INDICATOR) {
    cbprintf(user_callback, user_data, "setSleepIndicator(%p);\n", pinSleepIndicator);
  }
  if (jsiStatus&JSIS_ALLOW_DEEP_SLEEP) {
    user_callback("setDeepSleep(1);\n", user_data);
  }

  jsiDumpSerialInitialisation(user_callback, user_data, "USB", addObjectProperties);
  int i;
  for (i=0;i<USART_COUNT;i++)
    jsiDumpSerialInitialisation(user_callback, user_data, jshGetDeviceString(EV_SERIAL1+i), addObjectProperties);
  for (i=0;i<SPI_COUNT;i++)
    jsiDumpDeviceInitialisation(user_callback, user_data, jshGetDeviceString(EV_SPI1+i));
  for (i=0;i<I2C_COUNT;i++)
    jsiDumpDeviceInitialisation(user_callback, user_data, jshGetDeviceString(EV_I2C1+i));
  // pins
  Pin pin;
  for (pin=0;jshIsPinValid(pin) && pin<255;pin++) {
    if (IS_PIN_USED_INTERNALLY(pin)) continue;
    JshPinState state = jshPinGetState(pin);
    JshPinState statem = state&JSHPINSTATE_MASK;
    if (statem == JSHPINSTATE_GPIO_OUT || statem == JSHPINSTATE_GPIO_OUT_OPENDRAIN) {
      bool isOn = (state&JSHPINSTATE_PIN_IS_ON)!=0;
      if (!isOn && IS_PIN_A_LED(pin)) continue;
      cbprintf(user_callback, user_data, "digitalWrite(%p,%d);\n",pin,isOn?1:0);
    } else if (/*statem == JSHPINSTATE_GPIO_IN ||*/statem == JSHPINSTATE_GPIO_IN_PULLUP || statem == JSHPINSTATE_GPIO_IN_PULLDOWN) {
#ifdef DEFAULT_CONSOLE_RX_PIN
      // the console input pin is always a pullup now - which is expected
      if (pin == DEFAULT_CONSOLE_RX_PIN &&
          statem == JSHPINSTATE_GPIO_IN_PULLUP) continue;
#endif
      // don't bother with normal inputs, as they come up in this state (ish) anyway
      const char *s = "";
      if (statem == JSHPINSTATE_GPIO_IN_PULLUP) s="_pullup";
      if (statem == JSHPINSTATE_GPIO_IN_PULLDOWN) s="_pulldown";
      cbprintf(user_callback, user_data, "pinMode(%p,\"input%s\");\n",pin,s);
    }

    if (statem == JSHPINSTATE_GPIO_OUT_OPENDRAIN)
      cbprintf(user_callback, user_data, "pinMode(%p,\"opendrain\");\n",pin);
  }
}

// Used when shutting down before flashing
// 'release' anything we are using, but ensure that it doesn't get freed
void jsiSoftKill() {
  inputCursorPos = 0;
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine=0;

  // kill any wrapped stuff
  jswKill();
  // Stop all active timer tasks
  jstReset();
  // Unref Watches/etc
  if (events) {
    jsvUnLock(events);
    events=0;
  }
  if (timerArray) {
    jsvUnRefRef(timerArray);
    timerArray=0;
  }
  if (watchArray) {
    // Check any existing watches and disable interrupts for them
    JsVar *watchArrayPtr = jsvLock(watchArray);
    JsvObjectIterator it;
    jsvObjectIteratorNew(&it, watchArrayPtr);
    while (jsvObjectIteratorHasValue(&it)) {
      JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
      JsVar *watchPin = jsvObjectGetChild(watchPtr, "pin", 0);
      jshPinWatch(jshGetPinFromVar(watchPin), false);
      jsvUnLock2(watchPin, watchPtr);
      jsvObjectIteratorNext(&it);
    }
    jsvObjectIteratorFree(&it);
    jsvUnRef(watchArrayPtr);
    jsvUnLock(watchArrayPtr);
    watchArray=0;
  }
  // Save initialisation information
  JsVar *initCode = jsvNewFromEmptyString();
  if (initCode) { // out of memory
    JsvStringIterator it;
    jsvStringIteratorNew(&it, initCode, 0);
    jsiDumpHardwareInitialisation((vcbprintf_callback)&jsvStringIteratorPrintfCallback, &it, false);
    jsvStringIteratorFree(&it);
    jsvObjectSetChild(execInfo.hiddenRoot, JSI_INIT_CODE_NAME, initCode);
    jsvUnLock(initCode);
  }
}

void jsiSemiInit(bool autoLoad) {
  jspInit();

  // Set state
  interruptedDuringEvent = false;
  // Set defaults
  jsiStatus = JSIS_NONE;
  pinBusyIndicator = DEFAULT_BUSY_PIN_INDICATOR;

  /* If flash contains any code, then we should
     Try and load from it... */
  bool loadFlash = autoLoad && jsfFlashContainsCode();
  if (loadFlash) {
    jspSoftKill();
    jsvSoftKill();
    jsfLoadFromFlash();
    jsvSoftInit();
    jspSoftInit();
  }

  // Softinit may run initialisation code that will overwrite defaults
  jsiSoftInit();

  if (jsiEcho()) { // intentionally not using jsiShowInputLine()
    if (!loadFlash) {
      jsiConsolePrint(
#ifndef LINUX
          // set up terminal to avoid word wrap
          "\e[?7l"
#endif
          // rectangles @ http://www.network-science.de/ascii/
          "\n"
          " _____                 _ \n"
          "|   __|___ ___ ___ _ _|_|___ ___ \n"
          "|   __|_ -| . |  _| | | |   | . |\n"
          "|_____|___|  _|_| |___|_|_|_|___|\n"
          "          |_| http://espruino.com\n"
          " "JS_VERSION" Copyright 2015 G.Williams\n");
    }
    jsiConsolePrint("\n"); // output new line
    inputLineRemoved = true; // we need to put the input line back...
  }
}

// The 'proper' init function - this should be called only once at bootup
void jsiInit(bool autoLoad) {
#if defined(LINUX) || !defined(USB)
  consoleDevice = DEFAULT_CONSOLE_DEVICE;
#else
  consoleDevice = EV_LIMBO;
#endif

  jsiSemiInit(autoLoad);
}

#ifndef LINUX
// This should get jsiOneSecondAfterStartupcalled from jshardware.c one second after startup,
// it does initialisation tasks like setting the right console device
void jsiOneSecondAfterStartup() {
  /* When we start up, we put all console output into 'Limbo' (EV_LIMBO),
     because we want to get started immediately, but we don't know where
     to send any console output (USB takes a while to initialise). Not only
     that but if we start transmitting on Serial right away, the first
     char or two can get corrupted.
   */
#ifdef USB
  if (consoleDevice == EV_LIMBO) {
    consoleDevice = DEFAULT_CONSOLE_DEVICE;
    if (jshIsUSBSERIALConnected())
      consoleDevice = EV_USBSERIAL;
    // now move any output that was made to Limbo to the given device
    jshTransmitMove(EV_LIMBO, consoleDevice);
    // finally, kick output - just in case
    jshUSARTKick(consoleDevice);
  } else {
    // the console has already been moved
    jshTransmitClearDevice(EV_LIMBO);
  }
#endif
}
#endif

void jsiKill() {
  jsiSoftKill();

  jspKill();
}

int jsiCountBracketsInInput() {
  int brackets = 0;

  JsLex lex;
  jslInit(&lex, inputLine);
  while (lex.tk!=LEX_EOF && lex.tk!=LEX_UNFINISHED_COMMENT) {
    if (lex.tk=='{' || lex.tk=='[' || lex.tk=='(') brackets++;
    if (lex.tk=='}' || lex.tk==']' || lex.tk==')') brackets--;
    if (brackets<0) break; // closing bracket before opening!
    jslGetNextToken(&lex);
  }
  if (lex.tk==LEX_UNFINISHED_COMMENT)
    brackets=1000; // if there's an unfinished comment, we're in the middle of something
  jslKill(&lex);

  return brackets;
} 

/// Tries to get rid of some memory (by clearing command history). Returns true if it got rid of something, false if it didn't.
bool jsiFreeMoreMemory() {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  if (!history) return 0;
  JsVar *item = jsvArrayPopFirst(history);
  bool freed = item!=0;
  jsvUnLock2(item, history);
  // TODO: could also free the array structure?
  // TODO: could look at all streams (Serial1/HTTP/etc) and see if their buffers contain data that could be removed

  return freed;
}

// Add a new line to the command history
void jsiHistoryAddLine(JsVar *newLine) {
  if (!newLine || jsvGetStringLength(newLine)==0) return;
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, JSV_ARRAY);
  if (!history) return; // out of memory
  // if it was already in history, remove it - we'll put it back in front
  JsVar *alreadyInHistory = jsvGetArrayIndexOf(history, newLine, false/*not exact*/);
  if (alreadyInHistory) {
    jsvRemoveChild(history, alreadyInHistory);
    jsvUnLock(alreadyInHistory);
  }
  // put it back in front
  jsvArrayPush(history, newLine);
  jsvUnLock(history);
}

JsVar *jsiGetHistoryLine(bool previous /* next if false */) {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  JsVar *historyLine = 0;
  if (history) {
    JsVar *idx = jsvGetArrayIndexOf(history, inputLine, true/*exact*/); // get index of current line
    if (idx) {
      if (previous && jsvGetPrevSibling(idx)) {
        historyLine = jsvSkipNameAndUnLock(jsvLock(jsvGetPrevSibling(idx)));
      } else if (!previous && jsvGetNextSibling(idx)) {
        historyLine = jsvSkipNameAndUnLock(jsvLock(jsvGetNextSibling(idx)));
      }
      jsvUnLock(idx);
    } else {
      if (previous) historyLine = jsvSkipNameAndUnLock(jsvGetArrayItem(history, jsvGetArrayLength(history)-1));
      // if next, we weren't using history so couldn't go forwards
    }

    jsvUnLock(history);
  }
  return historyLine;
}

bool jsiIsInHistory(JsVar *line) {
  JsVar *history = jsvObjectGetChild(execInfo.hiddenRoot, JSI_HISTORY_NAME, 0);
  if (!history) return false;
  JsVar *historyFound = jsvGetArrayIndexOf(history, line, true/*exact*/);
  bool inHistory = historyFound!=0;
  jsvUnLock2(historyFound, history);
  return inHistory;
}

void jsiReplaceInputLine(JsVar *newLine) {
  if (jsiShowInputLine()) {
    size_t oldLen =  jsvGetStringLength(inputLine);
    jsiMoveCursorChar(inputLine, inputCursorPos, oldLen); // move cursor to end
    jsiConsoleEraseStringVarBackwards(inputLine);
    jsiConsolePrintStringVarWithNewLineChar(newLine,0,':');
  }
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine = jsvLockAgain(newLine);
  inputCursorPos = jsvGetStringLength(inputLine);
}

void jsiChangeToHistory(bool previous) {
#ifdef USE_DEBUGGER
  if (jsiStatus & JSIS_IN_DEBUGGER) return;
#endif
  JsVar *nextHistory = jsiGetHistoryLine(previous);
  if (nextHistory) {
    jsiReplaceInputLine(nextHistory);
    jsvUnLock(nextHistory);
    hasUsedHistory = true;
  } else if (!previous) { // if next, but we have something, just clear the line
    if (jsiShowInputLine()) {
      jsiConsoleEraseStringVarBackwards(inputLine);
    }
    jsiInputLineCursorMoved();
    jsvUnLock(inputLine);
    inputLine = jsvNewFromEmptyString();
    inputCursorPos = 0;
  }
}

void jsiIsAboutToEditInputLine() {
  // we probably plan to do something with the line now - check it wasn't in history
  // and if it was, duplicate it
  if (hasUsedHistory) {
    hasUsedHistory = false;
    if (jsiIsInHistory(inputLine)) {
      JsVar *newLine = jsvCopy(inputLine);
      if (newLine) { // could have been out of memory!
        jsiInputLineCursorMoved();
        jsvUnLock(inputLine);
        inputLine = newLine;
      }
    }
  }
}

void jsiHandleDelete(bool isBackspace) {
  size_t l = jsvGetStringLength(inputLine);
  if (isBackspace && inputCursorPos==0) return; // at beginning of line
  if (!isBackspace && inputCursorPos>=l) return; // at end of line
  // work out if we are deleting a newline
  bool deleteNewline = (isBackspace && jsvGetCharInString(inputLine,inputCursorPos-1)=='\n') ||
      (!isBackspace && jsvGetCharInString(inputLine,inputCursorPos)=='\n');
  // If we mod this to keep the string, use jsiIsAboutToEditInputLine
  if (deleteNewline && jsiShowInputLine()) {
    jsiConsoleEraseStringVarFrom(inputLine, inputCursorPos, true/*before newline*/); // erase all in front
    if (isBackspace) {
      // delete newline char
      jsiConsolePrint("\x08 "); // delete and then send space
      jsiMoveCursorChar(inputLine, inputCursorPos, inputCursorPos-1); // move cursor back
      jsiInputLineCursorMoved();
    }
  }

  JsVar *v = jsvNewFromEmptyString();
  size_t p = inputCursorPos;
  if (isBackspace) p--;
  if (p>0) jsvAppendStringVar(v, inputLine, 0, p); // add before cursor (delete)
  if (p+1<l) jsvAppendStringVar(v, inputLine, p+1, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
  jsiInputLineCursorMoved();
  jsvUnLock(inputLine);
  inputLine=v;
  if (isBackspace)
    inputCursorPos--; // move cursor back

  // update the console
  if (jsiShowInputLine()) {
    if (deleteNewline) {
      // we already removed everything, so just put it back
      jsiConsolePrintStringVarWithNewLineChar(inputLine, inputCursorPos, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos); // move cursor back
    } else {
      // clear the character and move line back
      if (isBackspace) jsiConsolePrintChar(0x08);
      jsiConsolePrintStringVarUntilEOL(inputLine, inputCursorPos, 0xFFFFFFFF, true/*and backup*/);
    }
  }
}

void jsiHandleHome() {
  while (inputCursorPos>0 && jsvGetCharInString(inputLine,inputCursorPos-1)!='\n') {
    if (jsiShowInputLine()) jsiConsolePrintChar(0x08);
    inputCursorPos--;
  }
}

void jsiHandleEnd() {
  size_t l = jsvGetStringLength(inputLine);
  while (inputCursorPos<l && jsvGetCharInString(inputLine,inputCursorPos)!='\n') {
    if (jsiShowInputLine())
      jsiConsolePrintChar(jsvGetCharInString(inputLine,inputCursorPos));
    inputCursorPos++;
  }
}

/** Page up/down move cursor to beginnint or end */
void jsiHandlePageUpDown(bool isDown) {
  size_t x,y;
  jsvGetLineAndCol(inputLine, inputCursorPos, &y, &x);
  if (!isDown) { // up
    inputCursorPos = 0;
  } else { // down
    inputCursorPos = jsvGetStringLength(inputLine);
  }
  size_t newX=x,newY=y;
  jsvGetLineAndCol(inputLine, inputCursorPos, &newY, &newX);
  jsiMoveCursor(x,y,newX,newY);
}

void jsiHandleMoveUpDown(int direction) {
  size_t x,y, lines=jsvGetLinesInString(inputLine);
  jsvGetLineAndCol(inputLine, inputCursorPos, &y, &x);
  size_t newX=x,newY=y;
  newY = (size_t)((int)newY + direction);
  if (newY<1) newY=1;
  if (newY>lines) newY=lines;
  // work out cursor pos and feed back through - we might not be able to get right to the same place
  // if we move up
  inputCursorPos = jsvGetIndexFromLineAndCol(inputLine, newY, newX);
  jsvGetLineAndCol(inputLine, inputCursorPos, &newY, &newX);
  if (jsiShowInputLine()) {
    jsiMoveCursor(x,y,newX,newY);
  }
}

bool jsiAtEndOfInputLine() {
  size_t i = inputCursorPos, l = jsvGetStringLength(inputLine);
  while (i < l) {
    if (!isWhitespace(jsvGetCharInString(inputLine, i)))
      return false;
    i++;
  }
  return true;
}

void jsiCheckErrors() {
  JsVar *exception = jspGetException();
  if (exception) {
    jsiConsolePrintf("Uncaught %v\n", exception);
    jsvUnLock(exception);
  }
  if (jspIsInterrupted()) {
    jsiConsoleRemoveInputLine();
    jsiConsolePrint("Execution Interrupted\n");
    jspSetInterrupted(false);
  }
  JsVar *stackTrace = jspGetStackTrace();
  if (stackTrace) {
    jsiConsolePrintStringVar(stackTrace);
    jsvUnLock(stackTrace);
  }
}

void jsiHandleNewLine(bool execute) {
  if (jsiAtEndOfInputLine()) { // at EOL so we need to figure out if we can execute or not
    if (execute && jsiCountBracketsInInput()<=0) { // actually execute!
      if (jsiShowInputLine()) {
        jsiConsolePrint("\n");
      }
      if (!(jsiStatus & JSIS_ECHO_OFF_FOR_LINE))
        inputLineRemoved = true;

      // Get line to execute, and reset inputLine
      JsVar *lineToExecute = jsvStringTrimRight(inputLine);
      jsiInputLineCursorMoved();
      jsvUnLock(inputLine);
      inputLine = jsvNewFromEmptyString();
      inputCursorPos = 0;
#ifdef USE_DEBUGGER
      if (jsiStatus & JSIS_IN_DEBUGGER) {
        jsiDebuggerLine(lineToExecute);
        jsvUnLock(lineToExecute);
      } else
#endif
      {
        // execute!
        JsVar *v = jspEvaluateVar(lineToExecute, 0, false);
        // add input line to history
        jsiHistoryAddLine(lineToExecute);
        jsvUnLock(lineToExecute);
        // print result (but NOT if we had an error)
        if (jsiEcho() && !jspHasError()) {
          jsiConsolePrintChar('=');
          jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
          jsiConsolePrint("\n");
        }
        jsvUnLock(v);
      }
      jsiCheckErrors();
      // console will be returned next time around the input loop
      // if we had echo off just for this line, reinstate it!
      jsiStatus &= ~JSIS_ECHO_OFF_FOR_LINE;
    } else {
      // Brackets aren't all closed, so we're going to append a newline
      // without executing
      if (jsiShowInputLine()) jsiConsolePrint("\n:");
      jsiIsAboutToEditInputLine();
      jsiAppendToInputLine("\n");
      inputCursorPos++;
    }
  } else { // new line - but not at end of line!
    jsiIsAboutToEditInputLine();
    if (jsiShowInputLine()) jsiConsoleEraseStringVarFrom(inputLine, inputCursorPos, false/*no need to erase the char before*/); // erase all in front
    JsVar *v = jsvNewFromEmptyString();
    if (inputCursorPos>0) jsvAppendStringVar(v, inputLine, 0, inputCursorPos);
    jsvAppendCharacter(v, '\n');
    jsvAppendStringVar(v, inputLine, inputCursorPos, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
    jsiInputLineCursorMoved();
    jsvUnLock(inputLine);
    inputLine=v;
    if (jsiShowInputLine()) { // now print the rest
      jsiConsolePrintStringVarWithNewLineChar(inputLine, inputCursorPos, ':');
      jsiMoveCursorChar(inputLine, jsvGetStringLength(inputLine), inputCursorPos+1); // move cursor back
    }
    inputCursorPos++;
  }
}

void jsiHandleChar(char ch) {
  // jsiConsolePrintf("[%d:%d]\n", inputState, ch);
  //
  // special stuff
  // 1 - Ctrl-a - beginning of line
  // 4 - Ctrl-d - backwards delete
  // 5 - Ctrl-e - end of line
  // 21 - Ctrl-u - delete line
  // 23 - Ctrl-w - delete word (currently just does the same as Ctrl-u)
  //
  // 27 then 91 then 68 - left
  // 27 then 91 then 67 - right
  // 27 then 91 then 65 - up
  // 27 then 91 then 66 - down
  // 27 then 91 then 50 then 75 - Erases the entire current line.
  // 27 then 91 then 51 then 126 - backwards delete
  // 27 then 91 then 52 then 126 - numpad end
  // 27 then 91 then 49 then 126 - numpad home
  // 27 then 91 then 53 then 126 - pgup
  // 27 then 91 then 54 then 126 - pgdn
  // 27 then 79 then 70 - home
  // 27 then 79 then 72 - end
  // 27 then 10 - alt enter


  if (ch == 0) {
    inputState = IS_NONE; // ignore 0 - it's scary
  } else if (ch == 1) { // Ctrl-a
    jsiHandleHome();
  } else if (ch == 4) { // Ctrl-d
    jsiHandleDelete(false/*not backspace*/);
  } else if (ch == 5) { // Ctrl-e
    jsiHandleEnd();
  } else if (ch == 21 || ch == 23) { // Ctrl-u or Ctrl-w
    jsiClearInputLine();
  } else if (ch == 27) {
    inputState = IS_HAD_27;
  } else if (inputState==IS_HAD_27) {
    inputState = IS_NONE;
    if (ch == 79)
      inputState = IS_HAD_27_79;
    else if (ch == 91)
      inputState = IS_HAD_27_91;
    else if (ch == 10)
      jsiHandleNewLine(false);
  } else if (inputState==IS_HAD_27_79) { // Numpad
    inputState = IS_NONE;
    if (ch == 70) jsiHandleEnd();
    else if (ch == 72) jsiHandleHome();
    else if (ch == 111) jsiHandleChar('/');
    else if (ch == 106) jsiHandleChar('*');
    else if (ch == 109) jsiHandleChar('-');
    else if (ch == 107) jsiHandleChar('+');
    else if (ch == 77) jsiHandleChar('\r');
  } else if (inputState==IS_HAD_27_91) {
    inputState = IS_NONE;
    if (ch==68) { // left
      if (inputCursorPos>0 && jsvGetCharInString(inputLine,inputCursorPos-1)!='\n') {
        inputCursorPos--;
        if (jsiShowInputLine()) {
          jsiConsolePrint("\x1B[D"); // 27,91,68 - left
        }
      }
    } else if (ch==67) { // right
      if (inputCursorPos<jsvGetStringLength(inputLine) && jsvGetCharInString(inputLine,inputCursorPos)!='\n') {
        inputCursorPos++;
        if (jsiShowInputLine()) {
          jsiConsolePrint("\x1B[C"); // 27,91,67 - right
        }
      }
    } else if (ch==65) { // up
      size_t l = jsvGetStringLength(inputLine);
      if ((l==0 || jsiIsInHistory(inputLine)) && inputCursorPos==l)
        jsiChangeToHistory(true); // if at end of line
      else
        jsiHandleMoveUpDown(-1);
    } else if (ch==66) { // down
      size_t l = jsvGetStringLength(inputLine);
      if ((l==0 || jsiIsInHistory(inputLine)) && inputCursorPos==l)
        jsiChangeToHistory(false); // if at end of line
      else
        jsiHandleMoveUpDown(1);
    } else if (ch==49) {
      inputState=IS_HAD_27_91_49;
    } else if (ch==50) {
      inputState=IS_HAD_27_91_50;
    } else if (ch==51) {
      inputState=IS_HAD_27_91_51;
    } else if (ch==52) {
      inputState=IS_HAD_27_91_52;
    } else if (ch==53) {
      inputState=IS_HAD_27_91_53;
    } else if (ch==54) {
      inputState=IS_HAD_27_91_54;
    }
  } else if (inputState==IS_HAD_27_91_49) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad Home
      jsiHandleHome();
    }
  } else if (inputState==IS_HAD_27_91_50) {
    inputState = IS_NONE;
    if (ch==75) { // Erase current line
      jsiClearInputLine();
    }
  } else if (inputState==IS_HAD_27_91_51) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad (forwards) Delete
      jsiHandleDelete(false/*not backspace*/);
    }
  } else if (inputState==IS_HAD_27_91_52) {
    inputState = IS_NONE;
    if (ch==126) { // Numpad End
      jsiHandleEnd();
    }
  } else if (inputState==IS_HAD_27_91_53) {
    inputState = IS_NONE;
    if (ch==126) { // Page Up
      jsiHandlePageUpDown(0);
    }
  } else if (inputState==IS_HAD_27_91_54) {
    inputState = IS_NONE;
    if (ch==126) { // Page Down
      jsiHandlePageUpDown(1);
    }
  } else if (ch==16 && jsvGetStringLength(inputLine)==0) {
    /* DLE - Data Link Escape
    Espruino uses DLE on the start of a line to signal that just the line in
    question should be executed without echo */
    jsiStatus  |= JSIS_ECHO_OFF_FOR_LINE;
  } else {  
    inputState = IS_NONE;
    if (ch == 0x08 || ch == 0x7F /*delete*/) {
      jsiHandleDelete(true /*backspace*/);
    } else if (ch == '\n' && inputState == IS_HAD_R) {
      inputState = IS_NONE; //  ignore \ r\n - we already handled it all on \r
    } else if (ch == '\r' || ch == '\n') { 
      if (ch == '\r') inputState = IS_HAD_R;
      jsiHandleNewLine(true);
    } else if (ch>=32 || ch=='\t') {
      // Add the character to our input line
      jsiIsAboutToEditInputLine();
      char buf[2] = {ch,0};
      const char *strToAppend = (ch=='\t') ? "    " : buf;
      size_t strSize = (ch=='\t') ? 4 : 1;

      if (inputLineLength < 0)
        inputLineLength = (int)jsvGetStringLength(inputLine);

      if ((int)inputCursorPos>=inputLineLength) { // append to the end
        jsiAppendToInputLine(strToAppend);
      } else { // add in halfway through
        JsVar *v = jsvNewFromEmptyString();
        if (inputCursorPos>0) jsvAppendStringVar(v, inputLine, 0, inputCursorPos);
        jsvAppendString(v, strToAppend);
        jsvAppendStringVar(v, inputLine, inputCursorPos, JSVAPPENDSTRINGVAR_MAXLENGTH); // add the rest
        jsiInputLineCursorMoved();
        jsvUnLock(inputLine);
        inputLine=v;
        if (jsiShowInputLine()) jsiConsolePrintStringVarUntilEOL(inputLine, inputCursorPos, 0xFFFFFFFF, true/*and backup*/);
      }
      inputCursorPos += strSize; // no need for jsiInputLineCursorMoved(); as we just appended
      if (jsiShowInputLine()) {
        jsiConsolePrint(strToAppend);
      }
    }
  }
}

/// Queue a function, string, or array (of funcs/strings) to be executed next time around the idle loop
void jsiQueueEvents(JsVar *object, JsVar *callback, JsVar **args, int argCount) { // an array of functions, a string, or a single function
  assert(argCount<10);

  JsVar *event = jsvNewWithFlags(JSV_OBJECT);
  if (event) { // Could be out of memory error!
    jsvUnLock(jsvAddNamedChild(event, callback, "func"));

    if (argCount) {
      JsVar *arr = jsvNewArray(args, argCount);
      if (arr) {
        jsvUnLock2(jsvAddNamedChild(event, arr, "args"), arr);
      }
    }
    if (object) jsvUnLock(jsvAddNamedChild(event, object, "this"));

    jsvArrayPushAndUnLock(events, event);
  }
}

bool jsiObjectHasCallbacks(JsVar *object, const char *callbackName) {
  JsVar *callback = jsvObjectGetChild(object, callbackName, 0);
  bool hasCallbacks = !jsvIsUndefined(callback);
  jsvUnLock(callback);
  return hasCallbacks;
}

void jsiQueueObjectCallbacks(JsVar *object, const char *callbackName, JsVar **args, int argCount) {
  JsVar *callback = jsvObjectGetChild(object, callbackName, 0);
  if (!callback) return;
  jsiQueueEvents(object, callback, args, argCount);
  jsvUnLock(callback);
}

void jsiExecuteEvents() {
  bool hasEvents = !jsvArrayIsEmpty(events);
  if (hasEvents) jsiSetBusy(BUSY_INTERACTIVE, true);
  while (!jsvArrayIsEmpty(events)) {
    JsVar *event = jsvSkipNameAndUnLock(jsvArrayPopFirst(events));
    // Get function to execute
    JsVar *func = jsvObjectGetChild(event, "func", 0);
    JsVar *thisVar = jsvObjectGetChild(event, "this", 0);
    JsVar *argsArray = jsvObjectGetChild(event, "args", 0);
    // free actual event
    jsvUnLock(event);
    // now run..
    jsiExecuteEventCallbackArgsArray(thisVar, func, argsArray);
    jsvUnLock(argsArray);
    //jsPrint("Event Done\n");
    jsvUnLock2(func, thisVar);
  }
  if (hasEvents) {
    jsiSetBusy(BUSY_INTERACTIVE, false);
    if (jspIsInterrupted() || jsiTimeSinceCtrlC<CTRL_C_TIME_FOR_BREAK)
      interruptedDuringEvent = true;
  }
}

NO_INLINE bool jsiExecuteEventCallbackArgsArray(JsVar *thisVar, JsVar *callbackVar, JsVar *argsArray) { // array of functions or single function
  unsigned int l = (unsigned int)jsvGetArrayLength(argsArray);
  JsVar **args = 0;
  if (l) {
    args = alloca(sizeof(JsVar*) * l);
    if (!args) return false;
    jsvGetArrayItems(argsArray, l, args); // not very fast
  }
  bool r = jsiExecuteEventCallback(thisVar, callbackVar, l, args);
  jsvUnLockMany(l, args);
  return r;
}

NO_INLINE bool jsiExecuteEventCallback(JsVar *thisVar, JsVar *callbackVar, unsigned int argCount, JsVar **argPtr) { // array of functions or single function
  JsVar *callbackNoNames = jsvSkipName(callbackVar);

  bool ok = true;
  if (callbackNoNames) {
    if (jsvIsArray(callbackNoNames)) {
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, callbackNoNames);
      while (ok && jsvObjectIteratorHasValue(&it)) {
        JsVar *child = jsvObjectIteratorGetValue(&it);
        ok &= jsiExecuteEventCallback(thisVar, child, argCount, argPtr);
        jsvUnLock(child);
        jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
    } else if (jsvIsFunction(callbackNoNames)) {
      jsvUnLock(jspExecuteFunction(callbackNoNames, thisVar, (int)argCount, argPtr));
    } else if (jsvIsString(callbackNoNames)) {
      jsvUnLock(jspEvaluateVar(callbackNoNames, 0, false));
    } else
      jsError("Unknown type of callback in Event Queue");
    jsvUnLock(callbackNoNames);
  }
  if (!ok || jspIsInterrupted() || jsiTimeSinceCtrlC<CTRL_C_TIME_FOR_BREAK) {
    interruptedDuringEvent = true;
    return false;
  }
  return true;
}

bool jsiHasTimers() {
  if (!timerArray) return false;
  JsVar *timerArrayPtr = jsvLock(timerArray);
  bool hasTimers = !jsvArrayIsEmpty(timerArrayPtr);
  jsvUnLock(timerArrayPtr);
  return hasTimers;
}

/// Is the given watch object meant to be executed when the current value of the pin is pinIsHigh
bool jsiShouldExecuteWatch(JsVar *watchPtr, bool pinIsHigh) {
  int watchEdge = (int)jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "edge", 0));
  return watchEdge==0 || // any edge
      (pinIsHigh && watchEdge>0) || // rising edge
      (!pinIsHigh && watchEdge<0); // falling edge
}

bool jsiIsWatchingPin(Pin pin) {
  bool isWatched = false;
  JsVar *watchArrayPtr = jsvLock(watchArray);
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, watchArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
    JsVar *pinVar = jsvObjectGetChild(watchPtr, "pin", 0);
    if (jshGetPinFromVar(pinVar) == pin)
      isWatched = true;
    jsvUnLock2(pinVar, watchPtr);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  jsvUnLock(watchArrayPtr);
  return isWatched;
}

void jsiHandleIOEventForUSART(JsVar *usartClass, IOEvent *event) {
  /* work out byteSize. On STM32 we fake 7 bit, and it's easier to
   * check the options and work out the masking here than it is to
   * do it in the IRQ */
  unsigned char bytesize = 8;
  JsVar *options = jsvObjectGetChild(usartClass, DEVICE_OPTIONS_NAME, 0);
  if(jsvIsObject(options)) {
    unsigned char c = (unsigned char)jsvGetIntegerAndUnLock(jsvObjectGetChild(options, "bytesize", 0));
    if (c>=7 && c<10) bytesize = c;
  }
  jsvUnLock(options);

  JsVar *stringData = jsvNewFromEmptyString();
  if (stringData) {
    JsvStringIterator it;
    jsvStringIteratorNew(&it, stringData, 0);

    int i, chars = IOEVENTFLAGS_GETCHARS(event->flags);
    while (chars) {
      for (i=0;i<chars;i++) {
        char ch = (char)(event->data.chars[i] & ((1<<bytesize)-1)); // mask
        jsvStringIteratorAppend(&it, ch);
      }
      // look down the stack and see if there is more data
      if (jshIsTopEvent(IOEVENTFLAGS_GETTYPE(event->flags))) {
        jshPopIOEvent(event);
        chars = IOEVENTFLAGS_GETCHARS(event->flags);
      } else
        chars = 0;
    }
    jsvStringIteratorFree(&it);

    // Now run the handler
    jswrap_stream_pushData(usartClass, stringData, true);
    jsvUnLock(stringData);
  }
}

void jsiHandleIOEventForConsole(IOEvent *event) {
  int i, c = IOEVENTFLAGS_GETCHARS(event->flags);
  jsiSetBusy(BUSY_INTERACTIVE, true);
  for (i=0;i<c;i++) jsiHandleChar(event->data.chars[i]);
  jsiSetBusy(BUSY_INTERACTIVE, false);
}

void jsiIdle() {
  // This is how many times we have been here and not done anything.
  // It will be zeroed if we do stuff later
  if (loopsIdling<255) loopsIdling++;

  // Handle hardware-related idle stuff (like checking for pin events)
  bool wasBusy = false;
  IOEvent event;
  int maxEvents = IOBUFFERMASK+1; // ensure we can't get totally swamped by having more events than we can process
  while (maxEvents-- && jshPopIOEvent(&event)) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    wasBusy = true;

    IOEventFlags eventType = IOEVENTFLAGS_GETTYPE(event.flags);

    loopsIdling = 0; // because we're not idling
    if (eventType == consoleDevice) {
      jsiHandleIOEventForConsole(&event);
      /** don't allow us to read data when the device is our
       console device. It slows us down and just causes pain. */
    } else if (DEVICE_IS_USART(eventType)) {
      // ------------------------------------------------------------------------ SERIAL CALLBACK
      JsVar *usartClass = jsvSkipNameAndUnLock(jsiGetClassNameFromDevice(IOEVENTFLAGS_GETTYPE(event.flags)));
      if (jsvIsObject(usartClass)) {
        jsiHandleIOEventForUSART(usartClass, &event);
      }
      jsvUnLock(usartClass);
    } else if (DEVICE_IS_EXTI(eventType)) { // ---------------------------------------------------------------- PIN WATCH
      // we have an event... find out what it was for...
      // Check everything in our Watch array
      JsVar *watchArrayPtr = jsvLock(watchArray);
      JsvObjectIterator it;
      jsvObjectIteratorNew(&it, watchArrayPtr);
      while (jsvObjectIteratorHasValue(&it)) {
        bool hasDeletedWatch = false;
        JsVar *watchPtr = jsvObjectIteratorGetValue(&it);
        Pin pin = jshGetPinFromVarAndUnLock(jsvObjectGetChild(watchPtr, "pin", 0));

        if (jshIsEventForPin(&event, pin)) {
          /** Work out event time. Events time is only stored in 32 bits, so we need to
           * use the correct 'high' 32 bits from the current time.
           *
           * We know that the current time is always newer than the event time, so
           * if the bottom 32 bits of the current time is less than the bottom
           * 32 bits of the event time, we need to subtract a full 32 bits worth
           * from the current time.
           */
          JsSysTime time = jshGetSystemTime();
          if (((unsigned int)time) < (unsigned int)event.data.time)
            time = time - 0x100000000LL;
          // finally, mask in the event's time
          JsSysTime eventTime = (time & ~0xFFFFFFFFLL) | (JsSysTime)event.data.time;

          // Now actually process the event
          bool pinIsHigh = (event.flags&EV_EXTI_IS_HIGH)!=0;

          bool executeNow = false;
          JsVarInt debounce = jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "debounce", 0));
          if (debounce<=0) {
            executeNow = true;
          } else { // Debouncing - use timeouts to ensure we only fire at the right time
            // store the current state of the pin
            bool oldWatchState = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr, "state",0));
            jsvObjectSetChildAndUnLock(watchPtr, "state", jsvNewFromBool(pinIsHigh));

            JsVar *timeout = jsvObjectGetChild(watchPtr, "timeout", 0);
            if (timeout) { // if we had a timeout, update the callback time
              JsSysTime timeoutTime = jsiLastIdleTime + (JsSysTime)jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timeout, "time", 0));
              jsvUnLock(jsvObjectSetChild(timeout, "time", jsvNewFromLongInteger((JsSysTime)(eventTime - jsiLastIdleTime) + debounce)));
              if (eventTime > timeoutTime) {
                // timeout should have fired, but we didn't get around to executing it!
                // Do it now (with the old timeout time)
                executeNow = true;
                eventTime = timeoutTime - debounce;
                pinIsHigh = oldWatchState;
              }
            } else { // else create a new timeout
              timeout = jsvNewWithFlags(JSV_OBJECT);
              if (timeout) {
                jsvObjectSetChild(timeout, "watch", watchPtr); // no unlock
                jsvObjectSetChildAndUnLock(timeout, "time", jsvNewFromLongInteger((JsSysTime)(eventTime - jsiLastIdleTime) + debounce));
                jsvObjectSetChildAndUnLock(timeout, "callback", jsvObjectGetChild(watchPtr, "callback", 0));
                jsvObjectSetChildAndUnLock(timeout, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
                jsvObjectSetChildAndUnLock(timeout, "pin", jsvNewFromPin(pin));
                // Add to timer array
                jsiTimerAdd(timeout);
                // Add to our watch
                jsvObjectSetChild(watchPtr, "timeout", timeout); // no unlock
              }
            }
            jsvUnLock(timeout);
          }

          // If we want to execute this watch right now...
          if (executeNow) {
            JsVar *timePtr = jsvNewFromFloat(jshGetMillisecondsFromTime(eventTime)/1000);
            if (jsiShouldExecuteWatch(watchPtr, pinIsHigh)) { // edge triggering
              JsVar *watchCallback = jsvObjectGetChild(watchPtr, "callback", 0);
              bool watchRecurring = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr,  "recur", 0));
              JsVar *data = jsvNewWithFlags(JSV_OBJECT);
              if (data) {
                jsvObjectSetChildAndUnLock(data, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
                // set both data.time, and watch.lastTime in one go
                jsvObjectSetChild(data, "time", timePtr); // no unlock
                jsvObjectSetChildAndUnLock(data, "pin", jsvNewFromPin(pin));
                jsvObjectSetChildAndUnLock(data, "state", jsvNewFromBool(pinIsHigh));
              }
              if (!jsiExecuteEventCallback(0, watchCallback, 1, &data) && watchRecurring) {
                jsError("Ctrl-C while processing watch - removing it.");
                jsErrorFlags |= JSERR_CALLBACK;
                watchRecurring = false;
              }
              jsvUnLock(data);
              if (!watchRecurring) {
                // free all
                jsvObjectIteratorRemoveAndGotoNext(&it, watchArrayPtr);
                hasDeletedWatch = true;
                if (!jsiIsWatchingPin(pin))
                  jshPinWatch(pin, false);
              }
              jsvUnLock(watchCallback);
            }
            jsvObjectSetChildAndUnLock(watchPtr, "lastTime", timePtr);
          }
        }

        jsvUnLock(watchPtr);
        if (!hasDeletedWatch)
          jsvObjectIteratorNext(&it);
      }
      jsvObjectIteratorFree(&it);
      jsvUnLock(watchArrayPtr);
    }
  }

  // Reset Flow control if it was set...
  if (jshGetEventsUsed() < IOBUFFER_XON) { 
    jshSetFlowControlXON(EV_USBSERIAL, true);
    int i;
    for (i=0;i<USART_COUNT;i++)
      jshSetFlowControlXON(EV_SERIAL1+i, true);
  }

  // Check timers
  JsSysTime minTimeUntilNext = JSSYSTIME_MAX;
  JsSysTime time = jshGetSystemTime();
  JsSysTime timePassed = time - jsiLastIdleTime;
  jsiLastIdleTime = time;
  // add time to Ctrl-C counter, checking for overflow
  uint32_t oldTimeSinceCtrlC = jsiTimeSinceCtrlC;
  jsiTimeSinceCtrlC += (uint32_t)timePassed;
  if (oldTimeSinceCtrlC > jsiTimeSinceCtrlC)
    jsiTimeSinceCtrlC = 0xFFFFFFFF;

  jsiStatus = jsiStatus & ~JSIS_TIMERS_CHANGED;
  JsVar *timerArrayPtr = jsvLock(timerArray);
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, timerArrayPtr);
  while (jsvObjectIteratorHasValue(&it) && !(jsiStatus & JSIS_TIMERS_CHANGED)) {
    bool hasDeletedTimer = false;
    JsVar *timerPtr = jsvObjectIteratorGetValue(&it);
    JsSysTime timerTime = (JsSysTime)jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timerPtr, "time", 0));
    JsSysTime timeUntilNext = timerTime - timePassed;

    if (timeUntilNext<=0) {
      // we're now doing work
      jsiSetBusy(BUSY_INTERACTIVE, true);
      wasBusy = true;
      JsVar *timerCallback = jsvObjectGetChild(timerPtr, "callback", 0);
      JsVar *watchPtr = jsvObjectGetChild(timerPtr, "watch", 0); // for debounce - may be undefined
      bool exec = true;
      JsVar *data = 0;
      if (watchPtr) {
        data = jsvNewWithFlags(JSV_OBJECT);
        // if we were from a watch then we were delayed by the debounce time...
        if (data) {
          JsVarInt delay = jsvGetIntegerAndUnLock(jsvObjectGetChild(watchPtr, "debounce", 0));
          // Create the 'time' variable that will be passed to the user
          JsVar *timePtr = jsvNewFromFloat(jshGetMillisecondsFromTime(jsiLastIdleTime+timeUntilNext-delay)/1000);
          // if it was a watch, set the last state up
          bool state = jsvGetBoolAndUnLock(jsvObjectSetChild(data, "state", jsvObjectGetChild(watchPtr, "state", 0)));
          exec = jsiShouldExecuteWatch(watchPtr, state);
          // set up the lastTime variable of data to what was in the watch
          jsvObjectSetChildAndUnLock(data, "lastTime", jsvObjectGetChild(watchPtr, "lastTime", 0));
          // set up the watches lastTime to this one
          jsvObjectSetChild(watchPtr, "lastTime", timePtr); // don't unlock
          jsvObjectSetChildAndUnLock(data, "time", timePtr);
        }
      }
      JsVar *interval = jsvObjectGetChild(timerPtr, "interval", 0);
      if (exec) {
        bool execResult;
        if (data) {
          execResult = jsiExecuteEventCallback(0, timerCallback, 1, &data);
        } else {
          JsVar *argsArray = jsvObjectGetChild(timerPtr, "args", 0);
          execResult = jsiExecuteEventCallbackArgsArray(0, timerCallback, argsArray);
          jsvUnLock(argsArray);
        }
        if (!execResult && interval) {
          jsError("Ctrl-C while processing interval - removing it.");
          jsErrorFlags |= JSERR_CALLBACK;
          // by setting interval to 0, we now think we've for a Timeout,
          // which will get removed.
          jsvUnLock(interval);
          interval = 0;
        }
      }
      jsvUnLock(data);
      if (watchPtr) { // if we had a watch pointer, be sure to remove us from it
        jsvObjectSetChild(watchPtr, "timeout", 0);
        // Deal with non-recurring watches
        if (exec) {
          bool watchRecurring = jsvGetBoolAndUnLock(jsvObjectGetChild(watchPtr,  "recur", 0));
          if (!watchRecurring) {
            JsVar *watchArrayPtr = jsvLock(watchArray);
            JsVar *watchNamePtr = jsvGetArrayIndexOf(watchArrayPtr, watchPtr, true);
            if (watchNamePtr) {
              jsvRemoveChild(watchArrayPtr, watchNamePtr);
              jsvUnLock(watchNamePtr);
            }
            jsvUnLock(watchArrayPtr);
            Pin pin = jshGetPinFromVarAndUnLock(jsvObjectGetChild(watchPtr, "pin", 0));
            if (!jsiIsWatchingPin(pin))
              jshPinWatch(pin, false);
          }
        }
        jsvUnLock(watchPtr);
      }

      if (interval) {
        timeUntilNext = timeUntilNext + jsvGetLongIntegerAndUnLock(interval);
      } else {
        // free
        // Beware... may have already been removed!
        jsvObjectIteratorRemoveAndGotoNext(&it, timerArrayPtr);
        hasDeletedTimer = true;
        timeUntilNext = -1;
      }
      jsvUnLock(timerCallback);

    }
    // update the time until the next timer
    if (timeUntilNext>=0 && timeUntilNext < minTimeUntilNext)
      minTimeUntilNext = timeUntilNext;
    // update the timer's time
    if (!hasDeletedTimer) {
      jsvObjectSetChildAndUnLock(timerPtr, "time", jsvNewFromLongInteger(timeUntilNext));
      jsvObjectIteratorNext(&it);
    }
    jsvUnLock(timerPtr);
  }
  jsvObjectIteratorFree(&it);
  jsvUnLock(timerArrayPtr);
  /* We might have left the timers loop with stuff to do because the contents of it
   * changed. It's not a big deal because it could only have changed because a timer
   * got executed - so `wasBusy` got set and we know we're going to go around the
   * loop again before sleeping.
   */ 

  // Check for events that might need to be processed from other libraries
  if (jswIdle()) wasBusy = true;

  // Just in case we got any events to do and didn't clear loopsIdling before
  if (wasBusy || !jsvArrayIsEmpty(events) )
    loopsIdling = 0;

  if (wasBusy)
    jsiSetBusy(BUSY_INTERACTIVE, false);

  // execute any outstanding events
  if (!jspIsInterrupted()) {
    jsiExecuteEvents();
  }
  if (interruptedDuringEvent) {
    jspSetInterrupted(false);
    interruptedDuringEvent = false;
    jsiConsoleRemoveInputLine();
    jsiConsolePrint("Execution Interrupted during event processing.\n");
  }

  // check for TODOs
  if (jsiStatus&JSIS_TODO_MASK) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_RESET) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;
      // shut down everything and start up again
      jsiKill();
      jsvKill();
      jshReset();
      jsvInit();
      jsiSemiInit(false); // don't autoload
    }
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_FLASH_SAVE) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;

      jsvGarbageCollect(); // nice to have everything all tidy!
      jsiSoftKill();
      jspSoftKill();
      jsvSoftKill();
      jsfSaveToFlash();
      jshReset();
      jsvSoftInit();
      jspSoftInit();
      jsiSoftInit();
    }
    if ((jsiStatus&JSIS_TODO_MASK) == JSIS_TODO_FLASH_LOAD) {
      jsiStatus &= (JsiStatus)~JSIS_TODO_MASK;

      jsiSoftKill();
      jspSoftKill();
      jsvSoftKill();
      jshReset();
      jsfLoadFromFlash();
      jsvSoftInit();
      jspSoftInit();
      jsiSoftInit();
    }
    jsiSetBusy(BUSY_INTERACTIVE, false);
  }

  /* if we've been around this loop, there is nothing to do, and
   * we have a spare 10ms then let's do some Garbage Collection
   * just in case. */
  if (loopsIdling==1 &&
      minTimeUntilNext > jshGetTimeFromMilliseconds(10)) {
    jsiSetBusy(BUSY_INTERACTIVE, true);
    jsvGarbageCollect();
    jsiSetBusy(BUSY_INTERACTIVE, false);
  }

  // Go to sleep!
  if (loopsIdling>1 && // once around the idle loop without having done any work already (just in case)
#ifdef USB
      !jshIsUSBSERIALConnected() && // if USB is on, no point sleeping (later, sleep might be more drastic)
#endif
      !jshHasEvents() && //no events have arrived in the mean time
      !jshHasTransmitData()/* && //nothing left to send over serial?
      minTimeUntilNext > SYSTICK_RANGE*5/4*/) { // we are sure we won't miss anything - leave a little leeway (SysTick will wake us up!)
    jshSleep(minTimeUntilNext);
  }
}

bool jsiLoop() {
  // idle stuff for hardware
  jshIdle();
  // Do general idle stuff
  jsiIdle();
  // check for and report errors
  jsiCheckErrors();

  // If Ctrl-C was pressed, clear the line
  if (execInfo.execute & EXEC_CTRL_C_MASK) {
    execInfo.execute = execInfo.execute & (JsExecFlags)~EXEC_CTRL_C_MASK;
    if (jsvIsEmptyString(inputLine)) {
#ifndef EMBEDDED
      if (jsiTimeSinceCtrlC < jshGetTimeFromMilliseconds(5000))
        exit(0); // exit if ctrl-c on empty input line
      else
        jsiConsolePrintf("Press Ctrl-C again to exit\n");
#endif
      jsiTimeSinceCtrlC = 0;
    }
    jsiClearInputLine();
  }

  // return console (if it was gone!)
  jsiReturnInputLine();

  return loopsIdling==0;
}



/** Output current interpreter state such that it can be copied to a new device */
void jsiDumpState(vcbprintf_callback user_callback, void *user_data) {
  JsvObjectIterator it;

  jsvObjectIteratorNew(&it, execInfo.root);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *child = jsvObjectIteratorGetKey(&it);
    JsVar *data = jsvObjectIteratorGetValue(&it);
    char childName[JSLEX_MAX_TOKEN_LENGTH];
    jsvGetString(child, childName, JSLEX_MAX_TOKEN_LENGTH);

    if (jswIsBuiltInObject(childName)) {
      jsiDumpObjectState(user_callback, user_data, child, data);
    } else if (jsvIsStringEqual(child, JSI_TIMERS_NAME)) {
      // skip - done later
    } else if (jsvIsStringEqual(child, JSI_WATCHES_NAME)) {
      // skip - done later
    } else if (child->varData.str[0]==JS_HIDDEN_CHAR ||
        jshFromDeviceString(childName)!=EV_NONE) {
      // skip - don't care about this stuff
    } else if (!jsvIsNative(data)) { // just a variable/function!
      if (jsvIsFunction(data)) {
        // function-specific output
        cbprintf(user_callback, user_data, "function %v", child);
        jsfGetJSONForFunctionWithCallback(data, JSON_SHOW_DEVICES, user_callback, user_data);
        user_callback("\n", user_data);
        // print any prototypes we had
        jsiDumpObjectState(user_callback, user_data, child, data);
      } else {
        // normal variable definition
        cbprintf(user_callback, user_data, "var %v = ", child);
        bool hasProto = false;
        if (jsvIsObject(data)) {
          JsVar *proto = jsvObjectGetChild(data, JSPARSE_INHERITS_VAR, 0);
          if (proto) {
            JsVar *protoName = jsvGetPathTo(execInfo.root, proto, 4, data);
            if (protoName) {
              cbprintf(user_callback, user_data, "Object.create(%v);\n", protoName);
              jsiDumpObjectState(user_callback, user_data, child, data);
              hasProto = true;
            }
          }
        }
        if (!hasProto) {
          jsiDumpJSON(user_callback, user_data, data, child);
          user_callback(";\n", user_data);
        }
      }
    }
    jsvUnLock2(data, child);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  // Now do timers
  JsVar *timerArrayPtr = jsvLock(timerArray);
  jsvObjectIteratorNew(&it, timerArrayPtr);
  jsvUnLock(timerArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *timer = jsvObjectIteratorGetValue(&it);
    JsVar *timerCallback = jsvSkipOneNameAndUnLock(jsvFindChildFromString(timer, "callback", false));
    JsVar *timerInterval = jsvObjectGetChild(timer, "interval", 0);
    user_callback(timerInterval ? "setInterval(" : "setTimeout(", user_data);
    jsiDumpJSON(user_callback, user_data, timerCallback, 0);
    cbprintf(user_callback, user_data, ", %f);\n", jshGetMillisecondsFromTime(timerInterval ? jsvGetLongInteger(timerInterval) : jsvGetLongIntegerAndUnLock(jsvObjectGetChild(timer, "time", 0))));
    jsvUnLock2(timerInterval, timerCallback);
    // next
    jsvUnLock(timer);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  // Now do watches
  JsVar *watchArrayPtr = jsvLock(watchArray);
  jsvObjectIteratorNew(&it, watchArrayPtr);
  jsvUnLock(watchArrayPtr);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *watch = jsvObjectIteratorGetValue(&it);
    JsVar *watchCallback = jsvSkipOneNameAndUnLock(jsvFindChildFromString(watch, "callback", false));
    bool watchRecur = jsvGetBoolAndUnLock(jsvObjectGetChild(watch, "recur", 0));
    int watchEdge = (int)jsvGetIntegerAndUnLock(jsvObjectGetChild(watch, "edge", 0));
    JsVar *watchPin = jsvObjectGetChild(watch, "pin", 0);
    JsVarInt watchDebounce = jsvGetIntegerAndUnLock(jsvObjectGetChild(watch, "debounce", 0));
    user_callback("setWatch(", user_data);
    jsiDumpJSON(user_callback, user_data, watchCallback, 0);
    cbprintf(user_callback, user_data, ", %j, { repeat:%s, edge:'%s'",
        watchPin,
        watchRecur?"true":"false",
            (watchEdge<0)?"falling":((watchEdge>0)?"rising":"both"));
    if (watchDebounce>0)
      cbprintf(user_callback, user_data, ", debounce : %f", jshGetMillisecondsFromTime(watchDebounce));
    user_callback(" });\n", user_data);
    jsvUnLock2(watchPin, watchCallback);
    // next
    jsvUnLock(watch);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  // and now the actual hardware
  jsiDumpHardwareInitialisation(user_callback, user_data, true);
}

JsVarInt jsiTimerAdd(JsVar *timerPtr) {
  JsVar *timerArrayPtr = jsvLock(timerArray);
  JsVarInt itemIndex = jsvArrayAddToEnd(timerArrayPtr, timerPtr, 1) - 1;
  jsvUnLock(timerArrayPtr);
  return itemIndex;
}

void jsiTimersChanged() {
  jsiStatus |= JSIS_TIMERS_CHANGED;
}

#ifdef USE_DEBUGGER
void jsiDebuggerLoop() {
  if ((jsiStatus & JSIS_IN_DEBUGGER) ||
      (execInfo.execute & EXEC_PARSE_FUNCTION_DECL)) return;
  execInfo.execute &= (JsExecFlags)~(
      EXEC_CTRL_C_MASK |
      EXEC_DEBUGGER_NEXT_LINE |
      EXEC_DEBUGGER_STEP_INTO |
      EXEC_DEBUGGER_FINISH_FUNCTION);
  jsiClearInputLine();
  jsiConsoleRemoveInputLine();
  jsiStatus = (jsiStatus & ~JSIS_ECHO_OFF_MASK) | JSIS_IN_DEBUGGER;

  if (execInfo.lex)
    jslPrintTokenLineMarker((vcbprintf_callback)jsiConsolePrint, 0, execInfo.lex, execInfo.lex->tokenLastStart);

  while (!(jsiStatus & JSIS_EXIT_DEBUGGER) &&
         !(execInfo.execute & EXEC_CTRL_C_MASK)) {
    jsiReturnInputLine();
    // idle stuff for hardware
    jshIdle();
    // Idle just for debug (much stuff removed) -------------------------------
    IOEvent event;
    // If we have too many events (> half full) drain the queue
    while (jshGetEventsUsed()>IOBUFFERMASK*1/2) {
      if (jshPopIOEvent(&event) && IOEVENTFLAGS_GETTYPE(event.flags)==consoleDevice)
        jsiHandleIOEventForConsole(&event);
    }
    // otherwise grab the remaining console events
    while (jshPopIOEventOfType(consoleDevice, &event)) {
      jsiHandleIOEventForConsole(&event);
    }
    // -----------------------------------------------------------------------
  }
  jsiConsoleRemoveInputLine();
  if (execInfo.execute & EXEC_CTRL_C_MASK)
    execInfo.execute |= EXEC_INTERRUPTED;
  jsiStatus &= ~(JSIS_IN_DEBUGGER|JSIS_EXIT_DEBUGGER);
}

void jsiDebuggerPrintScope(JsVar *scope) {
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, scope);
  bool found = false;
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *k = jsvObjectIteratorGetKey(&it);
    JsVar *ks = jsvAsString(k, false);
    JsVar *v = jsvObjectIteratorGetValue(&it);
    size_t l = jsvGetStringLength(ks);

    if (!jsvIsStringEqual(ks, JSPARSE_RETURN_VAR)) {
      found = true;
      jsiConsolePrintChar(' ');
      if (jsvIsFunctionParameter(k)) {
        jsiConsolePrint("param ");
        l+=6;
      }
      jsiConsolePrintStringVar(ks);
      while (l<20) {
        jsiConsolePrintChar(' ');
        l++;
      }
      jsiConsolePrint(" : ");
      jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
      jsiConsolePrint("\n");
    }

    jsvUnLock3(k, ks, v);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  if (!found) {
    jsiConsolePrint(" [No variables]\n");
  }
}

/// Interpret a line of input in the debugger
void jsiDebuggerLine(JsVar *line) {
  assert(jsvIsString(line));
  JsLex lex;
  jslInit(&lex, line);
  bool handled = false;
  if (lex.tk == LEX_ID) {
    handled = true;
    char *id = jslGetTokenValueAsString(&lex);

    if (!strcmp(id,"help") || !strcmp(id,"h")) {
      jsiConsolePrint("Commands:\n"
                      "help / h           - this information\n"
                      "quit / q / Ctrl-C  - Quit debug mode, break execution\n"
                      "reset              - Soft-reset Espruino\n"
                      "continue / c       - Continue execution\n"
                      "next / n           - execute to next line\n"
                      "step / s           - execute to next line, or step into function call\n"
                      "finish / f         - finish execution of the function call\n"
                      "print ... / p ...  - evaluate and print the next argument\n"
                      "info ... / i ...   - print information. Type 'info' for help \n");
    } else if (!strcmp(id,"quit") || !strcmp(id,"q")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_INTERRUPTED;
    } else if (!strcmp(id,"reset")) {
      jsiStatus = (JsiStatus)(jsiStatus & ~JSIS_TODO_MASK) | JSIS_EXIT_DEBUGGER | JSIS_TODO_RESET;
      execInfo.execute |= EXEC_INTERRUPTED;
    } else if (!strcmp(id,"continue") || !strcmp(id,"c")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
    } else if (!strcmp(id,"next") || !strcmp(id,"n")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_NEXT_LINE;
    } else if (!strcmp(id,"step") || !strcmp(id,"s")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_NEXT_LINE|EXEC_DEBUGGER_STEP_INTO;
    } else if (!strcmp(id,"finish") || !strcmp(id,"f")) {
      jsiStatus |= JSIS_EXIT_DEBUGGER;
      execInfo.execute |= EXEC_DEBUGGER_FINISH_FUNCTION;
    } else if (!strcmp(id,"print") || !strcmp(id,"p")) {
      jslGetNextToken(&lex);
      JsExecInfo oldExecInfo = execInfo;
      execInfo.lex = &lex; // execute with the remainder of the line
      execInfo.execute = EXEC_YES;
      JsVar *v = jsvSkipNameAndUnLock(jspParse());
      execInfo = oldExecInfo;
      jsiConsolePrintChar('=');
      jsfPrintJSON(v, JSON_LIMIT | JSON_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES);
      jsiConsolePrint("\n");
      jsvUnLock(v);
    } else if (!strcmp(id,"info") || !strcmp(id,"i")) {
       jslGetNextToken(&lex);
       id = jslGetTokenValueAsString(&lex);
       if (!strcmp(id,"locals") || !strcmp(id,"l")) {
         if (execInfo.scopeCount==0)
           jsiConsolePrint("No locals found\n");
         else {
           jsiConsolePrintf("Locals:\n--------------------------------\n");
           jsiDebuggerPrintScope(execInfo.scopes[execInfo.scopeCount-1]);
           jsiConsolePrint("\n\n");
         }
       } else if (!strcmp(id,"scopechain") || !strcmp(id,"s")) {
         if (execInfo.scopeCount==0) jsiConsolePrint("No scopes found\n");
         int i;
         for (i=0;i<execInfo.scopeCount;i++) {
           jsiConsolePrintf("Scope %d:\n--------------------------------\n", i);
           jsiDebuggerPrintScope(execInfo.scopes[i]);
           jsiConsolePrint("\n\n");
         }
       } else {
         jsiConsolePrint("Unknown command:\n"
                         "info locals     (l) - output local variables\n"
                         "info scopechain (s) - output all variables in all scopes\n");
       }
    } else
      handled = false;
  }
  if (!handled) {
    jsiConsolePrint("In debug mode: Expected a simple ID, type 'help' for more info.\n");
  }

  jslKill(&lex);
}
#endif // USE_DEBUGGER