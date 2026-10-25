"""Repair Ninja MSVC dependencies using the real /c output channel.

Default mode probes one generated source and repairs this build tree.
--verify is read-only and checks object dependencies after the caller's build.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = getattr(subprocess, 'CREATE_NO_WINDOW', 0) | getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)


class DependencyError(RuntimeError):
    pass


def within(path, directory):
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def validate_build_directory(requested):
    project = ROOT.resolve(strict=True)
    base = (project / 'build').resolve(strict=True)
    build = (requested if requested.is_absolute() else project / requested).resolve(strict=True)
    if not within(base, project) or build == base or not within(build, base):
        raise DependencyError('Build directory must be a generated child of this project/build.')
    cache_path = build / 'CMakeCache.txt'
    rules = build / 'CMakeFiles/rules.ninja'
    for path in (cache_path, rules, build / 'build.ninja'):
        if not path.is_file() or not within(path.resolve(strict=True), build):
            raise DependencyError('Not a self-contained generated CMake/Ninja build: ' + str(build))
    cache = {}
    for line in cache_path.read_text(encoding='utf-8').splitlines():
        match = re.match(r'^([^#/:][^:]*):[^=]*=(.*)$', line)
        if match:
            cache[match.group(1)] = match.group(2)
    if cache.get('CMAKE_GENERATOR') != 'Ninja':
        raise DependencyError('Only the Ninja generator is supported.')
    home = Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve(strict=True)
    production = home == (project / 'dual_machine_runtime').resolve(strict=True)
    fixture = cache.get('CMAKE_PROJECT_NAME') == 'VisionForgeDependencyFixture' and within(home, project / 'audit')
    if not production and not fixture:
        raise DependencyError('CMake source identity is not this runtime or its isolated audit fixture.')
    compiler = Path(cache.get('CMAKE_CXX_COMPILER', ''))
    if compiler.name.casefold() != 'cl.exe' or not compiler.is_file():
        raise DependencyError('This generated tree does not use an available MSVC cl.exe.')
    ninja = Path(cache.get('CMAKE_MAKE_PROGRAM') or shutil.which('ninja') or '')
    if not ninja.is_file():
        raise DependencyError('Ninja executable not found.')
    return build, rules, compiler, ninja, production


def run_ninja(build, ninja, *arguments):
    result = subprocess.run([str(ninja), '-C', str(build), *arguments], capture_output=True, creationflags=FLAGS)
    if result.returncode:
        raise DependencyError('Ninja dependency command failed: ' + result.stderr.decode('utf-8', errors='replace'))
    return result.stdout.decode('utf-8', errors='replace')


def object_key(path):
    return path.replace('\\', '/').casefold()


STANDARD_HEADERS = frozenset(('algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex '
    'concepts condition_variable coroutine deque exception execution expected filesystem format forward_list fstream '
    'functional future initializer_list iomanip ios iosfwd iostream istream iterator latch limits list locale map memory '
    'memory_resource mutex new numbers numeric optional ostream queue random ranges ratio regex scoped_allocator semaphore '
    'set shared_mutex source_location span sstream stack stdexcept stop_token streambuf string string_view strstream '
    'syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray '
    'variant vector version cassert cctype cerrno cfenv cfloat cinttypes ciso646 climits clocale cmath csetjmp csignal '
    'cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype '
    'assert.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h signal.h '
    'stdarg.h stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h time.h uchar.h wchar.h wctype.h').split())
WINDOWS_HEADERS = frozenset(('windows.h windowsx.h winuser.h winbase.h winnt.h commctrl.h shellapi.h shlobj.h '
    'shlwapi.h iphlpapi.h winsock2.h ws2tcpip.h tlhelp32.h psapi.h wincrypt.h dwmapi.h d3d11.h dxgi.h dxgi1_2.h '
    'wrl/client.h winerror.h sdkddkver.h objbase.h oleauto.h ole2.h guiddef.h initguid.h wingdi.h processthreadsapi.h '
    'profileapi.h synchapi.h handleapi.h fileapi.h memoryapi.h minwindef.h wincon.h malloc.h io.h conio.h '
    'fcntl.h process.h direct.h').split())


def compilation_sources(build, ninja):
    """Map actual Ninja compile outputs to TUs; unknown/ambiguous sources stay unknown."""
    try:
        commands = json.loads(run_ninja(build, ninja, '-t', 'compdb'))
    except (DependencyError, ValueError, TypeError):
        return {}
    if not isinstance(commands, list):
        return {}
    mapping = {}
    for entry in commands:
        if not isinstance(entry, dict) or not all(isinstance(entry.get(key), str) for key in ('output', 'file', 'directory', 'command')):
            continue
        if not entry['output'].casefold().endswith('.obj') or Path(entry['file']).suffix.casefold() not in {'.cpp', '.cc', '.cxx', '.c'}:
            continue
        directory = Path(entry['directory'])
        output = Path(entry['output'])
        output = (output if output.is_absolute() else directory / output).resolve()
        source = Path(entry['file'])
        source = (source if source.is_absolute() else directory / source).resolve()
        if not within(output, build):
            continue
        key = object_key(output.relative_to(build).as_posix())
        if key in mapping:
            mapping[key] = None
        else:
            include_dirs = []
            for quoted, plain in re.findall(r'(?:^|\s)[/-]I\s*(?:"([^"]+)"|([^\s]+))', entry['command']):
                value = Path(quoted or plain)
                include_dirs.append((value if value.is_absolute() else directory / value).resolve())
            mapping[key] = {'source':source, 'command':entry['command'], 'include_dirs':include_dirs}
    return mapping


def classify_zero_dependency(entry):
    if not entry:
        return {'allowed':False, 'reason':'unknown_compile_source'}
    source, command = entry['source'], entry['command']
    if not within(source, ROOT.resolve()) or not source.is_file():
        return {'allowed':False, 'reason':'unknown_or_external_compile_source'}
    if re.search(r'(?:^|\s)[/-](?:FI|Yu|Yc)', command, re.IGNORECASE):
        return {'allowed':False, 'reason':'forced_include_or_pch_requires_dependencies'}
    try:
        text = source.read_text(encoding='utf-8-sig')
    except (OSError, UnicodeError):
        return {'allowed':False, 'reason':'unreadable_compile_source'}
    text = re.sub(r'\\\r?\n', '', text)
    if (re.search(r'(?m)^\s*(?:%:|\?\?=)', text)
            or re.search(r'(?m)^\s*#[^\r\n]*/\*', text)
            or re.search(r'\binclude_alias\s*\(', text)):
        return {'allowed':False, 'reason':'unclassified_preprocessor_header_routing'}
    if re.search(r'(?m)^\s*(?:export\s+)?import\b', text):
        return {'allowed':False, 'reason':'module_or_header_import_not_classified'}
    headers = []
    for directive, operand in re.findall(r'(?m)^\s*#\s*(include_next|include|import)\b([^\r\n]*)', text):
        match = re.match(r'\s*<([^>]+)>\s*(?://.*)?$', operand)
        if directive != 'include' or not match:
            return {'allowed':False, 'reason':'quoted_macro_or_unknown_include', 'source':str(source)}
        name = match.group(1)
        if name not in STANDARD_HEADERS and name.casefold() not in WINDOWS_HEADERS:
            return {'allowed':False, 'reason':'project_or_unclassified_angle_include', 'source':str(source), 'header':name}
        if any(within(directory, ROOT.resolve()) and (directory / name).is_file()
               for directory in entry.get('include_dirs', [])):
            return {'allowed':False, 'reason':'system_header_name_shadowed_by_project_include', 'source':str(source), 'header':name}
        headers.append(name)
    return {'allowed':True, 'reason':'only_recognized_system_standard_headers', 'source':str(source), 'headers':headers}


def dependency_state(build, ninja, production):
    output = run_ninja(build, ninja, '-t', 'deps')
    entries = {}
    current = None
    for line in output.splitlines():
        match = re.match(r'^(.*): #deps (\d+), deps mtime \d+ \(([^)]+)\)$', line)
        if match:
            current = object_key(match.group(1))
            entries[current] = {'count':int(match.group(2)), 'status':match.group(3), 'headers':[]}
        elif line.startswith('    ') and current is not None:
            entries[current]['headers'].append(object_key(line.strip()))
        elif not line:
            current = None
    objects = []
    for path in (build / 'CMakeFiles').rglob('*.obj'):
        relative = path.relative_to(build)
        if len(relative.parts) < 3 or not relative.parts[1].endswith('.dir'):
            continue
        if not within(path.resolve(strict=True), build):
            raise DependencyError('A generated object resolves outside the verified build tree.')
        objects.append(relative.as_posix())
    issues = []
    legitimate_zero = []
    source_map = None
    counts = {}
    for name in sorted(objects):
        entry = entries.get(object_key(name))
        counts[name] = entry['count'] if entry else None
        if entry is None:
            issues.append({'object':name, 'reason':'missing_dependency_record'})
        elif entry['count'] == 0:
            if source_map is None:
                source_map = compilation_sources(build, ninja)
            classification = classify_zero_dependency(source_map.get(object_key(name)))
            if classification['allowed'] and entry['status'] == 'VALID':
                legitimate_zero.append({'object':name, **classification})
            else:
                issues.append({'object':name, 'reason':'zero_header_dependencies', 'classification':classification})
        elif entry['status'] != 'VALID':
            issues.append({'object':name, 'reason':'stale_dependency_record'})
    expected = {
        'host_display_catalog.cpp.obj':('host_display_catalog.hpp', 'protocol.hpp'),
        'host_runtime_service.cpp.obj':('host_mobile_stream_policy.hpp', 'protocol.hpp'),
        'streamer_desktop_app.cpp.obj':('host_display_catalog.hpp', 'protocol.hpp'),
        'desktop_video_agent.cpp.obj':('desktop_video_agent.hpp', 'host_encoder_policy.hpp'),
    }
    host_objects = [name for name in objects if '/visionforgehost.dir/' in object_key(name)]
    if production and host_objects:
        for filename, headers in expected.items():
            matches = [name for name in host_objects if name.casefold().endswith('/'+filename)]
            if not matches:
                continue  # A single-target build need not compile unrelated Host objects.
            if len(matches) != 1:
                issues.append({'object':filename, 'reason':'ambiguous_critical_host_object'})
                continue
            entry = entries.get(object_key(matches[0]), {'headers':[]})
            for header in headers:
                if not any(path.endswith('/'+header) for path in entry['headers']):
                    issues.append({'object':matches[0], 'reason':'critical_header_not_tracked', 'header':header})
    return {'compiled_objects':len(objects), 'dependency_counts':counts, 'legitimate_zero':legitimate_zero, 'issues':issues}


def compile_prefix(build, compiler):
    scratch = build / '.vfdual-dependency-probe'
    scratch.mkdir(exist_ok=True)
    if not within(scratch.resolve(strict=True), build):
        raise DependencyError('Dependency probe directory resolves outside the build tree.')
    header = scratch / 'vfdual_dependency_probe.hpp'
    source = scratch / 'vfdual_dependency_probe.cpp'
    obj = scratch / 'vfdual_dependency_probe.obj'
    header.write_text('#define VFDUAL_DEPENDENCY_PROBE 640\n', encoding='ascii')
    source.write_text('#include "vfdual_dependency_probe.hpp"\nint vfdual_dependency_probe = VFDUAL_DEPENDENCY_PROBE;\n', encoding='ascii')
    # /EP emits a different encoding here. Match the real Ninja /c rule.
    result = subprocess.run([str(compiler), '/nologo', '/utf-8', '/showIncludes', '/c', str(source), '/Fo'+str(obj)],
                            capture_output=True, creationflags=FLAGS)
    if result.returncode:
        raise DependencyError('MSVC compile-prefix probe failed: ' + result.stderr.decode('utf-8', errors='replace'))
    for line in (result.stdout + result.stderr).splitlines():
        if header.name.encode('ascii') not in line:
            continue
        path = re.search(rb'[A-Za-z]:[\\/]', line)
        if path:
            prefix = line[:path.start()].rstrip(b' \t')
            if prefix and not any(value in prefix for value in (b'\n', b'\r', b'$')):
                return prefix
    raise DependencyError('Real /c output did not expose an MSVC include prefix.')


def repair(requested, verify=False):
    build, rules, compiler, ninja, production = validate_build_directory(requested)
    state = dependency_state(build, ninja, production)
    if verify:
        if not state['compiled_objects']:
            state['issues'].append({'reason':'no_compiled_objects_to_verify'})
        return {'mode':'verify', 'build_dir':str(build), 'passed':not state['issues'], **state}
    prefix = compile_prefix(build, compiler)
    data = rules.read_bytes()
    marker = build / '.native-include-prefix'
    previous = marker.read_bytes() if marker.is_file() else None
    match = re.findall(rb'(?m)^msvc_deps_prefix = ([^\r\n]*)', data)
    if len(match) != 1:
        raise DependencyError('Expected one generated MSVC dependency prefix.')
    actual_channel_changed = previous != prefix
    rule_needs_rewrite = match[0].rstrip(b' \t') != prefix
    # CMake regeneration may overwrite only the generated rule. Healthy
    # existing dependency records need no clean when the actual channel stayed
    # the same; repair that rule before the next build instead.
    clean_required = actual_channel_changed or bool(state['issues'])
    if clean_required:
        # Only the absolute, identity-checked generated build tree is cleaned.
        # Ninja removes generated outputs; no recursive filesystem delete.
        run_ninja(build, ninja, '-t', 'clean')
    updated, count = re.subn(rb'(?m)^msvc_deps_prefix = [^\r\n]*',
        lambda _:b'msvc_deps_prefix = '+prefix, data)
    if count != 1:
        raise DependencyError('Generated prefix replacement failed.')
    if updated != data:
        rules.write_bytes(updated)
    if previous != prefix:
        marker.write_bytes(prefix)
    return {'mode':'repair', 'build_dir':str(build), 'prefix_hex':prefix.hex(),
        'probe_channel':'cl /c /showIncludes', 'prefix_changed':actual_channel_changed,
        'actual_channel_changed':actual_channel_changed, 'rule_needs_rewrite':rule_needs_rewrite,
        'generated_clean':clean_required, 'previous_dependency_issues':state['issues'],
        'legitimate_zero':state['legitimate_zero'],
        'verify_after_build':'python scripts/repair-native-dependencies.py --build-dir "'+str(build)+'" --verify'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build/host'))
    parser.add_argument('--verify', action='store_true', help='Read-only post-build header dependency checks')
    args = parser.parse_args()
    try:
        result = repair(args.build_dir, args.verify)
    except (DependencyError, OSError, ValueError) as error:
        print(json.dumps({'passed':False, 'error':str(error)}, ensure_ascii=True, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0 if result.get('passed', True) else 1


if __name__ == '__main__':
    raise SystemExit(main())
