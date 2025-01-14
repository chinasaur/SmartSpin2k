/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "Main.h"
#include "Version_Converter.h"
#include "Builtin_Pages.h"
#include "HTTP_Server_Basic.h"
#include "cert.h"
#include "SS2KLog.h"
#include <WebServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <DNSServer.h>

File fsUploadFile;

TaskHandle_t webClientTask;
#define MAX_BUFFER_SIZE 20

IPAddress myIP;
bool internetConnection = false;

// DNS server
const byte DNS_PORT = 53;
DNSServer dnsServer;

WiFiClientSecure client;
WebServer server(80);

#ifdef USE_TELEGRAM
#include <UniversalTelegramBot.h>
TaskHandle_t telegramTask;
bool telegramMessageWaiting = false;
UniversalTelegramBot bot(TELEGRAM_TOKEN, client);
String telegramMessage = "";
#endif

// ********************************WIFI Setup*************************
void startWifi() {
  int i = 0;

  // Trying Station mode first:
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "Connecting to: %s", userConfig.getSsid());
  if (String(WiFi.SSID()) != userConfig.getSsid()) {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(userConfig.getSsid(), userConfig.getPassword());
  }

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(1000 / portTICK_RATE_MS);
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Waiting for connection to be established...");
    i++;
    if (i > WIFI_CONNECT_TIMEOUT || (String(userConfig.getSsid()) == DEVICE_NAME)) {
      i = 0;
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Couldn't Connect. Switching to AP mode");
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    myIP               = WiFi.localIP();
    internetConnection = true;
  }

  // Couldn't connect to existing network, Create SoftAP
  if (WiFi.status() != WL_CONNECTED) {
    String t_pass = DEFAULT_PASSWORD;
    if (String(userConfig.getSsid()) == DEVICE_NAME) {
      // If default SSID is still in use, let the user
      // select a new password.
      // Else Fall Back to the default password (probably "password")
      String t_pass = String(userConfig.getPassword());
    }
    WiFi.softAP(userConfig.getDeviceName(), t_pass.c_str());
    vTaskDelay(50);
    myIP = WiFi.softAPIP();
    /* Setup the DNS server redirecting all the domains to the apIP */
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", myIP);
  }

  if (!MDNS.begin(userConfig.getDeviceName())) {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Error setting up MDNS responder!");
  }

  MDNS.addService("http", "_tcp", 80);
  MDNS.addServiceTxt("http", "_tcp", "lf", "0");
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "Connected to %s IP address: %s", userConfig.getSsid(), myIP.toString().c_str());
  SEND_TO_TELEGRAM("Connected to " + String(userConfig.getSsid()) + " IP address: " + myIP.toString());
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "Open http://%s.local/", userConfig.getDeviceName());
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if (WiFi.getMode() == WIFI_STA) {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Syncing clock...");
    configTime(0, 0, "pool.ntp.org");  // get UTC time via NTP
    time_t now = time(nullptr);
    while (now < 10) {  // wait 10 seconds
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Waiting for clock sync...");
      delay(100);
      now = time(nullptr);
    }
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Clock synced to: %.f", difftime(now, (time_t)0));
    Serial.println(now);
  }
}

void startHttpServer() {
  server.onNotFound([]() { SS2K_LOG(HTTP_SERVER_LOG_TAG, "Link Not Found: %s", server.uri().c_str()); });

  /********************************************Begin
   * Handlers***********************************/
  server.on("/", handleIndexFile);
  server.on("/index.html", handleIndexFile);
  server.on("/generate_204", handleIndexFile);         // Android captive portal
  server.on("/fwlink", handleIndexFile);               // Microsoft captive portal
  server.on("/hotspot-detect.html", handleIndexFile);  // Apple captive portal
  server.on("/style.css", handleSpiffsFile);
  server.on("/btsimulator.html", handleSpiffsFile);
  server.on("/settings.html", handleSpiffsFile);
  server.on("/status.html", handleSpiffsFile);
  server.on("/bluetoothscanner.html", handleSpiffsFile);
  server.on("/hrtowatts.html", handleSpiffsFile);
  server.on("/favicon.ico", handleSpiffsFile);
  server.on("/send_settings", settingsProcessor);
  server.on("/jquery.js.gz", handleSpiffsFile);

  server.on("/BLEScan", []() {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Scanning from web request");
    String response =
        "<!DOCTYPE html><html><body>Scanning for BLE Devices. Please wait "
        "15 seconds.</body><script> setTimeout(\"location.href = 'http://" +
        myIP.toString() + "/bluetoothscanner.html';\",15000);</script></html>";
    spinBLEClient.resetDevices();
    spinBLEClient.serverScan(true);
    server.send(200, "text/html", response);
  });

  server.on("/load_defaults.html", []() {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Setting Defaults from Web Request");
    SPIFFS.format();
    userConfig.setDefaults();
    userConfig.saveToSPIFFS();
    String response =
        "<!DOCTYPE html><html><body><h1>Defaults have been "
        "loaded.</h1><p><br><br> Please reconnect to the device on WiFi "
        "network: " +
        myIP.toString() + "</p></body></html>";
    server.send(200, "text/html", response);
    ESP.restart();
  });

  server.on("/reboot.html", []() {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Rebooting from Web Request");
    String response = "Rebooting....<script> setTimeout(\"location.href = 'http://" + myIP.toString() + "/index.html';\",500); </script>";
    server.send(200, "text/html", response);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ESP.restart();
  });

  server.on("/hrslider", []() {
    String value = server.arg("value");
    if (value == "enable") {
      userConfig.setSimulateHr(true);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "HR Simulator turned on");
    } else if (value == "disable") {
      userConfig.setSimulateHr(false);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "HR Simulator turned off");
    } else {
      userConfig.setSimulatedHr(value.toInt());
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "HR is now: %d", userConfig.getSimulatedHr());
      server.send(200, "text/plain", "OK");
    }
  });

  server.on("/wattsslider", []() {
    String value = server.arg("value");
    if (value == "enable") {
      userConfig.setSimulateWatts(true);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Watt Simulator turned on");
    } else if (value == "disable") {
      userConfig.setSimulateWatts(false);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Watt Simulator turned off");
    } else {
      userConfig.setSimulatedWatts(value.toInt());
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Watts are now: %d", userConfig.getSimulatedWatts());
      server.send(200, "text/plain", "OK");
    }
  });

  server.on("/cadslider", []() {
    String value = server.arg("value");
    if (value == "enable") {
      userConfig.setSimulateCad(true);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "CAD Simulator turned on");
    } else if (value == "disable") {
      userConfig.setSimulateCad(false);
      server.send(200, "text/plain", "OK");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "CAD Simulator turned off");
    } else {
      userConfig.setSimulatedCad(value.toInt());
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "CAD is now: %f", userConfig.getSimulatedCad());
      server.send(200, "text/plain", "OK");
    }
  });

  server.on("/configJSON", []() {
    String tString;
    tString = userConfig.returnJSON(!server.arg("includeDebugLog").isEmpty());
    server.send(200, "text/plain", tString);
  });

  server.on("/PWCJSON", []() {
    String tString;
    tString = userPWC.returnJSON();
    server.send(200, "text/plain", tString);
  });

  server.on("/login", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", OTALoginIndex);
  });

  server.on("/OTAIndex", HTTP_GET, []() {
    spinBLEClient.disconnect();
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", OTAServerIndex);
  });

  /*handling uploading firmware file */
  server.on(
      "/update", HTTP_POST,
      []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.filename == String("firmware.bin").c_str()) {
          if (upload.status == UPLOAD_FILE_START) {
            SS2K_LOG(HTTP_SERVER_LOG_TAG, "Update: %s", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {  // start with max
                                                       // available size
              Update.printError(Serial);
            }
          } else if (upload.status == UPLOAD_FILE_WRITE) {
            /* flashing firmware to ESP*/
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
              Update.printError(Serial);
            }
          } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {  // true to set the size to the
                                     // current progress
              server.send(200, "text/plain", "Firmware Uploaded Successfully. Rebooting...");
              vTaskDelay(2000 / portTICK_PERIOD_MS);
              ESP.restart();
            } else {
              Update.printError(Serial);
            }
          }
        } else {
          if (upload.status == UPLOAD_FILE_START) {
            String filename = upload.filename;
            if (!filename.startsWith("/")) {
              filename = "/" + filename;
            }
            SS2K_LOG(HTTP_SERVER_LOG_TAG, "handleFileUpload Name: %s", filename.c_str());
            fsUploadFile = SPIFFS.open(filename, "w");
            filename     = String();
          } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (fsUploadFile) {
              fsUploadFile.write(upload.buf, upload.currentSize);
            }
          } else if (upload.status == UPLOAD_FILE_END) {
            if (fsUploadFile) {
              fsUploadFile.close();
            }
            SS2K_LOG(HTTP_SERVER_LOG_TAG, "handleFileUpload Size: %zu", upload.totalSize);
            server.send(200, "text/plain", String(upload.filename + " Uploaded Successfully."));
          }
        }
      });

  /********************************************End Server
   * Handlers*******************************/

  xTaskCreatePinnedToCore(webClientUpdate,                    /* Task function. */
                          "webClientUpdate",                  /* name of task. */
                          4500 + (DEBUG_LOG_BUFFER_SIZE * 2), /* Stack size of task Used to be 3000*/
                          NULL,                               /* parameter of the task */
                          1,                                  /* priority of the task  - 29 worked*/
                          &webClientTask,                     /* Task handle to keep track of created task */
                          1);                                 /* pin task to core 1 */

#ifdef USE_TELEGRAM
  xTaskCreatePinnedToCore(telegramUpdate,   /* Task function. */
                          "telegramUpdate", /* name of task. */
                          4900,             /* Stack size of task*/
                          NULL,             /* parameter of the task */
                          1,                /* priority of the task  - higher number is higher priority*/
                          &telegramTask,    /* Task handle to keep track of created task */
                          1);               /* pin task to core 1 */
#endif
  server.begin();
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "HTTP server started");
}

void webClientUpdate(void *pvParameters) {
  static unsigned long mDnsTimer = millis();  // NOLINT: There is no overload in String for uint64_t
  for (;;) {
    server.handleClient();
    vTaskDelay(WEBSERVER_DELAY / portTICK_RATE_MS);
    if (WiFi.getMode() == WIFI_AP) {
      dnsServer.processNextRequest();
    }
    // Keep MDNS alive
    if ((millis() - mDnsTimer) > 60000) {
      MDNS.addServiceTxt("http", "_tcp", "lf", String(mDnsTimer));
      mDnsTimer = millis();
    }
  }
}

void handleIndexFile() {
  String filename = "/index.html";
  if (SPIFFS.exists(filename)) {
    File file = SPIFFS.open(filename, FILE_READ);
    server.streamFile(file, "text/html");
    file.close();
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Served %s", filename.c_str());
  } else {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "%s not found. Sending builtin Index.html", filename.c_str());
    server.send(200, "text/html", noIndexHTML);
  }
}

void handleSpiffsFile() {
  String filename = server.uri();
  int dotPosition = filename.lastIndexOf(".");
  String fileType = filename.substring((dotPosition + 1), filename.length());
  if (SPIFFS.exists(filename)) {
    File file = SPIFFS.open(filename, FILE_READ);
    if (fileType == "gz") {
      fileType = "html";  // no need to change content type as it's done automatically by .streamfile below VV
    }
    server.streamFile(file, "text/" + fileType);
    file.close();
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "Served %s", filename.c_str());
  } else {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "%s not found. Sending 404.", filename.c_str());
    server.send(404, "text/html",
                "<html><body><h1>ERROR 404 <br> FILE NOT "
                "FOUND!</h1></body></html>");
  }
}

void settingsProcessor() {
  String tString;
  bool wasBTUpdate = false;
  if (!server.arg("ssid").isEmpty()) {
    tString = server.arg("ssid");
    tString.trim();
    userConfig.setSsid(tString);
  }
  if (!server.arg("password").isEmpty()) {
    tString = server.arg("password");
    tString.trim();
    userConfig.setPassword(tString);
  }
  if (!server.arg("deviceName").isEmpty()) {
    tString = server.arg("deviceName");
    tString.trim();
    userConfig.setDeviceName(tString);
  }
  if (!server.arg("shiftStep").isEmpty()) {
    uint64_t shiftStep = server.arg("shiftStep").toInt();
    if (shiftStep >= 50 && shiftStep <= 6000) {
      userConfig.setShiftStep(shiftStep);
    }
  }
  if (!server.arg("stepperPower").isEmpty()) {
    uint64_t stepperPower = server.arg("stepperPower").toInt();
    if (stepperPower >= 500 && stepperPower <= 2000) {
      userConfig.setStepperPower(stepperPower);
      updateStepperPower();
    }
  }
  // checkboxes don't report off, so need to check using another parameter
  // that's always present on that page
  if (!server.arg("stepperPower").isEmpty()) {
    if (!server.arg("autoUpdate").isEmpty()) {
      userConfig.setAutoUpdate(true);
    } else {
      userConfig.setAutoUpdate(false);
    }
    if (!server.arg("stealthchop").isEmpty()) {
      userConfig.setStealthChop(true);
      updateStealthchop();
    } else {
      userConfig.setStealthChop(false);
      updateStealthchop();
    }
  }
  if (!server.arg("inclineMultiplier").isEmpty()) {
    float inclineMultiplier = server.arg("inclineMultiplier").toFloat();
    if (inclineMultiplier >= 1 && inclineMultiplier <= 5) {
      userConfig.setInclineMultiplier(inclineMultiplier);
    }
  }
  if (!server.arg("powerCorrectionFactor").isEmpty()) {
    float powerCorrectionFactor = server.arg("powerCorrectionFactor").toFloat();
    if (powerCorrectionFactor >= 0 && powerCorrectionFactor <= 2) {
      userConfig.setPowerCorrectionFactor(powerCorrectionFactor);
    }
  }
  if (!server.arg("blePMDropdown").isEmpty()) {
    wasBTUpdate = true;
    if (server.arg("blePMDropdown")) {
      tString = server.arg("blePMDropdown");
      userConfig.setConnectedPowerMeter(server.arg("blePMDropdown"));
    } else {
      userConfig.setConnectedPowerMeter("any");
    }
  }
  if (!server.arg("bleHRDropdown").isEmpty()) {
    wasBTUpdate = true;
    if (server.arg("bleHRDropdown")) {
      tString = server.arg("bleHRDropdown");
      userConfig.setConnectedHeartMonitor(server.arg("bleHRDropdown"));
    } else {
      userConfig.setConnectedHeartMonitor("any");
    }
  }

  if (!server.arg("session1HR").isEmpty()) {  // Needs checking for unrealistic numbers.
    userPWC.session1HR = server.arg("session1HR").toInt();
  }
  if (!server.arg("session1Pwr").isEmpty()) {
    userPWC.session1Pwr = server.arg("session1Pwr").toInt();
  }
  if (!server.arg("session2HR").isEmpty()) {
    userPWC.session2HR = server.arg("session2HR").toInt();
  }
  if (!server.arg("session2Pwr").isEmpty()) {
    userPWC.session2Pwr = server.arg("session2Pwr").toInt();

    if (!server.arg("hr2Pwr").isEmpty()) {
      userPWC.hr2Pwr = true;
    } else {
      userPWC.hr2Pwr = false;
    }
  }
  String response = "<!DOCTYPE html><html><body><h2>";

  if (wasBTUpdate) {  // Special BT update response
    response +=
        "Selections Saved!</h2></body><script> setTimeout(\"location.href "
        "= 'http://" +
        myIP.toString() + "/bluetoothscanner.html';\",1000);</script></html>";
    spinBLEClient.resetDevices();
    spinBLEClient.serverScan(true);
  } else {  // Normal response
    response +=
        "Network settings will be applied at next reboot. <br> Everything "
        "else is availiable immediately.</h2></body><script> "
        "setTimeout(\"location.href = 'http://" +
        myIP.toString() + "/index.html';\",1000);</script></html>";
  }
  server.send(200, "text/html", response);
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "Config Updated From Web");
  userConfig.saveToSPIFFS();
  userConfig.printFile();
  userPWC.saveToSPIFFS();
  userPWC.printFile();
}

// github fingerprint
// 70:94:DE:DD:E6:C4:69:48:3A:92:70:A1:48:56:78:2D:18:64:E0:B7

void FirmwareUpdate() {
  HTTPClient http;
  // WiFiClientSecure client;

  client.setCACert(rootCACertificate);
  SS2K_LOG(HTTP_SERVER_LOG_TAG, "Checking for newer firmware:");
  http.begin(userConfig.getFirmwareUpdateURL() + String(FW_VERSIONFILE),
             rootCACertificate);  // check version URL
  delay(100);
  int httpCode = http.GET();  // get data from version file
  delay(100);
  String payload;
  if (httpCode == HTTP_CODE_OK) {  // if version received
    payload = http.getString();    // save received version
    payload.trim();
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "  - Server version: %s", payload.c_str());
    internetConnection = true;
  } else {
    SS2K_LOG(HTTP_SERVER_LOG_TAG, "error downloading %s %d", FW_VERSIONFILE, httpCode);
    internetConnection = false;
  }

  http.end();
  if (httpCode == HTTP_CODE_OK) {  // if version received
    bool updateAnyway = false;
    if (!SPIFFS.exists("/index.html")) {
      updateAnyway = true;
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "  -index.html not found. Forcing update");
    }
    Version availiableVer(payload.c_str());
    Version currentVer(FIRMWARE_VERSION);

    if ((availiableVer > currentVer) || (updateAnyway)) {
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "New firmware detected!");
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Upgrading from %s to %s", FIRMWARE_VERSION, payload.c_str());

      // Update Spiffs
      httpUpdate.setLedPin(LED_BUILTIN, LOW);
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "Updating FileSystem");
      t_httpUpdate_return ret = httpUpdate.updateSpiffs(client, userConfig.getFirmwareUpdateURL() + String(FW_SPIFFSFILE));
      vTaskDelay(100 / portTICK_PERIOD_MS);
      switch (ret) {
        case HTTP_UPDATE_OK:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "Saving Config.txt");
          userConfig.saveToSPIFFS();
          userPWC.saveToSPIFFS();
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "Updating Program");
          break;

        case HTTP_UPDATE_NO_UPDATES:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "HTTP_UPDATE_NO_UPDATES");
          break;

        case HTTP_UPDATE_FAILED:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "SPIFFS Update Failed: %d : %s", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
      }

      // Update Firmware
      ret = httpUpdate.update(client, userConfig.getFirmwareUpdateURL() + String(FW_BINFILE));
      switch (ret) {
        case HTTP_UPDATE_FAILED:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "HTTP_UPDATE_FAILED Error %d : %s", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;

        case HTTP_UPDATE_NO_UPDATES:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "HTTP_UPDATE_NO_UPDATES");
          break;

        case HTTP_UPDATE_OK:
          SS2K_LOG(HTTP_SERVER_LOG_TAG, "HTTP_UPDATE_OK");
          break;
      }
    } else {  // don't update
      SS2K_LOG(HTTP_SERVER_LOG_TAG, "  - Current Version: %s", FIRMWARE_VERSION);
    }
  }
}

#ifdef USE_TELEGRAM
// Function to handle sending telegram text to the non blocking task
void sendTelegram(String textToSend) {
  static int numberOfMessages = 0;
  static uint64_t timeout     = 120000;  // reset every two minutes
  static uint64_t startTime   = millis();

  if (millis() - startTime > timeout) {  // Let one message send every two minutes
    numberOfMessages = MAX_TELEGRAM_MESSAGES - 1;
    telegramMessage += " " + String(userConfig.getSsid()) + " ";
    startTime = millis();
  }

  if ((numberOfMessages < MAX_TELEGRAM_MESSAGES) && (WiFi.getMode() == WIFI_STA)) {
    telegramMessage += "\n" + textToSend;
    telegramMessageWaiting = true;
    numberOfMessages++;
  }
}

// Non blocking task to send telegram message
void telegramUpdate(void *pvParameters) {
  client.setInsecure();
  for (;;) {
    static int telegramFailures = 0;
    if (telegramMessageWaiting && internetConnection) {
      telegramMessageWaiting = false;
      bool rm                = (bot.sendMessage(TELEGRAM_CHAT_ID, telegramMessage, ""));
      if (!rm) {
        telegramFailures++;
        SS2K_LOG(HTTP_SERVER_LOG_TAG, "Telegram failed to send! %s", TELEGRAM_CHAT_ID);
        if (telegramFailures > 2) {
          internetConnection = false;
        }
      } else {  // Success - reset Telegram Failures
        telegramFailures = 0;
      }

      client.stop();
      telegramMessage = "";
    }
#ifdef DEBUG_STACK
    Serial.printf("Telegram: %d \n", uxTaskGetStackHighWaterMark(telegramTask));
    Serial.printf("Web: %d \n", uxTaskGetStackHighWaterMark(webClientTask));
    Serial.printf("Free: %d \n", ESP.getFreeHeap());
#endif
    vTaskDelay(4000 / portTICK_RATE_MS);
  }
}
#endif
