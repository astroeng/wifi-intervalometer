


/* Started with the IRServer Example. Went from there. */
 
#include <Arduino.h>
#if defined(ESP8266)
 #include <ESP8266WiFi.h>
 #include <ESP8266WebServer.h>
 #include <ESP8266mDNS.h>
#endif  // ESP8266
#if defined(ESP32)
 #include <WiFi.h>
 #include <WebServer.h>
 #include <ESPmDNS.h>
#endif  // ESP32
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFiClient.h>

const char* kSsid = "sensors";
const char* kPassword = "sensors123";
MDNSResponder mdns;

#undef HOSTNAME
#define HOSTNAME "nexInterval"

#if defined(ESP8266)
ESP8266WebServer server(80);
#endif  // ESP8266

#if defined(ESP32)
WebServer server(80);
#endif  // ESP32

const uint16_t kIrLed = 4;  // ESP GPIO pin to use. Recommended: 4 (D2).
IRsend irsend(kIrLed);  // Set the GPIO to be used to sending the message.

typedef struct 
{
  uint32_t image_count;
  uint32_t current_image;
  uint32_t image_duration_ms;
  uint32_t start_time_ms;
  bool     job_active;
  bool     job_abort;
} image_job_t;


typedef enum
{
  IMAGE_IDLE = 0,
  IMAGE_CAPTURE,
  IMAGE_DELAY,
  IMAGE_ABORT
} job_state_t;

static image_job_t image_job = {};

#define NEX_SHUTTER 0xB4B8F

void toggleShutter(uint32_t code)
{
  irsend.sendSony(code, 20);
  //delay(40);
  //irsend.sendSony(code, 20);
  //delay(40);
  //irsend.sendSony(code, 20);
}

void handleRoot() {
  server.send(200, "text/html",
              "<html>" \
                "<head><title>" HOSTNAME " </title>" \
                "<meta http-equiv=\"Content-Type\" " \
                    "content=\"text/html;charset=utf-8\">" \
                "</head>" \
                "<body>" \
                  "<h1>Hello, take a picture!</h1>" \
                  "<p>This is intended to be used with an NEX camera in bulb mode.</p>"
                  "<p>Typically I like to set the ISO around 1600 or 3200.</p>"
                  "<p><a href=\"ir?sony=740239\">ABORT! (or) Manual Shutter</a></p>" \
                  "<form action=\"ir?\" method=\"GET\" id=\"img_job\">" \
                    "<div><label for=\"imgcount\">Count</label><input id=\"imgcount\" name=\"imgcount\" value=\"10\" /></div>" \
                    "<div><label for=\"imgduration\">Duration</label><input id=\"imgduration\" name=\"imgduration\" value=\"120\" /></div>" \
                    "<div><label for=\"imgactive\">Active</label><input id=\"imgactive\" name=\"imgactive\" value=\"true\" /></div>" \
                  "</form>" \
                  "<button type=\"submit\" form=\"img_job\" value=\"Start\">Start</button>" \
                "</body>" \
              "</html>");
}

void handleIr() {
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "sony") {
      uint32_t code = strtoul(server.arg(i).c_str(), NULL, 10);
      image_job.job_abort = true;
      toggleShutter(code);
    }
    else if ((server.argName(i) == "imgcount") &&
             (image_job.job_active == false)) {
      image_job.image_count = strtoul(server.arg(i).c_str(), NULL, 10);
    }
    else if ((server.argName(i) == "imgduration") &&
             (image_job.job_active == false))  {
      image_job.image_duration_ms = strtoul(server.arg(i).c_str(), NULL, 10) * 1000;
    }
    else if ((server.argName(i) == "imgactive") &&
             (image_job.job_active == false))  {
      image_job.job_active = (server.arg(i) == String("true")); 
    }
  }

  image_job.current_image = 0;
  handleJob();
}

void handleJob() {
  server.send(200, "text/html",
              "<html>" + 
              String(image_job.image_count) + " x " + 
              String(image_job.image_duration_ms)/1000 + " seconds. Completed " + 
              String(image_job.current_image) + " with " + 
              String((millis() - image_job.start_time_ms)/1000) + " seconds captured on next image.</html>");
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  server.send(404, "text/plain", message);
}

void setup(void) {
  irsend.begin();

  Serial.begin(115200);
  WiFi.begin(kSsid, kPassword);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(kSsid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP().toString());

#if defined(ESP8266)
  if (mdns.begin(HOSTNAME, WiFi.localIP())) {
#else  // ESP8266
  if (mdns.begin(HOSTNAME)) {
#endif  // ESP8266
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);
  server.on("/ir", handleIr);
  server.on("/job", handleJob);

  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

void processJob()
{
  static job_state_t job_state = IMAGE_IDLE;

  // If an abort was issued, set that job state and capture the current 
  // time. The shutter event was processed immediatly with the incoming
  // http request. So in here abort just needs to delay and recover 
  // some state variables. 
  if (image_job.job_abort == true)
  {
    job_state = IMAGE_ABORT;
    image_job.start_time_ms = millis();
  }
  
  switch(job_state)
  {
    case IMAGE_IDLE:
      // This state is checking to see if the conditions a new job have been met.
      // Simplistically this is are there images to capture... This sets the active 
      // flag to prevent any odd behaviors on the network side changing the job.
      if (image_job.image_count > image_job.current_image)
      {
        image_job.job_active = true;
        toggleShutter(NEX_SHUTTER);
        delay(500);
        image_job.start_time_ms = millis();
        job_state = IMAGE_CAPTURE;
        Serial.println("Image Idle");
      }
      break;
    case IMAGE_CAPTURE:
      // This state times the image capture. It finishes by closing the 
      // shutter and moving to the delay state. 
      if (image_job.start_time_ms + image_job.image_duration_ms < millis())
      {
        toggleShutter(NEX_SHUTTER);
        job_state = IMAGE_DELAY;
        image_job.current_image++;
        image_job.job_active = false;
        Serial.println("Image Capture");
      }
      break;
    case IMAGE_DELAY:
      // This state allows the camera time to save the picture
      // and then be ready for the next shutter event. 
      if (image_job.start_time_ms + image_job.image_duration_ms + 5000 < millis())
      {
        job_state = IMAGE_IDLE;
        Serial.println("Image Delay");
      }
      break;
    case IMAGE_ABORT:
      image_job.image_count = 0;
      image_job.job_abort = false;
      image_job.job_active = false;
      job_state = IMAGE_IDLE;
      Serial.println("Image Abort");
      break;
  }
}

void loop(void) {
  server.handleClient();
  processJob();
}
