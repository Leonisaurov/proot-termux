import shutil, socket, struct, threading, time, unittest
from control_api import *
class TestControlAPI(unittest.TestCase):
 def hello(self): return HEADER.pack(MAGIC,VERSION,Message.HELLO,0,0)
 def test_fragmented_net(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); b.sendall(self.hello())
  p=struct.pack('<IiiHHHH16sBB64s128s',2,7,8,2,6,443,0,b'\x7f\0\0\1'+b'\0'*12,0,0,b'p\0'.ljust(64,b'\0'),b'\0'*128)
  raw=HEADER.pack(MAGIC,1,2,len(p),9)+p
  b.sendall(raw[:3]); b.sendall(raw[3:]); r=c.receive(); self.assertEqual((r.request_id,r.path if hasattr(r,'path') else r.proxy),(9,'p'))
 def test_legacy_first_frame_rejected_and_terminal(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); b.sendall(HEADER.pack(MAGIC,VERSION,Message.COMMAND_RESULT,0,1))
  self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_unknown_type_terminal(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,99,0,1))
  self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_invalid_path_terminal(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); payload=struct.pack('<II1024s1024s',1,7,b'guest\0'.ljust(1024,b'\0'),b'\0'*1024)
  b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,Message.PATH_ACCESS_REQUEST,len(payload),2)+payload)
  self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_reason_and_ids(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); b.sendall(self.hello())
  c.handshake(); c.allow_once(42,'why',9); raw=b.recv(20+98)
  self.assertEqual(struct.unpack('<IHHIQ',raw[:20]),(MAGIC,VERSION,Message.ALLOW_ONCE,98,42)); self.assertEqual(raw[20:22],b'\x01\x09')
 def test_proot_config_constructor(self):
  config=ProotConfig(proot_path='/custom/proot',args=('-r','/rootfs'),guest_command=('/bin/sh','-c','echo ok'),env={'PATH':'/bin'},cwd='/guest-cwd',timeout=2.0,keep_stdin=False,grace_period=0.25)
  self.assertEqual(config.proot_path,'/custom/proot')
  self.assertEqual(config.args,('-r','/rootfs'))
  self.assertEqual(config.guest_command,('/bin/sh','-c','echo ok'))
  self.assertEqual(config.env,{'PATH':'/bin'})
  self.assertEqual(config.timeout,2.0)
  self.assertFalse(config.keep_stdin)
  self.assertEqual(config.grace_period,0.25)
 def test_real_launcher_completes_hello(self):
  proot=shutil.which('proot')
  if proot is None: self.skipTest('proot is not installed')
  config=ProotConfig(proot_path=proot,guest_command=('/bin/sh','-c','printf integration-ok'))
  process=ProotProcess.spawn(config)
  try:
   stdout,stderr=process.process.communicate(timeout=3)
   self.assertEqual(stdout,b'integration-ok',stderr.decode(errors='replace'))
   self.assertEqual(process.returncode,0,stderr.decode(errors='replace'))
   self.assertEqual(process.channel.state,ChannelState.READY)
  finally:
   process.close()
 def test_serve_does_not_answer_events(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a,timeout=.05); payload=struct.pack('<II1024s1024s',1,2,b'/guest\0'.ljust(1024,b'\0'),b'\0'*1024)
  b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,Message.SHADOW_EVENT,len(payload),3)+payload); seen=[]
  def run():
   try: c.serve(lambda r: seen.append(r) or Decision.ALLOW)
   except ControlError: pass
  t=threading.Thread(target=run); t.start(); time.sleep(.02); b.close(); t.join(1)
  self.assertEqual(len(seen),1); self.assertEqual(c.state,ChannelState.FAILED)
 def test_bad_size(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a); b.sendall(HEADER.pack(MAGIC,1,2,MAX_FRAME+1,1)); self.assertRaises(InvalidFrame,c.recv_frame); self.assertEqual(c.state,ChannelState.FAILED)
