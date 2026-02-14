#include "OpenThermPlusESP32.h"
#include <math.h>

OpenThermPlusESP32* OpenThermPlusESP32::_isrInstance = nullptr;
static inline uint32_t usNow() { return (uint32_t)micros(); }

uint16_t OpenThermPlusESP32::encodeF8_8(float v) {
  int16_t scaled = (int16_t)lroundf(v * 256.0f);
  return (uint16_t)scaled;
}

float OpenThermPlusESP32::decodeF8_8(uint16_t raw) {
  int16_t s = (int16_t)raw;
  return ((float)s) / 256.0f;
}

bool OpenThermPlusESP32::begin(const Config& cfg) {
  _cfg = cfg;
  if (_cfg.txPin == 255 || _cfg.rxPin == 255) return false;

  _bitUs = _cfg.bitTimeUs;
  _halfUs = _bitUs / 2;

  pinMode(_cfg.txPin, OUTPUT);
  pinMode(_cfg.rxPin, INPUT);

  // Set initial idle on TX (idle assumed LOW on MCU side; can be inverted by option)
  writeLine(true);

  _isrInstance = this;
  detachInterrupt(digitalPinToInterrupt(_cfg.rxPin));
  attachInterrupt(digitalPinToInterrupt(_cfg.rxPin), isrEdgeThunk, CHANGE);

  _lastConversationEndMs = 0;
  _lastPollMs = millis();
  _pollPhase = 0;

  _hasLastStatus = false;
  _hasLastTboiler = false;

  // reset edge buffer
  _edgeHead = 0;
  _edgeTail = 0;
  _edgeOverflow = 0;

  _ready = true;

  if (_cfg.autoDetectLogic) {
    if (!autoDetectLogic()) {
      // keep user-provided inversion flags; still allow manual operation
      // but signal not ready
      _ready = false;
      return false;
    }
  }

  return true;
}

void IRAM_ATTR OpenThermPlusESP32::isrEdgeThunk() {
  if (_isrInstance) _isrInstance->onEdge();
}

void IRAM_ATTR OpenThermPlusESP32::onEdge() {
  uint32_t now = usNow();
  uint32_t delta = now - _lastEdgeUs;
  _lastEdgeUs = now;

  int lvl = digitalRead(_cfg.rxPin);
  if (_cfg.invertRx) lvl = !lvl;

  if (delta < _cfg.rxGlitchFilterUs) return;

  // Push into ring buffer (single-producer ISR).
  uint8_t head = _edgeHead;
  uint8_t next = (uint8_t)((head + 1) & (EDGE_BUF_SIZE - 1));
  if (next == _edgeTail) {
    // overflow (drop this edge)
    _edgeOverflow++;
    return;
  }
  _edgeDeltaBuf[head] = delta;
  _edgeLevelBuf[head] = (uint8_t)(lvl ? 1 : 0);
  _edgeHead = next;
}

inline void OpenThermPlusESP32::writeLine(bool idleLevel) {
  // MCU-side convention: idle=LOW, active=HIGH (after optional inversion)
  bool lvl = idleLevel ? 0 : 1;
  if (_cfg.invertTx) lvl = !lvl;
  digitalWrite(_cfg.txPin, lvl);
}

inline int OpenThermPlusESP32::readLine() {
  int lvl = digitalRead(_cfg.rxPin);
  if (_cfg.invertRx) lvl = !lvl;
  return lvl;
}

void OpenThermPlusESP32::enforceInterMessageGap() {
  uint32_t now = millis();
  uint32_t elapsed = now - _lastConversationEndMs;
  if (elapsed < _cfg.interMessageGapMs) delay(_cfg.interMessageGapMs - elapsed);
}

void OpenThermPlusESP32::sendManchesterBit(bool bit) {
  // bit=1 => active->idle (1->0 at mid-bit on wire logic)
  // bit=0 => idle->active
  if (bit) {
    writeLine(false);
    delayMicroseconds(_halfUs);
    writeLine(true);
    delayMicroseconds(_halfUs);
  } else {
    writeLine(true);
    delayMicroseconds(_halfUs);
    writeLine(false);
    delayMicroseconds(_halfUs);
  }
}

bool OpenThermPlusESP32::evenParity32(uint32_t x) {
  return (__builtin_popcount((unsigned)x) % 2) == 0;
}

uint32_t OpenThermPlusESP32::setParityBit(uint32_t payloadNoParity) {
  uint32_t p0 = payloadNoParity & 0x7FFFFFFFul;
  bool even = evenParity32(p0);
  uint32_t parityBit = even ? 0u : 1u;
  return (p0 | (parityBit << 31));
}

uint32_t OpenThermPlusESP32::buildPayload(MsgType type, uint8_t dataId, uint16_t value) {
  uint32_t payload = 0;
  payload |= ((uint32_t)((uint8_t)type & 0x7) << 28);
  payload |= ((uint32_t)dataId << 16);
  payload |= (uint32_t)value;
  payload &= 0x7FFFFFFFul;
  return setParityBit(payload);
}

OpenThermPlusESP32::Frame OpenThermPlusESP32::parsePayload(uint32_t payload) {
  Frame f{};
  uint32_t p = payload;
  uint8_t msg = (p >> 28) & 0x7;
  f.type = (MsgType)msg;
  f.dataId = (uint8_t)((p >> 16) & 0xFF);
  f.value = (uint16_t)(p & 0xFFFF);
  f.parityOk = evenParity32(p);
  return f;
}

bool OpenThermPlusESP32::sendPayload(uint32_t payload) {
  if (!_ready) return false;
  enforceInterMessageGap();

  // Start bit (1), 32 payload bits MSB->LSB, Stop bit (1)
  sendManchesterBit(true);
  for (int i = 31; i >= 0; --i) {
    sendManchesterBit(((payload >> i) & 0x1) != 0);
  }
  sendManchesterBit(true);

  writeLine(true);
  _lastConversationEndMs = millis();
  return true;
}

bool OpenThermPlusESP32::waitEdge(uint32_t timeoutUs, uint32_t& deltaUs, int& level) {
  uint32_t start = usNow();
  while ((usNow() - start) < timeoutUs) {
    // Pop from ring buffer (single-consumer in task context).
    if (_edgeTail != _edgeHead) {
      noInterrupts();
      uint8_t tail = _edgeTail;
      uint8_t head = _edgeHead;
      if (tail != head) {
        uint32_t d = _edgeDeltaBuf[tail];
        uint8_t l = _edgeLevelBuf[tail];
        _edgeTail = (uint8_t)((tail + 1) & (EDGE_BUF_SIZE - 1));
        interrupts();
        deltaUs = d;
        level = (int)l;
        return true;
      }
      interrupts();
    }
    delayMicroseconds(10);
  }
  return false;
}

static inline bool inRange(uint32_t v, uint32_t lo, uint32_t hi) {
  return (v >= lo && v <= hi);
}

bool OpenThermPlusESP32::receiveManchesterFrame(uint32_t& payload, uint32_t timeoutMs) {
  // Decode mid-bit transitions (~bitTimeUs apart). We accept a wider window for opto adapters.
  const uint32_t bitLo = (uint32_t)(_bitUs * 0.85f);
  const uint32_t bitHi = (uint32_t)(_bitUs * 1.30f);
  const uint32_t timeoutUs = timeoutMs * 1000u;

  uint32_t delta;
  int level;
  if (!waitEdge(timeoutUs, delta, level)) return false;

  int prevLevel = !level;

  auto decodeMidToBit = [&](int prev, int after) -> int {
    // MCU-side: idle=0, active=1
    // bit=1 => 1->0, bit=0 => 0->1
    if (prev == 1 && after == 0) return 1;
    if (prev == 0 && after == 1) return 0;
    return -1;
  };

  int startBit = decodeMidToBit(prevLevel, level);
  if (startBit != 1) return false;

  uint32_t raw = 0;

  // 32 payload bits + stop bit
  for (int bitIndex = 0; bitIndex < 33; ++bitIndex) {
    bool got = false;
    uint32_t d2 = 0;
    int l2 = 0;

    uint32_t localStart = usNow();
    while ((usNow() - localStart) < timeoutUs) {
      if (!waitEdge((uint32_t)(_bitUs * 2), d2, l2)) return false;
      if (inRange(d2, bitLo, bitHi)) { got = true; break; }
      prevLevel = l2;
    }
    if (!got) return false;

    int b = decodeMidToBit(prevLevel, l2);
    if (b < 0) return false;
    prevLevel = l2;

    if (bitIndex < 32) raw = (raw << 1) | (uint32_t)b;
    else { if (b != 1) return false; }
  }

  payload = raw;
  return true;
}

bool OpenThermPlusESP32::receivePayload(uint32_t& payload, uint32_t timeoutMs) {
  if (!_ready) return false;
  uint32_t p = 0;
  if (!receiveManchesterFrame(p, timeoutMs)) return false;
  if (!evenParity32(p)) return false;
  payload = p;
  return true;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::transact(MsgType reqType, uint8_t dataId, uint16_t value,
                                                        Frame& outResp, bool expectSameId) {
  if (!_ready) return Result::NOT_INITIALIZED;

  uint32_t req = buildPayload(reqType, dataId, value);

  if (!sendPayload(req)) return Result::FRAME_ERROR;

  uint32_t respPayload = 0;
  if (!receivePayload(respPayload, _cfg.slaveResponseTimeoutMs)) return Result::TIMEOUT;

  Frame r = parsePayload(respPayload);
  if (!r.parityOk) return Result::PARITY_ERROR;
  if (expectSameId && r.dataId != dataId) return Result::BAD_RESPONSE;

  outResp = r;
  return Result::OK;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::readData(uint8_t dataId, Frame& outResp) {
  Result last = Result::TIMEOUT;
  for (uint8_t i = 0; i <= _cfg.retries; ++i) {
    last = transact(MsgType::READ_DATA, dataId, 0x0000, outResp, true);
    if (last == Result::OK) return last;
    delay(50);
  }
  return last;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::writeData(uint8_t dataId, uint16_t value, Frame& outResp) {
  Result last = Result::TIMEOUT;
  for (uint8_t i = 0; i <= _cfg.retries; ++i) {
    last = transact(MsgType::WRITE_DATA, dataId, value, outResp, true);
    if (last == Result::OK) return last;
    delay(50);
  }
  return last;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::invalidData(uint8_t dataId, uint16_t value, Frame& outResp) {
  Result last = Result::TIMEOUT;
  for (uint8_t i = 0; i <= _cfg.retries; ++i) {
    last = transact(MsgType::INVALID_DATA, dataId, value, outResp, true);
    if (last == Result::OK) return last;
    delay(50);
  }
  return last;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::readStatus(Frame& outResp) {
  // Status exchange: READ-DATA id=0 with HB=masterStatus, LB=00
  uint16_t v = ((uint16_t)_masterStatus << 8);
  Result last = Result::TIMEOUT;
  for (uint8_t i = 0; i <= _cfg.retries; ++i) {
    last = transact(MsgType::READ_DATA, 0, v, outResp, true);
    if (last == Result::OK) return last;
    delay(50);
  }
  return last;
}

OpenThermPlusESP32::Result OpenThermPlusESP32::writeTset(float tC, Frame& outResp) {
  uint16_t v = encodeF8_8(tC);
  return writeData(1, v, outResp);
}

OpenThermPlusESP32::Result OpenThermPlusESP32::readBoilerTemp(float& outTC, Frame& outResp) {
  auto res = readData(25, outResp);
  if (res == Result::OK) outTC = decodeF8_8(outResp.value);
  return res;
}

void OpenThermPlusESP32::setCHEnable(bool en) {
  if (en) _masterStatus |= (1u << 0); else _masterStatus &= ~(1u << 0);
}

void OpenThermPlusESP32::setDHWEnable(bool en) {
  if (en) _masterStatus |= (1u << 1); else _masterStatus &= ~(1u << 1);
}

void OpenThermPlusESP32::setCoolingEnable(bool en) {
  if (en) _masterStatus |= (1u << 2); else _masterStatus &= ~(1u << 2);
}

void OpenThermPlusESP32::setOTCActive(bool en) {
  if (en) _masterStatus |= (1u << 3); else _masterStatus &= ~(1u << 3);
}

void OpenThermPlusESP32::setCH2Enable(bool en) {
  if (en) _masterStatus |= (1u << 4); else _masterStatus &= ~(1u << 4);
}

void OpenThermPlusESP32::setFaultReset(bool en) {
  // OpenTherm master status HB bit7: fault reset
  if (en) _masterStatus |= (1u << 7); else _masterStatus &= ~(1u << 7);
}

bool OpenThermPlusESP32::probeStatusOnce(Frame& outResp) {
  // One attempt without retries
  uint16_t v = ((uint16_t)_masterStatus << 8);
  uint32_t req = buildPayload(MsgType::READ_DATA, 0, v);
  if (!sendPayload(req)) return false;

  uint32_t respPayload = 0;
  if (!receivePayload(respPayload, _cfg.slaveResponseTimeoutMs)) return false;

  Frame r = parsePayload(respPayload);
  if (!r.parityOk) return false;
  if (r.dataId != 0) return false;
  // Typical response to status exchange is READ_ACK, but accept any sane slave response here.
  outResp = r;
  return true;
}

bool OpenThermPlusESP32::autoDetectLogic() {
  // Try combinations of invertTx/invertRx.
  // We consider "success" a valid, parity-correct reply to status probe (ID0).
  bool origInvTx = _cfg.invertTx;
  bool origInvRx = _cfg.invertRx;

  struct Combo { bool itx; bool irx; };
  Combo combos[4] = {
    {origInvTx, origInvRx},
    {origInvTx, !origInvRx},
    {!origInvTx, origInvRx},
    {!origInvTx, !origInvRx},
  };

  for (auto c : combos) {
    _cfg.invertTx = c.itx;
    _cfg.invertRx = c.irx;

    // re-init idle level and clear state
    writeLine(true);
    delay(150);

    Frame r{};
    if (probeStatusOnce(r)) {
      // Looks good: lock in these settings
      return true;
    }
  }

  // Restore original
  _cfg.invertTx = origInvTx;
  _cfg.invertRx = origInvRx;
  return false;
}

void OpenThermPlusESP32::loop() {
  if (!_cfg.enablePolling || !_ready) return;

  uint32_t now = millis();
  if ((uint32_t)(now - _lastPollMs) < _cfg.pollIntervalMs) return;
  _lastPollMs = now;

  // Simple round-robin poll phases
  Frame r{};
  if (_cfg.pollStatusId0 && (_pollPhase % 2 == 0)) {
    if (readStatus(r) == Result::OK) {
      _lastStatus = r;
      _hasLastStatus = true;
    }
  } else if (_cfg.pollBoilerTempId25) {
    if (readData(25, r) == Result::OK) {
      _lastTboiler = r;
      _hasLastTboiler = true;
    }
  }

  _pollPhase++;
}
