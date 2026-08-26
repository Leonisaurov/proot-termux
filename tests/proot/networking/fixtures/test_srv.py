import sys
import socket
import socketserver as ss

class MyHandler(ss.BaseRequestHandler):
    def handle(self):
        print(f"handler: client_address={self.client_address!r}", file=sys.stderr)
        self.request.send(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nOK")
        self.request.close()

class MyServer(ss.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

MyServer.address_family = socket.AF_INET
srv = MyServer(("127.0.0.1", 9999), MyHandler)
print("server created", file=sys.stderr)
print("serving...", file=sys.stderr)
srv.serve_forever()
