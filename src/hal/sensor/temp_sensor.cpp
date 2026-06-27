#include "temp_sensor.h"
#include <iostream>

namespace dev_sys::hal {

TempSensor::TempSensor(Type type, int bus_id, int addr)
    : type_(type), bus_id_(bus_id), addr_(addr) {}

bool TempSensor::init() {
    // TODO: Initialize 1-Wire / ADC interface
    // DS18B20: read device ID, verify
    // NTC: configure ADC channel
    return true;
}

bool TempSensor::is_healthy() const {
    // TODO: Check if sensor responds, value in valid range (-40 to 125 C)
    return true;
}

double TempSensor::read_value() {
    // TODO: Read temperature value
    // DS18B20: 1-Wire read scratchpad, parse temperature
    // NTC: ADC read -> voltage -> resistance -> temperature (Steinhart-Hart)
    last_value_ = 25.0; // placeholder
    return last_value_;
}

} // namespace dev_sys::hal
