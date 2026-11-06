#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Clockface
#include <Clockface.h>
// Commons
#include <WiFiController.h>
#include <CWDateTime.h>
#include <CWPreferences.h>
#include <CWWebServer.h>
#include <StatusController.h>
#include <AutoBrightnessController.h>

#define ESP32_LED_BUILTIN 2

MatrixPanel_I2S_DMA *dma_display = nullptr;

Clockface *clockface;

WiFiController wifi;
CWDateTime cwDateTime;

bool autoBrightEnabled;
uint32_t autoBrightSampleMillis = 0;
uint32_t autoBrightDebugMillis = 0;
uint16_t lastLdrReading = 0;
uint8_t lastAppliedBrightness = 0;
bool otaInProgress = false;
bool otaRestartPending = false;
uint32_t otaRestartAtMillis = 0;
int lastOtaPercent = -1;

void setupOtaDisplayCallbacks()
{
  ArduinoOTA
      .onStart([]() {
        otaInProgress = true;
        lastOtaPercent = -1;
        StatusController::getInstance()->otaUpdateStarted();
        SerialLog::instance().println("[OTA] Update started");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        const uint8_t percent = total == 0 ? 0 : (static_cast<uint64_t>(progress) * 100) / total;
        if (percent == lastOtaPercent)
          return;
        lastOtaPercent = percent;
        StatusController::getInstance()->otaUpdateProgress(percent);
        SerialLog::instance().printf("[OTA] Progress: %u%%\n", percent);
      })
      .onEnd([]() {
        StatusController::getInstance()->otaUpdateFinished();
        SerialLog::instance().println("[OTA] Update complete; restarting");
      })
      .onError([](ota_error_t error) {
        StatusController::getInstance()->otaUpdateFailed(static_cast<uint8_t>(error));
        SerialLog::instance().printf("[OTA] Update failed with error %u\n", static_cast<uint8_t>(error));
        if (otaInProgress) {
          otaRestartPending = true;
          otaRestartAtMillis = millis() + 2000;
        } else {
          otaInProgress = false;
        }
      });
}

bool isValidI2SSpeed(uint32_t speed) {
  return speed == 8000000 || speed == 16000000 || speed == 20000000;
}

bool isValidDriver(uint32_t drv) {
  return drv >= 0 && drv <= 5;
}



void displaySetup(bool swapBlueGreen, bool swapBlueRed, uint8_t displayBright, uint8_t displayRotation, uint8_t driver, uint32_t i2cSpeed, uint8_t E_pin)
{
  HUB75_I2S_CFG mxconfig(64, 64, 1);

  if (swapBlueGreen)
  {
    // Swap Blue and Green pins because the panel is RBG instead of RGB.
    mxconfig.gpio.b1 = 26;
    mxconfig.gpio.b2 = 12;
    mxconfig.gpio.g1 = 27;
    mxconfig.gpio.g2 = 13;
  }

  if (swapBlueRed)
  {
    // Swap Blue and Red pins. 
    mxconfig.gpio.b1 = 25;
    mxconfig.gpio.b2 = 14;
    mxconfig.gpio.r1 = 27;
    mxconfig.gpio.r2 = 13;
  }

  mxconfig.gpio.e = E_pin;
  mxconfig.clkphase = false;

  if (isValidDriver(driver)) {
    mxconfig.driver = static_cast<HUB75_I2S_CFG::shift_driver>(driver);
  } else {
    SerialLog::instance().printf("[ERROR] Invalid driver from config:%d\n", driver);
  }
  if (isValidI2SSpeed(i2cSpeed)) {
    mxconfig.i2sspeed = static_cast<HUB75_I2S_CFG::clk_speed>(i2cSpeed);
  } else {
    SerialLog::instance().printf("[ERROR] Invalid I2S speed from config:%d\n", i2cSpeed);
  }

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(displayBright);
  dma_display->clearScreen();
  dma_display->setRotation(displayRotation);
}

// Feed the controller every 100 ms, advance the fade each loop, and log diagnostics once per second.
void automaticBrightControl()
{
  ClockwiseParams *params = ClockwiseParams::getInstance();
  autoBrightEnabled = params->autoBrightMax > params->autoBrightMin;
  if (!autoBrightEnabled)
    return;

  cw::AutoBrightnessController &controller = cw::AutoBrightnessController::instance();
  controller.configure(params->autoBrightMin, params->autoBrightMax, params->autoBrightLevels, params->autoBrightFade);

  const uint32_t now = millis();
  if (now - autoBrightSampleMillis >= 100) {
    autoBrightSampleMillis = now;
    lastLdrReading = analogRead(params->ldrPin);
    controller.addSample(lastLdrReading, params->displayBright);
  }

  const uint8_t brightness = controller.advance(now);
  if (brightness != lastAppliedBrightness) {
    dma_display->setBrightness8(brightness);
    lastAppliedBrightness = brightness;
  }

  if (now - autoBrightDebugMillis >= 1000) {
    autoBrightDebugMillis = now;
    SerialLog::instance().printf("[AutoBright] ADC: %u, level: %u/%u, target: %u, brightness: %u\n",
        lastLdrReading, controller.level(), controller.levels(), controller.targetBrightness(), brightness);
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(ESP32_LED_BUILTIN, OUTPUT);

  StatusController::getInstance()->blink_led(5, 100);

  ClockwiseParams::getInstance()->load();
  if (!LittleFS.begin(true))
    SerialLog::instance().println("[Canvas] LittleFS mount failed; local cache and upload are unavailable");

  pinMode(ClockwiseParams::getInstance()->ldrPin, INPUT);

  uint8_t driver = ClockwiseParams::getInstance()->driver;
  uint32_t i2cSpeed = ClockwiseParams::getInstance()->i2cSpeed;
  uint8_t E_pin = ClockwiseParams::getInstance()->E_pin;
  
  displaySetup(ClockwiseParams::getInstance()->swapBlueGreen, ClockwiseParams::getInstance()->swapBlueRed, ClockwiseParams::getInstance()->displayBright, ClockwiseParams::getInstance()->displayRotation, driver, i2cSpeed, E_pin);
  cw::AutoBrightnessController::instance().begin(ClockwiseParams::getInstance()->displayBright, millis());
  lastAppliedBrightness = ClockwiseParams::getInstance()->displayBright;
  autoBrightSampleMillis = millis() - 100;
  clockface = new Clockface(dma_display);

  autoBrightEnabled = (ClockwiseParams::getInstance()->autoBrightMax > 0);

  StatusController::getInstance()->clockwiseLogo();
  delay(1000);

  StatusController::getInstance()->wifiConnecting();
  wifi.begin(); // non-blocking; provisioning continues in loop()
  clockface->setup(&cwDateTime);
}

// Runs once, as soon as Wi-Fi is up (stored credentials, Improv or AP portal).
void onNetworkReady()
{
  ClockwiseParams *params = ClockwiseParams::getInstance();
  if (params->otaEnabled)
  {
    if (!params->otaPassword.isEmpty())
      ArduinoOTA.setPassword(params->otaPassword.c_str());
    setupOtaDisplayCallbacks();
    ArduinoOTA.begin();
    SerialLog::instance().println("[OTA] Ready for wireless updates");
  }
  else
  {
    SerialLog::instance().println("[OTA] Disabled in settings");
  }

  StatusController::getInstance()->ntpConnecting();
  cwDateTime.begin(ClockwiseParams::getInstance()->timeZone.c_str(), 
      ClockwiseParams::getInstance()->use24hFormat, 
      ClockwiseParams::getInstance()->ntpServer.c_str(),
      ClockwiseParams::getInstance()->manualPosix.c_str());
  clockface->onNetworkReady();
}

void loop()
{
  if (wifi.update()) // also services Improv Serial on every pass
  {
    onNetworkReady();
  }

  if (wifi.isConnected())
  {
    ArduinoOTA.handle();
    if (!otaInProgress) {
      ClockwiseWebServer *webServer = ClockwiseWebServer::getInstance();
      webServer->handleHttpRequest();
      if (webServer->takeCanvasDownload())
        clockface->downloadConfiguredUrl();
      if (webServer->takeCanvasUpload() && !clockface->loadUploadedDefinition())
        SerialLog::instance().println("[Canvas] Could not activate uploaded clockface");
      if (webServer->takeCanvasCacheClear() && !clockface->clearLocalCache())
        SerialLog::instance().println("[Canvas] Could not clear local cache");
    }
    ezt::events();
  }

  if (!otaInProgress) {
    clockface->update();
    automaticBrightControl();
  }

  if (otaRestartPending && static_cast<int32_t>(millis() - otaRestartAtMillis) >= 0) {
    ESP.restart();
  }
}
