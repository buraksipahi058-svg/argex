import pathlib,sys,unittest,struct
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'jetson'))
from protocol import *
class ProtocolTests(unittest.TestCase):
 def test_crc_known_vector(self):self.assertEqual(crc16(b'123456789'),0x29b1)
 def test_fragmentation_and_noise(self):
  f=command(255,-100,100,0,180,True,True);d=Decoder();out=[]
  for b in b'noise\xaa'+f:out+=d.feed(bytes([b]))
  self.assertEqual(out,[(2,255,struct.pack('<bbBBBBB',-100,100,0,180,1,255,1))])
 def test_corrupt_then_valid(self):
  bad=bytearray(command(1));bad[-1]^=1
  self.assertEqual(Decoder().feed(bad+command(2))[0][1],2)
 def test_wrong_version_and_length(self):
  self.assertEqual(Decoder().feed(b'\xaa\x55\x02\x02\x07\x00'+command(3))[0][1],3)
  self.assertEqual(Decoder().feed(b'\xaa\x55\x01\x02\xff\x00'+command(4))[0][1],4)
 def test_bounds(self):
  with self.assertRaises(ValueError):command(0,101)
  with self.assertRaises(ValueError):command(0,pan=181)
 def test_status_signed_and_flags(self):
  s=status(struct.pack('<bbBBBBBB',-30,40,90,90,0,2,1,0x25))
  self.assertEqual(s['left'],-30);self.assertTrue(s['armed']);self.assertFalse(s['hw_error'])
 def test_imu_roundtrip(self):
  payload=struct.pack('<hHB',-1234,27000,(3<<6)|(2<<4)|(1<<2)|0)
  out=Decoder().feed(frame(IMU,7,payload))
  self.assertEqual(out[0][0],IMU);self.assertEqual(out[0][1],7)
  d=imu(out[0][2])
  self.assertEqual(d['pitch'],-12.34);self.assertEqual(d['yaw'],270.0)
  self.assertEqual((d['cal_sys'],d['cal_gyro'],d['cal_accel'],d['cal_mag']),(3,2,1,0))
  self.assertTrue(d['calibrated'])
 def test_imu_bad_length(self):
  with self.assertRaises(ValueError):imu(b'\x00\x00\x00\x00')
if __name__=='__main__':unittest.main()
