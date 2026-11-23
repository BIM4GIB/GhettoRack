"""What survives a restart (state.json)."""
from .common import *
from .util import *


class State:
    """What survives a restart: last module, autostart, IR bindings.
    Written atomically; a missing or broken file means defaults."""

    def __init__(self, path):
        self.path = path
        self.data = {"last": None, "autostart_s": 5, "hdmi_audio": False,
                     "ir": {p: {} for p in IR_PROFILES}, "routes": {}, "presets": {}, "net_never": []}
        try:
            with open(path) as f:
                d = json.load(f)
            if isinstance(d, dict):
                self.data["last"] = d.get("last") if d.get("last") in [m["id"] for m in MODULES] else None
                a = d.get("autostart_s", 5)
                self.data["autostart_s"] = a if isinstance(a, int) and 0 <= a <= 60 else 5
                self.data["hdmi_audio"] = d.get("hdmi_audio") is True
                nv = d.get("net_never")
                if isinstance(nv, list):
                    self.data["net_never"] = [n for n in nv if isinstance(n, str) and 0 < len(n) <= 64][:64]
                from .router import valid_route
                ids = [m["id"] for m in MODULES]
                for mid, rs in (d.get("routes") or {}).items():
                    if mid in ids and isinstance(rs, list):
                        self.data["routes"][mid] = [r for r in map(valid_route, rs[:ROUTES_MAX]) if r]
                for mid, ps in (d.get("presets") or {}).items():
                    if mid in ids and isinstance(ps, dict):
                        self.data["presets"][mid] = {
                            n: [r for r in map(valid_route, rs[:ROUTES_MAX]) if r]
                            for n, rs in list(ps.items())[:32] if isinstance(n, str) and isinstance(rs, list)}
                for prof, binds in (d.get("ir") or {}).items():
                    if prof in IR_PROFILES and isinstance(binds, dict):
                        self.data["ir"][prof] = {k: v for k, v in binds.items() if self.valid(k, v)}
        except (OSError, ValueError):
            pass

    @staticmethod
    def valid(key, v):
        action, _, press = key.partition("/")
        return (action in IR_ACTIONS and press in ("tap", "hold")
                and isinstance(v, int) and 0 <= v < 1 << 32)

    def save(self):
        if not self.path:
            return
        tmp = self.path + ".tmp"
        try:
            with open(tmp, "w") as f:
                json.dump(self.data, f, indent=1)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("saving %s: %s", self.path, e)

    def bind(self, profile, action, press, scan):
        binds = self.data["ir"][profile]
        for k in [k for k, v in binds.items() if v == scan and k.endswith("/" + press)]:
            del binds[k]                # one action per button and press, per profile
        binds[action + "/" + press] = scan
        self.save()

    def forget(self, profile, action, press):
        self.data["ir"][profile].pop(action + "/" + press, None)
        self.save()

    def action_for(self, view, scan, press):
        """The action a button does in a view; the "all" profile fills gaps"""
        for prof in (view, "all"):
            for k, v in self.data["ir"].get(prof, {}).items():
                if v == scan and k.endswith("/" + press):
                    return k.split("/")[0]
        return None
