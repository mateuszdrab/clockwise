#pragma once

#include <WiFi.h>
#include <LittleFS.h>
#include <Update.h>
#include <CWPreferences.h>
#include <stdlib.h>
#include "AutoBrightnessController.h"
#include "SystemLog.h"
#include "StatusController.h"
#include "SettingsWebPage.h"

extern bool otaInProgress;
extern bool otaRestartPending;
extern uint32_t otaRestartAtMillis;

#ifndef CLOCKFACE_NAME
  #define CLOCKFACE_NAME "UNKNOWN"
#endif

WiFiServer server(80);

struct ClockwiseWebServer
{
  String httpBuffer;
  bool force_restart;
  bool canvasUploadReady = false;
  bool canvasDownloadReady = false;
  bool canvasCacheClearReady = false;
  const char* HEADER_TEMPLATE_D = "X-%s: %d\r\n";
  const char* HEADER_TEMPLATE_S = "X-%s: %s\r\n";
 
  static ClockwiseWebServer *getInstance()
  {
    static ClockwiseWebServer base;
    return &base;
  }

  void startWebServer()
  {
    server.begin();
    StatusController::getInstance()->blink_led(100, 3);
  }

  void stopWebServer()
  {
    server.stop();
  }

  String decodeUrlValue(const String &value)
  {
    String decoded;
    decoded.reserve(value.length());
    for (unsigned int index = 0; index < value.length(); index++)
    {
      if (value[index] == '%' && index + 2 < value.length())
      {
        char hex[3] = {value[index + 1], value[index + 2], '\0'};
        decoded += (char)strtol(hex, nullptr, 16);
        index += 2;
      }
      else if (value[index] == '+')
      {
        decoded += ' ';
      }
      else
      {
        decoded += value[index];
      }
    }
    return decoded;
  }

  void handleHttpRequest()
  {
    if (force_restart)
      StatusController::getInstance()->forceRestart();


    WiFiClient client = server.available();
    if (client)
    {
      StatusController::getInstance()->blink_led(100, 1);
      client.setTimeout(10000);
      String requestLine = client.readStringUntil('\n');
      requestLine.trim();
      const int methodEnd = requestLine.indexOf(' ');
      const int pathEnd = requestLine.indexOf(' ', methodEnd + 1);
      if (methodEnd < 0 || pathEnd < 0) {
        client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
        client.stop();
        return;
      }

      String method = requestLine.substring(0, methodEnd);
      String path = requestLine.substring(methodEnd + 1, pathEnd);
      uint32_t contentLength = 0;
      String otaPasswordHeader;
      while (client.connected()) {
        String header = client.readStringUntil('\n');
        header.trim();
        if (header.length() == 0)
          break;
        const int separator = header.indexOf(':');
        if (separator > 0) {
          String name = header.substring(0, separator);
          String value = header.substring(separator + 1);
          name.trim();
          value.trim();
          if (name.equalsIgnoreCase("Content-Length"))
            contentLength = value.toInt();
          else if (name.equalsIgnoreCase("X-OTA-Password"))
            otaPasswordHeader = value;
        }
      }

      String key = "";
      String value = "";
      if (method == "POST" && path == "/ota/upload") {
        receiveFirmwareUpload(client, contentLength, otaPasswordHeader);
      } else if (method == "POST" && path == "/canvas/upload") {
        receiveCanvasUpload(client, contentLength);
      } else {
        if (path.indexOf('?') > 0) {
          const int queryStart = path.indexOf('?');
          const int equals = path.indexOf('=', queryStart + 1);
          if (equals > 0) {
            key = path.substring(queryStart + 1, equals);
            value = decodeUrlValue(path.substring(equals + 1));
          }
          path = path.substring(0, queryStart);
        }
        if (method == "GET" && (path == "/logs" || path == "/canvas/logs")) {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: text/plain; charset=utf-8");
          client.println("Cache-Control: no-store");
          client.println("Connection: close");
          client.println();
          SystemLog::instance().writeTo(client);
        } else {
          processRequest(client, method, path, key, value);
        }
      }
      delay(1);
      client.stop();
    }
  }

  bool takeCanvasUpload()
  {
    const bool ready = canvasUploadReady;
    canvasUploadReady = false;
    return ready;
  }

  bool takeCanvasDownload()
  {
    const bool ready = canvasDownloadReady;
    canvasDownloadReady = false;
    return ready;
  }

  bool takeCanvasCacheClear()
  {
    const bool ready = canvasCacheClearReady;
    canvasCacheClearReady = false;
    return ready;
  }

  void receiveCanvasUpload(WiFiClient &client, uint32_t contentLength)
  {
    if (contentLength == 0 || contentLength > 32768) {
      client.println(contentLength > 32768 ? "HTTP/1.1 413 Payload Too Large" : "HTTP/1.1 400 Empty Upload");
      client.println("Connection: close\r\n");
      return;
    }

    LittleFS.remove("/canvas.upload");
    File upload = LittleFS.open("/canvas.upload", FILE_WRITE);
    if (!upload) {
      client.println("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n");
      return;
    }

    uint8_t buffer[256];
    uint32_t receivedTotal = 0;
    bool success = true;
    while (receivedTotal < contentLength) {
      const int available = client.available();
      if (available <= 0) {
        if (!client.connected()) {
          success = false;
          break;
        }
        delay(1);
        continue;
      }
      const uint32_t remaining = contentLength - receivedTotal;
      const size_t requested = available < static_cast<int>(sizeof(buffer)) ? available : sizeof(buffer);
      const size_t count = client.read(buffer, requested < remaining ? requested : remaining);
      if (count == 0 || upload.write(buffer, count) != count) {
        success = false;
        break;
      }
      receivedTotal += count;
    }
    upload.close();

    if (!success || receivedTotal != contentLength) {
      LittleFS.remove("/canvas.upload");
      client.println("HTTP/1.1 400 Incomplete Upload\r\nConnection: close\r\n");
      return;
    }

    canvasDownloadReady = false;
    canvasUploadReady = true;
    client.println("HTTP/1.1 202 Accepted\r\nConnection: close\r\n");
  }

  bool matchesPassword(const String &provided, const String &expected)
  {
    if (provided.length() != expected.length() || expected.isEmpty())
      return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < expected.length(); ++index)
      difference |= static_cast<uint8_t>(provided[index] ^ expected[index]);
    return difference == 0;
  }

  void receiveFirmwareUpload(WiFiClient &client, uint32_t contentLength, const String &providedPassword)
  {
    ClockwiseParams *params = ClockwiseParams::getInstance();
    params->load();
    if (!params->otaEnabled || params->otaPassword.isEmpty()) {
      client.println("HTTP/1.1 403 OTA Disabled\r\nConnection: close\r\n");
      return;
    }
    if (!matchesPassword(providedPassword, params->otaPassword)) {
      client.println("HTTP/1.1 403 Invalid OTA Password\r\nConnection: close\r\n");
      return;
    }
    if (contentLength == 0) {
      client.println("HTTP/1.1 400 Empty Firmware\r\nConnection: close\r\n");
      return;
    }
    if (!Update.begin(contentLength, U_FLASH)) {
      Update.printError(SerialLog::instance());
      client.println("HTTP/1.1 413 Firmware Too Large\r\nConnection: close\r\n");
      return;
    }

    otaInProgress = true;
    StatusController::getInstance()->otaUpdateStarted();
    uint8_t buffer[1024];
    uint32_t receivedTotal = 0;
    uint8_t displayedPercent = 255;
    uint32_t lastDataMillis = millis();
    bool success = true;
    while (receivedTotal < contentLength) {
      const int available = client.available();
      if (available <= 0) {
        if (!client.connected() || millis() - lastDataMillis >= 15000) {
          success = false;
          break;
        }
        delay(1);
        continue;
      }

      const uint32_t remaining = contentLength - receivedTotal;
      const size_t requested = available < static_cast<int>(sizeof(buffer)) ? available : sizeof(buffer);
      const size_t count = client.read(buffer, requested < remaining ? requested : remaining);
      if (count == 0 || Update.write(buffer, count) != count) {
        success = false;
        break;
      }

      receivedTotal += count;
      lastDataMillis = millis();
      const uint8_t percent = static_cast<uint8_t>((static_cast<uint64_t>(receivedTotal) * 100) / contentLength);
      if (percent != displayedPercent) {
        displayedPercent = percent;
        StatusController::getInstance()->otaUpdateProgress(percent);
      }
    }

    if (!success || receivedTotal != contentLength || !Update.end()) {
      const uint8_t error = Update.getError();
      Update.abort();
      StatusController::getInstance()->otaUpdateFailed(error);
      client.println("HTTP/1.1 400 Firmware Update Failed\r\nConnection: close\r\n");
      client.flush();
      otaRestartPending = true;
      otaRestartAtMillis = millis() + 2000;
      return;
    }

    StatusController::getInstance()->otaUpdateFinished();
    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
    client.println("Firmware verified; restarting");
    client.flush();
    otaRestartPending = true;
    otaRestartAtMillis = millis() + 2000;
  }

  void processRequest(WiFiClient client, String method, String path, String key, String value)
  {
    if (method == "GET" && path == "/") {
      client.println("HTTP/1.0 200 OK");
      client.println("Content-Type: text/html");
      client.println();
      client.println(SETTINGS_PAGE);
    } else if (method == "GET" && path == "/get") {
      getCurrentSettings(client);
    } else if (method == "GET" && path == "/read") {
      if (key == "pin") {
        readPin(client, key, value.toInt());
      }
    } else if (method == "POST" && path == "/canvas/download" && key == "url") {
      ClockwiseParams *params = ClockwiseParams::getInstance();
      params->load();
      params->canvasUrl = value;
      params->save();
      canvasDownloadReady = true;
      client.println("HTTP/1.1 202 Accepted\r\nConnection: close\r\n");
    } else if (method == "POST" && path == "/canvas/clear") {
      canvasCacheClearReady = true;
      client.println("HTTP/1.1 202 Accepted\r\nConnection: close\r\n");
    } else if (method == "POST" && path == "/restart") {
      client.println("HTTP/1.0 204 No Content");
      force_restart = true;
    } else if (method == "POST" && path == "/set") {
      ClockwiseParams::getInstance()->load();
      //a baby seal has died due this ifs
      if (key == ClockwiseParams::getInstance()->PREF_DISPLAY_BRIGHT) {
        ClockwiseParams::getInstance()->displayBright = value.toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_WIFI_SSID) {
        ClockwiseParams::getInstance()->wifiSsid = value;
      } else if (key == ClockwiseParams::getInstance()->PREF_WIFI_PASSWORD) {
        ClockwiseParams::getInstance()->wifiPwd = value;
      } else if (key == ClockwiseParams::getInstance()->PREF_OTA_ENABLED) {
        ClockwiseParams::getInstance()->otaEnabled = (value == "1");
      } else if (key == ClockwiseParams::getInstance()->PREF_WIFI_AUTO_CONFIG) {
        ClockwiseParams::getInstance()->wifiAutoConfig = (value == "1");
      } else if (key == ClockwiseParams::getInstance()->PREF_OTA_PASSWORD) {
        ClockwiseParams::getInstance()->otaPassword = value;
      } else if (key == "autoBright") {   //autoBright=0010,0800
        ClockwiseParams::getInstance()->autoBrightMin = value.substring(0,4).toInt();
        ClockwiseParams::getInstance()->autoBrightMax = value.substring(5,9).toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_LEVELS) {
        ClockwiseParams::getInstance()->autoBrightLevels = value.toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_FADE) {
        ClockwiseParams::getInstance()->autoBrightFade = value.toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_SWAP_BLUE_GREEN) {
        ClockwiseParams::getInstance()->swapBlueGreen = (value == "1");
      } else if (key == ClockwiseParams::getInstance()->PREF_SWAP_BLUE_RED) {
        ClockwiseParams::getInstance()->swapBlueRed = (value == "1");
      } else if (key == ClockwiseParams::getInstance()->PREF_USE_24H_FORMAT) {
        ClockwiseParams::getInstance()->use24hFormat = (value == "1");
      } else if (key == ClockwiseParams::getInstance()->PREF_LDR_PIN) {
        ClockwiseParams::getInstance()->ldrPin = value.toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_TIME_ZONE) {
        ClockwiseParams::getInstance()->timeZone = value;
      } else if (key == ClockwiseParams::getInstance()->PREF_NTP_SERVER) {
        ClockwiseParams::getInstance()->ntpServer = value;
      } else if (key == ClockwiseParams::getInstance()->PREF_CANVAS_FILE) {
        ClockwiseParams::getInstance()->canvasFile = value;
        ClockwiseParams::getInstance()->canvasUrl = "";
      } else if (key == ClockwiseParams::getInstance()->PREF_CANVAS_SERVER) {
        ClockwiseParams::getInstance()->canvasServer = value;
        ClockwiseParams::getInstance()->canvasUrl = "";
      } else if (key == ClockwiseParams::getInstance()->PREF_MANUAL_POSIX) {
        ClockwiseParams::getInstance()->manualPosix = value;
      } else if (key == ClockwiseParams::getInstance()->PREF_DISPLAY_ROTATION) {
        ClockwiseParams::getInstance()->displayRotation = value.toInt();
      } else if (key == ClockwiseParams::getInstance()->PREF_DRIVER) {
        ClockwiseParams::getInstance()->driver = value.toInt();
      }  else if (key == ClockwiseParams::getInstance()->PREF_I2CSPEED) {
        ClockwiseParams::getInstance()->i2cSpeed = value.toInt();
      }  else if (key == ClockwiseParams::getInstance()->PREF_E_PIN) {
        ClockwiseParams::getInstance()->E_pin = value.toInt();
      }
      ClockwiseParams::getInstance()->save();
      client.println("HTTP/1.0 204 No Content");
    }
  }


  // Return the live ADC reading alongside the filtered level and applied/target brightness.
  void readPin(WiFiClient client, String key, uint16_t pin) {
    ClockwiseParams::getInstance()->load();

    client.println("HTTP/1.0 204 No Content");
    client.printf(HEADER_TEMPLATE_D, key, analogRead(pin));
    client.printf(HEADER_TEMPLATE_D, "level", cw::AutoBrightnessController::instance().level());
    client.printf(HEADER_TEMPLATE_D, "levels", cw::AutoBrightnessController::instance().levels());
    client.printf(HEADER_TEMPLATE_D, "brightness", cw::AutoBrightnessController::instance().brightness());
    client.printf(HEADER_TEMPLATE_D, "target", cw::AutoBrightnessController::instance().targetBrightness());
    
    client.println();
  }


  void getCurrentSettings(WiFiClient client) {
    ClockwiseParams::getInstance()->load();

    client.println("HTTP/1.0 204 No Content");

    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_BRIGHT, ClockwiseParams::getInstance()->displayBright);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_MIN, ClockwiseParams::getInstance()->autoBrightMin);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_MAX, ClockwiseParams::getInstance()->autoBrightMax);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_LEVELS, ClockwiseParams::getInstance()->autoBrightLevels);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_ABC_FADE, ClockwiseParams::getInstance()->autoBrightFade);
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_CANVAS_URL, ClockwiseParams::getInstance()->canvasUrl.c_str());
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_SWAP_BLUE_GREEN, ClockwiseParams::getInstance()->swapBlueGreen);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_SWAP_BLUE_RED, ClockwiseParams::getInstance()->swapBlueRed);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_USE_24H_FORMAT, ClockwiseParams::getInstance()->use24hFormat);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_LDR_PIN, ClockwiseParams::getInstance()->ldrPin);    
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_TIME_ZONE, ClockwiseParams::getInstance()->timeZone.c_str());
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_WIFI_SSID, ClockwiseParams::getInstance()->wifiSsid.c_str());
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_OTA_ENABLED, ClockwiseParams::getInstance()->otaEnabled);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_WIFI_AUTO_CONFIG, ClockwiseParams::getInstance()->wifiAutoConfig);
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_NTP_SERVER, ClockwiseParams::getInstance()->ntpServer.c_str());
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_CANVAS_FILE, ClockwiseParams::getInstance()->canvasFile.c_str());
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_CANVAS_SERVER, ClockwiseParams::getInstance()->canvasServer.c_str());
    client.printf(HEADER_TEMPLATE_S, ClockwiseParams::getInstance()->PREF_MANUAL_POSIX, ClockwiseParams::getInstance()->manualPosix.c_str());
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DISPLAY_ROTATION, ClockwiseParams::getInstance()->displayRotation);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_DRIVER, ClockwiseParams::getInstance()->driver);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_I2CSPEED, ClockwiseParams::getInstance()->i2cSpeed);
    client.printf(HEADER_TEMPLATE_D, ClockwiseParams::getInstance()->PREF_E_PIN, ClockwiseParams::getInstance()->E_pin);

    client.printf(HEADER_TEMPLATE_S, "CW_FW_VERSION", CW_FW_VERSION);
    client.printf(HEADER_TEMPLATE_S, "CW_FW_NAME", CW_FW_NAME);
    client.printf(HEADER_TEMPLATE_S, "CLOCKFACE_NAME", CLOCKFACE_NAME);
    client.println();
  }
  
};
