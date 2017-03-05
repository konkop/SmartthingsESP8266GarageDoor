/*
    This sketch demonstrates how to set up a simple REST-like server
    to monitor a door/window opening and closing.
    A reed switch is connected to gnd and the other end is
    connected to gpio2, when door 
    is closed, reed switch should be closed and vice versa.
    ip is the IP address of the ESP8266 module if you want to hard code it,
    Otherwise remove ip, gateway and subnet and also WiFi.config to use dhcp.
    
    Status of contact is returned as json: {"name":"contact","status":0}
    To get status: http://{ip}:{serverPort}/getstatus
    
    Any time status changes, the same json above will be posted to {hubIp}:{hubPort}.
*/

#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "FS.h"

#define CLOSED_DOOR_PIN 2 //d4=led
#define OPEN_DOOR_PIN 5 //d1
#define RELAY_PIN 4 //d2
//void contactChanged();

const char APPSETTINGS[] PROGMEM = "/appSettings.json";
const char LOADED[] PROGMEM = " loaded: ";
const char HUBPORT[] PROGMEM = "hubPort";
const char HUPIP[] PROGMEM = "hubIp";
const char DEVICENAME[] PROGMEM = "deviceName";

const unsigned int serverPort = 9060; // port to run the http server on

// Smartthings hub information
//IPAddress hubIp = 192.168.1.100;//INADDR_NONE; // smartthings hub ip
IPAddress hubIp(192, 168, 1, 100); // smartthings hub ip
unsigned int hubPort = 39500;//0; // smartthings hub port

String deviceName = "ESP8266 Contact Sensor";
const char *OTApassword = "1234"; //you should probably change thisâ€¦

byte oldClosedDoorState, currentClosedDoorState;
byte oldOpenDoorState, currentOpenDoorState;
volatile unsigned long last_micros;
long debounceDelay = 10;    // the debounce time (in ms); increase if false positives
bool sendUpdate = false;

WiFiServer server(serverPort); //server
WiFiClient client; //client

IPAddress IPfromString(String address) {
  int ip1, ip2, ip3, ip4;
  ip1 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip2 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip3 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip4 = address.toInt();
  return IPAddress(ip1, ip2, ip3, ip4);
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println(F("Entered config mode"));
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

bool loadAppConfig() {
  File configFile = SPIFFS.open(FPSTR(APPSETTINGS), "r");
  if (!configFile) {
    Serial.println(F("Failed to open config file"));
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println(F("Config file size is too large"));
    return false;
  }

  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  configFile.close();

  const int BUFFER_SIZE = JSON_OBJECT_SIZE(3);
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println(F("Failed to parse application config file"));
    return false;
  }

  hubPort = json[FPSTR(HUBPORT)];
  Serial.print(FPSTR(HUBPORT));
  Serial.print(FPSTR(LOADED));
  Serial.println(hubPort);
  String hubAddress = json[FPSTR(HUPIP)];
  Serial.print(FPSTR(HUPIP));
  Serial.print(FPSTR(LOADED));
  Serial.println(hubAddress);
  hubIp = IPfromString(hubAddress);
  String savedDeviceName = json[FPSTR(DEVICENAME)];
  deviceName = savedDeviceName;
  Serial.print(FPSTR(DEVICENAME));
  Serial.print(FPSTR(LOADED));
  Serial.println(deviceName);
  return true;
}

bool saveAppConfig(String jsonString) {
  Serial.print(F("Saving new settings: "));
  Serial.println(jsonString);
  File configFile = SPIFFS.open(FPSTR(APPSETTINGS), "w");
  if (!configFile) {
    Serial.println(F("Failed to open application config file for writing"));
    return false;
  }
  configFile.print(jsonString);
  configFile.close();
  return true;
}

void setup() {
  delay(10);
  Serial.begin(9600);

  Serial.print(F("Sketch size: "));
  Serial.println(ESP.getSketchSize());
  Serial.print(F("Free size: "));
  Serial.print(ESP.getFreeSketchSpace());

  Serial.println();
  Serial.println(F("Initializing IO..."));
  // prepare GPIO2
  pinMode(CLOSED_DOOR_PIN, INPUT_PULLUP);
  pinMode(OPEN_DOOR_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  attachInterrupt(CLOSED_DOOR_PIN, closedContactChanged, CHANGE);
  attachInterrupt(OPEN_DOOR_PIN, openContactChanged, CHANGE);

  Serial.println(F("Mounting FS..."));

  if (!SPIFFS.begin()) {
    Serial.println(F("Failed to mount file system"));
    return;
  }

  // DEBUG: remove all files from file system
//  SPIFFS.format();

  if (!loadAppConfig()) {
    Serial.println(F("Failed to load application config"));
  } else {
    Serial.println(F("Application config loaded"));
  }

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  // DEBUG: reset WiFi settings
//  wifiManager.resetSettings();
  // set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);
  // tries to connect to last known settings
  // if it does not connect it starts an access point
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect()) {
    Serial.println(F("Failed to connect to WiFi and hit timeout"));
    // reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }
  Serial.print(F("Successfully connected to SSID '"));
  Serial.print(WiFi.SSID());
  Serial.print(F("' - local IP: "));
  Serial.println(WiFi.localIP());
  // Start the application server
  Serial.print(F("Starting Application HTTP Server on port "));
  Serial.println(serverPort);
  server.begin();

  // Enable Arduino OTA (if we have enough room to update in place)
  if (ESP.getSketchSize() < ESP.getFreeSketchSpace()) {
    Serial.println(F("Starting OTA Server..."));
    ArduinoOTA.setHostname(deviceName.c_str());
    ArduinoOTA.setPassword(OTApassword);
  
    ArduinoOTA.onStart([]() {
      Serial.println(F("Start"));
    });
    ArduinoOTA.onEnd([]() {
      Serial.println(F("End"));
      ESP.reset();
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf((const char *)F("Progress: %u%%\n"), (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf((const char *)F("Error[%u]: "), error);
      if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
      else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
      ESP.reset();
    });
    ArduinoOTA.begin();
  }

  Serial.println(F("*** Application ready."));
}

// send json data to client connection
void sendJSONData(WiFiClient client) {
  client.println(F("CONTENT-TYPE: application/json"));
  client.println(F("CONTENT-LENGTH: 29"));
  client.println();
  client.print("{\"name\":\"contact\",\"status\":");

  //client.print(currentClosedDoorState);
  if(currentClosedDoorState == 0){ 
    client.print(2);
  }else if(currentOpenDoorState == 0){
    client.print(1);
  }else{
    client.print(0);
  }
  
  client.println("}");
}

// send response to client for a request for status
void handleRequest(WiFiClient client)
{
  // Wait until the client sends some data
  Serial.print(F("Received request: "));
  while(!client.available()){
    delay(1);
  }

  // Read the first line of the request
  String req = client.readStringUntil('\r');
  Serial.println(req);
  if (!req.indexOf(F("GET")))
  {
    client.flush(); // we don't care about anything else for a GET request
  }

  // Match the request
  if (req.indexOf(F("/getstatus")) != -1) {
    client.println(F("HTTP/1.1 200 OK")); //send new page
    sendJSONData(client);
  }
  else if (req.indexOf(F("/setstatus")) != -1) {
    digitalWrite(RELAY_PIN, HIGH);   // turn the RELAY on
    delay(500);                       // wait for 1/2 second
    digitalWrite(RELAY_PIN, LOW);    // turn the RELAY off
  
    client.println(F("HTTP/1.1 200 OK")); //send new page
    sendJSONData(client);
  }
  else if (req.indexOf(F("POST /updateSettings")) != -1) {
    // get the body in order to retrieve the POST data
    while (client.available() && client.readStringUntil('\r').length() > 1);
    String settingsJSON = client.readStringUntil('\r');
    client.flush();
    const int BUFFER_SIZE = JSON_OBJECT_SIZE(3);
    StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(settingsJSON);
    if (root[FPSTR(HUPIP)]) hubIp = IPfromString(root[FPSTR(HUPIP)]);
    if (root[FPSTR(HUBPORT)]) hubPort = root[FPSTR(HUBPORT)];
    if (root[FPSTR(DEVICENAME)]) { 
      String newDeviceName = root[FPSTR(DEVICENAME)];
      deviceName = newDeviceName;
    }
    saveAppConfig(settingsJSON);
    client.println(F("HTTP/1.1 204 No Content"));
    client.println();
    client.println();
  }
  else {
    client.println(F("HTTP/1.1 204 No Content"));
    client.println();
    client.println();
  }

  delay(1);
  //stopping client
  client.stop();
}

// send data
int sendNotify() //client function to send/receieve POST data.
{
  int returnStatus = 1;
  if (client.connect(hubIp, hubPort)) {
    client.println(F("POST / HTTP/1.1"));
    client.print(F("HOST: "));
    client.print(hubIp);
    client.print(F(":"));
    client.println(hubPort);
    sendJSONData(client);
    //Serial.println(F("Pushing new values to hub..."));
    if(currentClosedDoorState == 0){ 
      Serial.println(F("Pushing 2 to hub..."));
    }else if(currentOpenDoorState == 0){
      Serial.println(F("Pushing 1 to hub..."));
    }else{
      Serial.println(F("Pushing 0 to hub..."));
    }
  }
  else {
    //connection failed
    returnStatus = 0;
    Serial.println(F("Connection to hub failed."));
  }

  // read any data returned from the POST
  while(client.connected() && !client.available()) delay(1); //waits for data
  while (client.connected() || client.available()) { //connected or data available
    char c = client.read();
  }

  delay(1);
  client.stop();
  return returnStatus;
}

void closedContactChanged() {
  if((long)(micros() - last_micros) >= debounceDelay * 1000) {
    currentClosedDoorState = digitalRead(CLOSED_DOOR_PIN);
    if (currentClosedDoorState != oldClosedDoorState) {
      sendUpdate = true;
    }
    last_micros = micros();
  }
}

void openContactChanged() {
  if((long)(micros() - last_micros) >= debounceDelay * 1000) {
    currentOpenDoorState = digitalRead(OPEN_DOOR_PIN);
    if (currentOpenDoorState != oldOpenDoorState) {
      sendUpdate = true;
    }
    last_micros = micros();
  }
}

void loop() {
  if (sendUpdate) {
    if (hubIp != INADDR_NONE && sendNotify()) {
      // update old sensor state after weâ€™ve sent the notify
      oldClosedDoorState = currentClosedDoorState;
      oldOpenDoorState = currentOpenDoorState;
    }
    sendUpdate = false;
  }
  ArduinoOTA.handle();
  // Handle any incoming requests
  WiFiClient client = server.available();
  if (client) {
    handleRequest(client);
  }  
}
