# NewentorReceiverMQTT
ESP8266 project aimed to receive the RF 433MHz signal from external sensors of the [Newentor Q5 weather station](https://www.amazon.com/Newentor-Wireless-Multiple-Thermometer-Backlight/dp/B085R9KBN1/) and send values to MQTT broker.
This is the PlatformIO project but code base is Arduino compatible.
## Hardware
[Newentor Q5 weather station on AliExpress](https://www.aliexpress.com/item/1005002533165074.html)
- ESP8266 Wemos D1 mini or compatible
- RF-433MHz receiver. Data out connected to D1 input of ESP8266
- Button connected to D2 input of ESP8266 for settings reset during boot and forcing the Autoconnect configuration interface
## RF Protocol
#### Sensors message format is 40 bit with ASK+PWM modulation:
- Preamble is 3-4 short impulses followed by long pause of about 8ms, then 40 bits are transmitted
- Message is repeated 6 times
- Bit transmission is complete on rising edge by measuring the pause between pulses
- Pulses between bits are around 650 microseconds
- Bit 0 length is around 1800 microseconds
- Bit 1 length is around 4000 microseconds
#### Message contains:
    Address | -?- |B|??|Temperature |Humidity|??|Ch
    00011101 01111 0 01 011000100111 00111000 00 01
- Random address of 8 bits which is renewed on battery replacement.
- Channel (Ch), 2 bits
- 12 bits of temperature are coded in Fahrenheit degrees multiplied by 10 and added constant of 900 to eliminate negative values
- Humidity is encoded as two 4-bit BCD digits.
- Battery status (B) is 1 bit, meaning if battery needs replacement or not
- There are 9 bits that are still unknown, can be either rolling code or some sort of weird checksum or reserved for future use. I couldn't decode that, so any help would be greatly appreciated!
## Software features
- WiFiManager - the library that allows configuration of Wifi and other settings on first start or if settings reset button was pressed during power-on
- mDNS - allows the sensor to appear in your network as web server with name set in Autoconnect interface
- Web server interface that allows re-configuration of parameters
- ArduinoOTA - allows code re-flashing over Wi-Fi
- PubSubClient - sends the received sensor data to configured MQTT broker
- Messages are filtered to prevent repeating 6 times, only one message is sent if it is the same message as before