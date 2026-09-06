import { expect, test } from "bun:test";
import { PassThrough } from "node:stream";
import { ControlChannel, ProotCommand, MAGIC, Message } from "./control_api.ts";


const frame=(type:number,id:bigint,payload=Buffer.alloc(0))=>{const h=Buffer.alloc(20);h.writeUInt32LE(MAGIC,0);h.writeUInt16LE(1,4);h.writeUInt16LE(type,6);h.writeUInt32LE(payload.length,8);h.writeBigUInt64LE(id,12);return Buffer.concat([h,payload])};
const hello=()=>frame(Message.HELLO,0n);
test("launcher handles spawn failure",async()=>{
 await expect(new ProotCommand({prootPath:"/nonexistent-prct-audit"}).spawn()).rejects.toThrow("ENOENT");
});
test("launcher close kills and reaps a peer ignoring TERM",async()=>{
 const proot=(globalThis.process.env.PREFIX??"/data/data/com.termux/files/usr")+"/bin/proot";
 const shell=(globalThis.process.env.PREFIX??"/data/data/com.termux/files/usr")+"/bin/sh";
 const p=await new ProotCommand({prootPath:proot,args:["-r","/","-w",globalThis.process.env.PREFIX??"/data/data/com.termux/files/usr"],guestCommand:[shell,"-c","trap '' TERM; sleep 60"],graceMs:20,timeout:2000,keepStdin:false}).spawn();
 await p.close();await p.close();
 expect(p.process.signalCode).toBe("SIGKILL");
 expect(p.channel.state).toBe("closed");
});
test("accepts SOCKET and PUBLICATION families", async()=>{
 for(const [op,family] of [[5,1],[5,16],[3,0],[2,10]]){
  const io=new PassThrough(); const c=ControlChannel.fromStream(io,100);
  try {const p=Buffer.alloc(230);p.writeUInt32LE(op,0);p.writeUInt16LE(family,12);
   io.write(Buffer.concat([hello(),frame(Message.NET_ACCESS_REQUEST,1n,p)]));
   expect((await c.recv() as any).family).toBe(family);
  } finally {c.close();}
 }
});
test("rejects unterminated strings", async()=>{
 const io=new PassThrough(); const c=ControlChannel.fromStream(io,100);
 try {const p=Buffer.alloc(2056);p.fill(47,8,1032);
  io.write(Buffer.concat([hello(),frame(Message.PATH_ACCESS_REQUEST,1n,p)]));
  await expect(c.recv()).rejects.toThrow("unterminated string");expect(c.state).toBe("failed");
 } finally {c.close();}
});
test("closed channel cannot send", async()=>{
 const io=new PassThrough();const c=ControlChannel.fromStream(io);c.close();
 await expect(c.sendFrame(Message.GET_STATE,1n)).rejects.toThrow("terminal");
 expect(c.state).toBe("closed");
});
test("command result mismatch is terminal", async()=>{
 // Separate input and output so the command cannot be read as a peer event.
 const input=new PassThrough();
 const io={ [Symbol.asyncIterator]:()=>input[Symbol.asyncIterator](),write:(_data:any,cb:()=>void)=>{cb();return true},destroy:()=>input.destroy() };
 const c=ControlChannel.fromStream(io,100);
 try {input.write(Buffer.concat([hello(),frame(Message.COMMAND_RESULT,99n,Buffer.alloc(20))]));
  await expect(c.getState()).rejects.toThrow("request-id mismatch");
  expect(c.state).toBe("failed");
 } finally {c.close();}
});
test("requires HELLO and fails unknown types",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,50);io.write(frame(Message.COMMAND_RESULT,1n));await expect(c.recv()).rejects.toThrow();expect(c.state).toBe("failed");});
test("uses one deadline for fragmented frames",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,20);const h=frame(Message.HELLO,0n,Buffer.alloc(10));io.write(h.subarray(0,20+5));const read=c.recvFrame();const assertion=expect(read).rejects.toThrow("timeout");await new Promise(r=>setTimeout(r,30));io.write(h.subarray(25));await assertion;expect(c.state).toBe("failed");});
test("serve does not answer shadow events",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,50);const p=Buffer.alloc(2056);p.writeUInt32LE(1,0);p.writeUInt32LE(2,4);Buffer.from("/guest\0").copy(p,8);io.write(Buffer.concat([hello(),frame(Message.SHADOW_EVENT,3n,p)]));const seen:any[]=[];const serving=c.serve(r=>{seen.push(r);return 1 as any});await new Promise(r=>setTimeout(r,10));expect(seen).toHaveLength(1);expect(io.read()).toBeNull();c.close();await serving.catch(()=>{});});
