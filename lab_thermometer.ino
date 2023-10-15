#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
// #include <ArduinoOTA.h>
#include "DHT.h"
#include "time.h"
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

/*
 *  Please update the following settings in "Secret" tab:
 *
 *  SECRET_DEVICE         "YOUR DEVICE NAME"
 *  SECRET_WIFI_SSID      "SSID"
 *  SECRET_WIFI_PASSWORD  "PASSWORD"
 */
#define DEVICE SECRET_DEVICE

#define WIFI_SSID SECRET_WIFI_SSID
#define WIFI_PASSWORD SECRET_WIFI_PASSWORD

#define LAB_THERMOMETER_VERSION (5) //2023-10-12[5]


#define DHTTYPE DHT11 // DHT 11
#define DHTPIN 13     // Digital pin connected to the DHT sensor

DHT dht(DHTPIN, DHTTYPE);

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

#define MAIN_LOOP_DELAY   1000 // msec
#define DEFAULT_SAMPLE_PERIOD      10000 // msec
#define WDT_OFFLINE_DELAY 1800000 // msec
#define DELTA_UPLOAD_PERIOD 180000 //msec

// Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;


// unsigned long dataMillis = 0;
// int count = 0;

// static void fcsUploadCallback(CFS_UploadStatusInfo info);
static bool create_device_doc(FirebaseData *fbdo, char const *project, char *device_doc_path, char *name);
static bool add_record(FirebaseData *fbdo, const char *project, char *device_doc_path, char *utc_timestr, double t, double h);

static bool led_blink_enabled(FirebaseData *fbdo, const char *project, char *device_doc_path);
static int th_sample_period(FirebaseData *fbdo, char const *project, char *device_doc_path);
static bool th_suspended(FirebaseData *fbdo, char const *project, char *device_doc_path);
static bool th_fault_record_enabled(FirebaseData *fbdo, char const *project, char *device_doc_path);

bool in_OTA = false;
unsigned long wdt_check_ms = 0;

void setup()
{
  int cnt=0;
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  dht.begin(); // initialize the DHT sensor
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("Connection Failed!");
    delay(1000);
    if(cnt++ > 10)
    {
       Serial.println("REBOOTING...");
      ESP.restart();
    }
  }
  Serial.println("Connected to WiFi network with IP Address:");
  Serial.println(WiFi.localIP());
  Serial.print("\nDefault MAC Address: ");
  Serial.println(WiFi.macAddress());

  // ArduinoOTA
  //     .onStart([]()
  //             {
  //               String type;
  //               if (ArduinoOTA.getCommand() == U_FLASH)
  //                 Serial.println("Start updating sketch");
  //               else // U_SPIFFS
  //                 Serial.println("Start updating filesystem");

  //               in_OTA = true;

  //               // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
  //             })
  //     .onEnd([]()
  //           { in_OTA = false; 
  //             Serial.println("\nEnd"); })
  //     .onProgress([](unsigned int progress, unsigned int total)
  //                 { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
  //     .onError([](ota_error_t error)
  //             {
  //   Serial.printf("Error[%u]: ", error);
  //   if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
  //   else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
  //   else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
  //   else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
  //   else if (error == OTA_END_ERROR) Serial.println("End Failed"); });

  // ArduinoOTA.setHostname(DEVICE);
  // ArduinoOTA.setPassword(SECRET_OTA_PASSWORD);

  Serial.println("Turn on LED for 5 secs then turn off 3 secs");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(5000);
  digitalWrite(LED_BUILTIN, LOW); // turn the LED on (HIGH is the voltage level)
  delay(3000);

  configTime(0, 0, NTP_SERVER);

  Serial.print("Firebase Client v");
  Serial.println(FIREBASE_CLIENT_VERSION);

  /* Assign the api key (required) */
  config.api_key = API_KEY;

  /* Assign the user sign in credentials */
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  /* Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback; // see addons/TokenHelper.h

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

void loop()
{
  struct tm timeinfo;
  double h;
  double t;
  char utc_timestr[30];
  bool firebase_ready = true;
  int cnt = 0;

  if (!led_blink)
    digitalWrite(LED_BUILTIN, LOW);

  // ArduinoOTA.handle();

  if ((millis() - main_loop_ms) > MAIN_LOOP_DELAY)
  {
    if (!Firebase.ready())
    {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      firebase_ready = false;
      Serial.printf("ERROR Firebase not ready,millis=%ld\r\n", millis());
      if (wdt_check_ms > 0 && (millis() - wdt_check_ms) > WDT_OFFLINE_DELAY)
      {
        Serial.println("OFFLINE_WDT !! REBOOTING...");
        delay(3000); //to printout
        ESP.restart();
      }
      Firebase.reconnectWiFi(true);
      fbdo.keepAlive(5, 5, 1);
    }
    else
    {
      wdt_check_ms = millis();
      if (led_blink)
      {
        led_on = !led_on;
        if (led_on)
          digitalWrite(LED_BUILTIN, HIGH);
        else
          digitalWrite(LED_BUILTIN, LOW);
      }
    }
    main_loop_ms = millis();
  }

  if (in_OTA || (millis() - sample_ms) < sample_period)
  {
    goto LOOP_DONE;
  }
  sample_ms = millis();

  // Read humidity
  h = (double)dht.readHumidity();
  if (isnan(h))
  {
    h = -99;
    Serial.println("ERROR reading DHT h");
  }

  // Read temperature as Celsius (the default)
  t = (double)dht.readTemperature();
  if (isnan(t))
  {
    t = -99;
    Serial.println("ERROR reading DHT t");
  }

  while (!getLocalTime(&timeinfo))
  {
    if (++cnt < 3)
    {
      delay(500);
      continue;
    }

    Serial.println("ERROR obtaining time");
    goto LOOP_DONE;
  }
  memset(utc_timestr, 0, sizeof(utc_timestr));
  strftime(utc_timestr, 22, "%FT%TZ", &timeinfo);
  Serial.print(" record: ");
  Serial.println(utc_timestr);
  Serial.print(" t: ");
  Serial.println(t);

  // check again
  if (firebase_ready)
  {

    if (!device_doc_exist)
    {
      if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", DEVICE_DOC_PATH, "") || create_device_doc(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH, DEVICE))
      {
        Serial.println("device_doc: " DEVICE);
        device_doc_exist = true;
      }
      else
      {
        Serial.println("ERROR to get device_doc: " DEVICE);
        goto LOOP_DONE;
      }
    }
    
    if (version != LAB_THERMOMETER_VERSION)
    {
      int ver = th_version(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
      if (ver != LAB_THERMOMETER_VERSION)
      {
        //update the version
        FirebaseJson content;
        content.set("fields/"VERSION"/integerValue", LAB_THERMOMETER_VERSION);
        Serial.printf("Updating version: %d \n",LAB_THERMOMETER_VERSION);

        /** if updateMask contains the field name that exists in the remote document and
         * this field name does not exist in the document (content), that field will be deleted from remote document
         */

        Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "" /* databaseId can be (default) or empty */, DEVICE_DOC_PATH, content.raw(), ""VERSION /* updateMask */);
  
      }
      else
      {
        version = LAB_THERMOMETER_VERSION;
      }
    }

    led_blink = led_blink_enabled(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    Serial.printf("led_blink: %d\n", led_blink);  
    fault_record = th_fault_record_enabled(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    Serial.printf("fault_record: %d\n", fault_record);  
    bool s = th_suspended(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    if (suspended != s)
    {
      suspended = s;
      Serial.printf("suspended changed: %d\n", suspended);  
    }
    Serial.printf("s: %d\n", s);
    
    // don't want to enable this now
    // disable_delta = th_disable_delta(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    
    int p = th_sample_period(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);
    if (p != -1 && p != sample_period)
    {
      sample_period = p;
      Serial.printf("sample_period changed: %d\n", sample_period);  
    }
    Serial.printf("p: %d\n", p);  

    // Serial.printf("led_blink: %d\n", led_blink);
    
    if (suspended)
    {
      Serial.println("skip uploading due to suspended");
      goto LOOP_DONE;
    }
    
    
    if (fault_record || t!=-99)
    {
      if ((!disable_delta) && ((millis() - upload_ms) < DELTA_UPLOAD_PERIOD))
      {
        double t_delta = abs( t - t_uploaded);
        if (t_delta <1)
        {
          goto LOOP_DONE;
        }
      }
      
      t_uploaded = t;
      upload_ms = millis();
  
      
      // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
      FirebaseJson content;
  
      content.set("fields/temperature/doubleValue", t);
      content.set("fields/humidity/doubleValue", h);
      content.set("fields/sample_timestamp/timestampValue", utc_timestr);
  
      String documentPath = DEVICE_DOC_PATH;
      documentPath += "/records/";
      documentPath += utc_timestr;
  
      if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "" /* databaseId can be (default) or empty */, documentPath, content.raw()))
      {
        Serial.printf("%s [%s]: %f, %f\r\n", utc_timestr, DEVICE, t, h);
      }
      else
      {
        Serial.println("ERROR adding records");
        Serial.println(fbdo.errorReason());
      }
    }
    
    // fbdo.clear();
  }

LOOP_DONE:
  delay(1);
}

static bool create_device_doc(FirebaseData *fbdo, const char *project, char *device_doc_path, char *name)
{
  // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
  FirebaseJson content;

  content.set("fields/name/stringValue", name);
  content.set("fields/location/stringValue", "--");
  content.set("fields/description/stringValue", "--");

  if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, device_doc_path, content.raw()))
  {
    String documentPath = device_doc_path;
    documentPath += "/pixel-phones/_0_";
    if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, documentPath, content.raw()))
    {
      Serial.println("pixel-phones added...");
      return true;
    }
    else
    {
      Serial.println("ERROR adding pixel-phones");
    }
  }
  else
  {
    Serial.println("ERROR adding new device");
  }

  Serial.println(fbdo->errorReason());
  return false;
}

static bool led_blink_enabled(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, LED_BLINK))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" LED_BLINK "/booleanValue", false) && jsonData.boolValue == true)
      {
        return true;
      }
    }
  }
  return false;
}

static bool th_fault_record_enabled(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, FAULT_RECORD))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {
  
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" FAULT_RECORD "/booleanValue", false) && jsonData.boolValue == true)
      {
        return true;
      }
    }
  }
  return false;
}

static bool th_suspended(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, SUSPENDED))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {

      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" SUSPENDED "/booleanValue", false) && jsonData.boolValue == true)
      {
        return true;
      }
    }
  }
  return false;
}

static bool th_disable_delta(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, DISABLE_DELTA))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {

      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" DISABLE_DELTA "/booleanValue", false) && jsonData.boolValue == true)
      {
        return true;
      }
    }
  }
  return false;
}

static int th_sample_period(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, SAMPLE_PERIOD))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" SAMPLE_PERIOD "/integerValue", false))
      {
        int period = jsonData.intValue;
        return period * 1000;
      }
    }
  }
  return -1;
}

static int th_version(FirebaseData *fbdo, char const *project, char *device_doc_path)
{
  if (Firebase.Firestore.getDocument(fbdo, project, "", device_doc_path, VERSION))
  {

    // Create a FirebaseJson object and set content with received payload
    FirebaseJson payload;
    if(payload.setJsonData(fbdo->payload().c_str()))
    {
      // Get the data from FirebaseJson object
      FirebaseJsonData jsonData;
      if (payload.get(jsonData, "fields/" VERSION "/integerValue", false))
      {
        int version = jsonData.intValue;
        return version;
      }
    }
  }
  return -1;
}
