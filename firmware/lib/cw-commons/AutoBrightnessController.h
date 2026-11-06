#pragma once

#include <stdint.h>

namespace cw {

// Ambient brightness pipeline: take the median of the latest five ADC samples,
// then smooth each median with a fixed-point 1/8 EMA before mapping it to a target.
// Level 0 means off; lit levels range from 1 to `levels`. Counts are bounded to
// 2..256 (the settings UI presets are 10, 16, 32, 64, and 256). `fadeRate` is
// brightness counts per second; zero applies the target immediately.
class AutoBrightnessController
{
public:
    static AutoBrightnessController& instance()
    {
        static AutoBrightnessController controller;
        return controller;
    }

    void begin(uint8_t initialBrightness, uint32_t now)
    {
        currentBrightness_ = initialBrightness;
        targetBrightness_ = initialBrightness;
        targetLevel_ = 0;
        filteredSensorQ8_ = 0;
        sampleCount_ = 0;
        sampleIndex_ = 0;
        filterInitialized_ = false;
        lastRampMillis_ = now;
        fadeRemainder_ = 0;
        lastDirection_ = 0;
    }

    void configure(uint16_t sensorMin, uint16_t sensorMax, uint16_t levels, uint16_t fadeRate)
    {
        sensorMin_ = sensorMin;
        sensorMax_ = sensorMax;
        levels_ = levels < 2 ? 2 : (levels > 256 ? 256 : levels);
        fadeRate_ = fadeRate;
    }

    bool addSample(uint16_t reading, uint8_t maxBrightness)
    {
        samples_[sampleIndex_] = reading;
        sampleIndex_ = (sampleIndex_ + 1) % 5;
        if (sampleCount_ < 5)
            ++sampleCount_;
        if (sampleCount_ < 5)
            return false;

        uint16_t ordered[5];
        for (uint8_t i = 0; i < 5; ++i)
            ordered[i] = samples_[i];
        for (uint8_t i = 1; i < 5; ++i) {
            uint16_t value = ordered[i];
            int8_t j = i - 1;
            while (j >= 0 && ordered[j] > value) {
                ordered[j + 1] = ordered[j];
                --j;
            }
            ordered[j + 1] = value;
        }

        const int32_t medianQ8 = static_cast<int32_t>(ordered[2]) << 8;
        if (!filterInitialized_) {
            filteredSensorQ8_ = medianQ8;
            filterInitialized_ = true;
        } else {
            filteredSensorQ8_ += (medianQ8 - filteredSensorQ8_) / 8;
        }
        updateTarget(maxBrightness);
        return true;
    }

    uint8_t advance(uint32_t now)
    {
        uint32_t elapsed = now - lastRampMillis_;
        lastRampMillis_ = now;
        // Avoid turning a long main-loop pause into a sudden brightness jump.
        if (elapsed > 100)
            elapsed = 100;

        if (fadeRate_ == 0) {
            currentBrightness_ = targetBrightness_;
            fadeRemainder_ = 0;
            lastDirection_ = 0;
            return currentBrightness_;
        }

        const int16_t difference = static_cast<int16_t>(targetBrightness_) - currentBrightness_;
        const int8_t direction = difference > 0 ? 1 : (difference < 0 ? -1 : 0);
        if (direction == 0) {
            fadeRemainder_ = 0;
            lastDirection_ = 0;
            return currentBrightness_;
        }
        if (lastDirection_ != 0 && direction != lastDirection_)
            fadeRemainder_ = 0;
        lastDirection_ = direction;

        const uint32_t progress = fadeRemainder_ + static_cast<uint32_t>(fadeRate_) * elapsed;
        uint16_t step = progress / 1000;
        fadeRemainder_ = progress % 1000;
        if (step == 0)
            return currentBrightness_;

        const uint16_t distance = difference > 0 ? difference : -difference;
        if (step >= distance) {
            currentBrightness_ = targetBrightness_;
            fadeRemainder_ = 0;
            lastDirection_ = 0;
        } else {
            currentBrightness_ = static_cast<uint8_t>(currentBrightness_ + direction * step);
        }
        return currentBrightness_;
    }

    uint16_t level() const { return targetLevel_; }
    uint16_t levels() const { return levels_; }
    uint16_t filteredSensor() const { return filterInitialized_ ? filteredSensorQ8_ >> 8 : 0; }
    uint8_t brightness() const { return currentBrightness_; }
    uint8_t targetBrightness() const { return targetBrightness_; }

private:
    AutoBrightnessController()
        : sensorMin_(0), sensorMax_(0), levels_(16), fadeRate_(40), currentBrightness_(0),
          targetBrightness_(0), targetLevel_(0), filteredSensorQ8_(0), sampleCount_(0),
          sampleIndex_(0), filterInitialized_(false), lastRampMillis_(0), fadeRemainder_(0),
          lastDirection_(0)
    {
        for (uint8_t i = 0; i < 5; ++i)
            samples_[i] = 0;
    }

    void updateTarget(uint8_t maxBrightness)
    {
        if (sensorMax_ <= sensorMin_) {
            targetLevel_ = 0;
            targetBrightness_ = 0;
            return;
        }

        const uint16_t span = sensorMax_ - sensorMin_;
        // Require a small extra rise to turn on, while turning off below sensorMin_.
        const uint16_t hysteresis = span / 32 == 0 ? 1 : span / 32;
        const uint16_t sensor = filteredSensor();
        const uint16_t turnOnAt = sensorMin_ + hysteresis;
        if (sensor < sensorMin_ || (targetLevel_ == 0 && sensor < turnOnAt)) {
            targetLevel_ = 0;
            targetBrightness_ = 0;
            return;
        }

        const uint16_t boundedSensor = sensor > sensorMax_ ? sensorMax_ : sensor;
        const uint32_t scaled = static_cast<uint32_t>(boundedSensor - sensorMin_) * (levels_ - 1);
        const uint16_t levelIndex = (scaled + span / 2) / span;
        targetLevel_ = levelIndex + 1;

        const uint8_t minBrightness = maxBrightness < 4 ? maxBrightness : 4;
        const uint16_t brightnessRange = maxBrightness - minBrightness;
        targetBrightness_ = minBrightness +
            (static_cast<uint32_t>(levelIndex) * brightnessRange + (levels_ - 1) / 2) / (levels_ - 1);
    }

    uint16_t sensorMin_;
    uint16_t sensorMax_;
    uint16_t levels_;
    uint16_t fadeRate_;
    uint8_t currentBrightness_;
    uint8_t targetBrightness_;
    uint16_t targetLevel_;
    int32_t filteredSensorQ8_;
    uint16_t samples_[5];
    uint8_t sampleCount_;
    uint8_t sampleIndex_;
    bool filterInitialized_;
    uint32_t lastRampMillis_;
    uint32_t fadeRemainder_;
    int8_t lastDirection_;
};

}