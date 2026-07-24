import socket
import socketserver

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
print('socket domain:', s.family)
print('socket type:', s.type)

import struct
sa = socket.getaddrinfo('0.0.0.0', 8000, socket.AF_INET, socket.SOCK_STREAM)[0]
print('addr info:', sa)

s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.settimeout(10)

try:
    s.bind(('0.0.0.0', 8000))
    print('bind OK')
except Exception as e:
    print('bind FAILED:', e)

s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s2.bind(('', 8000))
    print('bind empty host OK')
except Exception as e:
    print('bind empty host FAILED:', e)

s3 = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
try:
    s3.bind(('::', 8000))
    print('bind IPv6 OK')
except Exception as e:
    print('bind IPv6 FAILED:', e)

s4 = socket.socket(sa[0], sa[1])
print('family from getaddrinfo:', sa[0], 'type:', sa[1])
try:
    s4.bind(sa[4])
    print('bind getaddrinfo OK')
except Exception as e:
    print('bind getaddrinfo FAILED:', e)

s.close()
s2.close()
s3.close()
s4.close()
