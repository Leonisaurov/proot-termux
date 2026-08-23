use prct_control_api::{ControlChannel,Decision,Request};
use std::os::unix::net::UnixStream;
use std::time::Duration;
fn main()->Result<(),Box<dyn std::error::Error>> { let stream=UnixStream::connect("/run/proot-control.sock")?; let mut ch=ControlChannel::from_stream(stream,Duration::from_secs(1))?; ch.serve(|r| { if let Request::Net(n)=r { println!("guest network op {}",n.operation); Some(Decision::Deny) } else { None } })?; Ok(()) }
