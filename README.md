# Ghost ESP: Revival


> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP) which has been archived and not in development anymore.



**⭐️ Enjoying Ghost ESP? Please give the repo a star!**





Ghost ESP turns your ESP32 into a powerful, cheap and helpful wireless testing tool. Built on ESP-IDF.






---





## Getting Started





1. Flash your device at https://flasher.ghostesp.net


2. Join our **NEW** community on [Discord](https://discord.gg/4svN9aPH) for support and feedback.


3. Visit our [Official Website](https://ghostesp.net) to stay in touch!





---


## Key Features





<details>


<summary>WiFi Features</summary>





- **AP Scanning** – Detect nearby WiFi networks.


- **Station Scanning** – Monitor connected WiFi clients.


- **Combined AP/Station Scan** – Perform both AP and station scans in one command (`scanall`).


- **IP Lookup** – Retrieve local network IP information (`scanlocal`).


- **Beacon Spam** – Broadcast customizable SSID beacons.


- **Beacon Spam List Management** – Manage SSID lists (`beaconadd`, `beaconremove`, `beaconclear`, `beaconshow`) and spam them (`beaconspamlist`).


- **Deauthentication Attacks** – Disconnect clients from specific networks.


- **DHCP Starvation** – Flood DHCP requests to exhaust network leases (`dhcpstarve`).


- **WiFi Capture** – Log probe requests, beacon frames, deauth packets, and raw data *(requires SD card or compatible storage)*.


- **Evil Portal** – Set up a fake WiFi portal with a custom SSID and domain.


- **Pineapple Detection** – Detect Wi-Fi Pineapples and Evil Twin Attacks.


- **Web-UI** – Built-in interface for changing settings and sending commands easily.


- **Port Scanning** – Scan your local network for open ports.





</details>





<details>


<summary>BLE Features</summary>





- **BLE Scanning** – Detect BLE devices, including specialized modes for AirTags, Flipper Zeros, and more.


- **BLE Packet Capture** – Capture and analyze BLE traffic.


- **BLE Wardriving** – Map and track BLE devices in your vicinity.


- **Flipper Zero RSSI Tracking** – Detect and monitor the signal strength (RSSI) of Flipper Zero devices (`blescan -f`).


- **AirTag Spoofing** – Spoof the identity of a selected AirTag device (`spoofairtag`).





</details>





<details>


<summary>Additional Features</summary>





- **GPS Integration** – Retrieve location info via the `gpsinfo` command *(on supported hardware)*.


- **RGB LED Modes** – Customizable LED feedback (Stealth, Normal, Rainbow).


- **DIAL & Chromecast V2 Support** – Interact with DIAL-capable devices (e.g., Roku, Chromecast).


- **Flappy Ghost and Rave Modes** – Extra apps for boards with displays.


- **Network Printer Output** – Print custom text to a LAN printer (`powerprinter`).


- **Timezone Configuration** – Change system timezone string (`timezone`).





</details>





> **Note:** BLE Spam is **NOT** supported at this time.





---





## Supported ESP32 Models





- **ESP32 Wroom**


- **ESP32 S2**


- **ESP32 C3**


- **ESP32 S3**


- **ESP32 C5**


- **ESP32 C6**





> **Note:** Feature availability may vary by model.





---





## Supported Boards





<details>


<summary>Supported Boards</summary>





- DevKitC-ESP32


- DevKitC-ESP32-S2


- DevKitC-ESP32-C3


- DevKitC-ESP32-S3


- DevKitC-ESP32-C5


- DevKitC-ESP32-C6


- RabbitLabs GhostBoard


- AWOK Mini


- M5 Cardputer


- FlipperHub Rocket


- FlipperHub Pocker Marauder


- RabbitLabs Phantom

- RabbitLabs Yapper Board (GPS NOT SUPPORTED AT THIS TIME)


- Waveshare 7″ Touch


- 'CYD2 USB'


- 'CYD2 USB 2.4″'


- 'CYD2 USB 2.4″ (C Variant)'


- 'CYD Micro USB'


- 'CYD Dual USB'


- 'S3 T-Watch'


- Marauder V4


- Marauder V6




</details>





---





## Acknowledgments





Special thanks to:





- **[JustCallMeKoKo](https://github.com/justcallmekoko/ESP32Marauder):** For foundational ESP32 development.


- **[thibauts](https://github.com/thibauts/node-castv2-client):** For CastV2 protocol insights.


- **[MarcoLucidi01](https://github.com/MarcoLucidi01/ytcast/tree/master/dial):** For DIAL protocol integration.


- **[SpacehuhnTech](https://github.com/SpacehuhnTech/esp8266_deauther):** For reference deauthentication code.





---





## Legal Disclaimer





Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Always obtain proper permissions before conducting any network tests.





---





## Open Source Contributions





This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
