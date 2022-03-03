#include <Arduino.h>
//times are in microseconds
#define DATAPIN  D1  // RF input pin. should be able to attach interrupt. internal pin 4
#define DATAGRAM 40  // total number of bits to receive
#define LEDPIN D4     //embedded led internal pin 2. pulled up internally
#define RESETPIN D7  //reset settings button. internal pin 13

#define DEBUG true      //enable debugging
//#define DEBUG433 true   //enable debugging for RF signal

#include <LittleFS.h>             //LittleFS support (replaces SPIFFS)
#include <ESP8266WiFi.h>          //esp8266 wifi support required for mqtt client
#include <PubSubClient.h>         //mqtt client
#include <DNSServer.h>            //mdns server - required for wifiManager
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>     //web server - required for wifiManager
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

ESP8266WebServer webserver(80); //web server
WiFiClient espClient;
PubSubClient mqtt_client(espClient); //mqtt client

bool datagram[DATAGRAM]; //datagram array to receive weather sensor data

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[64] = "";
char mqtt_port[6] = "1883";
char mqtt_topic[64] = "";
char admin_pass[23] = "p4ssw0rd"; //defaul admin password

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
  StaticJsonDocument<256> json; //create JSON buffer to write config
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_topic"] = mqtt_topic;
  json["admin_pass"] = admin_pass;
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
  delay(100);
  configFile.close();
  //end save
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
      StaticJsonDocument<256> json; //create json buffer to store config in memory
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
      } else {
        #if DEBUG
        Serial.println(error.c_str());
        Serial.println("failed to load json config");
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

void mqtt_connect() {  //mqtt server connection function
  int failedattempts=0;
  while (!mqtt_client.connected()) { // Loop until we're reconnected
    char clientId[20];// Create a client ID from Chip ID
    sprintf(clientId,"Weather433-%08x",ESP.getChipId());
    #if DEBUG
    Serial.print("Attempting MQTT connection with ClientID: "); Serial.println(clientId);
    #endif
    // Attempt to connect
    if (mqtt_client.connect(clientId)) {
      #if DEBUG
      Serial.println("MQTT connected");
      #endif
    } else if (failedattempts<5) {
      #if DEBUG
      Serial.print("Failed, rc=");
      Serial.println(mqtt_client.state());
      Serial.println("Will try again in 2 seconds");
      #endif
      failedattempts++;
      delay(2000);      // Wait 2 seconds before retrying
    } else {
      #if DEBUG
      Serial.print("MQTT server is unreachable, restarting...");
      #endif
      delay(100);
      ESP.reset(); // reset
    }
  }
}

void send_datagram(){
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
  Serial.print("address:"); Serial.print(addr,HEX);
  Serial.print(" channel:"); Serial.print(chan,BIN);
  Serial.print(" tempF\":"); Serial.print(tempF);
  Serial.print(" tempC\":"); Serial.print(tempC);
  Serial.print(" humidity:"); Serial.print(humidint,DEC);
  Serial.print(" battery:");Serial.println(datagram[13]);
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
    Serial.print("Publish message: "); Serial.println(msg);
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
  strcpy(oldmsg,msg);
}

IRAM_ATTR void isrhandler() {
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
      if (duration>7000 && duration<9000){ // potentially new data during receiving datagram
        #if DEBUG433
        Serial.print("new_new_data ");
        Serial.println(duration);
        #endif
        index=0; //reset index as it seem as a new packet
      }
      else if (duration>3200 && duration<4900){ //received 1
        #if DEBUG433
        Serial.print("1 ");
        Serial.println(duration);
        #endif
        datagram[index]=1;
        index++;
      }
      else if (duration>1300 && duration<2300){ //received 0
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
      if (duration>7000 && duration<9000){ //new datagram preambula pause ~8ms
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
    if (receiving && (duration<400 || duration>900)){ //out of sync
      #if DEBUG433
      Serial.print("error_sep ");
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
    send_datagram(); //process and send datagram
    index=0;
    receiving=false;
    attachInterrupt(digitalPinToInterrupt(DATAPIN), isrhandler, CHANGE);
  }
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
  //Define default topic name to include device name
  sprintf(mqtt_topic,"Weather433-%08x",ESP.getChipId());
  //read configuration from FS json
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

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length  
  WiFiManagerParameter custom_admin_pass("password", "Admin password", admin_pass, 23);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server IP address", mqtt_server, 63);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 5);
  WiFiManagerParameter custom_mqtt_topic("topic", "Sensor name", mqtt_topic, 63);
  
  //WiFiManager
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
  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  if (no_config_file) {
    wifiManager.resetSettings(); //reset wifi settings if no config file was found.
  }
  //add all custom parameters
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_topic);
  wifiManager.addParameter(&custom_admin_pass);

  //reset wifi manager settings if sensor reset button pressed while booting
  if (digitalRead(RESETPIN)==0){
    #if DEBUG
    Serial.println("Config button pressed, starting on-demand autoconnect");
    #endif
    //wifiManager.resetSettings();
    while (digitalRead(RESETPIN)==0) {} //loop until sensor reset button is released, then reset.
    //ESP.eraseConfig();
    if (wifiManager.startConfigPortal(mqtt_topic, "p4ssw0rd")) { //start on-demand config portal
      if (shouldSaveConfig) {
        saveConfigFile();
      }
      delay(100);
    }
    ESP.reset(); //reset after manual config portal is exited. do not restart if exit is pressed.
  }
  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  #if DEBUG
  Serial.println("Starting wifiManager");
  #endif
  if (!wifiManager.autoConnect(mqtt_topic, "p4ssw0rd")) {
    #if DEBUG
    Serial.println("Failed to connect and hit timeout");
    #endif
    delay(100);
    ESP.reset(); //reset and try again
  }

  //if you get here you have connected to the WiFi
  #if DEBUG
  Serial.println("Connected to Wifi.");
  digitalWrite(LEDPIN,HIGH); //turn off led when connection is established
  #endif
  //read parameters from saved/updated config
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());

  //save the custom parameters to LittleFS
  if (shouldSaveConfig) {
    saveConfigFile();
  }
  #if DEBUG
  Serial.print("Assigned IP address: ");
  Serial.println(WiFi.localIP());
  #endif
  mqtt_client.setServer(mqtt_server, atoi(mqtt_port)); //set mqtt server parameters
  if (!mqtt_client.connected()) {
    mqtt_connect(); //connect to mqtt server
  }
  
  #if DEBUG
  Serial.println("Starting OTA server");
  #endif
  ArduinoOTA.begin();

  webserver.on("/", []() {
    if (!webserver.authenticate("admin", admin_pass)) {
      return webserver.requestAuthentication();
    }
    webserver.send(200, "text/plain", "Login OK");
  });
  #if DEBUG
  Serial.println("Starting Web server");
  #endif
  webserver.begin();
  #if DEBUG
  Serial.print("Web server is listening on http://");
  Serial.println(WiFi.localIP());
  #endif
  
  #if DEBUG
  Serial.println("Starting RF433 reception");
  #endif
  attachInterrupt(digitalPinToInterrupt(DATAPIN), isrhandler, CHANGE); // attach listening interrupt and start receiving
}

void loop() {
  if (!mqtt_client.connected()) {
    mqtt_connect(); //connect to mqtt server
  }
  mqtt_client.loop(); //do a mqtt client loop routine
  ArduinoOTA.handle(); //do an OTA loop routine
  webserver.handleClient(); //do a web server loop routine
}
