from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parents[1] / 'python'))
from control_api import ControlChannel, Decision, NetRequest, PathRequest
ControlChannel.from_fd(3).serve(lambda r: Decision.ALLOW
                                if isinstance(r, (NetRequest, PathRequest)) else None)
