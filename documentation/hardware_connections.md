# Hardware Connections – IoT-Based Power Factor Monitoring and Automatic Compensation in a Smart Microgrid

## 1. Hardware Architecture

The proposed system consists of three independent monitoring and control buses. Each bus is equipped with an ESP32 microcontroller and a PZEM-004T energy meter.

- **Bus 1:** Resistive load monitoring using three relay-controlled bulb loads.
- **Bus 2:** Resistive load monitoring using three bulb relay outputs and one additional relay output.
- **Bus 3:** RL load monitoring and automatic power factor correction using a relay-controlled capacitor bank.

Each ESP32 independently communicates with Firebase through Wi-Fi for centralized monitoring.

## 2. Hardware Components

| Component | Quantity | Purpose |
|---|---:|---|
| ESP32 development board | 3 | Independent bus monitoring and control |
| PZEM-004T CT-type energy meter | 3 | Electrical parameter measurement |
| Relay modules | As required | Load and capacitor switching |
| Resistive bulb loads | Bus 1 and Bus 2 | Resistive loading |
| RL load | 1 | Inductive loading for PF correction |
| Capacitors | 3 × 8 µF | Reactive power compensation |
| Wi-Fi network | 1 | Wireless communication |
| Firebase Realtime Database | 1 | Centralized data storage |

## 3. ESP32–PZEM-004T Communication

All three ESP32 controllers use UART communication to interface with their respective PZEM-004T meters.

| ESP32 Pin | PZEM-004T Pin |
|---|---|
| GPIO 16 (RX2) | TX |
| GPIO 17 (TX2) | RX |
| GND | GND (communication reference) |

**UART Configuration:**
- Baud rate: 9600
- Data format: 8 data bits, no parity, 1 stop bit (8N1)
- Communication: UART serial interface

The serial connections are crossed: ESP32 RX connects to PZEM TX, and ESP32 TX connects to PZEM RX.

### CT-Type Meter Connections

The CT-type PZEM-004T measures current through an external current transformer.

- The CT is installed around the designated current-carrying conductor.
- The meter's voltage measurement terminals are connected according to the module's wiring diagram.
- The CT secondary connection must be connected to the designated PZEM input.

The CT must be installed and oriented according to the manufacturer's instructions. Never assume that the ESP32 supplies or replaces the meter's AC measurement connections.

## 4. Bus 1 – Resistive Load Monitoring

**Controller:** ESP32-1

Bus 1 uses a resistive load arrangement consisting of three bulb loads controlled through individual relay outputs.

### GPIO Configuration

| Component | ESP32 GPIO |
|---|---:|
| PZEM RX | 16 |
| PZEM TX | 17 |
| Relay 1 – Bulb 1 | 23 |
| Relay 2 – Bulb 2 | 22 |
| Relay 3 – Bulb 3 | 21 |

### Working

1. The resistive loads are connected to the bus.
2. The PZEM-004T measures voltage, current, active power, energy and power factor.
3. The ESP32 reads the measured parameters through UART.
4. The measurements are transmitted to Firebase through Wi-Fi.
5. The relay outputs control the three resistive bulb loads.

The bus primarily provides electrical parameter monitoring under resistive loading. It does not perform capacitor-based power factor correction.

## 5. Bus 2 – Resistive Load Monitoring

**Controller:** ESP32-2

Bus 2 is another resistive-load monitoring node.

### GPIO Configuration

| Component | ESP32 GPIO |
|---|---:|
| PZEM RX | 16 |
| PZEM TX | 17 |
| Relay 1 | 23 |
| Relay 2 | 22 |
| Relay 3 | 21 |
| Additional relay output (`RELAY_CB_PIN`) | 19 |

### Working

1. The resistive loads are connected to Bus 2.
2. The PZEM-004T measures the electrical parameters.
3. ESP32-2 collects the measurements through UART.
4. The readings are transmitted to Firebase using Wi-Fi.
5. The relay outputs provide load switching.

**Note:** GPIO 19 is configured as an additional relay output in the firmware. Its physical application is not yet confirmed and should be documented after verifying the actual wiring.

Bus 2 does not perform automatic power factor correction.

## 6. Bus 3 – RL Load and Automatic Power Factor Correction

**Controller:** ESP32-3

Bus 3 is the power factor correction unit. It consists of an RL load, a PZEM-004T meter, three relay-controlled capacitor stages and an ESP32 controller.

### GPIO Configuration

| Component | ESP32 GPIO |
|---|---:|
| PZEM RX | 16 |
| PZEM TX | 17 |
| Capacitor Stage 1 Relay | 23 |
| Capacitor Stage 2 Relay | 22 |
| Capacitor Stage 3 Relay | 21 |

### Capacitor Bank Configuration

| Capacitor Stage | Capacitance |
|---|---:|
| Stage 1 | 8 µF |
| Stage 2 | 8 µF |
| Stage 3 | 8 µF |
| Total configured capacitance | 24 µF |

The total configured capacitance is available when all three stages are enabled, assuming the stages are connected in parallel.

### Working

1. The RL load draws active and reactive power.
2. The PZEM-004T measures the electrical parameters, including power factor.
3. ESP32-3 processes the measured power factor.
4. When the power factor is below the configured target, capacitor stages are switched ON sequentially.
5. When sufficient compensation is achieved, unnecessary capacitor stages can be switched OFF.
6. The corrected electrical measurements and capacitor switching states are uploaded to Firebase.

### Automatic Control Parameters

| Parameter | Firmware Configuration |
|---|---:|
| Target power factor | 0.90 |
| Capacitor stage size | 8 µF |
| Number of stages | 3 |
| Relay switching interval | 2.5 seconds |
| PZEM reading interval | 1.5 seconds |
| Firebase upload interval | 4 seconds |

The firmware also includes low-load protection, sensor timeout handling and overvoltage protection.

## 7. Relay Module Operation

The relay modules are configured with active-LOW logic.

| ESP32 Output | Relay State |
|---|---|
| LOW | ON |
| HIGH | OFF |

The startup behaviour in the firmware is:

- **Bus 1:** Three relay outputs are commanded ON.
- **Bus 2:** Four relay outputs are commanded ON.
- **Bus 3:** All capacitor relay stages are initialized OFF.

The actual relay contact configuration and load connections must follow the physical relay module and electrical circuit design.

## 8. Wi-Fi and Firebase Integration

Each ESP32 independently connects to the Wi-Fi network and uploads the corresponding bus measurements to Firebase Realtime Database.

The overall communication structure is:

1. Bus 1 ESP32 → Wi-Fi → Firebase
2. Bus 2 ESP32 → Wi-Fi → Firebase
3. Bus 3 ESP32 → Wi-Fi → Firebase

Firebase provides centralized storage and monitoring of electrical measurements from all three buses.

## 9. Hardware Connection Summary

| Feature | Bus 1 | Bus 2 | Bus 3 |
|---|---|---|---|
| ESP32 | ESP32-1 | ESP32-2 | ESP32-3 |
| PZEM-004T | 1 | 1 | 1 |
| Load type | Resistive | Resistive | RL |
| Relay outputs in firmware | 3 | 4 | 3 |
| Capacitor bank | No | No | Yes |
| Automatic PF correction | No | No | Yes |
| Wi-Fi communication | Yes | Yes | Yes |
| Firebase monitoring | Yes | Yes | Yes |

## 10. Electrical Safety Considerations

- Use appropriately rated relay modules, contactors and switching devices.
- Provide suitable overcurrent protection for the loads and capacitor bank.
- Use capacitors specifically rated for the intended AC application and switching duty.
- Provide appropriate capacitor discharge arrangements.
- Ensure correct isolation and voltage compatibility between ESP32 communication pins and the PZEM interface.
- Perform all mains-voltage wiring and testing under qualified supervision.
- Never connect or disconnect a CT secondary while its primary conductor is energized unless the manufacturer's instructions explicitly permit the procedure.

---

**Project Domain:** Electrical Engineering | Power Systems | Power Factor Correction | Embedded Systems | IoT | Smart Microgrid
