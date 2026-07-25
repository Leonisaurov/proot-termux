import socket as s
import sys

role = sys.argv[1]
port = 8080

if role == "server":
    x = s.socket(s.AF_INET, s.SOCK_STREAM)
    x.setsockopt(s.SOL_SOCKET, s.SO_REUSEADDR, 1)
    x.bind(("0.0.0.0", port))
    x.listen(1)
    print("server: listening on port", port, file=sys.stderr)
    sys.stdout.flush()
    while True:
        pass
elif role == "client":
    x = s.socket(s.AF_INET, s.SOCK_STREAM)
    x.settimeout(5)
    try:
        x.connect(("127.0.0.1", port))
        print("client: connected OK")
    except Exception as e:
        print("client: FAILED:", e)
