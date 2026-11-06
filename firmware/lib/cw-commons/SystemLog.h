#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

class SystemLog
{
public:
  static SystemLog &instance()
  {
    static SystemLog log;
    return log;
  }

  void logf(const char *format, ...)
  {
    char message[LINE_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    message[sizeof(message) - 1] = '\0';

    Serial.println(message);
    appendLine(message);
  }

  void appendChar(uint8_t value)
  {
    if (value == '\r')
      return;

    char completed[LINE_LENGTH];
    bool hasCompletedLine = false;
    Ring &buffer = ring();
    portENTER_CRITICAL(&buffer.mux);
    if (value == '\n') {
      pendingLine_[pendingLength_] = '\0';
      memcpy(completed, pendingLine_, pendingLength_ + 1);
      pendingLength_ = 0;
      hasCompletedLine = true;
    } else if (pendingLength_ < LINE_LENGTH - 1) {
      pendingLine_[pendingLength_++] = static_cast<char>(value);
    } else {
      pendingLine_[LINE_LENGTH - 1] = '\0';
      memcpy(completed, pendingLine_, LINE_LENGTH);
      pendingLength_ = 0;
      hasCompletedLine = true;
    }
    portEXIT_CRITICAL(&buffer.mux);

    if (hasCompletedLine)
      appendLine(completed);
  }

  void appendLine(const char *message)
  {
    char timestamped[LINE_LENGTH];
    snprintf(timestamped, sizeof(timestamped), "[%lu ms] %s", static_cast<unsigned long>(millis()), message);

    Ring &buffer = ring();
    portENTER_CRITICAL(&buffer.mux);
    uint8_t index;
    if (buffer.count < LINE_COUNT) {
      index = (buffer.first + buffer.count) % LINE_COUNT;
      ++buffer.count;
    } else {
      index = buffer.first;
      buffer.first = (buffer.first + 1) % LINE_COUNT;
    }
    strncpy(buffer.lines[index], timestamped, LINE_LENGTH - 1);
    buffer.lines[index][LINE_LENGTH - 1] = '\0';
    portEXIT_CRITICAL(&buffer.mux);
  }

  void writeTo(Print &output)
  {
    char snapshot[LINE_COUNT][LINE_LENGTH];
    uint8_t count;
    Ring &buffer = ring();
    portENTER_CRITICAL(&buffer.mux);
    count = buffer.count;
    for (uint8_t index = 0; index < count; ++index) {
      const uint8_t source = (buffer.first + index) % LINE_COUNT;
      strncpy(snapshot[index], buffer.lines[source], LINE_LENGTH);
      snapshot[index][LINE_LENGTH - 1] = '\0';
    }
    portEXIT_CRITICAL(&buffer.mux);

    if (count == 0) {
      output.println("No system log entries.");
      return;
    }
    for (uint8_t index = 0; index < count; ++index)
      output.println(snapshot[index]);
  }

private:
  static const uint8_t LINE_COUNT = 16;
  static const uint8_t LINE_LENGTH = 160;

  struct Ring
  {
    char lines[LINE_COUNT][LINE_LENGTH];
    uint8_t first;
    uint8_t count;
    portMUX_TYPE mux;

    Ring() : first(0), count(0), mux(portMUX_INITIALIZER_UNLOCKED)
    {
      memset(lines, 0, sizeof(lines));
    }
  };

  static Ring &ring()
  {
    static Ring buffer;
    return buffer;
  }

  char pendingLine_[LINE_LENGTH] = {};
  uint8_t pendingLength_ = 0;
};

class SerialLog : public Print
{
public:
  static SerialLog &instance()
  {
    static SerialLog logger;
    return logger;
  }

  size_t write(uint8_t value) override
  {
    Serial.write(value);
    SystemLog::instance().appendChar(value);
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override
  {
    const size_t written = Serial.write(buffer, size);
    for (size_t index = 0; index < written; ++index)
      SystemLog::instance().appendChar(buffer[index]);
    return written;
  }

  int printf(const char *format, ...)
  {
    char message[192];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
      return length;
    const size_t written = length < static_cast<int>(sizeof(message)) ? length : sizeof(message) - 1;
    write(reinterpret_cast<const uint8_t *>(message), written);
    return length;
  }
};
