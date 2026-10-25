"""Refresh the game-only extension after replacing the residential subscription.

Private configuration and rollback copies remain in Clash's own directory.
The project audit contains routing names, paths and hashes, never credentials.
"""
import copy
import datetime
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.parse
from file_transaction import commit_files

spec = importlib.util.spec_from_file_location(
    'routing', Path(__file__).with_name('configure-game-routing.py'))
routing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routing)
HOME = routing.HOME
AUDIT = Path(__file__).resolve().parents[1] / 'audit/research-tracking-20261019/network-routing.json'


def save(state):
    AUDIT.parent.mkdir(parents=True, exist_ok=True)
    AUDIT.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')


def prepare():
    profiles = routing.read(HOME / 'profiles.yaml')
    current = next(p for p in profiles['items'] if p['uid'] == profiles['current'])
    assert current.get('name') == 'RESIP纯净住宅IP'
    game = next(p for p in profiles['items'] if p['uid'] == routing.GAME_PROFILE)
    runtime = routing.read(HOME / 'clash-verge.yaml')
    live = routing.api('GET', '/configs')
    proxies = routing.api('GET', '/proxies')['proxies']
    assert live['mode'] == 'rule' and runtime['mode'] == 'rule'
    default = 'Proxy'
    residential_nodes = {p['name'] for p in runtime['proxies']}
    assert proxies[default]['now'] in residential_nodes
    source = routing.read(HOME / 'profiles' / game['file'])
    us_nodes = [p['name'] for p in source['proxies'] if '美国' in p['name'] and p.get('udp')]
    assert us_nodes
    group = {'name': routing.GROUP, 'type': 'select', 'use': [routing.PROVIDER]}
    provider = {'type': 'file', 'path': './profiles/' + game['file'], 'filter': '美国',
                'health-check': {'enable': True, 'url': 'https://www.gstatic.com/generate_204',
                                 'interval': 300, 'lazy': True}}
    # Local discovery, ADB and VF streaming must remain local. All other
    # non-game traffic resolves to the existing residential group.
    local_rules = [r for r in runtime['rules'] if r.startswith(('IP-CIDR,', 'IP-CIDR6,'))
                   and ',DIRECT' in r]
    rules = local_rules + routing.rules_for(default)
    candidate = copy.deepcopy(runtime)
    candidate.setdefault('proxy-providers', {})[routing.PROVIDER] = provider
    candidate['proxy-groups'] = [g for g in candidate['proxy-groups'] if g['name'] != routing.GROUP]
    candidate['proxy-groups'].append(group)
    candidate['rules'] = rules
    candidate['find-process-mode'] = 'always'
    changed = {k for k in set(candidate) | set(runtime) if candidate.get(k) != runtime.get(k)}
    assert changed <= {'proxy-providers', 'proxy-groups', 'rules', 'find-process-mode'}
    assert candidate['proxies'] == runtime['proxies']
    script_rel = 'profiles/' + current['option']['script'] + '.js'
    old_script = (HOME / script_rel).read_text(encoding='utf-8')
    assert len(old_script) < 160 or old_script.startswith('// Game-only routing;')
    backup = HOME / 'backups' / ('game-routing-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    backup.mkdir(parents=True, exist_ok=False)
    files = ['profiles.yaml', 'config.yaml', 'clash-verge.yaml', 'clash-verge-check.yaml', script_rel]
    hashes = {}
    for rel in files:
        destination = backup / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HOME / rel, destination)
        hashes[rel] = routing.digest(HOME / rel)
    j = lambda v: json.dumps(v, ensure_ascii=False)
    script = '// Game-only routing; retain the residential default for every other process.\n'
    script += 'function main(config, profileName) {\n'
    script += f'  if (!(config["proxy-groups"] || []).some(g => g.name === {j(default)})) throw new Error("Residential group missing");\n'
    script += '  config["proxy-providers"] = config["proxy-providers"] || {};\n'
    script += f'  config["proxy-providers"][{j(routing.PROVIDER)}] = {j(provider)};\n'
    script += f'  config["proxy-groups"] = config["proxy-groups"].filter(g => g.name !== {j(routing.GROUP)});\n'
    script += f'  config["proxy-groups"].push({j(group)});\n'
    script += f'  config.rules = {j(rules)};\n'
    script += '  config["find-process-mode"] = "always";\n  return config;\n}\n'
    (backup / 'candidate-script.js').write_text(script, encoding='utf-8')
    routing.write_yaml(backup / 'candidate.yaml', candidate)
    tested = subprocess.run([str(routing.EXE), '-t', '-d', str(HOME), '-f', str(backup / 'candidate.yaml')],
                            capture_output=True, timeout=45)
    (backup / 'validation.log').write_bytes(tested.stdout + tested.stderr)
    assert tested.returncode == 0, 'Validation failed; private log is in Clash backup directory'
    state = {'status': 'prepared', 'prepared_at': datetime.datetime.now().isoformat(),
             'backup': str(backup), 'original_hashes': hashes, 'current_profile': current['uid'],
             'script_relative': script_rel, 'default_group': default,
             'residential_selection': proxies[default]['now'], 'game_group': routing.GROUP,
             'game_profile': game['uid'], 'us_nodes': us_nodes, 'rules': rules,
             'candidate_validated': True}
    save(state)
    print(json.dumps({k: state[k] for k in ('status', 'current_profile', 'default_group', 'game_profile', 'rules')}, ensure_ascii=False))


def apply():
    state = json.loads(AUDIT.read_text(encoding='utf-8'))
    assert state['status'] == 'prepared'
    for rel, digest in state['original_hashes'].items():
        assert routing.digest(HOME / rel) == digest, 'Configuration changed since preparation'
    assert routing.api('GET', '/proxies')['proxies']['Proxy']['now'] == state['residential_selection']
    backup = Path(state['backup'])
    reload_attempted = False
    def reload_and_verify():
        nonlocal reload_attempted
        reload_attempted = True
        routing.api('PUT', '/configs', {'path': str(HOME / 'clash-verge.yaml')})
        assert routing.api('GET', '/configs')['mode'] == 'rule'
        proxies = routing.api('GET', '/proxies')['proxies']
        assert proxies['Proxy']['now'] == state['residential_selection']
        assert proxies[routing.GROUP]['now'] in state['us_nodes']
        actual = routing.api('GET', '/rules')['rules']
        assert actual[-1]['type'] == 'Match' and actual[-1]['proxy'] == 'Proxy'
        assert sum(r.get('proxy') == routing.GROUP for r in actual) == 5
        state['status'] = 'applied'
        state['applied_at'] = datetime.datetime.now().isoformat()
        state['game_node'] = proxies[routing.GROUP]['now']
        save(state)
    def reload_restored():
        if reload_attempted:
            routing.api('PUT', '/configs', {'path': str(HOME / 'clash-verge.yaml')})
    commit_files([(HOME / rel, backup / source) for source, rel in
                  [('candidate-script.js', state['script_relative']),
                   ('candidate.yaml', 'clash-verge.yaml'), ('candidate.yaml', 'clash-verge-check.yaml')]],
                 after_commit=reload_and_verify, after_rollback=reload_restored, callback_paths=[AUDIT])
    print(json.dumps({'status': state['status'], 'game_node': state['game_node'],
                      'residential_unchanged': True, 'existing_connections_closed': 0}, ensure_ascii=False))


def verify():
    state = json.loads(AUDIT.read_text(encoding='utf-8'))
    proxies = routing.api('GET', '/proxies')['proxies']
    group = proxies[routing.GROUP]
    delay = routing.api('GET', '/proxies/' + urllib.parse.quote(routing.GROUP, safe='') +
                        '/delay?timeout=8000&url=https%3A%2F%2Fwww.gstatic.com%2Fgenerate_204')
    connections = []
    for item in routing.api('GET', '/connections').get('connections', []):
        meta = item['metadata']
        proc = meta.get('process', '')
        if any(s in (proc + ' ' + meta.get('host', '')).lower() for s in
               ('codex', 'chatgpt', 'openai', 'battle', 'blizzard', 'overwatch')):
            connections.append({'process': proc, 'path': meta.get('processPath'),
                                'network': meta.get('network'), 'rule': item.get('rule'),
                                'chains': item.get('chains')})
    verification = {'at': datetime.datetime.now().isoformat(),
                    'mode': routing.api('GET', '/configs')['mode'],
                    'current_profile': routing.read(HOME / 'profiles.yaml')['current'],
                    'game_node': group['now'], 'game_probe': delay,
                    'residential_selection': proxies['Proxy']['now'],
                    'connections': connections}
    state['verification'] = verification
    save(state)
    print(json.dumps(verification, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    {'prepare': prepare, 'apply': apply, 'verify': verify}[sys.argv[1]]()
