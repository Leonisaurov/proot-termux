import { expect, test } from "bun:test";
import { PassThrough } from "node:stream";
import { ControlChannel, MAGIC, Message } from "./control_api.ts";

const frame=(type:number,id:bigint,payload=Buffer.alloc(0))=>{const h=Buffer.alloc(20);h.writeUInt32LE(MAGIC,0);h.writeUInt16LE(1,4);h.writeUInt16LE(type,6);h.writeUInt32LE(payload.length,8);h.writeBigUInt64LE(id,12);return Buffer.concat([h,payload])};
const hello=()=>frame(Message.HELLO,0n);
test("requires HELLO and fails unknown types",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,50);io.write(frame(Message.COMMAND_RESULT,1n));await expect(c.recv()).rejects.toThrow();expect(c.state).toBe("failed");});
test("uses one deadline for fragmented frames",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,20);const h=frame(Message.HELLO,0n,Buffer.alloc(10));io.write(h.subarray(0,20+5));const read=c.recvFrame();const assertion=expect(read).rejects.toThrow("timeout");await new Promise(r=>setTimeout(r,30));io.write(h.subarray(25));await assertion;expect(c.state).toBe("failed");});
test("serve does not answer shadow events",async()=>{const io=new PassThrough();const c=ControlChannel.fromStream(io,50);const p=Buffer.alloc(2056);p.writeUInt32LE(1,0);p.writeUInt32LE(2,4);Buffer.from("/guest\0").copy(p,8);io.write(Buffer.concat([hello(),frame(Message.SHADOW_EVENT,3n,p)]));const seen:any[]=[];const serving=c.serve(r=>{seen.push(r);return 1 as any});await new Promise(r=>setTimeout(r,10));expect(seen).toHaveLength(1);expect(io.read()).toBeNull();c.close();await serving.catch(()=>{});});
