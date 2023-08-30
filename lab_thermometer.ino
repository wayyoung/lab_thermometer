#include <WiFi.h>
#include "DHT.h"
#include "time.h"
#include "secret.h"

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

/*
 * Need to create secret.h with following define:
 *
 *   #define SECRET_DEVICE "YOUR DEVICE NAME"
 *
 *   #define SECRET_WIFI_SSID "SSID"
 *   #define SECRET_WIFI_PASSWORD "PASSWORD"
 *
 *   #define SECRET_API_KEY "check owner"
 *   #define SECRET_FIREBASE_PROJECT_ID "check owner"
 *   #define SECRET_USER_EMAIL "check owner"
 *   #define SECRET_USER_PASSWORD "check owner"
 */

#define DEVICE SECRET_DEVICE

#define WIFI_SSID SECRET_WIFI_SSID
#define WIFI_PASSWORD SECRET_WIFI_PASSWORD

#define SAMPLE_DELAY 10000 // msec

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

// Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// unsigned long dataMillis = 0;
// int count = 0;

// static void fcsUploadCallback(CFS_UploadStatusInfo info);
static bool create_device_doc(FirebaseData *fbdo, char const *project, char *device_doc_path, char *name);
static bool led_blink_enabled(FirebaseData *fbdo, const char *project, char *device_doc_path);
static bool add_record(FirebaseData *fbdo, const char *project, char *device_doc_path, char *utc_timestr, double t, double h);
void setup()
{
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  dht.begin(); // initialize the DHT sensor
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
  Serial.println(F("Connected to WiFi network with IP Address:"));
  Serial.println(WiFi.localIP());

  Serial.println(F("Turn on LED for 5 secs then turn off 3 secs"));
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
}

bool device_doc_exist = false;
int inner_loop_delay = 0;
bool led_on = false;
bool led_blink = false;
void loop()
{
  struct tm timeinfo;
  double h;
  double t;
  char utc_timestr[30];
  bool firebase_ready = true;
  int cnt = 0;

  digitalWrite(LED_BUILTIN, LOW);

  if (!Firebase.ready())
  {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    firebase_ready = false;
    Serial.println("ERROR Firebase not ready");
    delay(100);
  }
  else if (led_blink)
  {
    led_on = !led_on;
    if (led_on)
      digitalWrite(LED_BUILTIN, HIGH);
  }

  if (inner_loop_delay < SAMPLE_DELAY)
  {
    goto LOOP_DONE;
  }
  inner_loop_delay = 0;

  // Read humidity
  h = (double)dht.readHumidity();

  // Read temperature as Celsius (the default)
  t = (double)dht.readTemperature();

  // Check if any reads failed and exit early (to try again).
  if (isnan(h) || isnan(t))
  {
    Serial.println("ERROR reading DHT");
    goto LOOP_DONE;
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

    led_blink = led_blink_enabled(&fbdo, FIREBASE_PROJECT_ID, DEVICE_DOC_PATH);

    // Serial.printf("led_blink: %d\n", led_blink);

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
      Serial.printf("%s: %f, %f\n", utc_timestr, t, h);
    }
    else
    {
      Serial.println("ERROR adding record...");
      Serial.println(fbdo.errorReason());
    }
    // fbdo.clear();
  }

LOOP_DONE:
  delay(1000);
  inner_loop_delay += 1000;
}

// The Firestore payload upload callback function
// void fcsUploadCallback(CFS_UploadStatusInfo info)
// {
//   if (info.status == fb_esp_cfs_upload_status_init)
//   {
//     Serial.printf("\nUploading data (%d)...\n", info.size);
//   }
//   else if (info.status == fb_esp_cfs_upload_status_upload)
//   {
//     Serial.printf("Uploaded %d%s\n", (int)info.progress, "%");
//   }
//   else if (info.status == fb_esp_cfs_upload_status_complete)
//   {
//     Serial.println("Upload completed ");
//   }
//   else if (info.status == fb_esp_cfs_upload_status_process_response)
//   {
//     Serial.print("Processing the response... ");
//   }
//   else if (info.status == fb_esp_cfs_upload_status_error)
//   {
//     Serial.printf("Upload failed, %s\n", info.errorMsg.c_str());
//   }
// }

static bool create_device_doc(FirebaseData *fbdo, const char *project, char *device_doc_path, char *name)
{
  // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create_Edit_Parse/Create_Edit_Parse.ino
  FirebaseJson content;

  content.set("fields/name/stringValue", name);

  if (Firebase.Firestore.createDocument(fbdo, project, "" /* databaseId can be (default) or empty */, device_doc_path, content.raw()))
  {
    return true;
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
    payload.setJsonData(fbdo->payload().c_str());

    // Get the data from FirebaseJson object
    FirebaseJsonData jsonData;
    if (payload.get(jsonData, "fields/" LED_BLINK "/booleanValue", false) && jsonData.boolValue == true)
    {
      return true;
    }
  }
  return false;
}