"""Exercise ROS adapter without ROS/hardware; actual callbacks and timer run."""
import sys,types,pathlib,unittest,struct
from unittest.mock import patch
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'jetson'))
class Message:
 def __init__(self,**kwargs):self.__dict__.update(kwargs)
class Node:pass
r=types.ModuleType('rclpy');rn=types.ModuleType('rclpy.node');rn.Node=Node
sm=types.ModuleType('std_msgs.msg');sm.Bool=sm.Float32=sm.String=Message
gm=types.ModuleType('geometry_msgs.msg');gm.Twist=Message
se=types.ModuleType('serial');se.SerialException=type('SerialException',(Exception,),{});se.SerialTimeoutException=se.SerialException
for name,module in [('rclpy',r),('rclpy.node',rn),('std_msgs.msg',sm),('geometry_msgs.msg',gm),('serial',se)]:sys.modules[name]=module
from ika_ros2_bridge import Bridge
from protocol import *
class Port:
 def __init__(self):self.incoming=bytearray();self.written=[]
 @property
 def in_waiting(self):return len(self.incoming)
 def read(self,n):b=bytes(self.incoming[:n]);del self.incoming[:n];return b
 def write(self,b):self.written.append(b);return len(b)
class BridgeTests(unittest.TestCase):
 def setUp(self):
  self.b=Bridge.__new__(Bridge);b=self.b;b.max_v=b.max_w=1.;b.limit=30;b.seq=0;b.decoder=Decoder();b.ser=Port();b.reset_commands()
  b.pub=Message(publish=lambda m:None)
 def status(self,armed=True,mode=2):
  self.b.ser.incoming.extend(frame(1,0,struct.pack('<bbBBBBBB',0,0,90,90,0,mode,1,32 if armed else 0)))
 def last(self):
  commands=[p for p in self.b.ser.written if p[3]==2]
  return struct.unpack('<bbBBBBB',commands[-1][6:-2])
 def vel(self,v=1.,w=0.):self.b.velocity(Message(linear=Message(x=v),angular=Message(z=w)))
 def test_zero_handshake_and_mode_entry_clears_old_demand(self):
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.):
   self.vel();self.status(False);self.b.tick();self.assertEqual(self.last()[:2],(0,0));self.assertEqual(self.last()[-1],1)
   self.vel();self.status(True);self.b.tick();self.assertEqual(self.last()[:2],(30,30))
 def test_stale_velocity_and_laser(self):
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.):
   self.status();self.b.tick();self.vel();self.b.laser(Message(data=True))
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.21):
   self.status();self.b.tick();self.assertEqual(self.last()[:2],(0,0));self.assertEqual(self.last()[4],0)
 def test_stale_controller_status_disables_auto(self):
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.):self.status();self.b.tick()
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.31):
   self.vel();self.b.tick();self.assertEqual(self.last()[:2],(0,0));self.assertEqual(self.last()[-1],0)
 def test_ros_positive_yaw_and_invalid_values(self):
  with patch('ika_ros2_bridge.time.monotonic',return_value=10.):
   self.vel(0,1);self.assertEqual((self.b.left,self.b.right),(-30,30))
   self.vel(float('nan'),0);self.assertEqual((self.b.left,self.b.right),(0,0))
if __name__=='__main__':unittest.main()
