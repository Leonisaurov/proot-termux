import http.server
import socket
import socketserver as ss
import contextlib
import argparse
import sys
import os
import time

parser = argparse.ArgumentParser()
parser.add_argument('port', default=8000, type=int, nargs='?')
parser.add_argument('-b', '--bind', default=None)
parser.add_argument('-d', '--directory', default=None)
args = parser.parse_args(['8080', '--bind', '127.0.0.1'])

class DualStackServerMixin:
    def server_bind(self):
        with contextlib.suppress(Exception):
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        return super().server_bind()

    def finish_request(self, request, client_address):
        print(f"finish_request: client_address={client_address!r} type={type(client_address).__name__}", file=sys.stderr)
        self.RequestHandlerClass(request, client_address, self, directory=args.directory)

class MyServer(DualStackServerMixin, http.server.ThreadingHTTPServer):
    pass

infos = socket.getaddrinfo('127.0.0.1', 8080, type=socket.SOCK_STREAM, flags=socket.AI_PASSIVE)
family, type_, proto, canonname, sockaddr = next(iter(infos))
MyServer.address_family = family

srv = MyServer(sockaddr, http.server.SimpleHTTPRequestHandler)
print("Server created", file=sys.stderr)
srv.server_activate()

pid = os.fork()
if pid == 0:
    time.sleep(0.5)
    c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    c.connect(('127.0.0.1', 8080))
    print(c.recv(1024))
    c.close()
    os._exit(0)
else:
    conn, addr = srv.get_request()
    print(f"Accepted: addr={addr!r}", file=sys.stderr)
    h = http.server.SimpleHTTPRequestHandler(conn, addr, srv, directory='/root')
    print(f"Handler client_address = {h.client_address!r}", file=sys.stderr)
    print(f"address_string = {h.address_string()!r}", file=sys.stderr)
    os.waitpid(pid, 0)
