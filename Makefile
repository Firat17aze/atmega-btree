# ============================================================================
# SILICON DB - Makefile for ATmega328P
# ============================================================================
# 
# Bare metal compilation using avr-gcc toolchain.
# No Arduino IDE required - pure AVR toolchain.
#
# Requirements:
#   - avr-gcc (AVR cross-compiler)
#   - avr-libc (AVR C library)
#   - avrdude (Flash programmer)
#
# Installation (macOS):
#   brew tap osx-cross/avr
#   brew install avr-gcc avrdude
#
# Installation (Ubuntu/Debian):
#   sudo apt-get install gcc-avr avr-libc avrdude
#
# ============================================================================

# Target MCU
MCU = atmega328p

# CPU Frequency (16MHz for Arduino Uno)
F_CPU = 16000000UL

# Programmer settings (Arduino as ISP via USB)
PROGRAMMER = arduino
PORT = /dev/ttyACM0
BAUD = 115200

# Compiler and tools
CC = avr-gcc
OBJCOPY = avr-objcopy
OBJDUMP = avr-objdump
SIZE = avr-size
AVRDUDE = avrdude

# Compiler flags
CFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -Wall -Wextra -std=c99
CFLAGS += -funsigned-char -funsigned-bitfields -fpack-struct -fshort-enums
CFLAGS += -ffunction-sections -fdata-sections

# Linker flags
LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

# Source files
SRC = main.c
OBJ = $(SRC:.c=.o)

# Output files
TARGET = silicondb
ELF = $(TARGET).elf
HEX = $(TARGET).hex
MAP = $(TARGET).map
LSS = $(TARGET).lss

# ============================================================================
# Build targets
# ============================================================================

.PHONY: all clean flash size disasm help

# Default target
all: $(HEX) size

# Link object files into ELF
$(ELF): $(OBJ)
	@echo "Linking $(ELF)..."
	$(CC) $(LDFLAGS) -Wl,-Map=$(MAP) -o $@ $^

# Convert ELF to Intel HEX format
$(HEX): $(ELF)
	@echo "Creating $(HEX)..."
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# Compile C source to object files
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

# Display memory usage
size: $(ELF)
	@echo ""
	@echo "============================================"
	@echo "Memory Usage:"
	@echo "============================================"
	$(SIZE) --format=avr --mcu=$(MCU) $(ELF)
	@echo ""
	@echo "ATmega328P Limits:"
	@echo "  Flash: 32KB (32768 bytes)"
	@echo "  SRAM:  2KB  (2048 bytes)"
	@echo "  EEPROM: 1KB (1024 bytes)"
	@echo "============================================"

# Flash the MCU
flash: $(HEX)
	@echo "Flashing to $(MCU) via $(PORT)..."
	$(AVRDUDE) -c $(PROGRAMMER) -p $(MCU) -P $(PORT) -b $(BAUD) -U flash:w:$(HEX):i

# Generate disassembly listing
disasm: $(ELF)
	@echo "Generating disassembly..."
	$(OBJDUMP) -h -S $(ELF) > $(LSS)
	@echo "Created $(LSS)"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -f $(OBJ) $(ELF) $(HEX) $(MAP) $(LSS)

# Help
help:
	@echo ""
	@echo "SILICON DB Build System"
	@echo "======================="
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the project (default)"
	@echo "  flash   - Upload to ATmega328P"
	@echo "  size    - Show memory usage"
	@echo "  disasm  - Generate disassembly listing"
	@echo "  clean   - Remove build artifacts"
	@echo "  help    - Show this message"
	@echo ""
	@echo "Configuration:"
	@echo "  MCU:    $(MCU)"
	@echo "  F_CPU:  $(F_CPU)"
	@echo "  PORT:   $(PORT)"
	@echo ""
	@echo "To change the serial port:"
	@echo "  make flash PORT=/dev/ttyUSB0"
	@echo ""

# ============================================================================
# Fuse settings (for reference - DO NOT flash without understanding!)
# ============================================================================
# ATmega328P default fuses (8MHz internal oscillator):
#   lfuse: 0x62, hfuse: 0xD9, efuse: 0xFF
#
# Arduino Uno fuses (16MHz external crystal):
#   lfuse: 0xFF, hfuse: 0xDE, efuse: 0xFD
#
# WARNING: Incorrect fuse settings can brick your MCU!
# ============================================================================

