#include "ms5837.h"
#include <Arduino.h>
#include <Wire.h>
#include <cstring>

namespace ms5837 {

namespace {

// ---------- Commands ----------
constexpr uint8_t CMD_RESET          = 0x1E;
constexpr uint8_t CMD_ADC_READ       = 0x00;
constexpr uint8_t CMD_PROM_READ      = 0xA0;  // + 2*word
constexpr uint8_t CMD_CONVERT_D1_8192 = 0x4A;  // pressure, OSR 8192
constexpr uint8_t CMD_CONVERT_D2_8192 = 0x5A;  // temperature, OSR 8192

// OSR 8192 max conversion time per datasheet is 18.08 ms; round up for margin.
constexpr uint32_t CONV_TIME_MS = 20;

enum class State { IDLE, WAIT_D1, WAIT_D2 };

bool     gPresent    = false;
uint16_t gProm[8]    = {0};  // C[0]=factory+CRC, C[1..6]=calibration coeffs, C[7]=0 (unused, CRC pad)
State    gState       = State::IDLE;
uint32_t gConvStartMs = 0;
uint32_t gRawD1       = 0;
uint32_t gRawD2       = 0;
bool     gNewSample   = false;
float    gPressureMbar = 0.0f;
float    gTemperatureC = 0.0f;

bool writeCmd(uint8_t cmd) {
    Wire.beginTransmission(MS5837_ADDR);
    Wire.write(cmd);
    return Wire.endTransmission() == 0;
}

bool readWord(uint8_t cmd, uint16_t& out) {
    Wire.beginTransmission(MS5837_ADDR);
    Wire.write(cmd);
    if (Wire.endTransmission(false) != 0) return false;  // repeated start
    if (Wire.requestFrom((int)MS5837_ADDR, 2) != 2) return false;
    uint16_t hi = Wire.read();
    uint16_t lo = Wire.read();
    out = (hi << 8) | lo;
    return true;
}

bool readAdc24(uint32_t& out) {
    Wire.beginTransmission(MS5837_ADDR);
    Wire.write(CMD_ADC_READ);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)MS5837_ADDR, 3) != 3) return false;
    uint32_t b2 = Wire.read();
    uint32_t b1 = Wire.read();
    uint32_t b0 = Wire.read();
    out = (b2 << 16) | (b1 << 8) | b0;
    return true;
}

// Standard MS56xx/MS58xx-family CRC4 (datasheet Application Note AN520).
// Mutates a working copy: masks the CRC nibble out of word 0, zeroes word 7.
uint8_t crc4(uint16_t n_prom[8]) {
    uint16_t n_rem = 0;
    n_prom[0] = n_prom[0] & 0x0FFF;
    n_prom[7] = 0;
    for (uint8_t i = 0; i < 16; i++) {
        if (i % 2 == 1) n_rem ^= (uint16_t)(n_prom[i >> 1] & 0x00FF);
        else            n_rem ^= (uint16_t)(n_prom[i >> 1] >> 8);
        for (uint8_t bit = 8; bit > 0; bit--) {
            if (n_rem & 0x8000) n_rem = (n_rem << 1) ^ 0x3000;
            else                n_rem = (n_rem << 1);
        }
    }
    return (uint8_t)((n_rem >> 12) & 0x000F);
}

// MS5837-30BA temperature + 2nd-order pressure compensation (datasheet
// section 4). Pressure result is in units of 0.1 mbar; temperature in
// 0.01 degC. Coefficients are indexed C[1..6] as printed in the datasheet.
void compute(uint32_t d1, uint32_t d2) {
    int32_t dT = (int32_t)d2 - ((int32_t)gProm[5] << 8);

    int64_t SENS = ((int64_t)gProm[1] << 15) + (((int64_t)gProm[3] * dT) >> 8);
    int64_t OFF  = ((int64_t)gProm[2] << 16) + (((int64_t)gProm[4] * dT) >> 7);
    int32_t TEMP = 2000 + (int32_t)(((int64_t)dT * gProm[6]) >> 23);

    int64_t Ti = 0, OFFi = 0, SENSi = 0;
    if (TEMP < 2000) {
        Ti    = (3 * (int64_t)dT * (int64_t)dT) >> 33;
        OFFi  = (3 * (int64_t)(TEMP - 2000) * (TEMP - 2000)) / 2;
        SENSi = (5 * (int64_t)(TEMP - 2000) * (TEMP - 2000)) / 8;
        if (TEMP < -1500) {
            OFFi  += 7 * (int64_t)(TEMP + 1500) * (TEMP + 1500);
            SENSi += 4 * (int64_t)(TEMP + 1500) * (TEMP + 1500);
        }
    } else {
        Ti = (2 * (int64_t)dT * (int64_t)dT) >> 37;
        OFFi = (int64_t)(TEMP - 2000) * (TEMP - 2000) / 16;
        SENSi = 0;
    }

    int64_t OFF2  = OFF - OFFi;
    int64_t SENS2 = SENS - SENSi;
    TEMP -= (int32_t)Ti;

    int64_t P = (((int64_t)d1 * SENS2) / 2097152LL - OFF2) / 8192LL;

    gPressureMbar = (float)P / 10.0f;
    gTemperatureC = (float)TEMP / 100.0f;
}

}  // namespace

bool init() {
    gPresent = false;
    gState   = State::IDLE;

    if (!writeCmd(CMD_RESET)) return false;
    delay(10);  // one-time boot reset, datasheet-recommended settle time

    for (uint8_t word = 0; word < 7; word++) {
        if (!readWord(CMD_PROM_READ + word * 2, gProm[word])) return false;
    }
    gProm[7] = 0;

    uint16_t crcWorking[8];
    memcpy(crcWorking, gProm, sizeof(crcWorking));
    uint8_t crcRead       = (gProm[0] >> 12) & 0x0F;
    uint8_t crcCalculated = crc4(crcWorking);
    if (crcCalculated != crcRead) {
        Serial.printf("[MS5837] CRC mismatch (read=%X calc=%X) — sensor absent or faulty\n",
                      crcRead, crcCalculated);
        return false;
    }

    gPresent = true;
    return true;
}

bool isPresent() { return gPresent; }

void update() {
    if (!gPresent) return;
    uint32_t now = millis();

    switch (gState) {
        case State::IDLE:
            if (writeCmd(CMD_CONVERT_D1_8192)) {
                gConvStartMs = now;
                gState       = State::WAIT_D1;
            }
            break;
        case State::WAIT_D1:
            if (now - gConvStartMs >= CONV_TIME_MS) {
                readAdc24(gRawD1);
                if (writeCmd(CMD_CONVERT_D2_8192)) {
                    gConvStartMs = now;
                    gState       = State::WAIT_D2;
                } else {
                    gState = State::IDLE;
                }
            }
            break;
        case State::WAIT_D2:
            if (now - gConvStartMs >= CONV_TIME_MS) {
                readAdc24(gRawD2);
                compute(gRawD1, gRawD2);
                gNewSample = true;
                gState     = State::IDLE;
            }
            break;
    }
}

bool hasNewSample() {
    if (!gNewSample) return false;
    gNewSample = false;
    return true;
}

float pressureMbar() { return gPressureMbar; }
float temperatureC() { return gTemperatureC; }

}  // namespace ms5837
