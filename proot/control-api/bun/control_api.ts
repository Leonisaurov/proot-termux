/* Dependency-free PRCT client. Bun/Node streams are deliberately kept out of the codec. */
import { Socket } from "node:net";
import { closeSync, createReadStream, createWriteStream } from "node:fs";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { dlopen, FFIType, ptr } from "bun:ffi";
export const MAGIC=0x50524354, VERSION=1, MAX_FRAME=4096;
export enum Message{HELLO=1,NET_ACCESS_REQUEST=2,PATH_ACCESS_REQUEST=3,SHADOW_EVENT=4,COMMAND_RESULT=5,ALLOW_ONCE=16,ALLOW_ALWAYS=17,DENY_ONCE=18,DENY_ALWAYS=19,FORGET=20,SET_RULE=21,REVEAL_SHADOW=22,RESTORE_SHADOW=23,GET_STATE=24}
export enum Decision{DENY=0,ALLOW=1}
export type ChannelState="created"|"ready"|"failed"|"closed";
export class ControlError extends Error{readonly code:string;constructor(code:string){super(code);this.code=code}}
export type NetRequest={type:Message.NET_ACCESS_REQUEST,requestId:bigint,operation:number,guestPid:number,family:number,protocol:number,guestPort:number,hostPort:number,address:Uint8Array,virtualClass:number,realExposure:number,proxy:string,domain:string};
export type PathRequest={type:Message.PATH_ACCESS_REQUEST,requestId:bigint,operation:number,reason:number,path:string,otherPath:string};
export type ShadowEvent={type:Message.SHADOW_EVENT,requestId:bigint,operation:number,reason:number,path:string,otherPath:string};
export type CommandResult={type:Message.COMMAND_RESULT,requestId:bigint,status:number,flags:number,dynamicPathRules:number,dynamicNetRules:number,shadows:number};
export type Request=NetRequest|PathRequest|ShadowEvent|CommandResult;
const decoder=new TextDecoder("utf-8",{fatal:true});
const text=(b:Uint8Array)=>{const i=b.indexOf(0);if(i<0)throw new ControlError("unterminated string");return decoder.decode(b.slice(0,i))};
const guest=(b:Uint8Array)=>{const s=text(b);if(s&&!s.startsWith("/"))throw new ControlError("guest path");return s};
const path=(s:string)=>{const b=new TextEncoder().encode(s);if(!s.startsWith("/")||s.includes("\0")||b.length>=1024)throw new ControlError("guest path");const x=new Uint8Array(1024);x.set(b);return x};
export class ControlChannel{
 private input:AsyncIterator<Buffer>; private output:any; private pending=Buffer.alloc(0); private timeout:number; private stateValue:ChannelState="created"; private nextId=1n; private queue:Request[]=[]; private release?:()=>void;
 private constructor(input:AsyncIterator<Buffer>,output:any,timeout=1000){this.input=input;this.output=output;this.timeout=timeout;output.on?.("error",()=>{if(this.stateValue!=="closed")this.stateValue="failed"})}
 static fromStream(stream:any,timeout=1000){return new ControlChannel(stream[Symbol.asyncIterator](),stream,timeout)}
 static fromFd(fd:number,timeout=1000){
  const input=createReadStream(null as any,{fd,autoClose:false});
  const output=createWriteStream(null as any,{fd,autoClose:false});
  const channel=new ControlChannel(input[Symbol.asyncIterator](),output,timeout);
  let released=false;
  channel.release=()=>{if(released)return;released=true;input.destroy();output.destroy();try{closeSync(fd)}catch{} };
  return channel;
 }
 get state(){return this.stateValue}
 private fail<T>(e:ControlError):never{if(this.stateValue!=="closed")this.stateValue="failed";throw e}
 private async exact(n:number,end=performance.now()+this.timeout){while(this.pending.length<n){const left=end-performance.now();if(left<=0)this.fail(new ControlError("timeout"));let timerId:ReturnType<typeof setTimeout>|undefined;const timer=new Promise<never>((_,reject)=>{timerId=setTimeout(()=>reject(new ControlError("timeout")),left)});let it:any;try{it=await Promise.race([this.input.next(),timer])}catch(e){this.fail(e as ControlError)}finally{clearTimeout(timerId)}if(it.done||!it.value)this.fail(new ControlError("eof"));this.pending=Buffer.concat([this.pending,Buffer.from(it.value)])}const x=this.pending.subarray(0,n);this.pending=this.pending.subarray(n);return x}
 async handshake(){if(this.stateValue==="ready")return this;if(this.stateValue!=="created")this.fail(new ControlError("handshake state"));const f=await this.recvFrame();if(f.type!==Message.HELLO||f.requestId!==0n||f.payload.length)this.fail(new ControlError("invalid HELLO"));this.stateValue="ready";return this}
 async recvFrame(){if(this.stateValue==="failed"||this.stateValue==="closed")throw new ControlError("terminal channel");const end=performance.now()+this.timeout;const h=await this.exact(20,end);if(h.readUInt32LE(0)!==MAGIC||h.readUInt16LE(4)!==VERSION)this.fail(new ControlError("invalid header"));const t=h.readUInt16LE(6),n=h.readUInt32LE(8);if(n>MAX_FRAME||!Object.values(Message).some(v=>typeof v==="number"&&v===t))this.fail(new ControlError("invalid frame"));return{type:t as Message,requestId:h.readBigUInt64LE(12),payload:await this.exact(n,end)}}
 async sendFrame(type:Message,id=0n,payload=new Uint8Array()){if(this.stateValue==="failed"||this.stateValue==="closed")throw new ControlError("terminal channel");if(payload.length>MAX_FRAME)throw new ControlError("payload too large");const h=Buffer.alloc(20);h.writeUInt32LE(MAGIC,0);h.writeUInt16LE(VERSION,4);h.writeUInt16LE(type,6);h.writeUInt32LE(payload.length,8);h.writeBigUInt64LE(id,12);try{await new Promise<void>((resolve,reject)=>{
 const timer=setTimeout(()=>reject(new ControlError("timeout")),this.timeout);
 try{this.output.write(Buffer.concat([h,Buffer.from(payload)]),(error?:Error|null)=>{
  clearTimeout(timer);if(error)reject(error);else resolve();
 });}catch(error){clearTimeout(timer);reject(error)}
})}catch(error){this.fail(error instanceof ControlError?error:new ControlError("write"))}}
 private decode(f:any):Request{const p=f.payload;if(f.type===Message.NET_ACCESS_REQUEST){if(p.length!==230)this.fail(new ControlError("net payload"));const family=p.readUInt16LE(12);const op=p.readUInt32LE(0);if(!([1,2,4].includes(op)?family===2||family===10:op===3?family===0:op===5?family!==0:false))this.fail(new ControlError("net family"));return{type:f.type,requestId:f.requestId,operation:p.readUInt32LE(0),guestPid:p.readInt32LE(4),family,protocol:p.readUInt16LE(14),guestPort:p.readUInt16LE(16),hostPort:p.readUInt16LE(18),address:p.slice(20,36),virtualClass:p[36],realExposure:p[37],proxy:text(p.slice(38,102)),domain:text(p.slice(102,230))}}
 if(f.type===Message.PATH_ACCESS_REQUEST||f.type===Message.SHADOW_EVENT){if(p.length!==2056)this.fail(new ControlError("path payload"));return{type:f.type,requestId:f.requestId,operation:p.readUInt32LE(0),reason:p.readUInt32LE(4),path:guest(p.slice(8,1032)),otherPath:guest(p.slice(1032,2056))} as PathRequest|ShadowEvent}
 if(f.type===Message.COMMAND_RESULT){if(p.length!==20)this.fail(new ControlError("result payload"));return{type:f.type,requestId:f.requestId,status:p.readInt32LE(0),flags:p.readUInt32LE(4),dynamicPathRules:p.readUInt32LE(8),dynamicNetRules:p.readUInt32LE(12),shadows:p.readUInt32LE(16)}}this.fail(new ControlError("not an event"))}
 async recv():Promise<Request>{try{if(this.stateValue==="created"){const f=await this.recvFrame();if(f.type!==Message.HELLO||f.requestId!==0n||f.payload.length)this.fail(new ControlError("invalid HELLO"));this.stateValue="ready";return this.decode(await this.recvFrame())}return this.decode(await this.recvFrame())}catch(e){return this.fail(e instanceof ControlError?e:new ControlError("frame"))}}
 async respond(id:bigint,d:Decision,reason="",reasonCode=0,persistent=false){const r=new TextEncoder().encode(reason);if(r.length>=96||reasonCode<0||reasonCode>255)throw new ControlError("invalid reason");const p=new Uint8Array(98);p[0]=d;p[1]=reasonCode;p.set(r,2);await this.sendFrame(persistent?(d===Decision.ALLOW?Message.ALLOW_ALWAYS:Message.DENY_ALWAYS):(d===Decision.ALLOW?Message.ALLOW_ONCE:Message.DENY_ONCE),id,p)}
 allowOnce(id:bigint,r="",c=0){return this.respond(id,Decision.ALLOW,r,c)} allowAlways(id:bigint,r="",c=0){return this.respond(id,Decision.ALLOW,r,c,true)} denyOnce(id:bigint,r="",c=0){return this.respond(id,Decision.DENY,r,c)} denyAlways(id:bigint,r="",c=0){return this.respond(id,Decision.DENY,r,c,true)}
 private async command(t:Message,p:Uint8Array,handler?:(r:Request)=>Promise<void>|void){const id=BigInt(this.nextId++);await this.sendFrame(t,id,p);for(;;){const r=await this.recv();if(r.type===Message.COMMAND_RESULT){if(r.requestId!==id)this.fail(new ControlError("command result request-id mismatch"));return r;}if(handler)await handler(r);else this.queue.push(r)}}
 async setNetRule(operation:number,family:number,port:number,address:Uint8Array,decision:Decision,handler?:(r:Request)=>Promise<void>|void){if(address.length!==16||port<0||port>65535)throw new ControlError("network rule");const p=new Uint8Array(28),d=new DataView(p.buffer);d.setUint32(0,operation,true);d.setUint16(4,family,true);d.setUint16(6,port,true);p.set(address,8);p[24]=decision;return this.command(Message.SET_RULE,p,handler)}
 async setPathRule(operation:number,guestPath:string,decision:Decision,otherPath="",handler?:(r:Request)=>Promise<void>|void){const p=new Uint8Array(2060),d=new DataView(p.buffer);d.setUint32(0,operation,true);d.setUint32(8,decision,true);p.set(path(guestPath),12);if(otherPath)p.set(path(otherPath),1036);return this.command(Message.SET_RULE,p,handler)}
 async revealShadow(guestPath:string,recursive=false,handler?:(r:Request)=>Promise<void>|void){return this.shadow(Message.REVEAL_SHADOW,guestPath,recursive,handler)}
 async restoreShadow(guestPath:string,recursive=false,handler?:(r:Request)=>Promise<void>|void){return this.shadow(Message.RESTORE_SHADOW,guestPath,recursive,handler)}
 private shadow(t:Message,guestPath:string,recursive:boolean,handler?:(r:Request)=>Promise<void>|void){const p=new Uint8Array(2060),d=new DataView(p.buffer);d.setUint32(4,recursive?2:1,true);p.set(path(guestPath),12);return this.command(t,p,handler)}
 async getState(handler?:(r:Request)=>Promise<void>|void){return this.command(Message.GET_STATE,new Uint8Array(),handler)}
 async serve(handler:(r:Request)=>Decision|undefined|Promise<Decision|undefined>){if(this.stateValue==="created")await this.handshake();for(;;){const r=this.queue.shift()??await this.recv();const d=await handler(r);if((r.type===Message.NET_ACCESS_REQUEST||r.type===Message.PATH_ACCESS_REQUEST)&&d!==undefined)await this.respond(r.requestId,d)}}
 close(){if(this.stateValue==="closed")return;this.stateValue="closed";if(this.release)this.release();else this.output.destroy?.()}
}
export type ProotCommandOptions={prootPath?:string,args?:string[],guestCommand?:string[],env?:Record<string,string>,cwd?:string,timeout?:number,graceMs?:number,keepStdin?:boolean};
// Wait for exit without leaving timers/listeners behind on the common path.
async function waitForExit(child:ReturnType<typeof spawn>, grace:number):Promise<boolean>{
 if(child.exitCode!==null||child.signalCode!==null)return true;
 return new Promise(resolve=>{
  const done=()=>{clearTimeout(timer);resolve(true)};
  const timer=setTimeout(()=>{child.removeListener("exit",done);resolve(false)},grace);
  child.once("exit",done);
 });
}
async function stopProcess(child:ReturnType<typeof spawn>,grace:number){
 if(await waitForExit(child,grace))return;
 child.kill("SIGTERM");
 if(await waitForExit(child,grace))return;
 const exited=once(child,"exit");
 child.kill("SIGKILL");
 await exited;
}
export class ProotCommand{
 constructor(public options:ProotCommandOptions={}){}
 async spawn(){
  const o=this.options;
  if((o.args??[]).some(x=>x==="--control-fd"||x.startsWith("--control-fd=")))
   throw new ControlError("control-fd is managed");
  const grace=o.graceMs??500;
  if(!Number.isFinite(grace)||grace<0)throw new ControlError("invalid grace period");
  const runtime=globalThis.process;
  const api=dlopen(runtime.platform==="darwin"?"/usr/lib/libSystem.B.dylib":runtime.env.ANDROID_ROOT?"libc.so":"libc.so.6",{socketpair:{args:[FFIType.i32,FFIType.i32,FFIType.i32,FFIType.pointer],returns:FFIType.i32}});
  const fds=new Int32Array(2);
  try{if(api.symbols.socketpair(1,1,0,ptr(fds))!==0)throw new ControlError("socketpair")}
  finally{api.close()}
  let [parent,childFd]=fds;
  let channel:ControlChannel|undefined;
  let child:ReturnType<typeof spawn>|undefined;
  let spawned=false;
  try{
   channel=ControlChannel.fromFd(parent,o.timeout??1000);parent=-1;
   child=spawn(o.prootPath??"proot",[...(o.args??[]),"--control-fd","3",...(o.guestCommand??[])],{
    cwd:o.cwd,env:o.env,stdio:[o.keepStdin===false?"ignore":"inherit","pipe","pipe",childFd] as any
   });
   const started=once(child,"spawn");
   closeSync(childFd);childFd=-1;
   await started;spawned=true;
   await channel.handshake();
  }catch(error){
   channel?.close();
   if(parent>=0)closeSync(parent);
   if(childFd>=0)closeSync(childFd);
   if(child&&spawned)await stopProcess(child,0);
   child?.stdout?.destroy();child?.stderr?.destroy();
   throw error;
  }
  const process=child,ownedChannel=channel;
  let closing:Promise<void>|undefined;
  return {process,channel:ownedChannel,stdout:process.stdout,stderr:process.stderr,pid:process.pid,
   close:()=>closing??=(async()=>{
    ownedChannel.close();
    try{await stopProcess(process,grace)}
    finally{process.stdout?.destroy();process.stderr?.destroy()}
   })()};
 }
}
