# IoT-Based Power Factor Monitoring and Automatic Compensation in a Smart Microgrid

## 1. Project Overview

This project focuses on monitoring electrical parameters and improving power factor in a smart microgrid using ESP32-based IoT technology.

The system uses three monitoring and control buses to measure electrical parameters, monitor connected loads, and implement relay-based capacitor bank switching for reactive power compensation.

## 2. Objectives

- Monitor voltage, current, active power, energy and power factor.
- Monitor electrical loads across three buses.
- Implement automatic power factor correction using capacitor banks.
- Enable wireless data communication using ESP32.
- Store and visualize electrical measurements using Firebase.
- Improve energy efficiency through reactive power compensation.

## 3. System Architecture

The proposed system consists of three buses:

### Bus 1 – Electrical Parameter Monitoring
- PZEM-004T energy meter.
- ESP32-based data acquisition.
- Measurement of electrical parameters.

### Bus 2 – Load Monitoring and Control
- Relay-based load control.
- Monitoring of connected electrical loads.
- Electrical parameter measurement using PZEM-004T.

### Bus 3 – Power Factor Compensation
- Monitoring of power factor.
- Relay-controlled capacitor bank switching.
- Reactive power compensation for inductive loads.

![Three-Bus Smart Microgrid Architecture](<IoT Smart Microgrid Power Monitoring Infographic.png>)

## 4. Hardware Components

| Component | Application |
|---|---|
| ESP32 | Microcontroller and Wi-Fi communication |
| PZEM-004T | Electrical parameter measurement |
| Relay Modules | Load and capacitor bank switching |
| Capacitor Banks | Reactive power compensation |
| Inductive Loads | Test loads |
| Power Supply | System power |
 
## 5. Software and Technologies

- Embedded C / Arduino IDE
- ESP32
- UART Communication
- Wi-Fi
- Firebase Realtime Database

## 6. Working Principle

1. PZEM-004T meters measure electrical parameters.
2. ESP32 collects the measurements through UART communication.
3. Electrical loads are monitored and controlled through relay modules.
4. The controller evaluates the power factor.
5. Capacitor banks are switched to compensate for lagging reactive power.
6. Measurement data is transmitted to Firebase for monitoring.

## 7. Expected Outcomes

- Real-time electrical parameter monitoring.
- Improved power factor under inductive loading.
- Automated capacitor bank switching.
- Centralized monitoring through Firebase.
- A scalable IoT-based smart microgrid monitoring approach.

## 8. Future Scope

- Advanced reactive power compensation algorithms.
- Three-phase power monitoring.
- Improved switching coordination.
- Cloud-based analytics and visualization.
- Integration of predictive energy management.

## 9. Project Domain

Electrical Engineering | Power Systems | Power Factor Correction | Embedded Systems | IoT | Smart Grid

## 10. Authors

- [Bhagyashree Muthukumar](https://github.com/bhagyashree-muthukumar)
- [Ponabirami A](https://github.com/Ponabirami1718)
- Deepana S
