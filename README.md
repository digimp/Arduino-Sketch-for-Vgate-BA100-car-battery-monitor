### SECTIONS:

#### 1. [INTRODUCTION](#Introduction)
#### 2. [TOOLS](#Tools)
#### 3. [SCRIPTS](#Scripts)
#### 4. 
#### 5. [READING](#Reading)
#### 6. [CREDITS](#Credits)







#### <a name="Introduction"><a/>Introduction

Vgate BA100 is a BLE device that sends to a smartphone app hexadecimal data that app converts to Volts values. So you cannot read directly volts values 
connecting another device or  a smartphone without the Battery Assistant App.


#### <a name="Tools"><a/>Tools

If you scan with nRF connect mobile app, you can connect to your devices and have a lot of infos about. When you scan the area where there is a Vgate BA100, you should find "BATTERY ASST" BLE device. In this project the nRF app told me the essential data coming from the Vgate BA100 device: static BLE MAC Address (00:00:00:C3:72:68, in my case); service UUID (0000ae00-0000-1000-8000-00805f9b34fb in my case); char UUID (0000ae02-0000-1000-8000-00805f9b34fb); and, overall, the HEX data that it sends to the smartphone app ( i.e. AA-02-FA-55 ).   To find out how these Hexadecimals symbols work, I've connected the Vgate BA100 to a high precision analogic regulated DC power supply, double checking voltages with a digital multimeter, to simulate voltage variation around the car battery range.

<img src="https://github.com/user-attachments/assets/c0941414-51b3-4ca5-aa3e-4d3abc8c3079" width="150">

<img src="https://github.com/user-attachments/assets/d5e63a03-23bd-42ca-a1c9-dd517a3087be" width="200">

<img src="https://github.com/user-attachments/assets/fef98313-77f4-4b27-91f6-a5f392da9693" width="150">

With these tools I've found out that changing voltage values of the power supply the hexadecimal bytes sensitive to voltage variations were the second and third. So I therefore built a spreadsheet with many tested values, integrated with other interpolated ones. This shown here is only a sample of the spreadsheet with 475 vaules from 8,5V to 16V.

<img src="https://github.com/user-attachments/assets/6bf5a3c2-70aa-4bde-81f1-664015969fee" width="200">


#### <a name="Scripts"><a/>Scripts







