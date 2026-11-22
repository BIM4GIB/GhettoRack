# Project Name
TARGET = ApcBox

# Sources
CPP_SOURCES = main.cpp engine.cpp box.cpp usbdev.cpp
# ST's control-request handler rebuilt for 4 USB interfaces (serial + audio)
C_SOURCES = usbd_ctlreq_apc.c

# Runs from the Daisy bootloader (QSPI flash): the app is too big for the
# 128 KB internal flash. Flash the bootloader once with `make program-boot`.
APP_TYPE = BOOT_QSPI

# ReverbSc lives in the LGPL part of DaisySP; SD card needs FatFS
USE_DAISYSP_LGPL = 1
USE_FATFS = 1

# printf with floats (tempo, MB, calibration numbers); nano.specs drops it otherwise
LDFLAGS = -u _printf_float

# Library Locations
LIBDAISY_DIR = ../lib/libDaisy/
DAISYSP_DIR = ../lib/DaisySP/

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
