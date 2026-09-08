#!/usr/bin/env python3
"""Read controller status only. Does not send motor/servo/laser commands."""
import argparse,json,time
import serial
from protocol import Decoder,STATUS,IMU,status,imu
p=argparse.ArgumentParser();p.add_argument('port');a=p.parse_args();d=Decoder()
with serial.Serial(a.port,115200,timeout=0.1) as s:
 try:
  while True:
   for kind,seq,data in d.feed(s.read(256)):
    if kind==STATUS and len(data)==8:print(json.dumps(status(data)),flush=True)
    elif kind==IMU and len(data)==5:print(json.dumps(imu(data)),flush=True)
 except KeyboardInterrupt:pass
