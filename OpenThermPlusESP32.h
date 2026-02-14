#pragma once
#include <Arduino.h>
#include "OpenThermProtocolItems.h"

/*
  OpenTherm Plus (OT/+) library for ESP32 (Arduino framework)

  Key points from spec:
  - Master initiates a conversation (single request frame), Slave replies with single frame.
  - Frame: start bit '1' + 32-bit payload + stop bit '1'
  - Payload: [P][MSG-TYPE(3)][SPARE(4)][DATA-ID(8)][DATA-VALUE(16)]
  - Parity: even across ALL 32 payload bits (including parity bit itself).
  - Manchester / Bi-phase-L encoding.
  - Timing: slave response 20..800ms; master waits >=100ms between conversations; communicate at least every 1s.
*/

class OpenThermPlusESP32 {
public:
  enum class MsgType : uint8_t {
    // Master->Slave
    READ_DATA      = 0b000,
    WRITE_DATA     = 0b001,
    INVALID_DATA   = 0b010,
    // 0b011 reserved
    // Slave->Master
    READ_ACK       = 0b100,
    WRITE_ACK      = 0b101,
    DATA_INVALID   = 0b110,
    UNKNOWN_DATAID = 0b111,
  };

  enum class Result : uint8_t {
    OK = 0,
    TIMEOUT,
    FRAME_ERROR,
    PARITY_ERROR,
    BAD_RESPONSE,
    NOT_INITIALIZED
  };

  struct Frame {
    MsgType  type;
    uint8_t  dataId;
    uint16_t value;     // raw 16-bit value (f8.8, u16, etc.)
    bool     parityOk;
  };

  struct Config {
    // Required: connect ESP32 pins to adapter pins
    // txPin -> adapter "IN" (ESP32 drives opto LED)
    // rxPin -> adapter "OUT" (ESP32 reads opto transistor output)
    uint8_t txPin = 255;
    uint8_t rxPin = 255;

    // Practical options
    uint16_t bitTimeUs = 1000;         // nominal 1000us per OT/+ bit
    uint16_t rxGlitchFilterUs = 80;    // ignore edges faster than this (PC817-friendly)
    uint32_t slaveResponseTimeoutMs = 900; // wait for slave response (spec max 800ms; keep margin)
    uint16_t interMessageGapMs = 100;  // spec min 100ms

    // Robustness
    uint8_t retries = 2;               // additional attempts on failure

    // Logic handling
    bool invertTx = false;
    bool invertRx = false;
    bool autoDetectLogic = true;       // try combinations of invertTx/invertRx using an ID0 probe

    // Polling helpers (optional)
    bool enablePolling = false;
    uint16_t pollIntervalMs = 1000;    // spec: master should communicate at least every 1s (+15%)
    bool pollStatusId0 = true;         // ID 0 status exchange
    bool pollBoilerTempId25 = true;    // ID 25 boiler temp
  };

  // Constructors
  OpenThermPlusESP32() = default;

  // Setup
  bool begin(const Config& cfg);
  bool isReady() const { return _ready; }
  const Config& config() const { return _cfg; }

  // Helpers for OT formats
  static uint16_t encodeF8_8(float v);
  static float    decodeF8_8(uint16_t raw);

  // High-level controls
  // Master status bits are in HB for ID0 (CH enable bit0, DHW enable bit1, etc.)
  void setCHEnable(bool en);
  void setDHWEnable(bool en);
  void setCoolingEnable(bool en);
  void setOTCActive(bool en);
  void setCH2Enable(bool en);
  // Master status: Fault reset (bit7 in master status HB for ID0)
  void setFaultReset(bool en);

  // Transactions (one-shot)
  Result readData(uint8_t dataId, Frame& outResp);
  Result writeData(uint8_t dataId, uint16_t value, Frame& outResp);
  Result invalidData(uint8_t dataId, uint16_t value, Frame& outResp);

  // Special status exchange (ID0): master sends READ_DATA with HB=masterStatus, LB=00.
  Result readStatus(Frame& outResp);

  // Convenience: write CH control setpoint (ID1) in degrees C
  Result writeTset(float tC, Frame& outResp);

  // Convenience: read boiler flow temperature (ID25) as degrees C
  Result readBoilerTemp(float& outTC, Frame& outResp);

  // Polling API: call loop() frequently if enablePolling=true
  void loop();
  bool hasLastStatus() const { return _hasLastStatus; }
  bool hasLastBoilerTemp() const { return _hasLastTboiler; }
  Frame lastStatusFrame() const { return _lastStatus; }
  Frame lastBoilerTempFrame() const { return _lastTboiler; }
  float lastBoilerTempC() const { return decodeF8_8(_lastTboiler.value); }

  // RX diagnostics
  uint32_t edgeOverflowCount() const { return _edgeOverflow; }

  // Raw payload helpers (useful for debugging)
  static uint32_t buildPayload(MsgType type, uint8_t dataId, uint16_t value);
  static Frame    parsePayload(uint32_t payload);


  // Metadata (subset extracted from OT v2.2 tables; unknown IDs return nullptr)
  static const OpenThermProtocol::DataIdInfo* dataIdInfo(uint8_t dataId) { return OpenThermProtocol::find(dataId); }

  // Common field helpers
  static uint8_t hiByte(uint16_t v) { return (uint8_t)((v >> 8) & 0xFF); }
  static uint8_t loByte(uint16_t v) { return (uint8_t)(v & 0xFF); }
  static uint16_t packU8U8(uint8_t hb, uint8_t lb) { return (uint16_t)(((uint16_t)hb << 8) | lb); }
  static int16_t  asS16(uint16_t v) { return (int16_t)v; }

private:
  Config _cfg{};
  bool _ready = false;

  // Master status byte (HB) used in status exchange
  uint8_t _masterStatus = 0;

  // Timing
  uint32_t _lastConversationEndMs = 0;
  uint32_t _lastPollMs = 0;
  uint8_t _pollPhase = 0;

  // Cached polled values
  bool _hasLastStatus = false;
  bool _hasLastTboiler = false;
  Frame _lastStatus{};
  Frame _lastTboiler{};

  // ISR receive state (ring buffer of edges for better robustness on ESP32)
  volatile uint32_t _lastEdgeUs = 0;

  static constexpr uint8_t EDGE_BUF_SIZE = 32; // power of two
  volatile uint32_t _edgeDeltaBuf[EDGE_BUF_SIZE] = {0};
  volatile uint8_t  _edgeLevelBuf[EDGE_BUF_SIZE] = {0};
  volatile uint8_t  _edgeHead = 0; // next write
  volatile uint8_t  _edgeTail = 0; // next read
  volatile uint32_t _edgeOverflow = 0;

  static OpenThermPlusESP32* _isrInstance;
  static void IRAM_ATTR isrEdgeThunk();
  void IRAM_ATTR onEdge();

  // Line IO helpers
  inline void writeLine(bool idleLevel);
  inline int  readLine();

  // Manchester timing derived
  uint16_t _bitUs = 1000;
  uint16_t _halfUs = 500;

  // Core operations
  void enforceInterMessageGap();
  void sendManchesterBit(bool bit);
  bool sendPayload(uint32_t payload);

  bool waitEdge(uint32_t timeoutUs, uint32_t& deltaUs, int& level);
  bool receiveManchesterFrame(uint32_t& payload, uint32_t timeoutMs);
  bool receivePayload(uint32_t& payload, uint32_t timeoutMs);

  Result transact(MsgType reqType, uint8_t dataId, uint16_t value, Frame& outResp, bool expectSameId = true);

  // Parity
  static bool evenParity32(uint32_t x);
  static uint32_t setParityBit(uint32_t payloadNoParity);

  // Auto detect
  bool autoDetectLogic();
  bool probeStatusOnce(Frame& outResp);
};
