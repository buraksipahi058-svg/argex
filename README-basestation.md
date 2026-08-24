# UGV / İKA Base Station

Read-only monitoring system for the TEKNOFEST unmanned ground vehicle. It
**observes** the vehicle over a one-way telemetry path and streams the three
cameras — it can **never** actuate the vehicle.

```
STM32 ──UART v1 @115200 (FROZEN)──► Jetson gateway (observer) ─┬─ TELEMETRY  Protobuf/QUIC ─► Backend ─► SQLite + WebSocket(JSON) ─► React UI
                                                               └─ VIDEO      3× H.264/RTP ─► MediaMTX (WebRTC/WHEP) ─► React UI
```

Two **independent data planes**: telemetry (QUIC + Protobuf) and video
(RTP/UDP → WebRTC). They never share a channel.

---

## Source of truth (FROZEN — never modified)

The existing firmware/protocol in [`needtocheck/`](needtocheck/) is treated as an
external hardware API. Nothing there is edited, refactored, or renamed. The
Python parser is **imported unmodified** by the gateway and the simulator.

| STM32↔Jetson protocol | value |
|---|---|
| Link | STM USART3 (PB10/PB11) ↔ Jetson `/dev/ttyTHS1`, **115200 8N1** |
| Frame | `AA 55 | VER | TYPE | LEN | SEQ | PAYLOAD | CRC16-CCITT` (little-endian) |
| STATUS @20 Hz | `solMotor, sagMotor, pan, tilt, lazer, aktifMod(0=drive/1=laser), elrsLink, durum` |
| `durum` bits | JETSON_LINK, CMD_TIMEOUT, AUTO_EN, FAILSAFE, CRC_ERR |
| HEARTBEAT @10 Hz | `kaynak(0=STM/1=Jetson), uptime_ms` |

Every telemetry field maps 1:1 from a real byte — see the traceability table in
[`proto/telemetry.proto`](proto/telemetry.proto). **No invented fields**
(no battery/speed/RPM/temp/IMU/GPS/perception/RSSI/ammo/recording — none exist).

### Read-only guarantees
- `telemetry.proto` has **no command message**; the backend QUIC endpoint only receives; the UI has no control affordance.
- The Jetson gateway only **decodes** (`Protocol.feed`); it never builds COMMAND and never writes to the STM. It also does not disable the existing Jetson→STM heartbeat/autonomy traffic the firmware relies on.
- The Base Station is **not in the vehicle safety loop**: if any part of it dies, the vehicle continues under the existing STM32/Jetson logic.

### Distinct link/failure states (never conflated)
| concept | source |
|---|---|
| ELRS (RC) link | `STATUS.elrsLink` field |
| STM↔Jetson (STM's view) | `STATUS.durum & JETSON_LINK` |
| STM telemetry stale | gateway: STATUS stopped arriving |
| Base link | backend: gateway QUIC/heartbeat gap |
| Server link | browser↔backend WebSocket |

"STATUS stopped" is reported as *telemetry stale*, **not** "ELRS lost". When
telemetry goes stale the last-known values are frozen and flagged, never
rewritten or fabricated.

---

## Components

| path | role |
|---|---|
| `proto/telemetry.proto` | schema (real fields only, `oneof` envelope) |
| `gen/telemetry_pb2.py` | generated stubs (`python scripts/gen_proto.py`) |
| `jetson/` | vehicle-side observer: reader → mapper → QUIC client + video pipelines |
| `backend/` | receive-only QUIC server → SQLite → WebSocket + read-only REST |
| `mediamtx/mediamtx.yml` | WebRTC bridge for the 3 cameras |
| `src/` | React dashboard (WebSocket telemetry + WHEP video) |
| `sim/fake_stm.py` | emits valid STATUS/HEARTBEAT for hardware-free testing |
| `common/` | shared QUIC/framing helpers |

Telemetry rates are configurable in `jetson/config.yaml`:
high-frequency `TelemetryFrame` (QUIC datagrams, ≤20 Hz), event-driven `Event`
(reliable stream, on transition), `GatewayHeartbeat` (1 Hz).

---

## Run it (no hardware needed)

Prereqs: Python 3.10+ and Node 18+. Create a venv and install:

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r backend/requirements.txt -r jetson/requirements.txt
.venv/Scripts/python scripts/gen_proto.py          # generate gen/telemetry_pb2.py
npm install
```

Start the pieces (each in its own terminal), from the repo root:

```bash
# 1) Base Station backend (QUIC :4433, WebSocket/REST :8080)
.venv/Scripts/python -m backend.main

# 2) Jetson gateway (observer). Default source is UDP for the simulator.
.venv/Scripts/python -m jetson.gateway

# 3) Fake STM (only for testing without the vehicle)
.venv/Scripts/python sim/fake_stm.py

# 4) Frontend
npm run dev            # http://localhost:5173

# 5) (optional) Video bridge + a test pattern
mediamtx mediamtx/mediamtx.yml
gst-launch-1.0 videotestsrc ! x264enc tune=zerolatency ! rtspclientsink location=rtsp://127.0.0.1:8554/cam_front
```

On the real vehicle: set `source.type: serial` in `jetson/config.yaml`, run the
gateway on the Jetson (`quic.host` = base station), and launch
`python -m jetson.video_pipelines` (needs GStreamer + NVENC).

---

## Verify

```bash
python needtocheck/jetson_parser.py            # frozen CRC self-test -> 0x29B1
python scripts/ws_probe.py                     # print live WS telemetry
curl http://127.0.0.1:8080/api/history/frames?limit=5
curl http://127.0.0.1:8080/api/history/events?limit=20
```

Semantic checks (kill processes to observe distinct states):
- stop `fake_stm` → `STM_TELEMETRY_STALE` event, base link stays **up**;
- stop `jetson.gateway` → `BASE_LINK_LOST` event (backend-derived);
- during the sim's ELRS window → `ELRS_LINK_LOST` (a *field* event, distinct from the above).

REST is read-only; there is no control endpoint anywhere.
