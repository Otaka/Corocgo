#include "corocrpc.h"
#include <cstring>
#include <chrono>

namespace corocrpc {

// ── CRC16-IBM ─────────────────────────────────────────────────────────────

static const uint16_t CRC16_TABLE[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(crc ^ data[i]);
        crc = (crc >> 8) ^ CRC16_TABLE[idx];
    }
    return crc;
}

// ── StreamFramer ──────────────────────────────────────────────────────────

StreamFramer::StreamFramer()
    : state_(State::SEARCH_MAGIC_1), bytes_received_(0), content_size_(0) {
    writeCh = corocgo::makeChannel<RawChunk>(8);
    readCh  = corocgo::makeChannel<FramedPacket>(4);
    corocgo::coro([this]() { _parseLoop(); });
}

StreamFramer::~StreamFramer() {
    writeCh->close();
    readCh->close();
    delete writeCh;
    delete readCh;
}

FramedPacket StreamFramer::createPacket(uint16_t channel, const char* buffer, unsigned int length) {
    FramedPacket fp{};
    if (length > MAX_CONTENT_SIZE) return fp;  // size==0 signals error

    fp.data[0] = MAGIC_BYTE_1;
    fp.data[1] = MAGIC_BYTE_2;
    write_u16_le(&fp.data[2], static_cast<uint16_t>(length));

    uint16_t content_crc = (length > 0)
        ? crc16(reinterpret_cast<const uint8_t*>(buffer), length)
        : 0xFFFF;
    write_u16_le(&fp.data[4], content_crc);

    fp.data[6] = 0x00;
    fp.data[7] = 0x00;
    write_u16_le(&fp.data[8], channel);

    uint16_t header_crc = crc16(fp.data, 10);
    write_u16_le(&fp.data[10], header_crc);

    if (length > 0) std::memcpy(&fp.data[HEADER_SIZE], buffer, length);

    fp.size    = static_cast<uint16_t>(HEADER_SIZE + length);
    fp.channel = channel;
    return fp;
}

void StreamFramer::reset() {
    state_ = State::SEARCH_MAGIC_1;
    bytes_received_ = 0;
    content_size_ = 0;
}

uint16_t StreamFramer::read_u16_le(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

void StreamFramer::write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
}

void StreamFramer::_emit(uint16_t channel, uint16_t totalSize) {
    FramedPacket fp{};
    fp.channel = channel;
    fp.size    = totalSize;
    std::memcpy(fp.data, input_buffer_, totalSize);
    readCh->send(fp);  // yields until consumer reads if channel is full
}

void StreamFramer::_writeBytesInternal(const uint8_t* data, unsigned int length, int depth) {
    if (depth > 5) { reset(); return; }

    unsigned int offset = 0;
    while (offset < length) {
        if (state_ == State::SEARCH_MAGIC_1) {
            if (data[offset] == MAGIC_BYTE_1) {
                input_buffer_[0] = data[offset];
                bytes_received_ = 1;
                state_ = State::SEARCH_MAGIC_2;
            }
            offset++;
        } else if (state_ == State::SEARCH_MAGIC_2) {
            if (data[offset] == MAGIC_BYTE_2) {
                input_buffer_[1] = data[offset];
                bytes_received_ = 2;
                state_ = State::READING_HEADER;
                offset++;
            } else {
                reset();
                // Don't advance — retry this byte as SEARCH_MAGIC_1
            }
        } else if (state_ == State::READING_HEADER) {
            size_t bytes_needed    = HEADER_SIZE - bytes_received_;
            size_t bytes_available = length - offset;
            size_t bytes_to_copy   = (bytes_needed < bytes_available) ? bytes_needed : bytes_available;
            std::memcpy(&input_buffer_[bytes_received_], &data[offset], bytes_to_copy);
            bytes_received_ += bytes_to_copy;
            offset += bytes_to_copy;

            if (bytes_received_ >= HEADER_SIZE) {
                content_size_ = read_u16_le(&input_buffer_[2]);
                if (content_size_ > MAX_CONTENT_SIZE) {
                    uint8_t tmp[10]; std::memcpy(tmp, &input_buffer_[2], 10);
                    reset(); _writeBytesInternal(tmp, 10, depth + 1); continue;
                }
                uint16_t expected_hcrc = read_u16_le(&input_buffer_[10]);
                uint16_t actual_hcrc   = crc16(input_buffer_, 10);
                if (expected_hcrc == actual_hcrc) {
                    if (content_size_ == 0) {
                        uint16_t ch = read_u16_le(&input_buffer_[8]);
                        _emit(ch, static_cast<uint16_t>(HEADER_SIZE));
                        reset();
                    } else {
                        state_ = State::READING_CONTENT;
                    }
                } else {
                    uint8_t tmp[10]; std::memcpy(tmp, &input_buffer_[2], 10);
                    reset(); _writeBytesInternal(tmp, 10, depth + 1);
                }
            }
        } else { // READING_CONTENT
            size_t bytes_needed    = (HEADER_SIZE + content_size_) - bytes_received_;
            size_t bytes_available = length - offset;
            size_t bytes_to_copy   = (bytes_needed < bytes_available) ? bytes_needed : bytes_available;
            std::memcpy(&input_buffer_[bytes_received_], &data[offset], bytes_to_copy);
            bytes_received_ += bytes_to_copy;
            offset += bytes_to_copy;

            if (bytes_received_ >= HEADER_SIZE + content_size_) {
                uint16_t expected_ccrc = read_u16_le(&input_buffer_[4]);
                uint16_t actual_ccrc   = crc16(&input_buffer_[HEADER_SIZE], content_size_);
                if (expected_ccrc == actual_ccrc) {
                    uint16_t ch = read_u16_le(&input_buffer_[8]);
                    _emit(ch, static_cast<uint16_t>(HEADER_SIZE + content_size_));
                }
                reset();
            }
        }
    }
}

void StreamFramer::_parseLoop() {
    while (true) {
        auto res = writeCh->receive();
        if (res.error) break;  // writeCh closed → shut down
        _writeBytesInternal(res.value.data, res.value.len, 0);
    }
}

// ── RpcArg ────────────────────────────────────────────────────────────────

void RpcArg::reset() {
    writeIdx = 0;
    readIdx  = 0;
}

void RpcArg::putInt32(int32_t v) {
    if (writeIdx + 4 > RPC_ARG_BUF_SIZE) return;
    buf[writeIdx++] = (char)( v        & 0xFF);
    buf[writeIdx++] = (char)((v >>  8) & 0xFF);
    buf[writeIdx++] = (char)((v >> 16) & 0xFF);
    buf[writeIdx++] = (char)((v >> 24) & 0xFF);
}

void RpcArg::putBool(bool v) {
    if (writeIdx + 1 > RPC_ARG_BUF_SIZE) return;
    buf[writeIdx++] = v ? 1 : 0;
}

void RpcArg::putString(const char* s) {
    uint16_t len = (uint16_t)strlen(s);
    if (writeIdx + 2 + len > RPC_ARG_BUF_SIZE) return;
    buf[writeIdx++] = (char)( len       & 0xFF);
    buf[writeIdx++] = (char)((len >> 8) & 0xFF);
    memcpy(&buf[writeIdx], s, len);
    writeIdx += len;
}

void RpcArg::putBuffer(const void* data, uint16_t len) {
    if (writeIdx + 2 + len > RPC_ARG_BUF_SIZE) return;
    buf[writeIdx++] = (char)( len       & 0xFF);
    buf[writeIdx++] = (char)((len >> 8) & 0xFF);
    memcpy(&buf[writeIdx], data, len);
    writeIdx += len;
}

int32_t RpcArg::getInt32() {
    if (readIdx + 4 > writeIdx) return 0;
    int32_t v = ((uint8_t)buf[readIdx  ]      ) |
                ((uint8_t)buf[readIdx+1] <<  8) |
                ((uint8_t)buf[readIdx+2] << 16) |
                ((uint8_t)buf[readIdx+3] << 24);
    readIdx += 4;
    return v;
}

bool RpcArg::getBool() {
    if (readIdx + 1 > writeIdx) return false;
    return buf[readIdx++] != 0;
}

int RpcArg::getString(char* out, int outSize) {
    if (readIdx + 2 > writeIdx || outSize <= 0) return -1;
    uint16_t len = (uint8_t)buf[readIdx] | ((uint8_t)buf[readIdx+1] << 8);
    readIdx += 2;
    if (readIdx + len > writeIdx) return -1;
    int copy = (len < (uint16_t)(outSize - 1)) ? len : (uint16_t)(outSize - 1);
    memcpy(out, &buf[readIdx], copy);
    out[copy] = '\0';
    readIdx += len;
    return len;
}

uint16_t RpcArg::getBuffer(void* out, uint16_t maxLen) {
    if (readIdx + 2 > writeIdx) return 0;
    uint16_t len = (uint8_t)buf[readIdx] | ((uint8_t)buf[readIdx+1] << 8);
    readIdx += 2;
    if (readIdx + len > writeIdx) return 0;
    uint16_t copy = (len < maxLen) ? len : maxLen;
    memcpy(out, &buf[readIdx], copy);
    readIdx += len;
    return copy;
}

// ── RpcManager ────────────────────────────────────────────────────────────

int64_t RpcManager::_nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

RpcPacket RpcManager::_makePacket(uint16_t methodId, uint32_t callId,
                                   bool isResponse, RpcArg* arg) {
    RpcPacket pkt;
    pkt.data[0] = (uint8_t)( methodId       & 0xFF);
    pkt.data[1] = (uint8_t)((methodId >>  8) & 0xFF);
    pkt.data[2] = (uint8_t)( callId          & 0xFF);
    pkt.data[3] = (uint8_t)((callId  >>  8)  & 0xFF);
    pkt.data[4] = (uint8_t)((callId  >> 16)  & 0xFF);
    pkt.data[5] = (uint8_t)((callId  >> 24)  & 0xFF);
    pkt.data[6] = isResponse ? 0x01 : 0x00;
    int payloadLen = (arg && arg->writeIdx > 0) ? arg->writeIdx : 0;
    if (payloadLen > 0) {
        memcpy(&pkt.data[RPC_HEADER_SIZE], arg->buf, payloadLen);
    }
    pkt.size = (uint16_t)(RPC_HEADER_SIZE + payloadLen);
    return pkt;
}

RpcManager::RpcManager(corocgo::Channel<RpcPacket>* outCh,
                        corocgo::Channel<RpcPacket>* inCh,
                        int timeoutMs)
    : _outCh(outCh), _inCh(inCh), _timeoutMs(timeoutMs),
      _running(true), _nextCallId(1) {
    memset(_poolUsed, 0, sizeof(_poolUsed));
    corocgo::coro([this]() { _dispatchLoop(); });
    corocgo::coro([this]() { _timeoutLoop(); });
}

RpcManager::~RpcManager() {
    _running = false;
}

void RpcManager::registerMethod(uint16_t methodId,
                                 std::function<RpcArg*(RpcArg*)> handler) {
    _methods[methodId] = std::move(handler);
}

RpcArg* RpcManager::getRpcArg() {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!_poolUsed[i]) {
            _poolUsed[i] = true;
            _pool[i].reset();
            return &_pool[i];
        }
    }
    return nullptr; // pool exhausted
}

void RpcManager::disposeRpcArg(RpcArg* arg) {
    if (!arg) return;
    int idx = (int)(arg - _pool);
    if (idx >= 0 && idx < POOL_SIZE) {
        _poolUsed[idx] = false;
    }
}

RpcResult RpcManager::call(uint16_t methodId, RpcArg* arg) {
    uint32_t callId = _nextCallId++;

    PendingCall pending;
    pending.monitor    = corocgo::_monitor_create();
    pending.result     = nullptr;
    pending.done       = false;
    pending.timedOut   = false;
    pending.deadlineMs = _nowMs() + _timeoutMs;

    _pending[callId] = &pending;

    RpcPacket pkt = _makePacket(methodId, callId, false, arg);
    _outCh->send(pkt);

    while (!pending.done && !pending.timedOut) {
        corocgo::_monitor_wait(pending.monitor);
    }

    _pending.erase(callId);
    corocgo::_monitor_destroy(pending.monitor);

    if (pending.timedOut) {
        return {RPC_TIMEOUT, nullptr};
    }
    return {RPC_OK, pending.result};
}

void RpcManager::_dispatchLoop() {
    while (_running) {
        auto res = _inCh->receive();
        if (res.error) break; // channel closed

        RpcPacket& pkt = res.value;
        if (pkt.size < RPC_HEADER_SIZE) continue;

        uint16_t methodId  = (uint16_t)(pkt.data[0] | (pkt.data[1] << 8));
        uint32_t callId    = (uint32_t)(pkt.data[2] | (pkt.data[3] << 8) |
                                        (pkt.data[4] << 16) | (pkt.data[5] << 24));
        bool     isResponse = (pkt.data[6] & 0x01) != 0;
        int      payloadLen = pkt.size - RPC_HEADER_SIZE;

        if (isResponse) {
            // Client side: wake the waiting call()
            auto it = _pending.find(callId);
            if (it != _pending.end()) {
                PendingCall* pc = it->second;
                if (payloadLen > 0) {
                    RpcArg* result = getRpcArg();
                    if (result) {
                        memcpy(result->buf, &pkt.data[RPC_HEADER_SIZE], payloadLen);
                        result->writeIdx = payloadLen;
                        pc->result = result;
                    }
                }
                pc->done = true;
                corocgo::_monitor_wake(pc->monitor);
            }
        } else {
            // Server side: dispatch to registered handler
            auto it = _methods.find(methodId);
            if (it != _methods.end()) {
                RpcArg* inArg = getRpcArg();
                if (inArg) {
                    if (payloadLen > 0) {
                        memcpy(inArg->buf, &pkt.data[RPC_HEADER_SIZE], payloadLen);
                        inArg->writeIdx = payloadLen;
                    }
                    RpcArg* outArg = it->second(inArg);
                    disposeRpcArg(inArg);
                    RpcPacket resp = _makePacket(methodId, callId, true, outArg);
                    disposeRpcArg(outArg);
                    _outCh->send(resp);
                }
            }
        }
    }
}

void RpcManager::_timeoutLoop() {
    while (_running && !_inCh->isClosed()) {
        corocgo::sleep(1000);
        if (_inCh->isClosed()) break;
        int64_t now = _nowMs();
        for (auto& [callId, pc] : _pending) {
            if (!pc->done && !pc->timedOut && now >= pc->deadlineMs) {
                pc->timedOut = true;
                corocgo::_monitor_wake(pc->monitor);
            }
        }
    }
}

} // namespace corocrpc
