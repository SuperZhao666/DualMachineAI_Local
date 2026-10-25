"""Scoped local Clash routing. Credentials stay inside Clash's configuration folder."""
from pathlib import Path
import copy, datetime, hashlib, http.client, json, re, shutil, subprocess, sys
import urllib.parse
import yaml
from file_transaction import commit_files

sys.stdout.reconfigure(encoding="utf-8")
HOME = Path.home() / "AppData/Roaming/io.github.clash-verge-rev.clash-verge-rev"
AUDIT = Path(__file__).resolve().parents[1] / "audit/live-20261018/network-routing.json"
GROUP = "战网与守望先锋 · 美国专用"
PROVIDER = "overwatch-us-subscription"
GAME_PROFILE = "Rj7nJa5MWD4h"
RESIDENTIAL_PROFILE = "RX4QkFvaqDjW"
EXE = Path(r"F:\Program Files\Clash Verge\verge-mihomo.exe")

def read(path):
    return yaml.safe_load(path.read_text(encoding="utf-8"))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def write_yaml(path, value):
    path.write_text(yaml.safe_dump(value, allow_unicode=True, sort_keys=False), encoding="utf-8")

class PipeSocket:
    def __init__(self, file): self.file = file
    def makefile(self, *args, **kwargs): return self.file

def api(method, endpoint, value=None):
    config = read(HOME / "clash-verge.yaml")
    payload = b"" if value is None else json.dumps(value, ensure_ascii=False).encode()
    with open(config["external-controller-pipe"], "r+b", buffering=0) as pipe:
        headers = (f"{method} {endpoint} HTTP/1.1\r\nHost: localhost\r\n"
                   f"Authorization: Bearer {config.get('secret', '')}\r\n"
                   f"Content-Type: application/json\r\nContent-Length: {len(payload)}\r\n"
                   "Connection: close\r\n\r\n")
        pipe.write(headers.encode() + payload)
        response = http.client.HTTPResponse(PipeSocket(pipe))
        response.begin()
        data = response.read()
        if response.status not in (200, 204):
            raise RuntimeError(f"Local controller {method} {endpoint}: HTTP {response.status}")
        return json.loads(data) if data else None

def saved():
    return json.loads(AUDIT.read_text(encoding="utf-8"))

def save(state):
    AUDIT.parent.mkdir(parents=True, exist_ok=True)
    AUDIT.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")

def rules_for(default):
    # Full paths avoid sending unrelated applications named Agent.exe to this subscription.
    executables = [
        r"E:\40_Applications\Networking\ZhanWang\Battle.net\Battle.net.exe",
        r"E:\40_Applications\Networking\ZhanWang\Battle.net\Battle.net Launcher.exe",
        r"F:\Overwatch\_retail_\Overwatch.exe",
        r"E:\30_Games\Overwatch\_retail_\Overwatch.exe",
    ]
    rules = [f"PROCESS-PATH-REGEX,(?i)^{re.escape(path)}$,{GROUP}" for path in executables]
    rules.append(r"PROCESS-PATH-REGEX,(?i)^C:\\ProgramData\\Battle\.net\\Agent\\Agent\.[^\\]+\\Agent\.exe$," + GROUP)
    rules.append("MATCH," + default)
    return rules

def prepare():
    profiles = read(HOME / "profiles.yaml")
    assert profiles["current"] == RESIDENTIAL_PROFILE, "Active subscription changed; inspect before proceeding"
    current = next(x for x in profiles["items"] if x["uid"] == RESIDENTIAL_PROFILE)
    secondary = next(x for x in profiles["items"] if x["uid"] == GAME_PROFILE)
    runtime = read(HOME / "clash-verge.yaml")
    live = api("GET", "/configs")
    proxies = api("GET", "/proxies")["proxies"]
    assert live["mode"] == "global", "Plan is designed to preserve current global-mode routing"
    default = proxies["GLOBAL"]["now"]
    assert default in {g["name"] for g in runtime["proxy-groups"]}
    source = read(HOME / "profiles" / secondary["file"])
    us = [x for x in source["proxies"] if "美国" in x["name"] and x.get("udp")]
    assert us, "No UDP-capable US nodes in the requested subscription"
    rules = rules_for(default)
    provider = {"type": "file", "path": "./profiles/" + secondary["file"],
                "filter": "美国", "health-check": {"enable": True, "url": "https://www.gstatic.com/generate_204", "interval": 300, "lazy": True}}
    group = {"name": GROUP, "type": "select", "use": [PROVIDER]}
    candidate = copy.deepcopy(runtime)
    candidate.setdefault("proxy-providers", {})[PROVIDER] = provider
    candidate["proxy-groups"].append(group)
    candidate["rules"] = rules
    candidate["mode"] = "rule"
    candidate["find-process-mode"] = "always"
    # Every old proxy, group, DNS/TUN option and listener is unchanged.
    changed = {k for k in set(candidate) | set(runtime) if candidate.get(k) != runtime.get(k)}
    assert changed <= {"proxy-providers", "proxy-groups", "rules", "mode", "find-process-mode"}
    assert candidate["proxies"] == runtime["proxies"]
    assert candidate["proxy-groups"][:-1] == runtime["proxy-groups"]
    backup = HOME / "backups" / ("game-routing-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    backup.mkdir(parents=True, exist_ok=False)
    script_rel = "profiles/" + current["option"]["script"] + ".js"
    assert len((HOME / script_rel).read_text(encoding="utf-8")) < 160, "Existing script needs a manual merge"
    files = ["config.yaml", "clash-verge.yaml", "clash-verge-check.yaml", "profiles.yaml", script_rel]
    hashes = {}
    for relative in files:
        target = backup / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HOME / relative, target)
        hashes[relative] = digest(HOME / relative)
    script = "// Game-only routing; retain the residential default for every other process.\nfunction main(config, profileName) {\n"
    script += "  if (!(config['proxy-groups'] || []).some(g => g.name === " + json.dumps(default, ensure_ascii=False) + ")) throw new Error('Residential fallback group is missing');\n"
    script += "  config['proxy-providers'] = config['proxy-providers'] || {};\n"
    script += "  config['proxy-providers'][" + json.dumps(PROVIDER) + "] = " + json.dumps(provider, ensure_ascii=False) + ";\n"
    script += "  config['proxy-groups'] = config['proxy-groups'].filter(g => g.name !== " + json.dumps(GROUP, ensure_ascii=False) + ");\n"
    script += "  config['proxy-groups'].push(" + json.dumps(group, ensure_ascii=False) + ");\n"
    script += "  config.rules = " + json.dumps(rules, ensure_ascii=False) + ";\n"
    script += "  config['find-process-mode'] = 'always';\n  return config;\n}\n"
    (backup / "candidate-script.js").write_text(script, encoding="utf-8")
    write_yaml(backup / "candidate.yaml", candidate)
    tested = subprocess.run([str(EXE), "-t", "-d", str(HOME), "-f", str(backup / "candidate.yaml")], capture_output=True, timeout=45)
    if tested.returncode:
        # Log stays beside the private configuration, never in the project or tool output.
        (backup / "validation.log").write_bytes(tested.stdout + tested.stderr)
        raise RuntimeError("Candidate validation failed; private log saved in backup folder")
    state = {"prepared_at": datetime.datetime.now().isoformat(), "backup": str(backup),
             "script_relative": script_rel, "original_hashes": hashes, "current_profile": profiles["current"],
             "default_group": default, "game_group": GROUP, "game_nodes": len(us), "candidate_validated": True,
             "rules": rules, "status": "prepared"}
    save(state)
    print(json.dumps({k:v for k,v in state.items() if k != "original_hashes"}, ensure_ascii=False, indent=2))

def apply():
    state = saved(); assert state["status"] == "prepared"
    backup = Path(state["backup"])
    for relative, expected in state["original_hashes"].items():
        assert digest(HOME / relative) == expected, f"Configuration changed since preparation: {relative}"
    assert api("GET", "/configs")["mode"] == "global"
    assert api("GET", "/proxies")["proxies"]["GLOBAL"]["now"] == state["default_group"]
    config = read(HOME / "config.yaml"); config["mode"] = "rule"
    reload_attempted = False
    def reload_and_verify():
        nonlocal reload_attempted
        reload_attempted = True
        api("PUT", "/configs", {"path": str(HOME / "clash-verge.yaml")})
        assert api("GET", "/configs")["mode"] == "rule"
        actual = api("GET", "/rules")["rules"]
        assert actual[-1]["type"] == "Match" and actual[-1]["proxy"] == state["default_group"]
        state["status"] = "applied"
        state["applied_at"] = datetime.datetime.now().isoformat()
        save(state)
    def reload_restored():
        if reload_attempted:
            api("PUT", "/configs", {"path": str(HOME / "clash-verge.yaml")})
    commit_files([
        (HOME / state["script_relative"], backup / "candidate-script.js"),
        (HOME / "config.yaml", yaml.safe_dump(config, allow_unicode=True, sort_keys=False).encode('utf-8')),
        (HOME / "clash-verge.yaml", backup / "candidate.yaml"),
        (HOME / "clash-verge-check.yaml", backup / "candidate.yaml"),
    ], after_commit=reload_and_verify, after_rollback=reload_restored, callback_paths=[AUDIT])
    print("Applied scoped game routing; original residential default preserved; no existing connections closed.")

def verify():
    state = saved()
    profiles = read(HOME / "profiles.yaml")
    runtime = api("GET", "/configs")
    proxy = api("GET", "/proxies")["proxies"]
    actual = api("GET", "/rules")["rules"]
    connections = []
    for item in api("GET", "/connections").get("connections", []):
        m = item["metadata"]; proc = m.get("process", ""); host = m.get("host", "")
        if any(x in (proc + " " + host).lower() for x in ("codex", "chatgpt", "openai", "battle", "blizzard", "overwatch")):
            connections.append({"process": proc, "process_path": m.get("processPath", ""), "host": host,
                                "network": m.get("network"), "rule": item.get("rule"), "chains": item.get("chains")})
    evidence = {"at": datetime.datetime.now().isoformat(), "current_profile": profiles["current"], "mode": runtime["mode"],
                "process_mode": runtime.get("find-process-mode"), "game_group_type": proxy.get(GROUP, {}).get("type"),
                "game_node": proxy.get(GROUP, {}).get("now"), "rules": actual, "connections": connections}
    state["verification"] = evidence; save(state)
    print(json.dumps(evidence, ensure_ascii=False, indent=2))

if __name__ == "__main__":
    {"prepare": prepare, "apply": apply, "verify": verify}[sys.argv[1]]()
