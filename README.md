### SECTIONS:

#### 1. [INTRODUCTION](#Introduction)
#### 2. [TOOLS](#Tools)
#### 3. [SCREENSHOTS](#Screenshots)
#### 4. [SCRIPT](#Script)
#### 5. [READING](#Reading)
#### 6. [CREDITS](#Credits)







#### <a name="Introduction"><a/>Introduction

VaGate BA100 is a BLE device that sends to a smartphone app hexadecimal data that app converts to Volts values. So you cannot read directly volts values 
connecting another device or  a smartphone without the Battery Assitant App.


#### <a name="Tools"><a/>Tools

If you scan with nRF connect mobile app, you can connect to your devices and have a lot of infos about. When you scan the area where there is a Vgate BA100, you should find "BATTERY ASST" BLE device. In this project the nRF app told me the essential data coming from the Vgate BA100 device: static BLE MAC Address (00:00:00:C3:72:68, in my case); service UUID (0000ae00-0000-1000-8000-00805f9b34fb in my case); char UUID (0000ae02-0000-1000-8000-00805f9b34fb); and, overall, the HEX data that it sends to the smatrphone app ( i.e. AA-02-FA-55 ).   To find out how these Hexadecimals symbols work, I've connected the Vgate BA100 to a high precision analogic regulate DC power supply, double checking voltages with a digital multimeter, to simulate voltage variation around the car battery range.




