#include <Arduino.h>

#include <ESP8266WiFi.h> 
#include <ESP8266mDNS.h>
#include <IRremoteESP8266.h>
#include <ArduinoOTA.h>
//#include <PubSubClient.h>
#include <IRsend.h>
#include <ESP8266WebServer.h>             // WifiManager, Webserver
#include <FS.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#include <set>

#define IR_PIN D5


String devName("ChromecastHelper-");

WiFiManager wifiManager;
MDNSResponder::hMDNSServiceQuery hServiceQuery  = nullptr;
ESP8266WebServer webServer(80);
IRsend irsend(IR_PIN);
std::set<String> setChromecastNames;
String currentChromecastName("");
bool currentChromecastIsCasting = false;
bool turnTVOn  = true;
bool turnTVOff = true;

// Reverse the order of bits in a byte
// Example: 01000000 -> 00000010
unsigned char ReverseByte(unsigned char b)
{
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return(b);
}

// Calculate 32 bit NECx code
unsigned long GenNECXCode(unsigned char p_Device, unsigned char p_SubDevice, unsigned char p_Function)
{
  unsigned long ReverseDevice = (unsigned long)ReverseByte(p_Device);
  unsigned long ReverseSubDevice = (unsigned long)ReverseByte(p_SubDevice);
  unsigned long ReverseFunction = (unsigned long)ReverseByte(p_Function);
  return((ReverseDevice << 24) | (ReverseSubDevice << 16) | (ReverseFunction << 8) | ((~ReverseFunction) & 0xFF));
}

#define SAMSUNG_POWER_ON  GenNECXCode(7,7,153)
#define SAMSUNG_POWER_OFF  GenNECXCode(7,7,152)


void serviceQueryCallback(const MDNSResponder::MDNSServiceInfo& mdnsServiceInfo, MDNSResponder::AnswerType answerType, bool p_bSetContent)
{

  MDNSResponder::MDNSServiceInfo mdnsServiceInfo1 = mdnsServiceInfo;

  const char* serviceName = mdnsServiceInfo1.serviceDomain();

  if (String(serviceName).startsWith("Chromecast") && mdnsServiceInfo1.txtAvailable())
  {
    MDNSResponder::MDNSServiceInfo::KeyValueMap kvm = mdnsServiceInfo1.keyValues();
    bool hasCastingKey = false;
    bool isCurrentChromecast = false;
    bool isCasting = false;
    for(auto kv : kvm)
    {
      if (0 == strcmp(kv.first,"fn"))
      {
        if (currentChromecastName == kv.second)
          isCurrentChromecast = true;
        setChromecastNames.insert(kv.second);
      }
      else if (0 == strcmp(kv.first,"rs"))
      {
        hasCastingKey = true;
        if ('\0' != kv.second)
        {
          isCasting = true;
        }
        else
        {
          isCasting = false;
        }
        
      }
    }  
    if (isCurrentChromecast && hasCastingKey)
    {
      currentChromecastIsCasting = isCasting;
    }  
  }
}

// from https://tttapa.github.io/ESP8266/Chap11%20-%20SPIFFS.html
String getContentType(String filename) { // convert the file extension to the MIME type
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed verion
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = webServer.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    Serial.println(String("\tSent file: ") + path + String(", ") + String(sent,DEC) + String(" bytes") );
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);   // If the file doesn't exist, return false
  return false;
}

File fsUploadFile;              // a File object to temporarily store the received file
void handleFileUpload(){ // upload a new file to the SPIFFS
  HTTPUpload& upload = webServer.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    Serial.print("handleFileUpload Name: "); Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");            // Open the file for writing in SPIFFS (create if it doesn't exist)
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile) {                                    // If the file was successfully created
      fsUploadFile.close();                               // Close the file again
      Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
      webServer.sendHeader("Location","/upload_success");      // Redirect the client to the success page
      webServer.send(303);
    } else {
      webServer.send(500, "text/plain", "500: couldn't create file");
    }
  }
}


void setup()
{
  Serial.begin(115200);
  pinMode(IR_PIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output

  devName += String(ESP.getChipId(),HEX);

  wifiManager.setConfigPortalTimeout(120);

  wifiManager.autoConnect(devName.c_str());


  //WiFi.disconnect();
  
  if (!MDNS.begin(devName))
  {
    Serial.println("Error setting up mDNS");
  }
  
  hServiceQuery = MDNS.installServiceQuery("googlecast", "tcp", &serviceQueryCallback);
  if (!hServiceQuery)
    Serial.println("Error installing service");

  ArduinoOTA.onStart([]() 
  {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
    {
      type = "sketch";
    }
    else
    { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]()
  {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
  {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error)
  {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
    {
      Serial.println("Auth Failed");
    }
    else if (error == OTA_BEGIN_ERROR)
    {
      Serial.println("Begin Failed");
    }
    else if (error == OTA_CONNECT_ERROR)
    {
      Serial.println("Connect Failed");
    }
    else if (error == OTA_RECEIVE_ERROR)
    {
      Serial.println("Receive Failed");
    }
    else if (error == OTA_END_ERROR)
    {
      Serial.println("End Failed");
    }
  });
  
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  /*
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  */
  SPIFFS.begin();                           // Start the SPI Flash Files System


  File fsConfig = SPIFFS.open("config", "r");
  if (fsConfig)
  {
    currentChromecastName = fsConfig.readString(); 
    Serial.println("Current Chromecast:" + currentChromecastName); 
    fsConfig.close();
  }
  


  //setup
  webServer.on("/", HTTP_GET, 
  []()
  {
    String msg("<html><head><title>Configure ChromecastHelper</title></head><body>");

/*
    <form method=\"post\" enctype=\"multipart/form-data\">
    <label>Chromecast Name</label>
    "<select><option>On/Off</option><option>On</option><option>Off</option></select>");
  wifiManager.addParameter(&paramTVMode);

    <input type=\"file\" name=\"name\"><input class=\"button\" type=\"submit\" value=\"Upload\">
    </form>");
*/
    msg += "<form method=post>";
    msg += "<label>Chromecast Name: </label>";
    msg += "<select name='chromecastName'>";
    for (String chromecastName : setChromecastNames)
    {
      msg += "<option>";
      msg += chromecastName;
      msg += "</option>";
    }
    msg += "</select>";

    msg += "<br/>";


    msg += "<label>Helper Mode: </label>";
    msg += "<select name='tvMode'>";
    msg += "<option value=1>Turn TV On and Off</option><option value=2>Turn TV On</option><option value=3>Turn TV Off</option>";
    msg += "</select>";
    msg += "<input class=\"button\" type=\"submit\" value=\"Save Settings\">";
    msg += "</form>";

    msg += String("</body></html>");
    webServer.send(200, "text/html", msg);
  });

  webServer.on("/", HTTP_POST, 
  []()
  {
    
    String msg("save new settings");
    if (webServer.hasArg("chromecastName"))
    {
      currentChromecastName = webServer.arg("chromecastName");
    }
    if (webServer.hasArg("tvMode"))
    {
      String mode = webServer.arg("tvMode");
      turnTVOn = false;
      turnTVOff = false;
      if (mode == "0")
      {
        turnTVOn = true;
        turnTVOff = true;
      }
      else if (mode == "1")
        turnTVOn = true;
      else
        turnTVOff = true;
    }

    File fsConfig = SPIFFS.open("config", "w");
    if (fsConfig)
    {
      fsConfig.write((const uint8_t*)currentChromecastName.c_str(), currentChromecastName.length());    
      fsConfig.close();
    }
    webServer.send(200, "text/html", msg);
  });

  webServer.on("/wifi-disconnect", 
  []()
  {
    WiFi.disconnect();
    ESP.restart();
    delay(10000);
  });

  webServer.on("/gennecxcode", 
  []()
  {
    if (webServer.hasArg("device") && webServer.hasArg("subdevice") && webServer.hasArg("function"))
    {
      uint8_t device=0,subdevice=0,function=0;
      for (int i = 0; i < webServer.args(); ++i)
      {
        String value = webServer.arg(i);
        if (webServer.argName(i) == "device")
        {
          device = atoi(value.c_str());

        }
        else if (webServer.argName(i) == "subdevice")
        {
          // convert int string to int
          subdevice = atoi(value.c_str());
        }
        else if (webServer.argName(i) == "function")
        {
          function = atoi(value.c_str());
        }
      }
      webServer.send(200, "text/plain", String(GenNECXCode(device,subdevice,function),HEX));
    }
    else
    {
      webServer.send(400, "text/plain", "Bad Request.");
    }
    
  });
  webServer.on("/samsung", 
  []()
  {
    if (webServer.hasArg("data") && webServer.hasArg("bits") && webServer.hasArg("repeat"))
    {
      uint32_t bits=32, repeat=0;
      uint64_t data64 = 0;
      for (int i = 0; i < webServer.args(); ++i)
      {
        String value = webServer.arg(i);
        if (webServer.argName(i) == "data")
        {
          char* endptr = NULL;
          uint32_t val = strtoul (value.c_str(), &endptr, 16);
          Serial.print("value in hex: ");
          Serial.println(val, HEX);
          data64 = val;
        }
        else if (webServer.argName(i) == "bits")
        {
          // convert int string to int
          bits = atoi(value.c_str());
        }
        else if (webServer.argName(i) == "repeat")
        {
          repeat = atoi(value.c_str());
        }
      }
      
      String msg("Good Request: data=" );
      msg += String((uint32_t)data64, HEX);
      msg += ", bits=" + String(bits,DEC);
      msg += ", repeat=" + String(repeat,DEC);

      webServer.send(200, "text/plain", msg);
      irsend.sendSAMSUNG(data64, bits, repeat);

    }
    else 
      webServer.send(400, "text/plain", "Bad Request.");
  });

  webServer.onNotFound(
  []()
  {
    if (!handleFileRead(webServer.uri()))
      webServer.send(404, "text/plain", "Not Found.");
  });

  webServer.on("/upload", HTTP_GET, []()
  {                 // if the client requests the upload page
    String msg("<form method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"><input class=\"button\" type=\"submit\" value=\"Upload\"></form>");
    webServer.send(200, "text/html", msg);

  });

  webServer.on("/upload_success", HTTP_GET, []()
  {                 // if the client requests the upload page
    String msg("upload success");
    webServer.send(200, "text/plain", msg);

  });

  webServer.on("/upload", HTTP_POST,                       // if the client posts to the upload page
    [](){ webServer.send(200); },                          // Send status 200 (OK) to tell the client we are ready to receive
    handleFileUpload                                     // Receive and save the file
  );


  webServer.begin();
}

uint32_t nextQueryTime   = 0;
uint32_t nextTVStateTime   = 0;

#define TV_OFF_DELAY_SECONDS 300
#define TV_ON_DELAY_SECONDS 0


enum TV_STATE
{
  TV_STATE_UNINITIALIZED,
  TV_STATE_DELAYED_OFF,
  TV_STATE_OFF,
  TV_STATE_DELAYED_ON,
  TV_STATE_ON,
};

TV_STATE tvState = TV_STATE_UNINITIALIZED;

void checkGoogleCastService()
{
  uint32_t timeNow = millis();

  if (!currentChromecastIsCasting)
  {
    if (tvState != TV_STATE_DELAYED_OFF && tvState != TV_STATE_OFF )
    {
      Serial.println("chromecast not casting");
      if (tvState == TV_STATE_DELAYED_ON)
        tvState = TV_STATE_OFF;
      else
      {
        tvState = TV_STATE_DELAYED_OFF;
        nextTVStateTime = timeNow  + TV_OFF_DELAY_SECONDS * 1000;
      }
    }
  }
  else
  {
    if (tvState != TV_STATE_ON && tvState != TV_STATE_DELAYED_ON)
    {
      Serial.println("chromecast casting");
      if (tvState == TV_STATE_DELAYED_OFF)
        tvState = TV_STATE_ON;
      else
      {
        tvState = TV_STATE_DELAYED_ON;
        nextTVStateTime = timeNow  + TV_ON_DELAY_SECONDS * 1000;
      }
    }
  }

}



void loop()
{
    checkGoogleCastService();

    if (tvState == TV_STATE_DELAYED_ON  && nextTVStateTime < millis())
    {
      //Serial.println("turn tv on");
      tvState = TV_STATE_ON;
      // turn on tv
      if (turnTVOn)
        irsend.sendSAMSUNG(SAMSUNG_POWER_ON,32, 5);
    }
    else if (tvState == TV_STATE_DELAYED_OFF && nextTVStateTime < millis())
    {
      //Serial.println("turn tv off");
      tvState = TV_STATE_OFF;
      // turn off tv
      if (turnTVOff)
        irsend.sendSAMSUNG(SAMSUNG_POWER_OFF,32, 5);
    }

    webServer.handleClient();
    MDNS.update();
    ArduinoOTA.handle();
}


/*
gen http://192.168.31.244/gennecxcode?device=7&subdevice=7&function=152
on  http://192.168.31.244/samsung?data=e0e09966&bits=32&repeat=1
off http://192.168.31.244/samsung?data=e0e019e6&bits=32&repeat=1
*/
