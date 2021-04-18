/*************************************************************
 * Derek Schacht
 * 2021/04/18
 *
 * *** Use Statement and License ***
 *  Follows the license of the example on which this is based.
 *  Contact [dschacht ( - at - ) gmail ( - dot - ) com] for questions.
 *************************************************************
 */

/* Some considerations.
 *  Timer rollover is not dealt with. Please don't do a long exposure timelapse 
 *  for ~49 days. This is intended to be used for a couple of hours tops.
 *  
 *  The shutter state can get desyncronized with the statemachine. Keep an eye on
 *  things and ABORT when synchronization is lost. Future consideration for adding 
 *  a non ABORT manual shutter that could be used during a job.
 *  
 *  The camera seems to need several seconds after an image to then be ready for the
 *  next one to start. Emperically 5 seconds seemed to work pretty well, 2 seconds 
 *  did not.
 *  
 *  The shortest recommended capture is around 20 seconds. Otherwise, the image to dwell
 *  time is starting to get really close to 50%.
 *  
 *  This tool is not intended to assist in taking calibration frames. However, it works
 *  fine for darks. Flats and Bias frames are typically too short. Use a rubberband and
 *  a marble with continuous shooting. But bear in mind any heat generated from taking 
 *  pictures in rapid succession.
 *  
 *  I tested this with an Original NEX-5, I'm not sure if all of the NEX/Alpha/Sony cameras
 *  use the same IR pattern for the shutter. I got this shutter pattern by pointing my 
 *  existing IR remote at an arduino that was setup to receive IR patterns. 
 */

/* Resources:
 *  http://www.technoblogy.com/show?VFT
 *  http://www.sbprojects.com/knowledge/ir/sirc.php (Need to use the wayback machine)
 *  https://github.com/astroeng/ir_intervalometer
 */

/* Started with the IRServer Example. I used some inspriation from the Arduino based 
 * ir_intervalometer that I wrote several years ago and mushed some ideas together. 
 * Relevant credit information copied from the ESP8266 Arduino example provided below.
 *
 * IRremoteESP8266: IRServer - demonstrates sending IR codes controlled from a webserver
 * Version 0.3 May, 2019
 * Version 0.2 June, 2017
 * Copyright 2015 Mark Szabo
 * Copyright 2019 David Conran
 */

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

#define NEX_SHUTTER 0xB4B8F

typedef enum
{
  IMAGE_IDLE = 0,
  IMAGE_CAPTURE,
  IMAGE_DELAY,
  IMAGE_ABORT
} job_state_t;

typedef struct 
{
  uint32_t image_count;       // number of images to capture.
  uint32_t current_image;     // present number of completed images.
  uint32_t image_duration_ms; // the capture length of each image.
  uint32_t event_time_ms;     // the most recent event time START-CAPTURE, END-CAPTURE, ABORT-CAPTURE (ish)
  bool     job_active;        // true if a capture job is running, and has remaining images.
  bool     job_abort;         // true if the abort message was received.
} image_job_t;

// Global for the job information... This is shared between the 
// processing function and the function that receives the http 
// form submission. Ideally this would be removed and a more
// proper messaging stream would exist between the form reciever
// and the job processing function. This will suffice for this
// prototype.
static image_job_t image_job = {};

void toggleShutter(uint32_t code, uint32_t code_size_bits)
{
  irsend.sendSony(code, code_size_bits);
}

// Function to provide the root page for the webserver.
void handleRoot() {
  server.send(200, "text/html",
              "<html>" \
                "<head><title>" HOSTNAME " </title>" \
                "<meta http-equiv=\"Content-Type\" " \
                    "content=\"text/html;charset=utf-8\">" \
                "</head>" \
                "<body>" \
                  "<h1>Hello, take some pictures!</h1>" \
                  "<p>This is intended to be used with an NEX camera in bulb mode.</p>"
                  "<p>Typically I like to set the ISO around 1600 or 3200.</p>"
                  "<p><a href=\"start?sony=740239\">ABORT! (or) Manual Shutter</a></p>" \
                  "<p><a href=\"job\">Job Page</a></p>" \
                  "<form action=\"start?\" method=\"GET\" id=\"img_job\">" \
                    "<div><label for=\"imgcount\">Count</label><input id=\"imgcount\" name=\"imgcount\" value=\"10\" /></div>" \
                    "<div><label for=\"imgduration\">Duration</label><input id=\"imgduration\" name=\"imgduration\" value=\"120\" /></div>" \
                    "<div><label for=\"imgactive\">Active</label><input id=\"imgactive\" name=\"imgactive\" value=\"true\" /></div>" \
                  "</form>" \
                  "<button type=\"submit\" form=\"img_job\" value=\"Start\">Start</button>" \
                "</body>" \
              "</html>");
}

// Process the request and then provide the root page.
// Requests are simple ARG based URIs an example follows "<base-uri>\start?imgcount=10&imgduration=120&imgactive=true".
// sony - uses the provided IR pattern code and sends it to the camera. This is currently used for the Abort and Manual actions.
// imgcount - is simply the number of images to capture.
// imgduration - is the length for each image in seconds.
// imgactive - allows for inactive jobs to be submitted. Use Case Right now ... :shrug
void handleStart() {
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "sony") {
      uint32_t code = strtoul(server.arg(i).c_str(), NULL, 10);
      image_job.job_abort = true;
      toggleShutter(code, 20);
    }
    else if ((server.argName(i) == "imgcount") &&
             (image_job.job_active == false)) {
      image_job.image_count = strtoul(server.arg(i).c_str(), NULL, 10);
      
    }
    else if ((server.argName(i) == "imgduration") &&
             (image_job.job_active == false)) {
      image_job.image_duration_ms = strtoul(server.arg(i).c_str(), NULL, 10) * 1000;
    }
    else if ((server.argName(i) == "imgactive") &&
             (image_job.job_active == false)) {
      image_job.job_active = (server.arg(i) == String("true"));
      image_job.current_image = 0;
    }
  }
  handleRoot();
}

// Send out some information about the in progress job.
void handleJob() {
  server.send(200, "text/html",
              "<html>" + 
              String(image_job.image_count) + " x " + 
              String(image_job.image_duration_ms/1000) + " seconds. Completed " + 
              String(image_job.current_image) + " with " + 
              String((millis() - image_job.event_time_ms)/1000) + " seconds captured on next image.</html>");
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
  Serial.print("Hostname: ");
  Serial.println(HOSTNAME);

#if defined(ESP8266)
  if (mdns.begin(HOSTNAME, WiFi.localIP())) {
#else  // ESP8266
  if (mdns.begin(HOSTNAME)) {
#endif  // ESP8266
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/job", handleJob);

  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

// Function to process the job. This is implemented to be as non 
// blocking as possible. That way the Wi-Fi stack doesn't get starved 
// on an esp8266. When run on the esp32, thats less of an issue because 
// that processor is dual core... and running an OS that can perform 
// context switching. 
void processJob()
{
  static job_state_t job_state = IMAGE_IDLE;

  // If an abort was issued, set the abort state. The shutter event 
  // was processed immediatly with the incoming http request. So in
  // here abort just needs to delay and recover some state variables. 
  if (image_job.job_abort == true)
  {
    job_state = IMAGE_ABORT;
  }
  
  switch(job_state)
  {
    case IMAGE_IDLE:
      // This state is checking to see if the conditions a new job have been met.
      // Simplistically this is, are there images to capture?
      if (image_job.image_count > image_job.current_image)
      {
        toggleShutter(NEX_SHUTTER, 20);
        delay(500); // Small delay to allow the camera to start taking the picture. This is an emperical line of code based on camera observations.
        image_job.event_time_ms = millis();
        job_state = IMAGE_CAPTURE;
        Serial.println("Image Idle");
      }
      else
      {
        image_job.job_active = false;
      }
      break;
    case IMAGE_CAPTURE:
      // This state times the image capture. It finishes by closing the 
      // shutter and moving to the delay state. 
      if (image_job.event_time_ms + image_job.image_duration_ms < millis())
      {
        toggleShutter(NEX_SHUTTER, 20);
        job_state = IMAGE_DELAY;
        image_job.current_image++;
        image_job.event_time_ms = millis();
        Serial.println("Image Capture");
      }
      break;
    case IMAGE_DELAY:
      // This state allows the camera time to save the picture
      // and then be ready for the next shutter event. 
      if (image_job.event_time_ms + 5000 < millis())
      {
        job_state = IMAGE_IDLE;
        Serial.println("Image Delay");
      }
      break;
    case IMAGE_ABORT:
      // This state is a transitional state that allows an in progress
      // capture job to be terminated. This then cleans up job state
      // and gets ready for the next job.
      image_job.image_count = 0;
      image_job.job_abort = false;
      image_job.event_time_ms = millis();
      job_state = IMAGE_DELAY;
      Serial.println("Image Abort");
      break;
  }
}

void loop(void) {
  server.handleClient();
  processJob();
}
