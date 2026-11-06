#include "unity.h"
#include "../../lib/cw-commons/AutoBrightnessController.h"

void setUp(void) {
  // set stuff up here
}

void tearDown(void) {
  // clean stuff up here
}

void test_function_should_be_true(void) {
  TEST_ASSERT_TRUE(true);
}

void test_auto_brightness_median_rejects_single_spike(void) {
  cw::AutoBrightnessController &controller = cw::AutoBrightnessController::instance();
  controller.begin(32, 0);
  controller.configure(500, 3000, 16, 40);

  controller.addSample(1000, 255);
  controller.addSample(1010, 255);
  controller.addSample(4095, 255);
  controller.addSample(990, 255);
  controller.addSample(1005, 255);
  controller.addSample(998, 255);
  controller.addSample(1002, 255);
  controller.addSample(1004, 255);
  controller.addSample(997, 255);

  TEST_ASSERT_TRUE(controller.filteredSensor() >= 1000);
  TEST_ASSERT_TRUE(controller.filteredSensor() <= 1010);
  TEST_ASSERT_TRUE(controller.level() > 0);
}

void test_auto_brightness_ema_smooths_sustained_light_change(void) {
  cw::AutoBrightnessController &controller = cw::AutoBrightnessController::instance();
  controller.begin(32, 0);
  controller.configure(0, 4095, 16, 40);

  for (uint8_t i = 0; i < 9; ++i)
    controller.addSample(1000, 255);
  for (uint8_t i = 0; i < 9; ++i)
    controller.addSample(2000, 255);

  TEST_ASSERT_TRUE(controller.filteredSensor() > 1000);
  TEST_ASSERT_TRUE(controller.filteredSensor() < 1300);
}

void test_auto_brightness_fades_in_bounded_steps(void) {
  cw::AutoBrightnessController &controller = cw::AutoBrightnessController::instance();
  controller.begin(10, 0);
  controller.configure(0, 1000, 16, 20);
  for (uint8_t i = 0; i < 9; ++i)
    controller.addSample(1000, 255);

  TEST_ASSERT_EQUAL_UINT8(255, controller.targetBrightness());
  TEST_ASSERT_EQUAL_UINT8(12, controller.advance(100));
  for (uint32_t now = 200; now <= 13000; now += 100)
    controller.advance(now);
  TEST_ASSERT_EQUAL_UINT8(255, controller.brightness());
}

// Defined in WiFiSetupStateMachineTests.cpp
void run_wifi_setup_state_machine_tests(void);

int runUnityTests(void) {
  UNITY_BEGIN();
  RUN_TEST(test_function_should_be_true);
  RUN_TEST(test_auto_brightness_median_rejects_single_spike);
  RUN_TEST(test_auto_brightness_ema_smooths_sustained_light_change);
  RUN_TEST(test_auto_brightness_fades_in_bounded_steps);
  run_wifi_setup_state_machine_tests();
  return UNITY_END();
}


int main() {
  runUnityTests();  
}

