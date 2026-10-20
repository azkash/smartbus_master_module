#pragma once

#include <Arduino.h>

// Mirrors normal USB serial output into a bounded in-memory line buffer. The
// oldest lines are overwritten, so leaving the web monitor enabled cannot grow
// heap usage over time.
class MirroredSerial : public Print {
 public:
  explicit MirroredSerial(HardwareSerial& port) : port_(port) {}

  void begin(unsigned long baud) { port_.begin(baud); }
  int available() { return port_.available(); }
  String readStringUntil(char terminator) { return port_.readStringUntil(terminator); }
  void flush() { port_.flush(); }
  operator bool() const { return true; }

  size_t write(uint8_t value) override {
    const size_t written = port_.write(value);
    portENTER_CRITICAL(&mux_);
    if (value == '\n') {
      commitLineLocked();
    } else if (value != '\r') {
      if (currentLength_ >= LOG_LINE_LENGTH - 1) commitLineLocked();
      current_[currentLength_++] = static_cast<char>(value);
      current_[currentLength_] = '\0';
    }
    portEXIT_CRITICAL(&mux_);
    return written;
  }

  using Print::write;

  String snapshot() {
    char* linesCopy = static_cast<char*>(malloc(sizeof(lines_)));
    if (!linesCopy) return String("[WEB] insufficient heap to copy log\n");

    size_t headCopy;
    size_t countCopy;
    char currentCopy[LOG_LINE_LENGTH];
    portENTER_CRITICAL(&mux_);
    memcpy(linesCopy, lines_, sizeof(lines_));
    memcpy(currentCopy, current_, sizeof(current_));
    headCopy = head_;
    countCopy = count_;
    portEXIT_CRITICAL(&mux_);

    String output;
    output.reserve(countCopy * 80 + strlen(currentCopy) + 32);
    const size_t first = (headCopy + LOG_LINE_COUNT - countCopy) % LOG_LINE_COUNT;
    for (size_t i = 0; i < countCopy; ++i) {
      const size_t index = (first + i) % LOG_LINE_COUNT;
      output += linesCopy + index * LOG_LINE_LENGTH;
      output += '\n';
    }
    if (currentCopy[0] != '\0') output += currentCopy;
    free(linesCopy);
    return output;
  }

 private:
  static constexpr size_t LOG_LINE_COUNT = 80;
  static constexpr size_t LOG_LINE_LENGTH = 160;

  void commitLineLocked() {
    current_[currentLength_] = '\0';
    strlcpy(lines_[head_], current_, LOG_LINE_LENGTH);
    head_ = (head_ + 1) % LOG_LINE_COUNT;
    if (count_ < LOG_LINE_COUNT) ++count_;
    currentLength_ = 0;
    current_[0] = '\0';
  }

  HardwareSerial& port_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  char lines_[LOG_LINE_COUNT][LOG_LINE_LENGTH] = {};
  char current_[LOG_LINE_LENGTH] = {};
  size_t currentLength_ = 0;
  size_t head_ = 0;
  size_t count_ = 0;
};

static MirroredSerial SmartBusSerial(::Serial);
#undef Serial
#define Serial SmartBusSerial
