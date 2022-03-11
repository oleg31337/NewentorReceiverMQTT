#include <Arduino.h>

//Defining the time intervals to decode the signal. Times are in microseconds
#define NEWDATA_MIN 7000 //preamble min
#define NEWDATA_MAX 9000 //preamble max
#define ONE_MIN 3200 // 1 min
#define ONE_MAX 4900 // 1 max
#define ZERO_MIN 1200 // 0 min
#define ZERO_MAX 2400 // 0 max
#define PULSE_MIN 400 // pulse min
#define PULSE_MAX 900 // pulse max

#define DATAPIN D1  // RF input pin. should be able to attach interrupt. internal pin 4
#define DATAGRAM 40  // total number of bits to receive
#define LEDPIN D4     //embedded led internal pin 2. pulled up internally
#define RESETPIN D7  //reset settings button. internal pin 13

#define DEBUG true      //enable debugging
//#define DEBUG433 true   //enable debugging for RF signal

#include <LittleFS.h>             //LittleFS support (replaces SPIFFS)
#include <ESP8266WiFi.h>          //esp8266 wifi support required for mqtt client
#include <PubSubClient.h>         //mqtt client https://github.com/knolleary/pubsubclient
#include <DNSServer.h>            //dns server - required for wifiManager captive portal
#include <ESP8266mDNS.h>          //mdns server - for web interface
#include <ArduinoOTA.h>           //ota upgrades support (flash over the wifi)
#include <ESP8266WebServer.h>     //web server - required for wifiManager
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

WiFiClient espClient; //wifi client for mqtt
PubSubClient mqtt_client(espClient); //mqtt client
ESP8266WebServer webserver(80); //web server on port 80

bool datagram[DATAGRAM]; //datagram array to receive weather sensor data

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[65] = "";
char mqtt_port[6] = "1883";
char mqtt_topic[65] = "NewentorReceiver433"; //default topic name
char admin_username[6] = "admin"; //default admin username
char admin_pass[23] = "p4ssw0rd"; //default admin password for wifi ap, web interface, and OTA
char hostname[33] = "NewentorReceiver433"; //default mDNS hostname

bool shouldSaveConfig = false;//flag for saving data
bool no_config_file = false; //flag for not finding config file

//callback notifying us of the need to save config
void saveConfigCallback () {
  #if DEBUG
  Serial.println("Should save config");
  #endif
  shouldSaveConfig = true;
}

void saveConfigFile() {
  #if DEBUG
  Serial.println("Saving config");
  #endif
  StaticJsonDocument<512> json; //create JSON buffer to write config
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_topic"] = mqtt_topic;
  json["admin_pass"] = admin_pass;
  json["hostname"] = hostname;
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    #if DEBUG
    Serial.println("Failed to open config file for writing");
    #endif
  }
  if (serializeJson(json, configFile)==0) {
    #if DEBUG
    Serial.println("Failed to write config file");
    #endif
  }
  #if DEBUG
  serializeJson(json, Serial);
  Serial.println();
  #endif
  configFile.close();
}

void loadConfigFile() {
  if (LittleFS.exists("/config.json")) {
    //file exists, reading and loading
    #if DEBUG
    Serial.println("Reading config file");
    #endif
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      #if DEBUG
      Serial.println("Opened config file");
      size_t size = configFile.size();
      Serial.print("Config file size: ");
      Serial.println(size);
      #endif
      StaticJsonDocument<512> json; //create json buffer to store config in memory
      DeserializationError error = deserializeJson (json,configFile);
      serializeJson(json, Serial);
      if (!error) {
        #if DEBUG
        Serial.println("\nparsed json");
        #endif
        strcpy(mqtt_server, json["mqtt_server"]);
        strcpy(mqtt_port, json["mqtt_port"]);
        strcpy(mqtt_topic, json["mqtt_topic"]);
        strcpy(admin_pass, json["admin_pass"]);
        strcpy(hostname, json["hostname"]);
      } else {
        #if DEBUG
        Serial.println(error.c_str());
        Serial.println("failed to load json config");
        no_config_file = true; //set the global flag that config file not found or corrupt and needs to be re-written
        #endif
      }
      configFile.close();
    }
  } else {
    #if DEBUG
    Serial.println("Config file not found. Forcing configuration page");
    #endif
    no_config_file = true; //set the global flag that config file not found
  }
}

double convertFtoC(double f) { return round(((f-32.0)*0.55555)*10)/10.0; } //convert F to C and round to 1

void mqttConnect() {  //mqtt server connection function. Will re-try every 5 seconds
  static unsigned long lastconnectattempt=0; // last time tried to connect
  unsigned long timesincelastattempt=millis()-lastconnectattempt; //time since last attempt
  if (timesincelastattempt>5000 || lastconnectattempt==0){
    #if DEBUG
    Serial.print("Connecting to MQTT server ");
    Serial.println(mqtt_server);
    #endif
    if (mqtt_client.connect(hostname)) { //connect to MQTT broker using hostname
      #if DEBUG
      Serial.println("MQTT connected");
      #endif
    } else {
        #if DEBUG
        Serial.print("Failed to connect to MQTT, rc=");
        Serial.println(mqtt_client.state());
        Serial.println("Will try again in 5 seconds");
        #endif
        lastconnectattempt=millis();
    }
  }
}

void sendDatagram(){
  #if DEBUG || DEBUG433
  digitalWrite(LEDPIN,LOW); //DEBUG: turn on led until datagram is sent to mqtt
  #endif
  char msg[128]=""; //mqtt message json
  static char oldmsg[128]="z"; //previous mqtt message json
  byte h=0;
  unsigned int t=0;
  byte addr=0;
  static unsigned long lasttime=0; //last millis time message received
  long lastmsgtime=millis()-lasttime; //time in ms since last message
  lasttime=millis();
  
  for (int i=0;i<DATAGRAM;i++){
    #if DEBUG433
    Serial.print(datagram[i]);
    Serial.print(" ");
    #endif
    if (i>=0 && i<8){
      bitWrite(addr,7-(i),datagram[i]);
    }
    if (i>15 && i<28){
      bitWrite(t,11-(i-16),datagram[i]);
    }
    if (i>27 && i<36){
      bitWrite(h,7-(i-28),datagram[i]);
    }
  }
  double tempF=(t-900)/10.0;
  double tempC=convertFtoC(tempF);
  char humid[3]="";
  sprintf(humid,"%02x",h);
  int humidint=atoi(humid);
  char address[3]="";
  sprintf(address,"%02x",addr);
  byte chan=byte(datagram[39])+byte(datagram[38])*2;
  #if DEBUG || DEBUG433
  Serial.println();
  Serial.print("addr:"); Serial.print(addr,HEX);
  Serial.print(" ch:"); Serial.print(chan,BIN);
  Serial.print(" tempF:"); Serial.print(tempF);
  Serial.print(" tempC:"); Serial.print(tempC);
  Serial.print(" humid:"); Serial.print(humidint,DEC);
  Serial.print(" batt:");Serial.println(datagram[13]);
  #endif
  StaticJsonDocument<128> json; //create JSON buffer to send MQTT message
  json["SensorAddress"]=address;
  json["Channel"]=chan;
  json["TemperatureF"]=tempF;
  json["TemperatureC"]=tempC;
  json["Humidity"]=humidint;
  json["BatteryLow"]=datagram[13];
  serializeJson(json, msg); //convert JSON object to char array
  if (strcmp(msg,oldmsg)!=0 || lastmsgtime>6000){ //not the same message as before and not received within 6 sec
    #if DEBUG
    Serial.print("MQTT topic: "); Serial.println(mqtt_topic);
    Serial.print("Publishing message: "); Serial.println(msg);
    #endif
    if (!mqtt_client.publish(mqtt_topic, msg, false)){
      #if DEBUG
      Serial.println ("Failed to publish message. Retrying...");
      #endif
      if (!mqtt_client.publish(mqtt_topic, msg, false)){
        #if DEBUG
        Serial.println ("Failed to publish message second time.");
        #endif
      }
    }
  }
  else {
    #if DEBUG
    Serial.println ("Same datagram received, skipping.");
    #endif
  }
  strcpy(oldmsg,msg); // copy message to old message

  #if DEBUG || DEBUG433
  digitalWrite(LEDPIN,HIGH); //DEBUG: turn off led when datagram is sent
  #endif
}

IRAM_ATTR void interruptHandler() {
  static unsigned long duration = 0;
  static unsigned long lastTime = 0;
  static unsigned int index = 0;
  static bool receiving = false;
  bool state=digitalRead(DATAPIN);
  unsigned long time = micros();
  duration = time - lastTime;
  lastTime = time;
  
  if (state){//rising edge
    if (receiving){
      if (duration>NEWDATA_MIN && duration<NEWDATA_MAX){ // potentially new data during receiving datagram
        #if DEBUG433
        Serial.print("new_new_data ");
        Serial.println(duration);
        #endif
        index=0; //reset index as it seem as a new packet
      }
      else if (duration>ONE_MIN && duration<ONE_MAX){ //received 1
        #if DEBUG433
        Serial.print("1 ");
        Serial.println(duration);
        #endif
        datagram[index]=1;
        index++;
      }
      else if (duration>ZERO_MIN && duration<ZERO_MAX){ //received 0
        #if DEBUG433
        Serial.print("0 ");
        Serial.println(duration);
        #endif
        datagram[index]=0;
        index++;
      }
      else { //intervals do not match
        #if DEBUG433
        Serial.print("error_length ");
        Serial.println(duration);
        #endif
        index=0;
        receiving=false;
      }
    }
    else { //not receiving yet
      if (duration>NEWDATA_MIN && duration<NEWDATA_MAX){ //new datagram preambula pause ~8ms
        #if DEBUG433
        Serial.print("new_data ");
        Serial.println(duration);
        #endif
        index=0;
        receiving = true;
      }
    }
  }
  else { //falling edge
    if (receiving && (duration<PULSE_MIN || duration>PULSE_MAX)){ //out of sync
      #if DEBUG433
      Serial.print("error_pulse ");
      Serial.println(duration);
      #endif
      index=0;
      receiving=false;
    }
    else if (receiving) {
      #if DEBUG433
      Serial.print("sep ");
      Serial.println(duration);
      #endif
    }
  }
  if (index==DATAGRAM){
    detachInterrupt(digitalPinToInterrupt(DATAPIN)); //stop interrupt to prevent receiving in the middle of processing of the datagram
    if (mqtt_client.connected()){ //if MQTT is connected then send datagram
      sendDatagram(); //process and send datagram
    }
    else {
      #if DEBUG
      Serial.println("MQTT client is not connected!");
      #endif
    }
    index=0;
    receiving=false;
    attachInterrupt(digitalPinToInterrupt(DATAPIN), interruptHandler, CHANGE);
  }
}

void wifiManagerInit() {
  //WiFiManager initialization
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length  
  WiFiManagerParameter custom_admin_pass("password", "Admin password", admin_pass, 24);
  WiFiManagerParameter custom_admin_pass_text("<br>Admin password:");
  WiFiManagerParameter custom_hostname("hostname", "mDNS hostname", hostname, 64);
  WiFiManagerParameter custom_hostname_text("<br>Hostname:");
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server IP address", mqtt_server, 64);
  WiFiManagerParameter custom_mqtt_server_text("<br>MQTT server IP address:");
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 5);
  WiFiManagerParameter custom_mqtt_port_text("<br>MQTT port:");
  WiFiManagerParameter custom_mqtt_topic("topic", "Sensor name", mqtt_topic, 64);
  WiFiManagerParameter custom_mqtt_topic_text("<br>MQTT topic:");
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  //set various settings for wifi manager
  #if DEBUG
  wifiManager.setDebugOutput(true); //debug output to Serial
  #else
  wifiManager.setDebugOutput(false); //debug output to Serial
  #endif
  wifiManager.setMinimumSignalQuality(5); // set minimum wifi quality to 5%
  wifiManager.setRemoveDuplicateAPs(true); //remove duplicate ssid's from the list
  wifiManager.setTimeout(600); //set wifi manager timeout to 10 minutes
  wifiManager.setSaveConfigCallback(saveConfigCallback); //set config save notify callback
  if (no_config_file) { wifiManager.resetSettings(); } //reset wifi settings if no config file was found.
  //add all custom parameters
  wifiManager.addParameter(&custom_hostname_text);
  wifiManager.addParameter(&custom_hostname);
  wifiManager.addParameter(&custom_admin_pass_text);
  wifiManager.addParameter(&custom_admin_pass);
  wifiManager.addParameter(&custom_mqtt_server_text);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port_text);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_topic_text);
  wifiManager.addParameter(&custom_mqtt_topic);
  
  //reset wifi manager settings if settings reset button pressed while booting
  if (digitalRead(RESETPIN)==0){
    digitalWrite(LEDPIN,LOW); //turn on led to show that settings will reset
    #if DEBUG
    Serial.println("Config button pressed, starting on-demand autoconnect");
    #endif
    //wifiManager.resetSettings();
    while (digitalRead(RESETPIN)==0) {} //loop until sensor reset button is released, then reset.
    //ESP.eraseConfig();
    if (wifiManager.startConfigPortal(mqtt_topic, admin_pass)) { //start on-demand config portal
      if (shouldSaveConfig) { saveConfigFile(); } //save config if required
    }
    #if DEBUG
    Serial.println("Configuration complete. Restarting with new parameters.");
    #endif
    digitalWrite(LEDPIN,HIGH); //turn off led
    delay(100);
    ESP.reset(); //reset after manual config portal is exited. do not restart if exit is pressed.
  }
  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  #if DEBUG
  Serial.println("Starting wifiManager");
  #endif
  if (!wifiManager.autoConnect(mqtt_topic, admin_pass)) {
    #if DEBUG
    Serial.println("Failed to connect and hit timeout. Restarting.");
    #endif
    delay(100);
    ESP.reset(); //reset and try again
  }

  //if you get here you have connected to the WiFi
  #if DEBUG
  Serial.println("Connected to Wifi.");
  digitalWrite(LEDPIN,HIGH); //turn off led when connection is established
  #endif
  //read back parameters from saved/updated config
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());
  strcpy(admin_pass, custom_admin_pass.getValue());
  strcpy(hostname, custom_hostname.getValue());
  
  if (shouldSaveConfig) {//save the custom parameters to LittleFS if requested
    saveConfigFile();
  }

  #if DEBUG
  Serial.print("Assigned IP address: ");
  Serial.println(WiFi.localIP());
  #endif
}

void handleWebNotFound() {
  String uri = ESP8266WebServer::urlDecode(webserver.uri());
  String message = "404 Not Found\n\n";
  message += "URI: ";
  message += uri;
  webserver.send(404, "text/plain", message);
}

void handleWebRoot() {
  #if DEBUG
  Serial.println("Web GET request /");
  #endif
  webserver.send(200, "text/html", F("<html><h2>NewentorReceiver433</h2>\
  <p><input type=\"button\" value=\"Configuration\" onclick=\"window.location.replace('/config')\"></p>\
  </html>"));
}

void handleWebConfig() {
  #if DEBUG
  Serial.println("Web GET request /configure");
  #endif
  if (!webserver.authenticate(admin_username, admin_pass)) { //check for authentication
    return webserver.requestAuthentication(); // request authentication
  }
  char response[961];
  sprintf(response, "<html><h2>NewentorReceiver433 configuraion</h2><form action=\"/save\" method=\"post\" enctype=\"application/x-www-form-urlencoded\">\
  <table style=\"border: 0px\">\
  <tr><td>mDNS hostname:</td><td><input type=\"text\" name=\"hostname\" value=\"%s\"></td></tr>\
  <tr><td>Admin password:</td><td><input type=\"password\" name=\"admin_pass\" value=\"%s\"></td></tr>\
  <tr><td>MQTT server IP:</td><td><input type=\"text\" name=\"mqtt_server\" value=\"%s\"></td></tr>\
  <tr><td>MQTT port:</td><td><input type=\"text\" name=\"mqtt_port\" value=\"%s\"></td></tr>\
  <tr><td>MQTT topic:</td><td><input type=\"text\" name=\"mqtt_topic\" value=\"%s\"></td></tr>\
  <tr><td><input type=\"submit\" value=\"Save\"></td><td align=\"right\"><input type=\"button\" value=\"Cancel\" onclick=\"window.location.replace('/')\"></td></tr>\
  </table></form></html>",hostname,admin_pass,mqtt_server,mqtt_port,mqtt_topic);
  webserver.send(200, "text/html", response);
}

/* void handleWebGetparams() {
  #if DEBUG
  Serial.println("Web GET request /getparams");
  #endif
  if (!webserver.authenticate(admin_username, admin_pass)) { //check for authentication
    return webserver.requestAuthentication(); // request authentication
  }
  char response[267];
  sprintf(response, "{\"hostname\":\"%s\",\"admin_pass\":\"%s\",\"mqtt_server\":\"%s\",\"mqtt_port\":\"%s\";\"mqtt_topic\":\"%s\"}",
  hostname,admin_pass,mqtt_server,mqtt_port,mqtt_topic);
  webserver.send(200, "application/json", response);
} */

void handleWebSave() {
  #if DEBUG
  Serial.println("Web POST request /");
  #endif
  if (!webserver.authenticate(admin_username, admin_pass)) { //check for authentication
    Serial.println("NOT AUTHENTICATED FOR POST!");
    return webserver.requestAuthentication(); // request authentication
  }
  #if DEBUG
  String arguments="POST arguments:"+String(webserver.args())+"\n";
  #endif
  for (int i=0; i<webserver.args();i++){
    if (webserver.argName(i)=="mqtt_server") {webserver.arg(i).toCharArray(mqtt_server,65);}
    if (webserver.argName(i)=="mqtt_port") {webserver.arg(i).toCharArray(mqtt_port,6);}
    if (webserver.argName(i)=="mqtt_topic") {webserver.arg(i).toCharArray(mqtt_topic,65);}
    if (webserver.argName(i)=="admin_pass") {webserver.arg(i).toCharArray(admin_pass,23);}
    if (webserver.argName(i)=="hostname") {webserver.arg(i).toCharArray(hostname,33);}
    #if DEBUG
    arguments+=webserver.argName(i) + ": " + webserver.arg(i) + "\n";
    #endif
  }
  saveConfigFile(); //save config file
  #if DEBUG
  Serial.println(arguments);
  String uri = ESP8266WebServer::urlDecode(webserver.uri());
  Serial.println(uri);
  #endif
  webserver.send(200, "text/html", F("<html><h3>Settings saved successfully!<br>Restarting...</h3><script>setTimeout(function(){window.location.replace(\"/\")},3000)</script></html>"));
  for (int j=0;j<2000;j++){
    webserver.handleClient(); //do a web server loop routine for ~2 seconds
    delay(1);
  }  
  ESP.reset();
}

void setup() {
  #if DEBUG || DEBUG433
  Serial.begin(1000000); //using maxumum available speed of uart to reduce delay in the interrupt routines.
  Serial.println("\nStarted\n");
  #endif
  pinMode(LEDPIN,OUTPUT); //enable builtin led
  pinMode(DATAPIN, INPUT); //enable data input pin
  pinMode(RESETPIN, INPUT_PULLUP); //enable configuration clear button
  digitalWrite(LEDPIN,HIGH); //turn off built in led
  #if DEBUG
  digitalWrite(LEDPIN,LOW); //DEBUG: turn on led until wifi connection is established
  #endif
  
  //initialize LittleFS and read config file
  #if DEBUG
  Serial.println("Mounting LittleFS...");
  #endif
  if (LittleFS.begin()) { //initialize the LittleFS file system
    #if DEBUG
    Serial.println("Mounted file system");
    #endif
    loadConfigFile(); //read config file from LittleFS
  }
  else {
    #if DEBUG
    Serial.println("Failed to mount LittleFS. Trying to format...");
    #endif
    LittleFS.format();
    delay(100);
    #if DEBUG
    Serial.println("Resetting...");
    #endif
    ESP.reset();
  }
  //end read config file

  wifiManagerInit(); // run the WiFiManager
  
  //////////////////////////////// MQTT client connect
  mqtt_client.setServer(mqtt_server, atoi(mqtt_port)); //set mqtt server parameters
  if (!mqtt_client.connected()) {
    mqttConnect(); //connect to mqtt server
  }
  
  //////////////////////////////// OTA server
  #if DEBUG
  Serial.println("Starting OTA server");
  #endif
  ArduinoOTA.setHostname(hostname); //set OTA host name
  ArduinoOTA.begin(); // begin OTA routines
  
  /////////////////////////////// mDNS server
  #if DEBUG
  Serial.print("Starting mDNS service with hostname: ");
  Serial.print(hostname);
  Serial.println(".local");
  #endif
  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    #if DEBUG
    Serial.println("mDNS service started");
    #endif
  }
  else {
    #if DEBUG
    Serial.println("mDNS service failed to start!");
    #endif
  }

  /////////////////////////////// Web server
  webserver.on("/", HTTP_GET, handleWebRoot);
  //webserver.on("/getparams", HTTP_GET, handleWebGetparams);
  webserver.on("/config", HTTP_GET, handleWebConfig);
  webserver.on("/save", HTTP_POST, handleWebSave);
  webserver.onNotFound(handleWebNotFound);
  #if DEBUG
  Serial.println("Starting Web server");
  #endif
  webserver.begin();
  #if DEBUG
  Serial.print("Web server is listening on http://");
  Serial.println(WiFi.localIP());
  #endif
  
  /////////////////////////////  RF-433 reception
  #if DEBUG
  Serial.println("Starting RF433 reception");
  #endif
  attachInterrupt(digitalPinToInterrupt(DATAPIN), interruptHandler, CHANGE); // attach RF listening interrupt and start receiving
}

void loop() {
  if (!mqtt_client.connected()) {
    mqttConnect(); //reconnect to mqtt server if it gets disconnected
  }
  mqtt_client.loop(); //do a mqtt client loop routine
  ArduinoOTA.handle(); //do an OTA loop routine
  webserver.handleClient(); //do a web server loop routine
}
