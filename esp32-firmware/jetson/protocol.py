"""IKA v1 framing; compatible with the accompanying ESP32 firmware."""
import struct
HEADER = b'\xaa\x55'
STATUS, COMMAND, HEARTBEAT, IMU = 1, 2, 3, 4

def crc16(data):
    crc = 0xffff
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xffff
    return crc

def frame(kind, seq, payload):
    if len(payload) > 32:
        raise ValueError('payload too long')
    body = bytes((1, kind, len(payload), seq & 255)) + payload
    return HEADER + body + struct.pack('<H', crc16(body))

def command(seq, left=0, right=0, pan=90, tilt=90, laser=False, auto=False):
    if not (-100 <= left <= 100 and -100 <= right <= 100 and 0 <= pan <= 180 and 0 <= tilt <= 180):
        raise ValueError('command out of range')
    return frame(COMMAND, seq, struct.pack('<bbBBBBB', left, right, pan, tilt, int(laser), 255, int(auto)))

class Decoder:
    def __init__(self):
        self.buffer = bytearray()
    def feed(self, data):
        self.buffer.extend(data)
        if len(self.buffer) > 4096:
            del self.buffer[:-4096]
        out = []
        while len(self.buffer) >= 2:
            pos = self.buffer.find(HEADER)
            if pos < 0:
                self.buffer[:] = b'\xaa' if self.buffer[-1] == 0xaa else b''
                break
            del self.buffer[:pos]
            if len(self.buffer) < 6:
                break
            n = self.buffer[4]
            if self.buffer[2] != 1 or n > 32:
                del self.buffer[0]
                continue
            if len(self.buffer) < n + 8:
                break
            packet = bytes(self.buffer[:n+8])
            if crc16(packet[2:-2]) != int.from_bytes(packet[-2:], 'little'):
                del self.buffer[0]
                continue
            out.append((packet[3], packet[5], packet[6:-2]))
            del self.buffer[:n+8]
        return out

def status(payload):
    if len(payload) != 8:
        raise ValueError('status length')
    l,r,p,t,laser,mode,link,flags=struct.unpack('<bbBBBBBB',payload)
    return dict(left=l,right=r,pan=p,tilt=t,laser=laser,mode=mode,link=link,flags=flags,armed=bool(flags & 32),hw_error=bool(flags & 64))

def imu(payload):
    """TYPE_IMU (0x04) payload: pitch(int16, deg*100), yaw(uint16, deg*100), cal(uint8)."""
    if len(payload) != 5:
        raise ValueError('imu length')
    pitch_c, yaw_c, cal = struct.unpack('<hHB', payload)
    return dict(pitch=pitch_c / 100.0, yaw=yaw_c / 100.0,
                cal_sys=(cal >> 6) & 3, cal_gyro=(cal >> 4) & 3,
                cal_accel=(cal >> 2) & 3, cal_mag=cal & 3,
                calibrated=(cal >> 6) & 3 == 3)
