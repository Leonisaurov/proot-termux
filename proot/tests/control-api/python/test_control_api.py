import io, shutil, socket, struct, subprocess, threading, time, unittest
from contextlib import contextmanager
from unittest.mock import patch
from control_api import *
class TestControlAPI(unittest.TestCase):
 def hello(self): return HEADER.pack(MAGIC,VERSION,Message.HELLO,0,0)
 @contextmanager
 def channel_pair(self, timeout=1.0):
  a,b=socket.socketpair()
  c=ControlChannel.from_socket(a,timeout)
  try: yield c,b
  finally:
   c.close(); b.close()
 def test_fragmented_net(self):
  with self.channel_pair() as (c,b):
   b.sendall(self.hello())
   p=struct.pack('<IiiHHHH16sBB64s128s',2,7,8,2,6,443,0,b'\x7f\0\0\1'+b'\0'*12,0,0,b'p\0'.ljust(64,b'\0'),b'\0'*128)
   raw=HEADER.pack(MAGIC,1,2,len(p),9)+p
   b.sendall(raw[:3]); b.sendall(raw[3:]); r=c.receive(); self.assertEqual((r.request_id,r.proxy),(9,'p'))
 def test_legacy_first_frame_rejected_and_terminal(self):
  with self.channel_pair() as (c,b):
   b.sendall(HEADER.pack(MAGIC,VERSION,Message.COMMAND_RESULT,0,1))
   self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_unknown_type_terminal(self):
  with self.channel_pair() as (c,b):
   b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,99,0,1))
   self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_invalid_path_terminal(self):
  with self.channel_pair() as (c,b):
   payload=struct.pack('<II1024s1024s',1,7,b'guest\0'.ljust(1024,b'\0'),b'\0'*1024)
   b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,Message.PATH_ACCESS_REQUEST,len(payload),2)+payload)
   self.assertRaises(InvalidFrame,c.receive); self.assertEqual(c.state,ChannelState.FAILED)
 def test_reason_and_ids(self):
  with self.channel_pair() as (c,b):
   b.sendall(self.hello()); c.handshake(); c.allow_once(42,'why',9); raw=b.recv(20+98)
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
  config=ProotConfig(proot_path=proot,guest_command=('/bin/sh','-c','echo -n integration-ok'))
  process=ProotProcess.spawn(config)
  try:
   # PRCT is fail-closed: HELLO is only the start of the channel.  Keep
   # servicing requests while the guest lives instead of leaving it blocked.
   def serve():
    try:
     process.channel.serve(lambda r: Decision.ALLOW
                           if isinstance(r,(NetRequest,PathRequest)) else None)
    except ControlError:
     pass
   server=threading.Thread(target=serve,daemon=True); server.start()
   stdout,stderr=process.process.communicate(timeout=3)
   server.join(1)
   if process.returncode != 0 and b"fatal error:" in stderr:
    self.skipTest('installed proot cannot run a guest in this environment')
   self.assertEqual(stdout,b'integration-ok',stderr.decode(errors='replace'))
   self.assertEqual(process.returncode,0,stderr.decode(errors='replace'))
   # The peer EOF after a clean guest exit is terminal by design.
   self.assertEqual(process.channel.state,ChannelState.FAILED)
  finally:
   process.close()
 def test_serve_does_not_answer_events(self):
  with self.channel_pair(.05) as (c,b):
   payload=struct.pack('<II1024s1024s',1,2,b'/guest\0'.ljust(1024,b'\0'),b'\0'*1024)
   b.sendall(self.hello()+HEADER.pack(MAGIC,VERSION,Message.SHADOW_EVENT,len(payload),3)+payload); seen=[]
   def run():
    try: c.serve(lambda r: seen.append(r) or Decision.ALLOW)
    except ControlError: pass
   t=threading.Thread(target=run); t.start(); time.sleep(.02); b.close(); t.join(1)
   self.assertEqual(len(seen),1); self.assertEqual(c.state,ChannelState.FAILED)
 def test_bad_size(self):
  with self.channel_pair() as (c,b):
   b.sendall(HEADER.pack(MAGIC,1,2,MAX_FRAME+1,1)); self.assertRaises(InvalidFrame,c.recv_frame); self.assertEqual(c.state,ChannelState.FAILED)
 def test_channel_close_is_idempotent_and_owns_socket(self):
  a,b=socket.socketpair(); c=ControlChannel.from_socket(a)
  c.close(); c.close(); self.assertTrue(c.closed); self.assertEqual(a.fileno(),-1); b.close()
 def test_channel_close_after_eof_and_timeout(self):
  with self.channel_pair(.01) as (c,b):
   b.close(); self.assertRaises(ControlEOF,c.recv_frame); c.close(); c.close()
  with self.channel_pair(.01) as (c,b):
   self.assertRaises(ControlTimeout,c.recv_frame); c.close(); c.close()
 def test_from_fd_takes_ownership(self):
  a,b=socket.socketpair(); fd=a.detach(); c=ControlChannel.from_fd(fd)
  c.close(); b.close(); self.assertEqual(c.sock.fileno(),-1)
 def test_spawn_popen_failure_closes_socketpair(self):
  created=[]
  original=socket.socketpair
  def pair(*args,**kwargs):
   result=original(*args,**kwargs); created.extend(result); return result
  with patch('control_api.socket.socketpair',side_effect=pair), patch('control_api.subprocess.Popen',side_effect=OSError('no proot')):
   self.assertRaises(OSError,ProotProcess.spawn,ProotConfig())
  self.assertTrue(all(s.fileno()==-1 for s in created))
 def test_spawn_handshake_failure_closes_process_resources(self):
  process=unittest.mock.Mock(stdout=io.BytesIO(),stderr=io.BytesIO(),pid=123)
  process.wait.return_value=0
  with patch('control_api.subprocess.Popen',return_value=process), patch('control_api.ControlChannel.handshake',side_effect=ControlTimeout('handshake timeout')):
   self.assertRaises(ControlTimeout,ProotProcess.spawn,ProotConfig(timeout=.01))
  process.kill.assert_called_once_with(); process.wait.assert_called_once_with()
  self.assertTrue(process.stdout.closed); self.assertTrue(process.stderr.closed)
 def test_close_terminates_and_closes_streams(self):
  process=unittest.mock.Mock(pid=123,stdout=io.BytesIO(),stderr=io.BytesIO())
  process.wait.side_effect=[subprocess.TimeoutExpired('proot',.01),subprocess.TimeoutExpired('proot',.01),0]
  channel=unittest.mock.Mock(); config=ProotConfig(grace_period=.01)
  launcher=ProotProcess(process,channel,process.stdout,process.stderr,config)
  launcher.close(); launcher.close()
  channel.close.assert_called_once_with(); process.terminate.assert_called_once_with(); process.kill.assert_called_once_with()
  self.assertTrue(process.stdout.closed); self.assertTrue(process.stderr.closed)
