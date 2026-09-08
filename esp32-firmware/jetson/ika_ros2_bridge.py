#!/usr/bin/env python3
"""ROS 2 -> IKA serial adapter. Requires rclpy, geometry_msgs, std_msgs, pyserial.
This is a transport/drive adapter, not navigation or perception software.
"""
import json
import math
import struct
import time
import serial
import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist
from std_msgs.msg import Bool, Float32, String
from protocol import Decoder, command, frame, status, imu, STATUS, HEARTBEAT, IMU

class Bridge(Node):
    def __init__(self):
        super().__init__('ika_serial_bridge')
        self.declare_parameter('port','/dev/ttyUSB0')
        self.declare_parameter('max_linear_mps',1.0)
        self.declare_parameter('max_angular_radps',1.0)
        self.declare_parameter('drive_limit_pct',30)
        self.declare_parameter('cmd_vel_topic','/cmd_vel')
        self.max_v=float(self.get_parameter('max_linear_mps').value)
        self.max_w=float(self.get_parameter('max_angular_radps').value)
        self.limit=int(self.get_parameter('drive_limit_pct').value)
        if not all(math.isfinite(x) and x>0 for x in (self.max_v,self.max_w)) or not 1<=self.limit<=100:
            raise ValueError('Invalid speed scale/limit')
        self.ser=None; self.retry=0.; self.seq=0; self.decoder=Decoder()
        self.reset_commands()
        self.pub=self.create_publisher(String,'/ika/status',10)
        self.imu_pub=self.create_publisher(String,'/ika/imu',10)
        self.create_subscription(Twist,self.get_parameter('cmd_vel_topic').value,self.velocity,10)
        self.create_subscription(Float32,'/ika/pan',lambda m:self.axis('pan',m.data),10)
        self.create_subscription(Float32,'/ika/tilt',lambda m:self.axis('tilt',m.data),10)
        self.create_subscription(Bool,'/ika/laser',self.laser,10)
        self.create_timer(0.02,self.tick)

    def reset_commands(self):
        self.left=self.right=0; self.vel_time=0.; self.fire=False; self.fire_time=0.
        self.pan=self.tilt=90; self.state=None; self.state_time=0.; self.hb_time=0.

    def velocity(self,m):
        if not math.isfinite(m.linear.x) or not math.isfinite(m.angular.z):
            self.left=self.right=0; self.vel_time=0.; return
        v=max(-1.,min(1.,m.linear.x/self.max_v)); w=max(-1.,min(1.,m.angular.z/self.max_w))
        # ROS yaw positive = turn left: right wheels faster.
        l,r=v-w,v+w; scale=max(1.,abs(l),abs(r))
        self.left=round(l/scale*self.limit); self.right=round(r/scale*self.limit)
        self.vel_time=time.monotonic()

    def axis(self,name,value):
        if math.isfinite(value) and 0<=value<=180:
            setattr(self,name,round(value))

    def laser(self,m):
        self.fire=bool(m.data); self.fire_time=time.monotonic()

    def send(self,data):
        if self.ser.write(data) != len(data):
            raise serial.SerialTimeoutException('partial write')
        self.seq=(self.seq+1)&255

    def tick(self):
        now=time.monotonic()
        if self.ser is None:
            if now<self.retry:return
            self.retry=now+1.
            try:
                self.ser=serial.Serial(self.get_parameter('port').value,115200,timeout=0,write_timeout=0.05)
                self.ser.reset_input_buffer(); self.decoder=Decoder(); self.reset_commands()
                self.get_logger().info('Serial connected; waiting for fresh AUTO status and zero arming.')
            except (serial.SerialException,OSError) as e:
                self.ser=None; self.get_logger().warning(str(e)); return
        try:
            for typ,seq,payload in self.decoder.feed(self.ser.read(min(1024,self.ser.in_waiting))):
                if typ==IMU and len(payload)==5:
                    self.imu_pub.publish(String(data=json.dumps(imu(payload))));continue
                if typ!=STATUS or len(payload)!=8:continue
                new=status(payload)
                if self.state is None or new['mode']!=self.state['mode'] or new['link']!=self.state['link']:
                    self.left=self.right=0; self.vel_time=0.; self.fire=False; self.fire_time=0.
                    self.pan=new['pan']; self.tilt=new['tilt']
                self.state=new; self.state_time=now
                self.pub.publish(String(data=json.dumps(new)))
            active=bool(self.state and now-self.state_time<0.3 and self.state['mode']==2 and self.state['link'] and not self.state['hw_error'])
            ready=bool(active and self.state['armed'])
            l,r=(self.left,self.right) if ready and now-self.vel_time<0.2 else (0,0)
            laser=bool(ready and self.fire and now-self.fire_time<0.2)
            self.send(command(self.seq,l,r,self.pan,self.tilt,laser,active))
            if now-self.hb_time>=0.1:
                self.send(frame(HEARTBEAT,self.seq,struct.pack('<BI',1,int(now*1000)&0xffffffff)))
                self.hb_time=now
        except (serial.SerialException,OSError) as e:
            self.get_logger().error(str(e)); self.ser.close(); self.ser=None; self.reset_commands()

    def stop(self):
        if self.ser:
            try:self.send(command(self.seq,auto=False))
            except (serial.SerialException,OSError):pass
            self.ser.close()

def main():
    rclpy.init(); node=Bridge()
    try:rclpy.spin(node)
    except KeyboardInterrupt:pass
    finally:
        node.stop(); node.destroy_node()
        if rclpy.ok():rclpy.shutdown()

if __name__=='__main__':main()
