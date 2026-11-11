// Minimal pin definitions for the retry project.
//
// polling_keyboard.h (copied from the main firmware) only needs the keyboard
// interrupt line, but it includes this header by name, so this project provides
// its own small version instead of dragging in the whole RF configuration.
#pragma once

// TCA8418 /INT on the Cardputer ADV.
#define RF_KBD_INT 11

// CAP.CC1101 pins, for reference; the NFC unit is constructed with its CS pin.
#define RF_NFC_CS 6
