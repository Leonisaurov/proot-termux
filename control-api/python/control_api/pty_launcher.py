"""Generic PTY launcher for a PRCT-enabled PRoot process."""
from __future__ import annotations

import fcntl
import os
import pty
import signal
import socket
import struct
import subprocess
import termios
import time

from . import ControlChannel, ProotConfig


class PtyProotProcess:
    def __init__(self, process, master_fd, channel, config):
        self.process, self.master_fd = process, master_fd
        self.control_channel, self.config = channel, config
        self._closed = False

    @classmethod
    def spawn(cls, config=None, **kwargs):
        c = config or ProotConfig()
        for key, value in kwargs.items():
            setattr(c, key, value)
        if any(x == '--control-fd' or x.startswith('--control-fd=') for x in c.args):
            raise ValueError('control-fd is managed by launcher')
        master = slave = None
        parent = child = process = None
        try:
            master, slave = pty.openpty()
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            child.set_inheritable(True)
            fd = child.fileno()

            def setup_tty():
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                # Start with ECHO disabled so capability replies cannot echo.
                # Fish then configures its own raw line-editor mode.
                attrs = termios.tcgetattr(slave)
                attrs[3] &= ~(termios.ECHO | termios.ECHONL)
                termios.tcsetattr(slave, termios.TCSANOW, attrs)

            cmd = [c.proot_path, *c.args, '--control-fd', str(fd), *c.guest_command]
            process = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave,
                                       cwd=c.cwd, env=c.env, pass_fds=(fd,),
                                       start_new_session=False, preexec_fn=setup_tty)
            os.close(slave); slave = None
            child.close(); child = None
            os.set_blocking(master, False)
            channel = ControlChannel.from_socket(parent, c.timeout); parent = None
            channel.handshake()
            return cls(process, master, channel, c)
        except Exception:
            for fd in (master, slave):
                if fd is not None:
                    try: os.close(fd)
                    except OSError: pass
            for sock in (parent, child):
                if sock is not None: sock.close()
            if process is not None:
                try: process.kill()
                except OSError: pass
                process.wait()
            raise

    @property
    def pid(self): return self.process.pid
    @property
    def returncode(self): return self.process.poll()

    def resize(self, rows, cols):
        if rows < 1 or cols < 1:
            raise ValueError('invalid terminal size')
        fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        os.killpg(os.getpgid(self.pid), signal.SIGWINCH)

    def terminate(self):
        try: os.killpg(os.getpgid(self.pid), signal.SIGTERM)
        except (OSError, ProcessLookupError): pass

    def kill(self):
        try: os.killpg(os.getpgid(self.pid), signal.SIGKILL)
        except (OSError, ProcessLookupError): pass

    def close(self):
        if self._closed: return
        self._closed = True
        self.control_channel.close()
        self.terminate()
        try: self.process.wait(timeout=self.config.grace_period)
        except subprocess.TimeoutExpired:
            self.kill(); self.process.wait()
        try: os.close(self.master_fd)
        except OSError: pass
