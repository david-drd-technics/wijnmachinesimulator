# WijnmachineSimulator

Java Swing-bedieningspaneel voor een **Arduino Mega 2560** die via USB/COM communiceert. De Arduino voert uitsluitend directe, manueel verstuurde opdrachten uit. Er is **geen automatische wijncyclus**.

## Downloaden

Klik in GitHub op **Code → Download ZIP**, of gebruik:
https://github.com/david-drd-technics/wijnmachinesimulator/archive/refs/heads/main.zip

## Starten op Windows

1. Installeer **JDK 17+**.
2. Pak de volledige ZIP uit.
3. Upload de firmware uit \`arduino/WijnmachineMega_Manueel/WijnmachineMega_Manueel.ino\` met Arduino IDE (vereist bibliotheek MFRC522).
4. Sluit de Arduino Serial Monitor.
5. Dubbelklik **start-windows.bat**.
6. Kies de juiste COM-poort en klik **Verbinden** (9600 baud).

Het startscript maakt de map \`lib\` zelf aan en downloadt jSerialComm 2.11.2 van Maven Central. Een internetverbinding is bij de eerste start nodig. Linux: \`bash start-linux.sh\`.

## Onderdelen

- **Motor 1:** transportband (STEP 12, DIR 11, EN 10, HOME 4)
- **Motor 2:** deur (STEP 3, DIR 7, EN 13, HOME 6)
- **Motor 3:** glaslift (STEP 9, DIR 2, EN 8, HOME 5)
- **15 flessen:** resetcontacten 22-36, counters in EEPROM
- **15 uitgangen:** pinnen 54-68 (Mega analoge pinnen A0-A14)
- **3 knoppen:** pinnen 44, 46, 48
- **RFID:** MFRC522 SPI

De Java-interface heeft een schematische grafische weergave, bediening van alle motoren met stappen/interval, resetknoppen, uitgangen, RFID, virtuele drukknoppen en een TX/RX-monitor met CSV-export.

## Seriële commando's

Elke opdracht eindigt op een newline (\`\\n\`):

\`\`\`text
GET_STATUS
GEEFFLESINHOUD
SIM_MODE_ON
SIM_MODE_OFF
STOP_ALL
MOTOR_1_MOVE_FWD_200_1200
MOTOR_1_MOVE_REV_200_1200
MOTOR_2_HOME
MOTOR_3_STOP
BELT_FWD_100_1200
DOOR_OPEN_100_1200
LIFT_UP_100_2400
RESET_COUNTER_4
SET_COUNTER_4_7
OUTPUT_7_ON
OUTPUT_7_OFF
SIM_BUTTON_1_PRESS
SIM_BUTTON_1_RELEASE
SIM_RESET_2_PRESS
SIM_RESET_2_RELEASE
SIM_HOME_1_ON
SIM_HOME_1_OFF
SIM_CARD_A1:B2:C3:D4
\`\`\`

Arduino-antwoorden zijn JSON-regels, zichtbaar als RX in het logvenster. Uitgaande opdrachten verschijnen als TX.

## Veiligheid

- Test eerst in **simulatiemodus**. Bij ingeschakelde simulatie worden motorstappen virtueel geteld; fysieke uitgangen worden niet geschakeld.
- Bij **SIM_MODE_OFF** kunnen seriële opdrachten de **echte motoren en uitgangen** activeren.
- FWD/REV en OPEN/CLOSE zijn elektrische richtingen, **niet gekalibreerde mechanische posities**. Controleer de richting en maximale verplaatsing met zeer kleine stapwaarden.
- De voorstelling toont geen echte motorpositie. Er is geen algemene interlock voor deuren, lift, flessen, eindschakelaars of personen.
- Een **hardwarematige noodstop** en mechanische beveiliging blijven noodzakelijk.
- De software is nog niet getest tegen de fysieke installatie.

## Projectstructuur

\`\`\`text
arduino/WijnmachineMega_Manueel/WijnmachineMega_Manueel.ino
src/be/technodream/wijnmachine/WijnmachineSimulator.java
start-windows.bat
start-linux.sh
README.md
\`\`\`
