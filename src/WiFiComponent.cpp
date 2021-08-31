
// Basic headers
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Tweaked SDK configuration
#include "sdkconfig.h"

// Arduino includes
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ArduinoOTA.h>
#include "esp_sntp.h"

#include "parameter.h"

#include "WiFiComponent.h"
#include "led.h"

const char* worldNtpServer = "pool.ntp.org";
const char* eurNtpServer   = "europe.pool.ntp.org";
const char* nlNtpServer    = "nl.pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

// -----------------------------------------------
void WiFiComponent::wifiInfo(){
    WiFi.printDiag(telnetSerial);
    telnetSerial.println("SAVED: " + (String)wm.getWiFiIsSaved() ? "YES" : "NO");
    telnetSerial.println("SSID: " + (String)wm.getWiFiSSID());
    telnetSerial.println("PASS: " + (String)wm.getWiFiPass());
}

// -----------------------------------------------
void WiFiComponent::setup() {
    // Enable AP mode as well as STA, such that WM can create an access point if needed
    WiFi.mode(WIFI_MODE_APSTA);

    _hostname = String("ESP32_bt") + String(WIFI_getChipId(),HEX);
    
    // Load registered parameters from NVS
    loadParams();

    // Blocking if not connected WiFi AP
    setupConfigPortal();

    setupArduinoOTA();

    
    //configTime(gmtOffset_sec, daylightOffset_sec, nlNtpServer, eurNtpServer, worldNtpServer);
    // sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // sntp_setservername(0, nlNtpServer);
    // sntp_init();


}

// -----------------------------------------------
void WiFiComponent::loop() {
    SCOPED_STACK_ENTRY;
    do {
        ArduinoOTA.handle();
        feedLoopWDT();
        delay(10);
    } while(otaBusy);
    wm.process();
}

// -----------------------------------------------
void WiFiComponent::registerOtaStartCallback(std::function<void(void)> callback) {
    ota_callback_functions.push_back(callback);
}

// -----------------------------------------------
void WiFiComponent::registerParamSaveCallback(std::function<void(void)> callback) {
    paramSaved_callback_functions.push_back(callback);
}

// -----------------------------------------------
void WiFiComponent::setupConfigPortal() {
      
    wm.debugPlatformInfo();

    //reset settings - for testing
    // wm.resetSettings();
    // wm.erase();  

    // callbacks
    wm.setAPCallback(std::bind(&WiFiComponent::configModeCallback,this, std::placeholders::_1));
    wm.setWebServerCallback(std::bind(&WiFiComponent::bindServerCallback,this));
    wm.setSaveConfigCallback(std::bind(&WiFiComponent::saveWifiCallback,this));
    wm.setSaveParamsCallback(std::bind(&WiFiComponent::saveParamCallback,this));

    wm.setTitle("ESP32_bt");
    wm.setHostname(_hostname.c_str());

    // invert theme, dark
    wm.setDarkMode(true);

    // Show update button on info page
    wm.setShowInfoUpdate(true); 

    // Separate ParamsPage
    wm.setParamsPage(true);
    std::vector<const char *> menu = {"wifi","param","info","sep","erase","update","restart"};
    wm.setMenu(menu); // custom menu, pass vector


    //sets timeout until configuration portal gets turned off
    //useful to make it all retry or go to sleep in seconds
    wm.setConfigPortalTimeout(600); // 10 minutes

    // set connection timeout
    wm.setConnectTimeout(3);

    // set wifi connect retries
    wm.setConnectRetries(3);

    // connect after portal save toggle
    wm.setSaveConnect(true); // do not connect, only save

    wm.setWiFiAutoReconnect(true);

    String apName = String(WIFI_getChipId(),HEX);
    apName.toUpperCase();
    apName = "ESP32_bt_" + apName;

    //fetches ssid and pass and tries to connect
    //if it does not connect it starts an access point with the specified name
    //and goes into a blocking loop awaiting configuration
    // --> last parameter ensures a retry to the blocking loop to connect to known WiFi AP
    if(!wm.autoConnect(apName.c_str(),"abc45678", true)) {
        telnetSerial.println("failed to connect and hit timeout");
    }
    else {
        wm.setHttpdAuthEnable(true); 

        // Disable AP
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);

        //if you get here you have connected to the WiFi
        telnetSerial.println("connected...yeey :)");
        wm.startWebPortal();
    }

    wifiInfo();
}

// -----------------------------------------------
void WiFiComponent::setupArduinoOTA() {
    
    ArduinoOTA.setPort(3232);

    ArduinoOTA.setHostname("ESP32_BT");

    ArduinoOTA.setPassword("admin");

    ArduinoOTA
    .onStart([this]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
        else // U_SPIFFS
        type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        telnetSerial.println("Start updating " + type);

        for (auto& cb : ota_callback_functions) {
            cb();
        }

        // Start with 'blocking' ota loop:
        otaBusy = 1;
        led.set(OFF);
    })
    .onEnd([this]() {
        telnetSerial.println("\nEnd");
        otaBusy = 0;
        led.set(ON);
        // Succesful end will reset ESP itself
    })
    .onProgress([this](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));        
        // Have the led fade on 10 times
        led.set((progress / (total / 25))%255);
        
        feedLoopWDT();
    })
    .onError([this](ota_error_t error) {
        telnetSerial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) telnetSerial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) telnetSerial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) telnetSerial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) telnetSerial.println("Receive Failed");
        else if (error == OTA_END_ERROR) telnetSerial.println("End Failed");
        otaBusy = 0;
        led.set(16);
        
        // Reset in order to have a fresh start on required functionality...
        delay(100);
        ESP.restart();
    });

    ArduinoOTA.begin();
}

// -----------------------------------------------
void WiFiComponent::loadParams() {
    WiFiManagerParameter** params = wm.getParameters();
    uint16_t paramCount = wm.getParametersCount();
    for(uint16_t i = 0; i < paramCount; i++) {
        if(params[i]->getID()) {
            Parameter* p = static_cast<Parameter*>(params[i]);
            if(p) {
                p->initialize();
            }
        }
    }
}

// -----------------------------------------------
void WiFiComponent::storeParams() {
    WiFiManagerParameter** params = wm.getParameters();
    uint16_t paramCount = wm.getParametersCount();
    for(uint16_t i = 0; i < paramCount; i++) {
        if(params[i]->getID()) {
            Parameter* p = static_cast<Parameter*>(params[i]);
            if(p) {
                p->storeValue();
            }
        }
    }
}

// -----------------------------------------------
void WiFiComponent::addCustomHtmlPage(const char* path, customHtmlHandler_t handler) {
    custom_html_page_handlers.emplace_back(path, handler);
}

// -----------------------------------------------
void WiFiComponent::saveWifiCallback(){

}

// -----------------------------------------------
//gets called when WiFiManager enters configuration mode
void WiFiComponent::configModeCallback (WiFiManager *myWiFiManager) {
    // ToDo: stop Bluetooth processing!?
}

// -----------------------------------------------
void WiFiComponent::saveParamCallback(){
    storeParams();

    // Call registered callbacks to have them update the parameters
    for(auto& cb : paramSaved_callback_functions) {
        cb();
    }
    // wm.stopConfigPortal();
}

// -----------------------------------------------
void WiFiComponent::handleRoute(){
  wm.server->send(200, "text/plain", "hello from user code");
}

// -----------------------------------------------
void WiFiComponent::bindServerCallback(){
  wm.server->on("/custom",std::bind(&WiFiComponent::handleRoute,*this));
  // wm.server->on("/info",handleRoute); // you can override wm!
  
  for(auto& page : custom_html_page_handlers) {
      wm.server->on(page.path, page.handler);
  }
}

