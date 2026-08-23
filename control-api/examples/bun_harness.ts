import { ControlChannel, Decision, Message } from "../bun/control_api.ts";
export async function runHarness() {
  await ControlChannel.fromFd(3).serve(r =>
    r.type === Message.PATH_ACCESS_REQUEST || r.type === Message.NET_ACCESS_REQUEST
      ? Decision.ALLOW : undefined);
}
