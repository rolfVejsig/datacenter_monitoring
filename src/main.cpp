#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <MFRC522_I2C.h>
#include <Adafruit_NeoPixel.h>
#include <M5UnitENV.h>
#include <PubSubClient.h>

// Wi-Fi-indstillinger til det lokale netværk.
const char* WIFI_SSID = "ben_rolf_co_2.4GHz";
const char* WIFI_PASSWORD = "rolf_ben66";

// API-adressen til RFID-adgangskontrollen på Raspberry Pi.
const char* API_URL =
    "http://192.168.1.120:3000/clients/arrival";

// Navnet identificerer denne RFID-læser i API'et.
const char* CLIENT_NAME = "rfid_reader_1";

// MQTT-brokerens IP-adresse og port.
const char* MQTT_SERVER = "192.168.1.120";
const int MQTT_PORT = 1883;

// Topic, hvor M5GO modtager alarmkommandoer.
const char* MQTT_TOPIC = "office/alarm/rfid_reader_1";

// MQTT-klienten bruger Wi-Fi-forbindelsen.
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

// Holder styr på, om sikkerhedsalarmen er aktiv.
bool alarmActive = false;

// Holder styr på tidspunktet for den seneste alarmlyd.
unsigned long lastAlarmSound = 0;

// Intervallet mellem alarmlydene i millisekunder.
const unsigned long ALARM_SOUND_INTERVAL = 2000;

// Antal bip alarmen må afspille, før den tier stille.
const int ALARM_MAX_BEEPS = 4;

// Tæller hvor mange bip der er afspillet i den nuværende alarm.
int alarmBeepCount = 0;

// RFID-læserens I2C-adresse.
MFRC522_I2C rfid(0x28, -1);

// ENV III-sensoren måler temperatur og luftfugtighed.
SHT3X sht3x;
bool sensorAvailable = false;

float temperature = 0.0;
float humidity = 0.0;

// Gemmer tidspunktet for seneste sensoropdatering.
unsigned long lastSensorUpdate = 0;

// RFID-resultater, som vises på skærmen.
String lastUID = "Not scanned";
String accessMessage = "Scan your card";

bool accessGranted = false;
bool accessChecked = false;
bool accessExpired = false;

// Magnetkontakten er tilsluttet Grove Port B.
const int MAGNET_PIN = 36;

// LOW betyder, at magnetkontakten er aktiv.
// Kontrollér dette mod din konkrete kontakttype.
const int MAGNET_ACTIVE = LOW;

// Holder styr på dørens låse- og åbenstatus.
bool doorUnlocked = false;
bool doorOpen = false;

// Tidspunktet, hvor døren blev låst op.
unsigned long unlockTime = 0;

// Døren låses igen efter 10 sekunder,
// hvis den ikke er blevet åbnet.
const unsigned long UNLOCK_TIMEOUT = 10000;

// Debounce for at undgå gentagne signaler fra magnetkontakten.
const unsigned long DEBOUNCE_TIME = 50;

bool lastRawMagnetState = false;
bool stableMagnetState = false;
unsigned long lastMagnetChange = 0;

// Konfiguration af de 10 RGB-LED'er.
#define LED_PIN 15
#define NUM_LEDS 10

Adafruit_NeoPixel leds(
    NUM_LEDS,
    LED_PIN,
    NEO_GRB + NEO_KHZ800
);

// Tænder alle LED'er grønne.
void setGreen()
{
    // Grøn vises altid ved godkendt adgang — også selvom alarmen
    // lige har været aktiv, så skærmen ikke hænger på alarmvisning.
    for (int i = 0; i < NUM_LEDS; i++)
    {
        leds.setPixelColor(i, leds.Color(0, 255, 0));
    }

    leds.show();
}

// Tænder alle LED'er røde.
void setRed()
{
    for (int i = 0; i < NUM_LEDS; i++)
    {
        leds.setPixelColor(i, leds.Color(255, 0, 0));
    }

    leds.show();
}

// Slukker alle LED'er, medmindre alarmen er aktiv.
void setOff()
{
    if (alarmActive)
    {
        setRed();
        return;
    }

    leds.clear();
    leds.show();
}

// Afspiller lyd, når et RFID-kort godkendes.
void playAccessGrantedSound()
{
    // Godkendelseslyden skal altid afspilles, så brugeren får
    // bekræftelse på at kortet blev læst korrekt.
    M5.Speaker.setVolume(50);
    M5.Speaker.tone(1200, 150);
    delay(180);
    M5.Speaker.tone(1600, 200);
}

// Afspiller lyd, når et RFID-kort afvises.
void playAccessDeniedSound()
{
    // Sikkerhedsalarmen har prioritet over RFID-lyde.
    if (alarmActive)
    {
        return;
    }

    M5.Speaker.setVolume(80);
    M5.Speaker.tone(400, 250);
    delay(280);
    M5.Speaker.tone(300, 400);
}

// Viser RFID-status, dørstatus og sensordata.
void showMessage()
{
    M5.Display.clear();
    M5.Display.setCursor(10, 10);
    M5.Display.setTextSize(2);

    // Under alarm vises KUN alarmteksten, så der er plads på skærmen.
    if (alarmActive)
    {
        M5.Display.fillScreen(TFT_RED);
        M5.Display.setTextColor(TFT_WHITE, TFT_RED);
        M5.Display.setTextSize(3);
        M5.Display.setCursor(20, 40);
        M5.Display.println("SECURITY");
        M5.Display.setCursor(20, 80);
        M5.Display.println("ALARM!");
        M5.Display.setCursor(20, 140);
        M5.Display.println("ACTIVE");
        return;
    }

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

    M5.Display.println("DATACENTER");
    M5.Display.println("----------------");

    M5.Display.print("UID: ");
    M5.Display.println(lastUID);

    M5.Display.println();

    // Vis resultatet af den seneste RFID-scanning.
    if (accessChecked)
    {
        if (accessExpired)
        {
            M5.Display.println("ACCESS EXPIRED");
        }
        else if (accessGranted)
        {
            M5.Display.println("ACCESS GRANTED");
            M5.Display.println(accessMessage);
        }
        else
        {
            M5.Display.println("ACCESS DENIED");
            M5.Display.println(accessMessage);
        }
    }
    else
    {
        M5.Display.println(accessMessage);
    }

    M5.Display.println();
    M5.Display.println("----------------");

    // Vis dørens aktuelle tilstand.
    M5.Display.print("DOOR: ");

    if (doorOpen)
    {
        M5.Display.println("OPEN");
    }
    else if (doorUnlocked)
    {
        M5.Display.println("UNLOCKED");
    }
    else
    {
        M5.Display.println("LOCKED");
    }

    M5.Display.println();

    // Vis temperatur og luftfugtighed, hvis sensoren findes.
    if (sensorAvailable)
    {
        M5.Display.print("Temp: ");
        M5.Display.print(temperature, 1);
        M5.Display.println(" C");

        M5.Display.print("Humidity: ");
        M5.Display.print(humidity, 1);
        M5.Display.println(" %");
    }
    else
    {
        M5.Display.println("ENV III not found");
    }

    // Vis, hvis Wi-Fi ikke er forbundet.
    if (WiFi.status() != WL_CONNECTED)
    {
        M5.Display.println("Wi-Fi disconnected");
    }

    // Vis, hvis MQTT-forbindelsen mangler.
    if (!mqttClient.connected())
    {
        M5.Display.println("MQTT disconnected");
    }
}

// Opretter forbindelse til det lokale Wi-Fi-netværk.
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to Wi-Fi");

    unsigned long startTime = millis();

    // Vent højst 15 sekunder på Wi-Fi.
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < 15000)
    {
        delay(250);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("Wi-Fi connected. IP: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("Could not connect to Wi-Fi");
    }
}

// Behandler indgående MQTT-beskeder.
void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
)
{
    // Ignorer beskeder fra andre topics.
    if (String(topic) != MQTT_TOPIC)
    {
        return;
    }

    // Konverter MQTT-payload til en tekststreng.
    String message = "";

    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    message.trim();

    Serial.print("MQTT message received: ");
    Serial.println(message);

    // ALARM aktiverer rød belysning og op til 4 bip.
    if (message == "ALARM")
    {
        alarmActive = true;

        // Nulstil tælleren, så alarmen starter forfra.
        alarmBeepCount = 0;
        lastAlarmSound = 0;

        setRed();

        accessMessage = "SECURITY ALARM";
        showMessage();

        Serial.println("SECURITY ALARM ACTIVATED");
    }

    // CLEAR stopper alarmen og slukker LED'erne.
    else if (message == "CLEAR")
    {
        alarmActive = false;

        // Nulstil bip-tælleren, så næste alarm starter forfra.
        alarmBeepCount = 0;

        M5.Speaker.stop();
        setOff();

        accessMessage = "Scan your card";
        showMessage();

        Serial.println("SECURITY ALARM CLEARED");
    }
}

// Forsøger at forbinde til MQTT-brokeren.
void connectMQTT()
{
    // MQTT kræver en aktiv Wi-Fi-forbindelse.
    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    // Undgå at oprette forbindelse igen, hvis den allerede findes.
    if (mqttClient.connected())
    {
        return;
    }

    // Angiv MQTT-server og funktionen til indgående beskeder.
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    // Klient-ID skal være unikt for denne M5GO.
    String clientId = "m5go_" + String(CLIENT_NAME);

    Serial.println("Connecting to MQTT...");

    // Forbind til brokeren uden brugernavn og adgangskode.
    // Hvis din broker kræver login, skal dette ændres.
    if (mqttClient.connect(clientId.c_str()))
    {
        Serial.println("MQTT connected");

        // Abonnér på alarmbeskeder.
        if (mqttClient.subscribe(MQTT_TOPIC))
        {
            Serial.println("Subscribed to alarm topic");
        }
        else
        {
            Serial.println("MQTT subscription failed");
        }
    }
    else
    {
        Serial.print("MQTT connection failed. State: ");
        Serial.println(mqttClient.state());
    }
}

// Holder alarmen aktiv, indtil CLEAR modtages,
// men afspiller kun et begrænset antal bip.
void handleAlarm()
{
    if (!alarmActive)
    {
        return;
    }

    // Gendan rød farve, hvis andre funktioner har ændret LED'erne.
    setRed();

    // Afspil kun bip, indtil det maksimale antal er nået.
    if (alarmBeepCount < ALARM_MAX_BEEPS &&
        millis() - lastAlarmSound >= ALARM_SOUND_INTERVAL)
    {
        lastAlarmSound = millis();

        M5.Speaker.setVolume(35);
        M5.Speaker.tone(1200, 700);

        alarmBeepCount++;

        Serial.print("Alarm beep ");
        Serial.print(alarmBeepCount);
        Serial.print("/");
        Serial.println(ALARM_MAX_BEEPS);
    }
}

// Kontrollerer RFID-kortet via den eksisterende API.
void checkCard(String uid)
{
    accessChecked = true;
    accessGranted = false;
    accessExpired = false;

    if (WiFi.status() != WL_CONNECTED)
    {
        accessMessage = "Wi-Fi not connected";
        setRed();
        showMessage();
        return;
    }

    WiFiClient client;
    HTTPClient http;

    http.setTimeout(5000);

    if (!http.begin(client, API_URL))
    {
        accessMessage = "Could not start API";
        setRed();
        showMessage();
        return;
    }

    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<192> requestDoc;

    requestDoc["card_uid"] = uid;
    requestDoc["client_name"] = CLIENT_NAME;

    String requestBody;
    serializeJson(requestDoc, requestBody);

    Serial.println("Sending RFID data to API:");
    Serial.println(requestBody);

    int httpCode = http.POST(requestBody);

    if (httpCode <= 0)
    {
        accessMessage = "API not available";

        Serial.println(http.errorToString(httpCode));

        setRed();
        http.end();
        showMessage();
        return;
    }

    String response = http.getString();

    Serial.print("HTTP status: ");
    Serial.println(httpCode);
    Serial.println(response);

    StaticJsonDocument<384> responseDoc;

    DeserializationError jsonError =
        deserializeJson(responseDoc, response);

    if (jsonError ||
        httpCode < 200 ||
        httpCode >= 300)
    {
        accessMessage = "API or JSON error";
        setRed();

        http.end();
        showMessage();
        return;
    }

    accessGranted =
        responseDoc["access_granted"].as<bool>();

    if (accessGranted)
    {
        String firstName =
            responseDoc["first_name"] | "";

        String lastName =
            responseDoc["last_name"] | "";

        accessMessage = "Welcome " +
                        firstName + " " + lastName;

        // Et godkendt kort ophæver en eventuel aktiv alarm,
        // så skærmen går tilbage til velkomstvisning.
        if (alarmActive)
        {
            alarmActive = false;
            alarmBeepCount = 0;
            M5.Speaker.stop();
        }

        // Lås kun døren op, hvis den ikke allerede er åben.
        if (!doorOpen)
        {
            doorUnlocked = true;
            unlockTime = millis();
        }
        else
        {
            Serial.println("Door is already open");
        }

        setGreen();
        playAccessGrantedSound();

        Serial.println(accessMessage);

        showMessage();
    }
    else
    {
        // Brug fejlbeskeden fra API'et, hvis den findes.
        String errorMessage =
            responseDoc["error_message"] | "Access denied";

        accessMessage = errorMessage;

        setRed();
        playAccessDeniedSound();

        Serial.print("Access denied: ");
        Serial.println(accessMessage);

        showMessage();
    }

    http.end();
}

// Læser magnetkontakten og håndterer dørens tilstand.
void handleDoor()
{
    // Læs det aktuelle signal fra magnetkontakten.
    bool rawMagnetState =
        digitalRead(MAGNET_PIN) == MAGNET_ACTIVE;

    // Registrer, hvornår signalet ændres.
    if (rawMagnetState != lastRawMagnetState)
    {
        lastRawMagnetState = rawMagnetState;
        lastMagnetChange = millis();
    }

    // Accepter kun et signal, der er stabilt længe nok.
    if (millis() - lastMagnetChange >= DEBOUNCE_TIME)
    {
        if (stableMagnetState != rawMagnetState)
        {
            stableMagnetState = rawMagnetState;

            // Reager på overgangen til aktiv tilstand.
            if (stableMagnetState)
            {
                if (!doorUnlocked)
                {
                    Serial.println("Magnet detected - door is locked");
                    return;
                }

                if (!doorOpen)
                {
                    // Første aktivering åbner døren.
                    doorOpen = true;
                    accessMessage = "DOOR OPEN";

                    Serial.println("DOOR OPENED");
                }
                else
                {
                    // Næste aktivering lukker og låser døren.
                    doorOpen = false;
                    doorUnlocked = false;
                    accessExpired = false;

                    accessMessage = "DOOR CLOSED";

                    setOff();

                    Serial.println("DOOR CLOSED");
                    Serial.println("Door is now locked");
                }

                // Vis ikke den normale dørstatus i stedet for alarmen.
                if (alarmActive)
                {
                    accessMessage = "SECURITY ALARM";
                    setRed();
                }

                showMessage();
            }
        }
    }

    // Lås automatisk døren, hvis den ikke åbnes i tide.
    // En åben dør lukkes ikke automatisk af denne kode.
    if (doorUnlocked &&
        !doorOpen &&
        millis() - unlockTime >= UNLOCK_TIMEOUT)
    {
        doorUnlocked = false;
        accessExpired = true;
        accessMessage = "";

        setRed();

        showMessage();
    }
}

// Initialiserer M5GO, sensorer, Wi-Fi og MQTT.
void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);

    // Initialiser LED-strippen.
    pinMode(LED_PIN, OUTPUT_OPEN_DRAIN);

    leds.begin();
    leds.setBrightness(60);
    setOff();

    // Initialiser magnetkontakten på Grove Port B.
    pinMode(MAGNET_PIN, INPUT);

    lastRawMagnetState =
        digitalRead(MAGNET_PIN) == MAGNET_ACTIVE;

    stableMagnetState = lastRawMagnetState;
    lastMagnetChange = millis();

    Serial.println("Magnet contact started");

    // RFID og ENV III deler I2C-bussen.
    Wire.begin(21, 22);

    // Initialiser RFID-læseren.
    rfid.PCD_Init();

    // Forsøg at finde ENV III-sensoren.
    if (sht3x.begin(
            &Wire,
            SHT3X_I2C_ADDR,
            21,
            22,
            400000U))
    {
        sensorAvailable = true;
        Serial.println("ENV III found");
    }
    else
    {
        Serial.println("ENV III not found");
    }

    // Opret forbindelse til Wi-Fi.
    connectWiFi();

    // Konfigurer MQTT og forsøg at oprette forbindelse.
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(2);

    connectMQTT();

    Serial.println("RFID access control started");
    Serial.println("MQTT alarm receiver started");
    Serial.println("Door starts locked");

    showMessage();
}

// Hovedloopet holder MQTT, sensorer, RFID og dørfunktioner kørende.
void loop()
{
    // Opdater M5GO's interne funktioner.
    M5.update();

    // Genopret MQTT-forbindelsen, hvis den er faldet ud.
    // Forbindelsesforsøg foretages med mellemrum nedenfor.
    static unsigned long lastMqttAttempt = 0;

    if (WiFi.status() == WL_CONNECTED &&
        !mqttClient.connected() &&
        millis() - lastMqttAttempt >= 5000)
    {
        lastMqttAttempt = millis();
        connectMQTT();
    }

    // Behandl indgående MQTT-beskeder.
    if (mqttClient.connected())
    {
        mqttClient.loop();
    }

    // Håndter lyd og rød LED, hvis alarmen er aktiv.
    handleAlarm();

    // Håndter magnetkontakt og dør.
    handleDoor();

    // Opdater temperatur og luftfugtighed hvert minut.
    if (millis() - lastSensorUpdate >= 60000)
    {
        lastSensorUpdate = millis();

        if (sensorAvailable && sht3x.update())
        {
            temperature = sht3x.cTemp;
            humidity = sht3x.humidity;

            Serial.print("Temperature: ");
            Serial.println(temperature);

            Serial.print("Humidity: ");
            Serial.println(humidity);

            showMessage();
        }
    }

    // Kontroller, om et nyt RFID-kort er til stede.
    if (rfid.PICC_IsNewCardPresent() &&
        rfid.PICC_ReadCardSerial())
    {
        String uid = "";

        // Konverter kortets UID til en streng.
        for (byte i = 0; i < rfid.uid.size; i++)
        {
            if (rfid.uid.uidByte[i] < 0x10)
            {
                uid += "0";
            }

            uid += String(rfid.uid.uidByte[i], HEX);

            if (i < rfid.uid.size - 1)
            {
                uid += " ";
            }
        }

        uid.toUpperCase();
        lastUID = uid;

        Serial.print("RFID UID: ");
        Serial.println(lastUID);

        // Kontrollér kortet via API'et.
        checkCard(lastUID);

        // Afslut læsningen, så kortet kan læses igen senere.
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
    }

    // Kort pause uden at blokere MQTT i længere tid.
    delay(20);
}