#include <Arduino.h>
#include "flow_sensor.h"
#include "../board_pins.h"

namespace flow {

static FlowConfig gConfig;

// ISR-shared pulse counter (volatile for ISR safety)
static volatile uint32_t gPulseCount = 0;

// Snapshot values computed by update()
static float gFrequency_hz = 0.0f;
static float gFlowRate_lpm = 0.0f;       // instantaneous
static float gFlowRateAvg_lpm = 0.0f;    // EMA-smoothed
static uint32_t gLastPulses = 0;
static uint32_t gLastUpdateMicros = 0;

static void IRAM_ATTR onPulse() {
  gPulseCount++;
}

void init(const FlowConfig& cfg) {
  gConfig = cfg;
  pinMode(FLOW_SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), onPulse, RISING);
  gLastUpdateMicros = micros();
}

void setConfig(const FlowConfig& cfg) {
  gConfig = cfg;
}

void update() {
  // Atomically grab and reset pulse count
  noInterrupts();
  uint32_t pulses = gPulseCount;
  gPulseCount = 0;
  interrupts();

  uint32_t now = micros();
  float dt = (now - gLastUpdateMicros) / 1e6f;
  gLastUpdateMicros = now;

  if (dt > 0.0f) {
    gFrequency_hz = pulses / dt;
    gFlowRate_lpm = gFrequency_hz / gConfig.k_factor;

    // EMA smoothing: alpha = dt / (tau + dt)
    float tau = gConfig.avg_period_s;
    if (tau > 0.0f) {
      float alpha = dt / (tau + dt);
      gFlowRateAvg_lpm += alpha * (gFlowRate_lpm - gFlowRateAvg_lpm);
    } else {
      gFlowRateAvg_lpm = gFlowRate_lpm;
    }
  }
  gLastPulses = pulses;
}

float getFrequency_hz() {
  return gFrequency_hz;
}

float getFlowRate_lpm() {
  return gFlowRate_lpm;
}

float flowToSpeed_ms(float flow_lpm) {
  // L/min -> m³/s: divide by 60, divide by 1000
  float flow_m3s = flow_lpm / 60.0f / 1000.0f;
  return flow_m3s / gConfig.cross_section_m2;
}

float getSpeed_ms() {
  return flowToSpeed_ms(gFlowRateAvg_lpm);
}

uint32_t getLastPulseCount() {
  return gLastPulses;
}

}  // namespace flow
