//! Synchronous, std-only PRCT client and proot launcher.
use std::{convert::TryInto, io::{self,Read,Write}, os::unix::{io::AsRawFd,net::UnixStream,process::CommandExt}, path::{Path,PathBuf}, process::{Child,Command,Stdio}, time::{Duration,Instant}};
pub const MAGIC:u32=0x50524354; pub const VERSION:u16=1; pub const MAX_FRAME:usize=4096;
#[derive(Debug)] pub enum Error{Io(io::Error),Eof,Timeout,Invalid(&'static str),Desynchronized}
impl From<io::Error> for Error{fn from(e:io::Error)->Self{if e.kind()==io::ErrorKind::TimedOut{Error::Timeout}else{Error::Io(e)}}}
impl std::fmt::Display for Error{fn fmt(&self,f:&mut std::fmt::Formatter<'_>)->std::fmt::Result{match self{Error::Io(e)=>write!(f,"I/O error: {e}"),Error::Eof=>f.write_str("control channel EOF"),Error::Timeout=>f.write_str("control channel timeout"),Error::Invalid(s)=>write!(f,"invalid control frame: {s}"),Error::Desynchronized=>f.write_str("control channel is desynchronized")}}}
impl std::error::Error for Error{fn source(&self)->Option<&(dyn std::error::Error+'static)>{match self{Error::Io(e)=>Some(e),_=>None}}}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]pub enum Message{Hello=1,NetAccessRequest=2,PathAccessRequest=3,ShadowEvent=4,CommandResult=5,AllowOnce=16,AllowAlways=17,DenyOnce=18,DenyAlways=19,Forget=20,SetRule=21,RevealShadow=22,RestoreShadow=23,GetState=24}
impl Message{fn from(v:u16)->Result<Self,Error>{use Message::*;Ok(match v{1=>Hello,2=>NetAccessRequest,3=>PathAccessRequest,4=>ShadowEvent,5=>CommandResult,16=>AllowOnce,17=>AllowAlways,18=>DenyOnce,19=>DenyAlways,20=>Forget,21=>SetRule,22=>RevealShadow,23=>RestoreShadow,24=>GetState,_=>return Err(Error::Invalid("message type"))})}}
#[derive(Clone,Copy,Debug)]pub enum Decision{Deny=0,Allow=1}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]pub enum ChannelState{Created,Ready,Failed,Closed}
#[derive(Debug)]pub struct NetRequest{pub request_id:u64,pub operation:u32,pub guest_pid:i32,pub family:u16,pub protocol:u16,pub guest_port:u16,pub host_port:u16,pub address:[u8;16],pub virtual_class:u8,pub real_exposure:u8,pub proxy:String,pub domain:String}
#[derive(Debug)]pub struct PathRequest{pub request_id:u64,pub message:Message,pub operation:u32,pub reason:u32,pub path:String,pub other_path:String}
#[derive(Debug)]pub struct CommandResult{pub request_id:u64,pub status:i32,pub flags:u32,pub dynamic_path_rules:u32,pub dynamic_net_rules:u32,pub shadows:u32}
#[derive(Debug)]pub enum Request{Net(NetRequest),Path(PathRequest),Shadow(PathRequest),Result(CommandResult)}
pub struct ControlChannel<S>{stream:S,timeout:Duration,state:ChannelState,next_id:u64,queued:std::collections::VecDeque<Request>}
impl ControlChannel<UnixStream>{pub fn from_stream(s:UnixStream,t:Duration)->Result<Self,Error>{s.set_nonblocking(true)?;Ok(Self{stream:s,timeout:t,state:ChannelState::Created,next_id:1,queued:std::collections::VecDeque::new()})}pub fn connect(p:&Path,t:Duration)->Result<Self,Error>{Self::from_stream(UnixStream::connect(p)?,t)}pub fn state(&self)->ChannelState{self.state}}
impl<S:Read+Write> ControlChannel<S>{fn fail<T>(&mut self,e:Error)->Result<T,Error>{if self.state!=ChannelState::Closed{self.state=ChannelState::Failed;}Err(e)}fn exact(&mut self,n:usize, end:Instant)->Result<Vec<u8>,Error>{let mut b=vec![0;n];let mut p=0;while p<n{if Instant::now()>=end{return self.fail(Error::Timeout)}match self.stream.read(&mut b[p..]){Ok(0)=>return self.fail(Error::Eof),Ok(k)=>p+=k,Err(e) if e.kind()==io::ErrorKind::WouldBlock=>std::thread::sleep(Duration::from_millis(1)),Err(e)=>return self.fail(e.into())}}Ok(b)}pub fn handshake(&mut self)->Result<(),Error>{if self.state==ChannelState::Ready{return Ok(())}if self.state!=ChannelState::Created{return self.fail(Error::Desynchronized)}let(t,id,p)=self.recv_frame()?;if t!=Message::Hello||id!=0||!p.is_empty(){return self.fail(Error::Invalid("HELLO"))}self.state=ChannelState::Ready;Ok(())}
 pub fn recv_frame(&mut self)->Result<(Message,u64,Vec<u8>),Error>{if matches!(self.state,ChannelState::Failed|ChannelState::Closed){return Err(Error::Desynchronized)}let end=Instant::now()+self.timeout;let h=self.exact(20,end)?;let magic=u32::from_le_bytes(h[0..4].try_into().unwrap());let ver=u16::from_le_bytes(h[4..6].try_into().unwrap());let typ=u16::from_le_bytes(h[6..8].try_into().unwrap());let n=u32::from_le_bytes(h[8..12].try_into().unwrap())as usize;let id=u64::from_le_bytes(h[12..20].try_into().unwrap());if magic!=MAGIC||ver!=VERSION||n>MAX_FRAME{return self.fail(Error::Invalid("header"))}let t=match Message::from(typ){Ok(t)=>t,Err(e)=>return self.fail(e)};let p=self.exact(n,end)?;Ok((t,id,p))}
 pub fn send_frame(&mut self,t:Message,id:u64,p:&[u8])->Result<(),Error>{if matches!(self.state,ChannelState::Failed|ChannelState::Closed){return Err(Error::Desynchronized)}if p.len()>MAX_FRAME{return Err(Error::Invalid("payload"))}let mut h=Vec::with_capacity(20+p.len());h.extend_from_slice(&MAGIC.to_le_bytes());h.extend_from_slice(&VERSION.to_le_bytes());h.extend_from_slice(&(t as u16).to_le_bytes());h.extend_from_slice(&(p.len()as u32).to_le_bytes());h.extend_from_slice(&id.to_le_bytes());h.extend_from_slice(p);let end=Instant::now()+self.timeout;let mut offset=0;
 while offset<h.len(){
  if Instant::now()>=end{return self.fail(Error::Timeout)}
  match self.stream.write(&h[offset..]){
   Ok(0)=>return self.fail(Error::Eof),Ok(n)=>offset+=n,
   Err(e) if e.kind()==io::ErrorKind::Interrupted=>continue,
   Err(e) if e.kind()==io::ErrorKind::WouldBlock=>std::thread::sleep(Duration::from_millis(1)),
   Err(e)=>return self.fail(e.into()),
  }
 }Ok(())}
 pub fn receive(&mut self)->Result<Request,Error>{if self.state==ChannelState::Created{let(t,id,p)=self.recv_frame()?;if t!=Message::Hello||id!=0||!p.is_empty(){return self.fail(Error::Invalid("HELLO"))}self.state=ChannelState::Ready;let(t,id,p)=self.recv_frame()?;return self.decode_checked(t,id,p)}let(t,id,p)=self.recv_frame()?;self.decode_checked(t,id,p)}
 fn decode_checked(&mut self,t:Message,id:u64,p:Vec<u8>)->Result<Request,Error>{let r=self.decode(t,id,p);if r.is_err(){self.state=ChannelState::Failed}r}
 fn decode(&mut self,t:Message,id:u64,p:Vec<u8>)->Result<Request,Error>{match t{Message::NetAccessRequest=>{if p.len()!=230{return self.fail(Error::Invalid("net payload"))}let mut a=[0;16];a.copy_from_slice(&p[20..36]);let family=u16::from_le_bytes(p[12..14].try_into().unwrap());let operation=u32::from_le_bytes(p[0..4].try_into().unwrap());if !match operation{1|2|4=>family==2||family==10,3=>family==0,5=>family!=0,_=>false}{return self.fail(Error::Invalid("net family"))}Ok(Request::Net(NetRequest{request_id:id,operation:u32::from_le_bytes(p[0..4].try_into().unwrap()),guest_pid:i32::from_le_bytes(p[4..8].try_into().unwrap()),family,protocol:u16::from_le_bytes(p[14..16].try_into().unwrap()),guest_port:u16::from_le_bytes(p[16..18].try_into().unwrap()),host_port:u16::from_le_bytes(p[18..20].try_into().unwrap()),address:a,virtual_class:p[36],real_exposure:p[37],proxy:text(&p[38..102])?,domain:text(&p[102..230])?}))},Message::PathAccessRequest|Message::ShadowEvent=>{if p.len()!=2056{return self.fail(Error::Invalid("path payload"))}let path=guest(&p[8..1032])?;let other=guest(&p[1032..])?;let x=PathRequest{request_id:id,message:t,operation:u32::from_le_bytes(p[0..4].try_into().unwrap()),reason:u32::from_le_bytes(p[4..8].try_into().unwrap()),path,other_path:other};if t==Message::PathAccessRequest{Ok(Request::Path(x))}else{Ok(Request::Shadow(x))}},Message::CommandResult=>{if p.len()!=20{return self.fail(Error::Invalid("result payload"))}Ok(Request::Result(CommandResult{request_id:id,status:i32::from_le_bytes(p[0..4].try_into().unwrap()),flags:u32::from_le_bytes(p[4..8].try_into().unwrap()),dynamic_path_rules:u32::from_le_bytes(p[8..12].try_into().unwrap()),dynamic_net_rules:u32::from_le_bytes(p[12..16].try_into().unwrap()),shadows:u32::from_le_bytes(p[16..20].try_into().unwrap())}))},_=>self.fail(Error::Invalid("not an event"))}}
 pub fn respond_reason(&mut self,id:u64,d:Decision,reason:&str,code:u8,persistent:bool)->Result<(),Error>{let r=reason.as_bytes();if r.len()>=96{return Err(Error::Invalid("reason"))}let t=match(d,persistent){(Decision::Allow,false)=>Message::AllowOnce,(Decision::Allow,true)=>Message::AllowAlways,(Decision::Deny,false)=>Message::DenyOnce,(Decision::Deny,true)=>Message::DenyAlways};let mut p=[0;98];p[0]=d as u8;p[1]=code;p[2..2+r.len()].copy_from_slice(r);self.send_frame(t,id,&p)}
 pub fn allow_once(&mut self,id:u64,r:&str,c:u8)->Result<(),Error>{self.respond_reason(id,Decision::Allow,r,c,false)}pub fn allow_always(&mut self,id:u64,r:&str,c:u8)->Result<(),Error>{self.respond_reason(id,Decision::Allow,r,c,true)}pub fn deny_once(&mut self,id:u64,r:&str,c:u8)->Result<(),Error>{self.respond_reason(id,Decision::Deny,r,c,false)}pub fn deny_always(&mut self,id:u64,r:&str,c:u8)->Result<(),Error>{self.respond_reason(id,Decision::Deny,r,c,true)}pub fn respond(&mut self,id:u64,d:Decision)->Result<(),Error>{self.respond_reason(id,d,"",0,false)}
 fn command(&mut self,t:Message,p:&[u8])->Result<Request,Error>{let id=self.next_id;self.next_id+=1;self.send_frame(t,id,p)?;loop{let r=self.receive()?;let rid=match &r{Request::Net(x)=>x.request_id,Request::Path(x)|Request::Shadow(x)=>x.request_id,Request::Result(x)=>x.request_id};if matches!(r,Request::Result(_)){if rid!=id{return self.fail(Error::Invalid("command result request-id mismatch"))}return Ok(r)}self.queued.push_back(r)}}
 pub fn get_state(&mut self)->Result<Request,Error>{self.command(Message::GetState,&[])}
 pub fn set_net_rule(&mut self,op:u32,family:u16,port:u16,address:[u8;16],decision:Decision)->Result<Request,Error>{let mut p=[0;28];p[0..4].copy_from_slice(&op.to_le_bytes());p[4..6].copy_from_slice(&family.to_le_bytes());p[6..8].copy_from_slice(&port.to_le_bytes());p[8..24].copy_from_slice(&address);p[24]=decision as u8;self.command(Message::SetRule,&p)}
 pub fn set_path_rule(&mut self,op:u32,path:&str,other:&str,decision:Decision)->Result<Request,Error>{let mut p=[0;2060];p[0..4].copy_from_slice(&op.to_le_bytes());p[8..12].copy_from_slice(&(decision as u32).to_le_bytes());put_path(&mut p[12..1036],path)?;if !other.is_empty(){put_path(&mut p[1036..],other)?}self.command(Message::SetRule,&p)}
 pub fn reveal_shadow(&mut self,path:&str,recursive:bool)->Result<Request,Error>{self.shadow(Message::RevealShadow,path,recursive)}pub fn restore_shadow(&mut self,path:&str,recursive:bool)->Result<Request,Error>{self.shadow(Message::RestoreShadow,path,recursive)}fn shadow(&mut self,t:Message,path:&str,recursive:bool)->Result<Request,Error>{let mut p=[0;2060];p[4..8].copy_from_slice(&(if recursive{2u32}else{1}).to_le_bytes());put_path(&mut p[12..1036],path)?;self.command(t,&p)}
 pub fn serve<F:FnMut(Request)->Option<Decision>>(&mut self,mut f:F)->Result<(),Error>{if self.state==ChannelState::Created{self.handshake()?}loop{let r=if let Some(x)=self.queued.pop_front(){x}else{self.receive()?};match r{Request::Net(x)=>{let id=x.request_id;if let Some(d)=f(Request::Net(x)){if matches!(d,Decision::Allow|Decision::Deny){self.respond(id,d)?}}},Request::Path(x)=>{let id=x.request_id;if let Some(d)=f(Request::Path(x)){if matches!(d,Decision::Allow|Decision::Deny){self.respond(id,d)?}}},r@Request::Shadow(_)|r@Request::Result(_)=>{f(r);}}}}
 pub fn close(&mut self) where S:AsRawFd {
 if self.state==ChannelState::Closed{return}
 self.state=ChannelState::Closed;
 unsafe{extern "C"{fn shutdown(fd:i32,how:i32)->i32;}shutdown(self.stream.as_raw_fd(),2);}
}}
fn text(p:&[u8])->Result<String,Error>{let n=p.iter().position(|&x|x==0).ok_or(Error::Invalid("unterminated string"))?;String::from_utf8(p[..n].to_vec()).map_err(|_|Error::Invalid("utf8"))}fn guest(p:&[u8])->Result<String,Error>{let s=text(p)?;if !s.is_empty()&&!s.starts_with('/') {return Err(Error::Invalid("guest path"))}Ok(s)}fn put_path(d:&mut[u8],s:&str)->Result<(),Error>{if !s.starts_with('/')||s.as_bytes().contains(&0)||s.len()>=1024{return Err(Error::Invalid("guest path"))}d[..s.len()].copy_from_slice(s.as_bytes());Ok(())}

pub struct ProotCommand{pub proot_path:PathBuf,pub args:Vec<String>,pub guest_command:Vec<String>,pub timeout:Duration,pub grace:Duration,pub keep_stdin:bool}
impl Default for ProotCommand{fn default()->Self{Self{proot_path:PathBuf::from("proot"),args:vec![],guest_command:vec![],timeout:Duration::from_millis(1000),grace:Duration::from_millis(500),keep_stdin:true}}}
pub struct ProotProcess{pub child:Child,pub channel:ControlChannel<UnixStream>,pub stdout:std::process::ChildStdout,pub stderr:std::process::ChildStderr,grace:Duration}
impl ProotCommand{pub fn spawn(self)->Result<ProotProcess,Error>{if self.args.iter().any(|x|x=="--control-fd"||x.starts_with("--control-fd=")){return Err(Error::Invalid("control-fd is managed"))}let(a,b)=UnixStream::pair()?;let fd=b.as_raw_fd();let mut cmd=Command::new(self.proot_path);cmd.args(self.args).arg("--control-fd").arg(fd.to_string()).args(self.guest_command).stdout(Stdio::piped()).stderr(Stdio::piped());if !self.keep_stdin{cmd.stdin(Stdio::null());}unsafe{cmd.pre_exec(move||{extern "C"{fn fcntl(fd:i32,cmd:i32,...)->i32;}const F_GETFD:i32=1;const F_SETFD:i32=2;let flags=fcntl(fd,F_GETFD);if flags<0{return Err(io::Error::last_os_error())}if fcntl(fd,F_SETFD,flags&!1)<0{return Err(io::Error::last_os_error())}Ok(())})};let mut ch=ControlChannel::from_stream(a,self.timeout)?;let mut child=cmd.spawn()?;drop(b);if let Err(e)=ch.handshake(){let _=child.kill();let _=child.wait();return Err(e)}let out=child.stdout.take().unwrap();let err=child.stderr.take().unwrap();Ok(ProotProcess{child,channel:ch,stdout:out,stderr:err,grace:self.grace})}}

impl ProotProcess {
    /// Close the channel, bound guest shutdown, reap the child and release streams.
    /// Consuming self makes ownership of stdout/stderr explicit.
    pub fn close(mut self) -> Result<(), Error> {
        self.channel.close();
        if self.wait_grace()? { return Ok(()); }
        unsafe { extern "C" { fn kill(pid:i32,signal:i32)->i32; }
            kill(self.child.id() as i32,15);
        }
        if !self.wait_grace()? { self.child.kill()?; self.child.wait()?; }
        Ok(())
    }
    fn wait_grace(&mut self) -> Result<bool, Error> {
        let end=Instant::now()+self.grace;
        loop {
            if self.child.try_wait()?.is_some() { return Ok(true); }
            if Instant::now()>=end { return Ok(false); }
            std::thread::sleep(Duration::from_millis(5));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{io::Write, os::unix::net::UnixStream, thread, time::Duration};
    fn frame(t: u16, id: u64, p: &[u8]) -> Vec<u8> { let mut h=Vec::new(); h.extend_from_slice(&MAGIC.to_le_bytes()); h.extend_from_slice(&VERSION.to_le_bytes()); h.extend_from_slice(&t.to_le_bytes()); h.extend_from_slice(&(p.len() as u32).to_le_bytes()); h.extend_from_slice(&id.to_le_bytes()); h.extend_from_slice(p); h }
    #[test] fn hello_is_required() { let (a,mut b)=UnixStream::pair().unwrap(); b.write_all(&frame(2,1,&[])).unwrap(); let mut c=ControlChannel::from_stream(a,Duration::from_millis(100)).unwrap(); assert!(matches!(c.receive(),Err(Error::Invalid("HELLO")))); assert_eq!(c.state(),ChannelState::Failed); }
    #[test] fn unknown_type_fails_channel() { let (a,mut b)=UnixStream::pair().unwrap(); b.write_all(&frame(99,1,&[])).unwrap(); let mut c=ControlChannel::from_stream(a,Duration::from_millis(100)).unwrap(); assert!(matches!(c.recv_frame(),Err(Error::Invalid("message type")))); assert_eq!(c.state(),ChannelState::Failed); }
    #[test] fn fragmented_frame_uses_one_deadline() { let (a,mut b)=UnixStream::pair().unwrap(); let mut c=ControlChannel::from_stream(a,Duration::from_millis(30)).unwrap(); let h=frame(1,0,&[]); thread::spawn(move||{ b.write_all(&h[..10]).unwrap(); thread::sleep(Duration::from_millis(40)); b.write_all(&h[10..]).unwrap(); }); assert!(c.recv_frame().is_err()); assert_eq!(c.state(),ChannelState::Failed); }
}

#[cfg(test)]
mod regressions {
    use super::*;
    fn pair() -> (ControlChannel<UnixStream>, UnixStream) {
        let (a, b) = UnixStream::pair().unwrap();
        (ControlChannel::from_stream(a, Duration::from_millis(100)).unwrap(), b)
    }
    fn send(b: &mut UnixStream, typ: Message, id: u64, p: &[u8]) {
        let mut h = Vec::new();
        h.extend_from_slice(&MAGIC.to_le_bytes());
        h.extend_from_slice(&VERSION.to_le_bytes());
        h.extend_from_slice(&(typ as u16).to_le_bytes());
        h.extend_from_slice(&(p.len() as u32).to_le_bytes());
        h.extend_from_slice(&id.to_le_bytes());
        h.extend_from_slice(p);
        b.write_all(&h).unwrap();
    }
    #[test]
    fn socket_and_publication_families() {
        for (op, family) in [(5u32, 1u16), (5, 16), (3, 0), (2, 10)] {
            let (mut c, mut b) = pair();
            let mut p = [0; 230];
            p[..4].copy_from_slice(&op.to_le_bytes());
            p[12..14].copy_from_slice(&family.to_le_bytes());
            send(&mut b, Message::Hello, 0, &[]);
            send(&mut b, Message::NetAccessRequest, 1, &p);
            assert!(matches!(c.receive().unwrap(), Request::Net(x) if x.family == family));
        }
    }
    #[test]
    fn mismatched_command_result_is_terminal() {
        let (mut c, mut b) = pair();
        send(&mut b, Message::Hello, 0, &[]);
        send(&mut b, Message::CommandResult, 99, &[0; 20]);
        assert!(matches!(c.get_state(), Err(Error::Invalid(_))));
        assert_eq!(c.state(), ChannelState::Failed);
        assert!(matches!(c.send_frame(Message::GetState, 2, &[]), Err(Error::Desynchronized)));
    }
    #[test]
    fn guest_path_rejects_embedded_nul_and_missing_terminator() {
        assert!(put_path(&mut [0; 1024], "/allowed\0/hidden").is_err());
        assert!(guest(&[b'/'; 1024]).is_err());
    }
    #[test]
    fn closed_channel_cannot_send() {
        let (mut c, _b) = pair();
        c.close();
        assert!(matches!(c.send_frame(Message::GetState, 1, &[]), Err(Error::Desynchronized)));
        assert_eq!(c.state(), ChannelState::Closed);
    }
}
