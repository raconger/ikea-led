# Arduino Nano ESP32 Setup Guide for IKEA OBEGRÄNSAD

## Hardware Configuration

### Pin Mapping

| Arduino Nano ESP32 Pin | IKEA Lamp Connector | Function | GPIO | Notes |
|------------------------|---------------------|----------|------|-------|
| **5V** | VCC | Power (5V) | - | Use when USB-powered |
| **GND** | GND | Ground | - | Common ground |
| **D11** | Data | Serial Data (MOSI) | GPIO38 | Hardware SPI MOSI |
| **D13** | Clock | Serial Clock (SCK) | GPIO48 | Hardware SPI SCK |
| **D5** | Latch | Latch Signal | GPIO8 | Any GPIO works |
| **D6** | Enable | Output Enable | GPIO9 | Any GPIO works |
| **D2** | Button | User Button Input | GPIO5 | Has interrupt support |
| **GND** | Button GND | Button Ground | - | For physical button |

### Important Notes

- **Voltage Levels**: All GPIO pins output 3.3V logic (compatible with this lamp)
- **Power**: The 5V pin provides power to the LED panels (256 LEDs total)
- **SPI**: Uses hardware SPI at 10MHz for optimal performance
- **Button**: Single press = next plugin, Long press (1s+) = previous plugin

## Software Configuration

### Project Settings (already configured)

**File: `platformio.ini`**
- Added `[env:nano_esp32]` build environment
- Uses `esptool` upload protocol
- Board: `arduino_nano_esp32`

**File: `include/constants.h`**
- Pin definitions for Arduino Nano ESP32
- Weather location: `Evergreen, CO`
- Timezone: `MST7MDT,M3.2.0,M11.1.0` (Mountain Time)
- NTP Server: `us.pool.ntp.org`
- WiFi Manager SSID: `IKEA`

**File: `include/secrets.h`**
- OTA Username: `admin`
- OTA Password: `ikea-led-wall`
- WiFi: Configured via WiFi Manager (no hardcoding needed)

## Build and Upload Instructions

### Prerequisites

1. **Install PlatformIO** (choose one):
   - VS Code Extension: "PlatformIO IDE"
   - CLI: `yay -S platformio-core` (Arch Linux)
   - pip: `pip install --user platformio`

2. **Install udev rules** (Linux only):
   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   sudo usermod -a -G uucp,lock $USER
   ```
   **IMPORTANT**: Log out and back in after running these commands!

### Using VS Code (Recommended)

1. **Open project** in VS Code
2. **Connect** Arduino Nano ESP32 via USB-C
3. **Select environment**:
   - Click bottom toolbar where it says "Default (esp32dev)"
   - Select **"nano_esp32"** from the list
4. **Upload**:
   - Click the arrow (→) icon in bottom toolbar, OR
   - PlatformIO sidebar → nano_esp32 → Upload

### Using Command Line

```bash
# Build
pio run -e nano_esp32

# Upload
pio run -e nano_esp32 -t upload

# Monitor serial output
pio device monitor -e nano_esp32

# Upload + Monitor (combined)
pio run -e nano_esp32 -t upload && pio device monitor -e nano_esp32
```

### Check Connected Devices

```bash
pio device list
```

Look for `/dev/ttyACM0` or `/dev/ttyUSB0` (NOT `/dev/ttyS0`)

## First Boot Setup

### WiFi Configuration (using WiFi Manager)

1. Power on the board (after uploading firmware)
2. Board creates WiFi network named **"IKEA"**
3. Connect to "IKEA" network from phone/laptop
4. Captive portal opens automatically (or browse to `192.168.4.1`)
5. Enter WiFi credentials:
   - Network: `home`
   - Password: `Soulpatch`
6. Device reboots and connects to your network
7. Check serial monitor for IP address

### Access Web Interface

Once connected to WiFi:
```
http://<device-ip-address>
```

Features:
- Live drawing on 16x16 display
- Plugin selection (Clock, Weather, Snake, Breakout, etc.)
- Image upload
- Animation creator
- Plugin scheduler

## Over-The-Air (OTA) Updates

### Web Interface Method

1. Navigate to `http://<device-ip>/update`
2. Login with:
   - Username: `admin`
   - Password: `ikea-led-wall`
3. Upload firmware file: `.pio/build/nano_esp32/firmware.bin`
4. Device auto-reboots with new firmware

### PlatformIO Method

Uncomment and configure in `platformio.ini` (nano_esp32 section):
```ini
extra_scripts = upload.py
upload_protocol = custom
custom_upload_url = http://<device-ip>
custom_username = admin
custom_password = ikea-led-wall
```

Then upload normally via PlatformIO (it will use OTA).

## Troubleshooting

### Upload Fails - Port Not Found

**Symptoms**: `/dev/ttyS0` detected instead of `/dev/ttyACM0`

**Solutions**:
1. Check USB cable supports data (not charge-only)
2. Try different USB port
3. Run `pio device list` to verify board is detected
4. Install udev rules (see Prerequisites)
5. Add user to groups and **log out/in**
6. Manually specify port: `--upload-port /dev/ttyACM0`

### Upload Fails - Permission Denied

**Symptoms**: "Path '/dev/ttyACM0' is not readable"

**Solution**:
```bash
sudo usermod -a -G uucp,lock $USER
```
Then **log out and back in** (this is critical!)

**Quick temporary fix**:
```bash
sudo chmod 666 /dev/ttyACM0
```

### Wrong Environment Selected in VS Code

**Symptoms**: Building for "esp32dev" instead of "nano_esp32"

**Solution**:
- Click environment name in bottom toolbar
- Select "nano_esp32"

### Board Not Entering Bootloader Mode

**Solution**: Manual bootloader entry:
1. Hold **BOOT** button
2. While holding, press and release **RESET** button
3. Release **BOOT** button
4. Immediately run upload command

### Display Flickering

**Check**:
1. All solder connections (especially VCC and GND)
2. Power supply is adequate (5V, sufficient current)
3. No loose wires

## Physical Button Usage

- **Single press**: Switch to next plugin
- **Long press (1+ second)**: Return to previously used plugin

## Available Plugins

- Draw
- Game of Life
- Breakout
- Snake
- Stars
- Lines
- Circle
- Clock
- Big Clock
- Weather (Evergreen, CO)
- Rain
- Animation
- Firework
- DDP (Display Data Protocol)
- Pong Clock
- Ticking Clock

## Technical Specifications

- **Display**: 16x16 pixels (256 LEDs)
- **Refresh Rate**: 200µs interval (5 kHz)
- **SPI Speed**: 10 MHz
- **Brightness**: Per-pixel (0-255) + global multiplier
- **Protocol**: Shift register with latch/enable control
- **WiFi**: 2.4GHz 802.11 b/g/n
- **Microcontroller**: ESP32-S3 @ 240MHz
- **RAM**: 320KB
- **Flash**: 16MB

## Configuration Files Reference

| File | Purpose |
|------|---------|
| `platformio.ini` | Build environments and board configurations |
| `include/constants.h` | Pin definitions, display constants, timezone, weather location |
| `include/secrets.h` | WiFi hostname, OTA credentials |
| `src/screen.cpp` | Display rendering, SPI initialization |
| `src/main.cpp` | Setup and main loop |

## Support

- GitHub Issues: https://github.com/ph1p/ikea-led-obegraensad/issues
- Original README: See `README.md` in project root
