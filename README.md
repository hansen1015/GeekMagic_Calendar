# GeekMagic SmallTV-Ultra Calendar 📅

A highly optimized Google Calendar display firmware for the **GeekMagic SmallTV-Ultra** (ESP8266 version). 

This project transforms the gadget into a desk companion that shows your next **8 upcoming events** with their original Google Calendar colors, along with a live clock and date. It includes a web dashboard for configuring settings and performing wireless updates.

> **🤖 AI Disclaimer:** The code for this project (both the C++ Firmware and the Google Apps Script backend) was fully written by **Google Gemini**, serving as an AI Pair Programmer to the user.

---

## ✨ Features

* **Smart Agenda:** Displays the next 8 events from your Google Calendar.
* **Color Accurate:** Matches your specific Google Calendar event colors (Peacock, Tomato, Sage, etc.) using a custom palette map.
* **Memory Optimized:** Uses a "Stream -> Close -> Parse" pipeline to handle HTTPS requests on the memory-limited ESP8266.
* **Web Dashboard:** Host a local website on the device (`http://<IP>`) to:
    * Adjust **Brightness** (with cubic gamma correction for smooth dimming).
    * Set **Timezone** offset.
    * Perform **Native OTA** firmware updates.
* **Hardware Fixes:** Includes specific patches for the SmallTV-Ultra's quirks (Active-Low Backlight, BGR Screen Color Swap).

## 🛠️ Hardware

* **Device:** GeekMagic SmallTV-Ultra (ESP8266 / ESP-12F)
* **Screen:** 1.54" IPS TFT (ST7789 Driver)

## 🚀 Installation

This project requires two parts: the **Backend** (Google) and the **Frontend** (Device).

### Part 1: Google Apps Script (The Backend)
The ESP8266 cannot handle the full Google API OAuth flow. We use a lightweight "Middleware" script.

1.  Go to [script.google.com](https://script.google.com/) and create a new project.
2.  Paste the code from `GoogleScript.js` (found in this repo).
3.  Click **Deploy > New Deployment**.
4.  Select type: **Web App**.
5.  Set "Who has access" to: **Anyone**.
6.  **Copy the Deployment URL.** (You will need this ID for the firmware).

### Part 2: Firmware (The Device)
1.  Open this project in **VS Code** with **PlatformIO**.
2.  Open `src/main.cpp`.
3.  Find line: `const char* GOOGLE_SCRIPT_URL = "..."`
4.  Paste your Google Web App URL there.
5.  Connect your device via USB.
6.  Click **PlatformIO: Upload**.

## ⚙️ Usage

1.  On first boot, the screen will turn **RED** and show a WiFi Hotspot named `GeekMagic_Setup`.
2.  Connect to it with your phone/PC and enter your home WiFi credentials.
3.  The device will reboot and show the clock.
4.  **To change settings:** Find the device's IP address (e.g., `192.168.1.122`) and open it in a web browser.

## 🐛 Technical Notes & "Hacks"
* **Backlight:** This device uses an "Active Low" backlight circuit. The firmware maps 0% slider to `PWM 240` and 100% slider to `PWM 0`.
* **Color Space:** The specific ST7789 panel used in these units is wired as BGR. The firmware forces `TFT_RGB_ORDER=TFT_BGR` to correct blue/red swaps.
* **Memory:** The `ArduinoJson` buffer is set to 5KB. The firmware strictly manages the WiFiClientSecure lifecycle to prevent Heap Fragmentation crashes during SSL handshakes.

## 📜 License
MIT License