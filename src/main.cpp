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
const char* API_URL ="http://192.168.1.120:3000/clients/arrival";

// Basis-URL til client-endpoints.
const char* API_BASE = "http://192.168.1.120:3000/clients";

// Navnet identificerer denne RFID-læser i API'et.
const char* CLIENT_NAME = "rfid_reader_1";

// MQTT-brokerens IP-adresse og port.
const char* MQTT_SERVER = "192.168.1.120";
const int MQTT_PORT = 1883;

// Topic, hvor M5GO både sender og modtager alarm.
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

// Grænser hentet fra API'et.
float temperatureLow = 18.0;
float temperatureHigh = 21.0;
float humidityLow = 30.0;
float humidityHigh = 50.0;

// Måleinterval i minutter hentet fra API'et.
unsigned long databaseIntervalMinutes = 10;

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

// Hvor mange minutter døren må stå åben — hentes fra API.
unsigned long doorOpenAlarmMinutes = 3;

// Tidspunkt hvor døren blev åbnet (0 = ikke åben).
unsigned long doorOpenSince = 0;

// Har vi allerede sendt alarm for denne åbning?
bool doorAlarmSent = false;

// Viser grøn LED midlertidigt efter godkendt adgang.
bool accessFeedbackActive = false;
unsigned long accessFeedbackStart = 0;

// Hvor længe grøn vises efter godkendt adgang (ms).
const unsigned long ACCESS_FEEDBACK_MS = 3000;

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

// Returnerer "OK", "LOW" eller "HIGH" for en værdi.
String classifyValue(float value, float low, float high)
{
    if (value < low)
    {
        return "LOW";
    }

    if (value > high)
    {
        return "HIGH";
    }

    return "OK";
}

// Tjekker om både temperatur og luftfugtighed er inden for grænserne.
bool valuesAreWithinLimits()
{
    // Hvis sensoren ikke findes, kan vi ikke vurdere noget — vis grøn.
    if (!sensorAvailable)
    {
        return true;
    }

    return temperature >= temperatureLow &&
           temperature <= temperatureHigh &&
           humidity >= humidityLow &&
           humidity <= humidityHigh;
}

// Viser grøn LED i et par sekunder efter godkendt adgang.
void showAccessGrantedFeedback()
{
    accessFeedbackActive = true;
    accessFeedbackStart = millis();

    setGreen();
}

// Opdaterer LED-farven ud fra om værdierne er inden for grænserne.
void updateLedFromValues()
{
    if (alarmActive)
    {
        setRed();
        return;
    }

    // Grøn adgangsfeedback har prioritet i et par sekunder.
    if (accessFeedbackActive)
    {
        if (millis() - accessFeedbackStart < ACCESS_FEEDBACK_MS)
        {
            setGreen();
            return;
        }

        accessFeedbackActive = false;
    }

    if (valuesAreWithinLimits())
    {
        setGreen();
    }
    else
    {
        setRed();
    }
}

// Afspiller lyd, når et RFID-kort godkendes.
void playAccessGrantedSound()
{
    M5.Speaker.setVolume(50);
    M5.Speaker.tone(1200, 150);
    delay(180);
    M5.Speaker.tone(1600, 200);
}

// Afspiller lyd, når et RFID-kort afvises.
void playAccessDeniedSound()
{
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

    // Under alarm vises KUN alarmteksten.
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

    // Vis temperatur og luftfugtighed med OK/LOW/HIGH.
    if (sensorAvailable)
    {
        String tempState = classifyValue(
            temperature,
            temperatureLow,
            temperatureHigh
        );

        String humState = classifyValue(
            humidity,
            humidityLow,
            humidityHigh
        );

        M5.Display.print("Temp: ");
        M5.Display.print(temperature, 1);
        M5.Display.print(" C ");
        M5.Display.println(tempState);

        M5.Display.print("Hum:  ");
        M5.Display.print(humidity, 1);
        M5.Display.print(" % ");
        M5.Display.println(humState);
    }
    else
    {
        M5.Display.println("ENV III not found");
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        M5.Display.println("Wi-Fi disconnected");
    }

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
    if (String(topic) != MQTT_TOPIC)
    {
        return;
    }

    String message = "";

    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    message.trim();

    Serial.print("MQTT message received: ");
    Serial.println(message);

    if (message == "ALARM")
    {
        alarmActive = true;

        alarmBeepCount = 0;
        lastAlarmSound = 0;

        setRed();

        accessMessage = "SECURITY ALARM";
        showMessage();

        Serial.println("SECURITY ALARM ACTIVATED");
    }
    else if (message == "CLEAR")
    {
        alarmActive = false;
        alarmBeepCount = 0;

        M5.Speaker.stop();

        accessMessage = "Scan your card";

        // LED skal nu afspejle sensordata, ikke alarmen.
        updateLedFromValues();
        showMessage();

        Serial.println("SECURITY ALARM CLEARED");
    }
}

// Forsøger at forbinde til MQTT-brokeren.
void connectMQTT()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    if (mqttClient.connected())
    {
        return;
    }

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    String clientId = "m5go_" + String(CLIENT_NAME);

    Serial.println("Connecting to MQTT...");

    if (mqttClient.connect(clientId.c_str()))
    {
        Serial.println("MQTT connected");

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

// Holder alarmen aktiv, indtil CLEAR modtages.
void handleAlarm()
{
    if (!alarmActive)
    {
        return;
    }

    setRed();

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

// Henter client_id for denne enhed fra API'et.
int fetchClientId()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Wi-Fi not connected - cannot fetch client_id");
        return -1;
    }

    WiFiClient client;
    HTTPClient http;

    http.setTimeout(5000);

    String idUrl = String(API_BASE) +
                   "/get_client_id/" +
                   CLIENT_NAME;

    if (!http.begin(client, idUrl))
    {
        Serial.println("Could not reach get_client_id");
        return -1;
    }

    int code = http.GET();

    if (code != 200)
    {
        Serial.print("get_client_id failed: ");
        Serial.println(code);
        http.end();
        return -1;
    }

    String body = http.getString();
    http.end();

    StaticJsonDocument<128> idDoc;

    if (deserializeJson(idDoc, body))
    {
        Serial.println("get_client_id JSON error");
        return -1;
    }

    int clientId = idDoc["client_id"] | 0;

    if (clientId <= 0)
    {
        Serial.println("Invalid client_id");
        return -1;
    }

    return clientId;
}

// Henter alle sensor-settings og opdaterer de globale variabler.
void loadSensorSettings()
{
    int clientId = fetchClientId();

    if (clientId <= 0)
    {
        return;
    }

    WiFiClient client;
    HTTPClient http;

    http.setTimeout(5000);

    String settingsUrl = String(API_BASE) +
                         "/settings/" +
                         String(clientId);

    if (!http.begin(client, settingsUrl))
    {
        Serial.println("Could not reach settings");
        return;
    }

    int code = http.GET();

    if (code != 200)
    {
        Serial.print("settings failed: ");
        Serial.println(code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    StaticJsonDocument<512> arr;

    if (deserializeJson(arr, body))
    {
        Serial.println("settings JSON error");
        return;
    }

    JsonObject settings = arr[0];

    // Cast eksplicit til float, fordi API'et returnerer strenge.
    temperatureLow = settings["temperature_low"].as<float>();
    temperatureHigh = settings["temperature_high"].as<float>();
    humidityLow = settings["humidity_low"].as<float>();
    humidityHigh = settings["humidity_high"].as<float>();

    // Fallback hvis feltet mangler eller er 0.
    if (temperatureLow == 0.0f && temperatureHigh == 0.0f)
    {
        temperatureLow = 18.0;
        temperatureHigh = 21.0;
    }

    if (humidityLow == 0.0f && humidityHigh == 0.0f)
    {
        humidityLow = 30.0;
        humidityHigh = 50.0;
    }

    // Måleinterval i minutter.
    int interval = settings["database_interval"] | 10;

    if (interval >= 1 && interval <= 1440)
    {
        databaseIntervalMinutes = (unsigned long)interval;
    }

    // Dør-alarm-grænse.
    int doorMinutes = settings["door_open_alarm_minutes"] | 3;

    if (doorMinutes >= 1 && doorMinutes <= 60)
    {
        doorOpenAlarmMinutes = (unsigned long)doorMinutes;
    }

    Serial.println("Settings loaded:");
    Serial.print("  temperature_low:   ");
    Serial.println(temperatureLow);
    Serial.print("  temperature_high:  ");
    Serial.println(temperatureHigh);
    Serial.print("  humidity_low:      ");
    Serial.println(humidityLow);
    Serial.print("  humidity_high:     ");
    Serial.println(humidityHigh);
    Serial.print("  database_interval: ");
    Serial.println(databaseIntervalMinutes);
    Serial.print("  door_alarm_min:    ");
    Serial.println(doorOpenAlarmMinutes);
}

// Kontrollerer RFID-kortet via API.
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
                        firstName + " " +
                        lastName;

        if (alarmActive)
        {
            alarmActive = false;
            alarmBeepCount = 0;
            M5.Speaker.stop();
        }

        if (!doorOpen)
        {
            doorUnlocked = true;
            unlockTime = millis();
            doorAlarmSent = false;
        }
        else
        {
            Serial.println("Door is already open");
        }

        // Vis grøn feedback i et par sekunder.
        showAccessGrantedFeedback();
        playAccessGrantedSound();

        Serial.println(accessMessage);

        showMessage();
    }
    else
    {
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
    bool rawMagnetState =
        digitalRead(MAGNET_PIN) == MAGNET_ACTIVE;

    if (rawMagnetState != lastRawMagnetState)
    {
        lastRawMagnetState = rawMagnetState;
        lastMagnetChange = millis();
    }

    if (millis() - lastMagnetChange >= DEBOUNCE_TIME)
    {
        if (stableMagnetState != rawMagnetState)
        {
            stableMagnetState = rawMagnetState;

            if (stableMagnetState)
            {
                if (!doorUnlocked)
                {
                    Serial.println("Magnet detected - door is locked");
                    return;
                }

                if (!doorOpen)
                {
                    doorOpen = true;
                    accessMessage = "DOOR OPEN";

                    doorOpenSince = millis();
                    doorAlarmSent = false;

                    Serial.println("DOOR OPENED");
                }
                else
                {
                    doorOpen = false;
                    doorUnlocked = false;
                    accessExpired = false;

                    doorOpenSince = 0;
                    doorAlarmSent = false;

                    accessMessage = "DOOR CLOSED";

                    updateLedFromValues();

                    Serial.println("DOOR CLOSED");
                    Serial.println("Door is now locked");
                }

                if (alarmActive)
                {
                    accessMessage = "SECURITY ALARM";
                    setRed();
                }

                showMessage();
            }
        }
    }

    if (doorUnlocked &&
        !doorOpen &&
        millis() - unlockTime >= UNLOCK_TIMEOUT)
    {
        doorUnlocked = false;
        accessExpired = true;
        accessMessage = "";

        updateLedFromValues();

        showMessage();
    }

    if (doorOpen && doorOpenSince > 0 && !doorAlarmSent)
    {
        unsigned long openMs = millis() - doorOpenSince;
        unsigned long limitMs =
            doorOpenAlarmMinutes * 60UL * 1000UL;

        if (openMs >= limitMs)
        {
            doorAlarmSent = true;

            Serial.println(
                "Door open too long - publishing ALARM"
            );

            if (mqttClient.connected())
            {
                mqttClient.publish(
                    MQTT_TOPIC,
                    "ALARM",
                    false
                );
            }
            else
            {
                Serial.println(
                    "MQTT not connected - alarm not sent"
                );
            }
        }
    }
}

// Læser sensoren og opdaterer display og LED.
void updateSensor()
{
    if (!sensorAvailable)
    {
        return;
    }

    if (!sht3x.update())
    {
        Serial.println("Sensor update failed");
        return;
    }

    temperature = sht3x.cTemp;
    humidity = sht3x.humidity;

    Serial.print("Temperature: ");
    Serial.println(temperature);

    Serial.print("Humidity: ");
    Serial.println(humidity);

    updateLedFromValues();
    showMessage();
}

// Initialiserer M5GO, sensorer, Wi-Fi og MQTT.
void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT_OPEN_DRAIN);

    leds.begin();
    leds.setBrightness(60);
    setOff();

    pinMode(MAGNET_PIN, INPUT);

    lastRawMagnetState =
        digitalRead(MAGNET_PIN) == MAGNET_ACTIVE;

    stableMagnetState = lastRawMagnetState;
    lastMagnetChange = millis();

    Serial.println("Magnet contact started");

    Wire.begin(21, 22);

    rfid.PCD_Init();

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

    connectWiFi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(2);

    connectMQTT();

    // Hent alle grænser og måleinterval fra API.
    loadSensorSettings();

    // Tag den første måling med det samme.
    updateSensor();
    lastSensorUpdate = millis();

    Serial.println("RFID access control started");
    Serial.println("MQTT alarm receiver started");
    Serial.println("Door starts locked");

    showMessage();
}

// Hovedloopet holder MQTT, sensorer, RFID og dørfunktioner kørende.
void loop()
{
    M5.update();

    static unsigned long lastMqttAttempt = 0;

    if (WiFi.status() == WL_CONNECTED &&
        !mqttClient.connected() &&
        millis() - lastMqttAttempt >= 5000)
    {
        lastMqttAttempt = millis();
        connectMQTT();
    }

    if (mqttClient.connected())
    {
        mqttClient.loop();
    }

    handleAlarm();

    // Falder tilbage til sensorstatus når feedback er udløbet.
    if (accessFeedbackActive &&
        millis() - accessFeedbackStart >= ACCESS_FEEDBACK_MS)
    {
        accessFeedbackActive = false;
        updateLedFromValues();
    }

    handleDoor();

    // Mål med det interval der er sat i settings.
    unsigned long intervalMs =
        databaseIntervalMinutes * 60UL * 1000UL;

    if (millis() - lastSensorUpdate >= intervalMs)
    {
        lastSensorUpdate = millis();
        updateSensor();
    }

    if (rfid.PICC_IsNewCardPresent() &&
        rfid.PICC_ReadCardSerial())
    {
        String uid = "";

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

        checkCard(lastUID);

        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
    }

    delay(20);
}