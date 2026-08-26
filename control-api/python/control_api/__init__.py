"""Synchronous PRCT client and proot launcher (standard library only)."""
from dataclasses import dataclass
from enum import Enum, IntEnum
import os, select, socket, struct, subprocess, time

MAGIC, VERSION, MAX_FRAME = 0x50524354, 1, 4096
HEADER = struct.Struct('<IHHIQ')
NET_SIZE, PATH_SIZE, DECISION_SIZE, RESULT_SIZE = 230, 2056, 98, 20

class Message(IntEnum):
    HELLO=1; NET_ACCESS_REQUEST=2; PATH_ACCESS_REQUEST=3; SHADOW_EVENT=4; COMMAND_RESULT=5
    ALLOW_ONCE=16; ALLOW_ALWAYS=17; DENY_ONCE=18; DENY_ALWAYS=19; FORGET=20
    SET_RULE=21; REVEAL_SHADOW=22; RESTORE_SHADOW=23; GET_STATE=24
class NetOperation(IntEnum): BIND=1; CONNECT=2; PUBLICATION=3; DNS=4; SOCKET=5
class PathOperation(IntEnum): READ=1; WRITE=2; CREATE=3; DELETE=4; RENAME=5; METADATA=6
class ShadowScope(IntEnum): NODE=1; RECURSIVE=2
class Decision(IntEnum): DENY=0; ALLOW=1
class ChannelState(Enum): CREATED='created'; READY='ready'; FAILED='failed'; CLOSED='closed'
class ControlError(Exception): pass
class ControlEOF(ControlError): pass
class ControlTimeout(ControlError): pass
class InvalidFrame(ControlError): pass
class Desynchronized(ControlError): pass

def _path(value):
    if not isinstance(value, str) or not value.startswith('/') or '\x00' in value: raise ValueError('path must be an absolute guest path')
    raw=value.encode('utf-8')
    if len(raw)>=1024: raise ValueError('guest path is too long')
    return raw+b'\0'*(1024-len(raw))
def _cstring(raw): return raw.split(b'\0',1)[0].decode('utf-8','strict')
def _guest(raw):
    value=_cstring(raw)
    if value and (not value.startswith('/') or '\x00' in value): raise InvalidFrame('non-guest path')
    return value

@dataclass(frozen=True)
class Request: type: Message; request_id: int
@dataclass(frozen=True)
class NetRequest(Request):
    operation:int; guest_pid:int; family:int; protocol:int; guest_port:int; host_port:int; address:bytes; virtual_class:int; real_exposure:int; proxy:str; domain:str
@dataclass(frozen=True)
class PathRequest(Request): operation:int; reason:int; path:str; other_path:str
@dataclass(frozen=True)
class ShadowEvent(Request): operation:int; reason:int; path:str; other_path:str
@dataclass(frozen=True)
class CommandResult(Request): status:int; flags:int; dynamic_path_rules:int; dynamic_net_rules:int; shadows:int

class ControlChannel:
    def __init__(self, sock, timeout=1.0):
        if struct.pack('=I',1)!=struct.pack('<I',1): raise RuntimeError('PRCT requires little-endian host')
        self.sock,self.timeout=sock,float(timeout); self.state=ChannelState.CREATED; self.closed=False; self._next_id=1; self._queued=[]
        self.sock.setblocking(False)
    @classmethod
    def from_socket(cls,sock,timeout=1.0):
        """Create a channel that owns and closes ``sock``."""
        return cls(sock,timeout)
    @classmethod
    def from_fd(cls,fd,timeout=1.0):
        """Create a channel that owns and closes the supplied descriptor."""
        return cls(socket.socket(fileno=fd),timeout)
    def _fail(self, exc):
        if self.state not in (ChannelState.CLOSED,ChannelState.FAILED): self.state=ChannelState.FAILED
        raise exc
    def _exact(self,n,deadline=None):
        deadline=time.monotonic()+self.timeout if deadline is None else deadline; out=bytearray()
        while len(out)<n:
            left=deadline-time.monotonic()
            if left<=0: self._fail(ControlTimeout('frame deadline expired'))
            try: ready,_,_=select.select([self.sock],[],[],left)
            except OSError as e: self._fail(Desynchronized(str(e)))
            if not ready: self._fail(ControlTimeout('frame deadline expired'))
            try: part=self.sock.recv(n-len(out))
            except BlockingIOError: continue
            except OSError as e: self._fail(Desynchronized(str(e)))
            if not part: self._fail(ControlEOF('peer closed'))
            out.extend(part)
        return bytes(out)
    def handshake(self):
        if self.state is ChannelState.READY: return self
        if self.state is not ChannelState.CREATED: self._fail(Desynchronized('handshake unavailable'))
        typ,rid,p=self.recv_frame()
        if typ is not Message.HELLO or rid!=0 or p: self._fail(InvalidFrame('invalid HELLO'))
        self.state=ChannelState.READY; return self
    def recv_frame(self):
        if self.state in (ChannelState.FAILED,ChannelState.CLOSED): raise Desynchronized('channel is terminal')
        deadline=time.monotonic()+self.timeout; h=self._exact(HEADER.size,deadline)
        magic,version,typ,size,rid=HEADER.unpack(h)
        if magic!=MAGIC or version!=VERSION or size>MAX_FRAME: self._fail(InvalidFrame('header'))
        try: msg=Message(typ)
        except ValueError: self._fail(InvalidFrame('message type'))
        return msg,rid,self._exact(size,deadline)
    def send_frame(self,typ,request_id=0,payload=b''):
        if self.state in (ChannelState.FAILED,ChannelState.CLOSED): raise Desynchronized('channel is terminal')
        payload=bytes(payload)
        if len(payload)>MAX_FRAME or not 0<=request_id<=0xffffffffffffffff: raise ValueError('invalid frame')
        try: self.sock.sendall(HEADER.pack(MAGIC,VERSION,int(Message(typ)),len(payload),request_id)+payload)
        except OSError as e: self._fail(Desynchronized(str(e)))
    def receive(self):
        try: return self._receive()
        except ControlError:
            if self.state not in (ChannelState.CLOSED,ChannelState.FAILED): self.state=ChannelState.FAILED
            raise
    def _receive(self):
        if self.state is ChannelState.CREATED:
            typ,rid,p=self.recv_frame()
            if typ is not Message.HELLO or rid!=0 or p: self._fail(InvalidFrame('invalid HELLO'))
            self.state=ChannelState.READY
            typ,rid,p=self.recv_frame()
        else:
            typ,rid,p=self.recv_frame()
        if typ is Message.HELLO: self._fail(InvalidFrame('HELLO outside handshake'))
        if typ is Message.NET_ACCESS_REQUEST:
            if len(p)!=NET_SIZE: self._fail(InvalidFrame('net payload'))
            x=struct.unpack('<IiiHHHH16sBB64s128s',p)
            if ((x[0] in (1, 2) and x[3] not in (2,10)) or (x[0] == 3 and x[3] != 0) or (x[0] == 5 and x[3] == 0)): self._fail(InvalidFrame('net family'))
            return NetRequest(typ,rid,x[0],x[1],x[3],x[4],x[5],x[6],x[7],x[8],x[9],_cstring(x[10]),_cstring(x[11]))
        if typ in (Message.PATH_ACCESS_REQUEST,Message.SHADOW_EVENT):
            if len(p)!=PATH_SIZE: self._fail(InvalidFrame('path payload'))
            op,reason,path,other=struct.unpack('<II1024s1024s',p); cls=PathRequest if typ is Message.PATH_ACCESS_REQUEST else ShadowEvent
            return cls(typ,rid,op,reason,_guest(path),_guest(other))
        if typ is Message.COMMAND_RESULT:
            if len(p)!=RESULT_SIZE: self._fail(InvalidFrame('result payload'))
            return CommandResult(typ,rid,*struct.unpack('<iIIII',p))
        self._fail(InvalidFrame('not an event'))
    def _event(self):
        return self._queued.pop(0) if self._queued else self.receive()
    def respond(self,request_id,decision,reason='',reason_code=0,persistent=False):
        if not isinstance(reason,str) or not 0<=reason_code<=255: raise ValueError('invalid decision reason')
        raw=reason.encode('utf-8')
        if len(raw)>=96: raise ValueError('reason too long')
        d=Decision(decision); typ={ (Decision.ALLOW,False):Message.ALLOW_ONCE,(Decision.ALLOW,True):Message.ALLOW_ALWAYS,(Decision.DENY,False):Message.DENY_ONCE,(Decision.DENY,True):Message.DENY_ALWAYS}[d,persistent]
        self.send_frame(typ,request_id,struct.pack('<BB96s',int(d),reason_code,raw+b'\0'*(96-len(raw))))
    def allow_once(self,request_id,reason='',reason_code=0): return self.respond(request_id,Decision.ALLOW,reason,reason_code)
    def allow_always(self,request_id,reason='',reason_code=0): return self.respond(request_id,Decision.ALLOW,reason,reason_code,True)
    def deny_once(self,request_id,reason='',reason_code=0): return self.respond(request_id,Decision.DENY,reason,reason_code)
    def deny_always(self,request_id,reason='',reason_code=0): return self.respond(request_id,Decision.DENY,reason,reason_code,True)
    def _command(self,typ,payload,handler=None):
        rid=self._next_id; self._next_id+=1; self.send_frame(typ,rid,payload)
        while True:
            event=self.receive()
            if isinstance(event,CommandResult):
                if event.request_id != rid:
                    self._fail(InvalidFrame('command result request-id mismatch'))
                return event
            if handler is not None: handler(event)
            else: self._queued.append(event)
    def set_rule(self,operation,*,path=None,other_path='',family=2,port=0,address=b'\0'*16,decision=Decision.ALLOW,handler=None):
        if path is not None: p=struct.pack('<III1024s1024s',operation,0,int(decision),_path(path),_path(other_path) if other_path else b'\0'*1024)
        else:
            if len(address)!=16 or not 0<=port<=65535: raise ValueError('invalid network rule')
            p=struct.pack('<IHH16sB3x',operation,family,port,address,int(decision))
        return self._command(Message.SET_RULE,p,handler)
    def forget(self,*,operation=1,path=None,other_path='',family=2,port=0,address=b'\0'*16,handler=None):
        return self._command(Message.FORGET,struct.pack('<III1024s1024s',operation,0,0,_path(path) if path else b'\0'*1024,_path(other_path) if other_path else b'\0'*1024),handler) if path is not None else self._command(Message.FORGET,struct.pack('<IHH16sB3x',operation,family,port,address,0),handler)
    def reveal_shadow(self,path,scope=ShadowScope.NODE,handler=None): return self._command(Message.REVEAL_SHADOW,struct.pack('<III1024s1024s',1,int(scope),0,_path(path),b'\0'*1024),handler)
    def restore_shadow(self,path,scope=ShadowScope.NODE,handler=None): return self._command(Message.RESTORE_SHADOW,struct.pack('<III1024s1024s',1,int(scope),0,_path(path),b'\0'*1024),handler)
    def get_state(self,handler=None): return self._command(Message.GET_STATE,b'',handler)
    def serve(self,handler):
        if self.state is ChannelState.CREATED: self.handshake()
        while self.state is ChannelState.READY and not self.closed:
            req=self._event(); result=handler(req)
            if isinstance(req,(NetRequest,PathRequest)) and result is not None:
                if isinstance(result,tuple): self.respond(req.request_id,*result)
                else: self.respond(req.request_id,result)
    def close(self):
        if self.closed:
            return
        self.state=ChannelState.CLOSED
        self.closed=True
        if self.sock is None:
            return
        try: self.sock.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        self.sock.close()

    def detach_socket(self):
        """Transfer the owned socket to another channel implementation."""
        if self.closed or self.sock is None:
            raise Desynchronized('channel socket is not available')
        sock = self.sock
        self.sock = None
        self.closed = True
        self.state = ChannelState.CLOSED
        return sock

@dataclass
class ProotConfig:
    proot_path: str = 'proot'
    args: tuple[str, ...] = ()
    guest_command: tuple[str, ...] = ()
    env: dict[str, str] | None = None
    cwd: str | None = None
    timeout: float = 1.0
    keep_stdin: bool = True
    grace_period: float = 0.5
class ProotProcess:
    def __init__(self,process,channel,stdout,stderr,config):
        self.process,self.channel,self.stdout,self.stderr,self.config=process,channel,stdout,stderr,config
        self._closed=False
    @classmethod
    def spawn(cls,config=None,**kwargs):
        c=config or ProotConfig()
        for k,v in kwargs.items(): setattr(c,k,v)
        if any(x=='--control-fd' or x.startswith('--control-fd=') for x in c.args): raise ValueError('control-fd is managed by launcher')
        a=b=p=ch=None
        try:
            a,b=socket.socketpair(socket.AF_UNIX,socket.SOCK_STREAM)
            b.set_inheritable(True); fd=b.fileno()
            cmd=[c.proot_path,*c.args,'--control-fd',str(fd),*c.guest_command]
            p=subprocess.Popen(cmd,stdin=None if c.keep_stdin else subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=c.env,cwd=c.cwd,pass_fds=(fd,))
            b.close(); b=None
            ch=ControlChannel.from_socket(a,c.timeout); a=None
            ch.handshake()
        except Exception:
            if ch is not None: ch.close()
            if a is not None: a.close()
            if b is not None: b.close()
            if p is not None:
                try: p.kill()
                except OSError: pass
                try:
                    p.wait()
                finally:
                    for stream in (p.stdout,p.stderr):
                        if stream is not None: stream.close()
            raise
        return cls(p,ch,p.stdout,p.stderr,c)
    @property
    def pid(self): return self.process.pid
    @property
    def returncode(self): return self.process.poll()
    def close(self):
        if self._closed:
            return
        self._closed=True
        try:
            self.channel.close()
        finally:
            try: self.process.wait(timeout=self.config.grace_period)
            except subprocess.TimeoutExpired:
                try: self.process.terminate()
                except OSError: pass
                try: self.process.wait(timeout=self.config.grace_period)
                except subprocess.TimeoutExpired:
                    try: self.process.kill()
                    except OSError: pass
                    self.process.wait()
            finally:
                for stream in (self.stdout,self.stderr):
                    if stream is not None: stream.close()

# Optional event-loop/PTY facilities are imported lazily so the original
# standard-library-only API remains usable in minimal installations.
def __getattr__(name):
    if name == 'AsyncControlChannel':
        from .async_channel import AsyncControlChannel
        return AsyncControlChannel
    if name == 'PtyProotProcess':
        from .pty_launcher import PtyProotProcess
        return PtyProotProcess
    if name == 'termux_runtime_bindings':
        from .termux_paths import termux_runtime_bindings
        return termux_runtime_bindings
    if name == 'add_termux_runtime_binds':
        from .termux_paths import add_termux_runtime_binds
        return add_termux_runtime_binds
    raise AttributeError(name)
