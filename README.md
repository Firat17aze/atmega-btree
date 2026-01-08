# Silicon DB

A tiny database engine that runs on Arduino Uno. It stores key-value pairs in EEPROM (survives power loss), uses a B-Tree for fast lookups instead of scanning everything, and has a simple crash recovery system so your data doesn't get corrupted if power cuts out mid-write. Connect via serial at 9600 baud and use commands like `INSERT 10 1234`, `FIND 10`, `LIST`, and `FLUSH`. 

## Build & Flash

```bash
make
make flash PORT=/dev/cu.usbmodem1101
screen /dev/cu.usbmodem1101 9600
```

## Commands

- `INSERT <id> <value>` - Store a record
- `FIND <id>` - Look up a record
- `DELETE <id>` - Remove a record
- `LIST` - Show all records
- `FLUSH` - Save to EEPROM
- `STATS` - Show internals
