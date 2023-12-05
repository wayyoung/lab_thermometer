#include <WiFi.h>
#include <WiFiUdp.h>
#include "time.h"
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <OneWire.h>
#include <DallasTemperature.h>


#ifdef ARDUINO_NANO_ESP32
#define DHTPIN 1  // A0, Digital pin connected to the DHT sensor
#define VC_PIN 18
#define LED_PIN LED_BUILTIN
#else
#define DHTPIN 13
#define VC_PIN 12
#define LED_PIN 16
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, 4, 5);
#endif


/*
    Please update the following settings in "Secret" tab:

    SECRET_DEVICE         "YOUR DEVICE NAME"
    SECRET_WIFI_SSID      "SSID"
    SECRET_WIFI_PASSWORD  "PASSWORD"
*/
#define DEVICE SECRET_DEVICE

#define WIFI_SSID SECRET_WIFI_SSID
#define WIFI_PASSWORD SECRET_WIFI_PASSWORD

#define LAB_THERMOMETER_VERSION (12)  //



// DHT dht(DHTPIN, DHTTYPE);

OneWire oneWire(DHTPIN);                   // setup a oneWire instance
DallasTemperature datempSensor(&oneWire);  // pass oneWire to DallasTemperature library


#define NTP_SERVER "pool.ntp.org"

/* Define the API Key */
#define API_KEY SECRET_API_KEY

/* Define the project ID */
#define FIREBASE_PROJECT_ID SECRET_FIREBASE_PROJECT_ID

/* Define the user Email and password that alreadey registerd or added in your project */
#define USER_EMAIL SECRET_USER_EMAIL
#define USER_PASSWORD SECRET_USER_PASSWORD
// #define TH_SENSORS_COL_PATH "projects/" FIREBASE_PROJECT_ID "/databases/(default)/documents/things/iot/th_sensors/"
#define TH_SENSORS_COL_PATH "things/iot/th_sensors"
#define DEVICE_DOC_PATH TH_SENSORS_COL_PATH "/" DEVICE
#define LED_BLINK "led_blink"
#define FAULT_RECORD "fault_record"
#define SUSPENDED "suspended"
#define SAMPLE_PERIOD "sample_period"
#define DISABLE_DELTA "disable_delta"
#define VERSION "version"

#define MAIN_LOOP_DELAY 1000         // msec
#define DEFAULT_SAMPLE_PERIOD 10000  // msec
#define WDT_OFFLINE_DELAY 1800000    // msec
#define DELTA_UPLOAD_PERIOD 180000   //msec

// Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;


// unsigned long dataMillis = 0;
// int count = 0;

// static void fcsUploadCallback(CFS_UploadStatusInfo info);
static bool create_device_doc(FirebaseData *fbdo, char const *project, char *device_doc_path, char *name);
static bool add_record(FirebaseData *fbdo, const char *project, char *device_doc_path, char *utc_timestr, double t, double h);

static bool led_blink_enabled(FirebaseData *fbdo, char const *project, char const *device_doc_path);
static int th_sample_period(FirebaseData *fbdo, char const *project, char const *device_doc_path);
static bool th_suspended(FirebaseData *fbdo, char const *project, char const *device_doc_path);
static bool th_fault_record_enabled(FirebaseData *fbdo, char const *project, char const *device_doc_path);
static bool create_positions_doc(FirebaseData *fbdo, const char *project, char const *device_doc_path, char const *name, char const *utc_timestr);

bool in_OTA = false;
bool ip_ready = false;
unsigned long wdt_check_ms = 0;

static void lcd_in_main_loop(char const *timestr);

#if defined(ESP8266)
#define ON LOW
#define OFF HIGH
#else
#define ON HIGH
#define OFF LOW
#endif

void setup() {
  int cnt = 0;

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, OFF);
  Serial.begin(115200);
  pinMode(VC_PIN, OUTPUT);
  digitalWrite(VC_PIN, ON);
#ifndef ARDUINO_NANO_ESP32
  u8g2.begin();
#endif
  lcd_in_main_loop(NULL);

  // dht.begin(); // initialize the DHT sensor
  datempSensor.begin();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed!");
    delay(1000);
    if (cnt++ > 10) {
      Serial.println("REBOOTING...");
      ESP.restart();
    }
  }
  Serial.println("\nConnected to WiFi network with IP Address:");
  Serial.println(WiFi.localIP());
  Serial.print("\nDefault MAC Address: ");
  Serial.println(WiFi.macAddress());
  ip_ready = true;

#ifdef ARDUINO_NANO_ESP32
  Serial.println("Turn on LED for 3 secs then turn off 1 secs");
  digitalWrite(LED_PIN, ON);
  delay(3000);
  digitalWrite(LED_PIN, LOW);  // turn the LED on (HIGH is the voltage level)
  delay(1000);
#else
#endif

  configTime(0, 0, NTP_SERVER);

  Serial.print("Firebase Client v");
  Serial.println(FIREBASE_CLIENT_VERSION);

  /* Assign the api key (required) */
  config.api_key = API_KEY;

  /* Assign the user sign in credentials */
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  /* Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback;  // see addons/TokenHelper.h

  // Limit the size of response payload to be collected in FirebaseData
  fbdo.setResponseSize(4096);

  Firebase.begin(&config, &auth);

  Firebase.reconnectWiFi(true);

  // For sending payload callback
  // config.cfs.upload_callback = fcsUploadCallback;

  // You can use TCP KeepAlive in FirebaseData object and tracking the server connection status, please read this for detail.
  // https://github.com/mobizt/Firebase-ESP-Client#about-firebasedata-object
  fbdo.keepAlive(5, 5, 1);
  // ArduinoOTA.begin();
  wdt_check_ms = millis();
}

bool device_doc_exist = false;
bool led_on = false;
bool led_blink = false;
bool fault_record = false;
bool suspended = false;
bool disable_delta = false;
unsigned long main_loop_ms = 0;
unsigned long sample_ms = 0;
unsigned long upload_ms = 0;

unsigned long sample_period = DEFAULT_SAMPLE_PERIOD;

int version = -1;

double t_uploaded = -1;

double t = -99;
bool firebase_ready = true;
void loop() {
  struct tm timeinfo;
  double h;

  char utc_timestr[30];

  int cnt = 0;

  if (!led_blink)
    digitalWrite(LED_PIN, OFF);

  // ArduinoOTA.handle();

  if ((millis() - main_loop_ms) > MAIN_LOOP_DELAY) {
    if (!firebase_ready) {
      digitalWrite(LED_PIN, ON);
      delay(200);
      digitalWrite(LED_PIN, OFF);
      delay(100);
      digitalWrite(LED_PIN, ON);
      delay(200);
      digitalWrite(LED_PIN, OFF);
      Serial.printf("ERROR Firebase not ready,millis=%ld\r\n", millis());
      if (wdt_check_ms > 0 && (millis() - wdt_check_ms) > WDT_OFFLINE_DELAY) {
        Serial.println("OFFLINE_WDT !! REBOOTING...");
        delay(3000);  //to printout
        ESP.restart();
      }
      Firebase.reconnectWiFi(true);
      fbdo.keepAlive(5, 5, 1);
      firebase_ready = Firebase.ready();
    } else {
      wdt_check_ms = millis();
      if (led_blink) {
        led_on = !led_on;
        if (led_on)
          digitalWrite(LED_PIN, ON);
        else
          digitalWrite(LED_PIN, OFF);
      }
    }
    main_loop_ms = millis();
  }

  while (!getLocalTime(&timeinfo)) {
    if (++cnt < 3) {
      delay(500);
      continue;
    }

    Serial.println("ERROR obtaining time");
    goto LOOP_DONE;
  }
  memset(utc_timestr, 0, sizeof(utc_timestr));
  strftime(utc_timestr, 22, "%FT%TZ", &timeinfo);

  lcd_in_main_loop(utc_timestr);

  if (in_OTA || (millis() - sample_ms) < sample_period) {
    goto LOOP_DONE;
  }
  sample_ms = millis();

  datempSensor.requestTemperatures();   // send the command to get temperatures
  t = datempSensor.getTempCByIndex(0);  // read temperature in Celsius
  if (t <= -20) {
    Serial.println("ERROR reading DALLAS t");
    // Read temperature as Celsius (the default)
    // t = (double)dht.readTemperature();
    // if (isnan(t))
    // {
    //   t = -99;
    //   Serial.println("ERROR reading DHT t");
    // }
  }


  Serial.print(" record: ");
  Serial.println(utc_timestr);
  Serial.print(" t: ");
  Serial.println(t);

  // check again
  firebase_ready = Firebase.ready();
  if (firebase_ready) {

    if (!device_doc_exist) {
      if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", DEVICE_DOC_PATH, "") || create_device_doc(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH, DEVICE)) {
        create_positions_doc(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH, "--", utc_timestr);
        Serial.println("device_doc: " DEVICE);
        device_doc_exist = true;
      } else {
        Serial.println("ERROR to get device_doc: " DEVICE);
        goto LOOP_DONE;
      }
    }

    if (version != LAB_THERMOMETER_VERSION) {
      int ver = th_version(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
      if (ver != LAB_THERMOMETER_VERSION) {
        //update the version
        FirebaseJson content;
        content.set("fields/" VERSION "/integerValue", LAB_THERMOMETER_VERSION);
        Serial.printf("Updating version: %d \n", LAB_THERMOMETER_VERSION);

        /** if updateMask contains the field name that exists in the remote document and
           this field name does not exist in the document (content), that field will be deleted from remote document
        */

        Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "" /* databaseId can be (default) or empty */, DEVICE_DOC_PATH, content.raw(), "" VERSION /* updateMask */);
        create_positions_doc(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH, "--", utc_timestr);

      } else {
        version = LAB_THERMOMETER_VERSION;
      }
    }

    led_blink = led_blink_enabled(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    Serial.printf("led_blink: %d\n", led_blink);
    fault_record = th_fault_record_enabled(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    Serial.printf("fault_record: %d\n", fault_record);
    bool s = th_suspended(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    if (suspended != s) {
      suspended = s;
      Serial.printf("suspended changed: %d\n", suspended);
    }
    Serial.printf("s: %d\n", s);

    // don't want to enable this now
    // disable_delta = th_disable_delta(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);

    int p = th_sample_period(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    if (p != -1 && p != sample_period) {
      sample_period = p;
      Serial.printf("sample_period changed: %d\n", sample_period);
    }
    Serial.printf("p: %d\n", p);

    // Serial.printf("led_blink: %d\n", led_blink);

    if (suspended) {
      Serial.println("skip uploading due to suspended");
      goto LOOP_DONE;
    }


    if (fault_record || t >= -20) {
      if ((!disable_delta) && ((millis() - upload_ms) < DELTA_UPLOAD_PERIOD)) {
        double t_delta = abs(t - t_uploaded);
        if (t_delta < 1) {
          goto LOOP_DONE;
        }
      }

      t_uploaded = t;
      upload_ms = millis();


      // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
      FirebaseJson content;

      content.set("fields/temperature/doubleValue", t);
      // content.set("fields/humidity/doubleValue", h);
      content.set("fields/sample_timestamp/timestampValue", utc_timestr);

      String documentPath = DEVICE_DOC_PATH;
      documentPath += "/records/";
      documentPath += utc_timestr;

      if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "" /* databaseId can be (default) or empty */, documentPath, content.raw())) {
        Serial.printf("%s [%s]: %f\r\n", utc_timestr, DEVICE, t);
      } else {
        Serial.println("ERROR adding records!!");
        Serial.println(fbdo.errorReason());
        firebase_ready = false;
      }
    }

    fbdo.clear();
  }

LOOP_DONE:
  delay(1);
}

static bool create_device_doc(FirebaseData *fbdo, char const *project, char const *device_doc_path, char const *name) {
  // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
  FirebaseJson content;

  content.set("fields/name/stringValue", name);
  content.set("fields/location/stringValue", "--");
  content.set("fields/description/stringValue", "--");

  if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, device_doc_path, content.raw())) {
    String documentPath = device_doc_path;
    documentPath += "/pixel-phones/_0_";
    if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, documentPath, content.raw())) {
      Serial.println("pixel-phones added...");
      return true;
    } else {
      Serial.println("ERROR adding pixel-phones");
    }
  } else {
    Serial.println("ERROR adding new device");
  }

  Serial.println(fbdo->errorReason());
  return false;
}

static bool create_positions_doc(FirebaseData *fbdo, const char *project, char const *device_doc_path, char const *name, char const *utc_timestr) {
  // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
  FirebaseJson content;

  content.set("fields/name/stringValue", name);
  content.set("fields/setup_timestamp/timestampValue", utc_timestr);
  String documentPath = device_doc_path;
  documentPath += "/positions/";
  documentPath += utc_timestr;

  if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, documentPath, content.raw())) {
    Serial.println("position added...");
    return true;
  } else {
    Serial.println("ERROR adding new position");
  }

  Serial.println(fbdo->errorReason());
  return false;
}

static bool led_blink_enabled(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, LED_BLINK)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" LED_BLINK "/booleanValue", false) && jsonData.boolValue == true) {
        return true;
      }
    }
  }
  return false;
}

static bool th_fault_record_enabled(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, FAULT_RECORD)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {

      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" FAULT_RECORD "/booleanValue", false) && jsonData.boolValue == true) {
        return true;
      }
    }
  }
  return false;
}

static bool th_suspended(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, SUSPENDED)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {

      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" SUSPENDED "/booleanValue", false) && jsonData.boolValue == true) {
        return true;
      }
    }
  }
  return false;
}

static bool th_disable_delta(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, DISABLE_DELTA)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {

      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" DISABLE_DELTA "/booleanValue", false) && jsonData.boolValue == true) {
        return true;
      }
    }
  }
  return false;
}

static int th_sample_period(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, SAMPLE_PERIOD)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" SAMPLE_PERIOD "/integerValue", false)) {
        int period = jsonData.intValue;
        return period * 1000;
      }
    }
  }
  return -1;
}

static int th_version(FirebaseData *fbdo, char const *project, char const *device_doc_path) {
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, VERSION)) {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if (payload.setJsonData(fbdo->payload().c_str())) {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" VERSION "/integerValue", false)) {
        int version = jsonData.intValue;
        return version;
      }
    }
  }
  return -1;
}

static int xoff = 0;
static void lcd_in_main_loop(char const *timestr) {
#ifndef ARDUINO_NANO_ESP32
  String ip_string = (ip_ready) ? WiFi.localIP().toString() : String("0.0.0.0");
  String fready_string = String("firebase_ready: ");
  fready_string.concat(String(firebase_ready));
  String t_string = String("T: ");
  t_string.concat(String(t));



  u8g2.setFont(u8g2_font_squeezed_b7_tr);
  u8g2.firstPage();
  do {

    u8g2.drawStr(0, 8, DEVICE);
    u8g2.drawStr(xoff, 18, ip_string.c_str());
    u8g2.drawStr(xoff, 28, fready_string.c_str());
    u8g2.drawStr(xoff, 38, t_string.c_str());
    if (timestr != NULL)
      u8g2.drawStr(0, 62, timestr);
  } while (u8g2.nextPage());
  xoff = (xoff + 1) % 128;
#endif
}