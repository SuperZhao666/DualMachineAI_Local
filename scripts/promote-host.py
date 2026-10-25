"""Promote one manifest-verified Host to its stable firewall-bound launch path.

Only files are managed. This tool never launches/stops a process or changes
firewall, proxy, route, adapter or other network configuration.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile


class PromotionError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _within(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def _unique_json_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise PromotionError('Duplicate JSON key in validation manifest: ' + key)
        result[key] = value
    return result


def _host_name(name: str) -> bool:
    value = name.casefold()
    return value.endswith('.exe') and (
        value.startswith('vfhost') or value.startswith('visionforgehost'))


def verified_candidate(project: Path, manifest: Path) -> tuple[Path, str, int, str]:
    manifest = manifest.resolve(strict=True)
    if manifest.name != 'offline-validation.json' or not _within(manifest, project / 'audit'):
        raise PromotionError('Use an explicit same-project audit/offline-validation.json manifest.')
    try:
        document = json.loads(manifest.read_text(encoding='utf-8-sig'), object_pairs_hook=_unique_json_keys)
    except (ValueError, TypeError) as error:
        raise PromotionError('Invalid validation manifest: ' + str(error)) from error
    if not isinstance(document, dict):
        raise PromotionError('Validation manifest root must be an object.')
    status = document.get('status')
    if not isinstance(status, str) or status not in {'offline_passed', 'offline_passed_candidate_not_installed'}:
        raise PromotionError('Validation manifest does not record a passed offline candidate.')
    artifacts = document.get('artifacts')
    if not isinstance(artifacts, dict):
        raise PromotionError('Validation manifest must contain an artifacts object.')
    hosts = []
    for name, artifact in artifacts.items():
        if not isinstance(artifact, dict):
            continue
        value = artifact.get('path')
        if isinstance(value, str) and (_host_name(Path(value).name) or _host_name(name)):
            hosts.append(artifact)
    if len(hosts) != 1:
        raise PromotionError('Validation manifest must identify exactly one Host artifact; found ' + str(len(hosts)))
    artifact = hosts[0]
    raw_path = Path(artifact['path'])
    source = (raw_path if raw_path.is_absolute() else project / raw_path).resolve(strict=True)
    candidates = (project / 'dist' / 'candidates').resolve(strict=True)
    if not _within(candidates, project) or not _within(source, candidates) or not _host_name(source.name):
        raise PromotionError('Host candidate must be a same-project dist/candidates Host executable.')
    expected = artifact.get('sha256')
    size = artifact.get('bytes')
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', expected):
        raise PromotionError('Host artifact has no valid SHA-256.')
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        raise PromotionError('Host artifact must declare a positive byte count.')
    if not source.is_file() or source.stat().st_size != size or sha256(source) != expected.lower():
        raise PromotionError('Host candidate bytes/SHA-256 do not match the validation manifest.')
    return source, expected.lower(), size, sha256(manifest)


def windows_host_processes() -> list[dict]:
    """Read images, including renamed project EXEs, without WMI or process mutation."""
    if os.name != 'nt':
        raise PromotionError('Replacement requires the Windows Host process inventory.')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)

    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
            ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
            ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
            ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
            ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]

    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32FirstW.restype = wintypes.BOOL
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.restype = wintypes.BOOL
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
        wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
    if snapshot == ctypes.c_void_p(-1).value:
        raise PromotionError('Cannot enumerate Host processes: Win32 ' + str(ctypes.get_last_error()))
    result = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        available = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        if not available:
            raise PromotionError('Cannot read process inventory: Win32 ' + str(ctypes.get_last_error()))
        while available:
            item = {'pid':int(entry.th32ProcessID), 'name':entry.szExeFile, 'path':None}
            handle = kernel.OpenProcess(0x1000, False, entry.th32ProcessID)  # QUERY_LIMITED_INFORMATION
            if handle:
                try:
                    buffer = ctypes.create_unicode_buffer(32768)
                    size = wintypes.DWORD(len(buffer))
                    if kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                        item['path'] = buffer.value
                    else:
                        item['query_error'] = ctypes.get_last_error()
                finally:
                    kernel.CloseHandle(handle)
            else:
                # Unknown system processes are not evidence of a project Host.
                # An unreadable Host-named image is ambiguous and blocks replacement.
                if _host_name(entry.szExeFile):
                    item['query_error'] = ctypes.get_last_error()
            if item['path'] is not None or _host_name(entry.szExeFile):
                result.append(item)
            available = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise PromotionError('Incomplete Host process inventory: Win32 ' + str(ctypes.get_last_error()))
    finally:
        kernel.CloseHandle(snapshot)
    return result


def _ensure_no_running_host(project: Path, reader) -> None:
    blockers = []
    for process in reader():
        if process.get('pid') == os.getpid():
            continue  # The promotion interpreter itself may live in this project's .venv.
        value = process.get('path')
        if not value:
            blockers.append(process)
            continue
        image = Path(value).resolve()
        if _within(image, project):
            blockers.append(process)
    if blockers:
        raise PromotionError('Refusing replacement while a same-project or unidentifiable Host is running: '
            + json.dumps(blockers, ensure_ascii=False))


def windows_file_version(path: Path) -> str:
    if os.name != 'nt':
        raise PromotionError('Reading the old Host version requires Windows.')
    version = ctypes.WinDLL('version', use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [wintypes.LPCVOID, wintypes.LPCWSTR,
        ctypes.POINTER(wintypes.LPVOID), ctypes.POINTER(wintypes.UINT)]
    version.VerQueryValueW.restype = wintypes.BOOL
    ignored = wintypes.DWORD()
    size = version.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
    if not size:
        raise PromotionError('Cannot read old Host PE version; canonical file was not changed.')
    data = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, data):
        raise PromotionError('Cannot read old Host version data.')
    pointer = wintypes.LPVOID()
    length = wintypes.UINT()
    if not version.VerQueryValueW(data, '\\', ctypes.byref(pointer), ctypes.byref(length)) or length.value < 16:
        raise PromotionError('Old Host has no fixed file version.')
    words = ctypes.cast(pointer, ctypes.POINTER(wintypes.DWORD))
    if words[0] != 0xFEEF04BD:
        raise PromotionError('Old Host version signature is invalid.')
    numbers = [words[2] >> 16, words[2] & 65535, words[3] >> 16, words[3] & 65535]
    if numbers[-1] == 0:
        numbers.pop()
    return '.'.join(str(value) for value in numbers)


def _copy_exclusive(source: Path, destination: Path, expected: str) -> bool:
    """Create a backup once; an existing identical backup is reusable."""
    try:
        target = destination.open('xb')
    except FileExistsError:
        if destination.is_file() and not destination.is_symlink() and sha256(destination) == expected:
            return False
        raise PromotionError('Backup name conflict; refusing to overwrite: ' + str(destination))
    try:
        with target, source.open('rb') as original:
            shutil.copyfileobj(original, target, 1024 * 1024)
            target.flush()
            os.fsync(target.fileno())
        if sha256(destination) != expected:
            raise PromotionError('Old Host changed while its backup was being copied.')
    except BaseException:
        destination.unlink(missing_ok=True)  # Only the exclusive file created above.
        raise
    return True


def promote_host(manifest: Path, project: Path, *, process_reader=windows_host_processes,
        version_reader=windows_file_version, replace_file=os.replace) -> dict:
    """The CLI supplies its own fixed project root; injections are for isolated file fixtures."""
    project = project.resolve(strict=True)
    source, expected, size, manifest_hash = verified_candidate(project, manifest)
    dist = project / 'dist'
    canonical = dist / 'VFHost-Local.exe'
    if not _within(dist.resolve(strict=True), project) or canonical.is_symlink():
        raise PromotionError('Canonical Host path must stay inside this project dist directory.')
    old_hash = sha256(canonical) if canonical.is_file() else None
    result = {'status':'unchanged' if old_hash == expected else 'promoted',
        'canonical_path':str(canonical), 'candidate_path':str(source),
        'manifest_path':str(manifest.resolve()),
        'candidate_sha256':expected, 'candidate_bytes':size, 'manifest_sha256':manifest_hash,
        'previous_sha256':old_hash, 'backup_path':None, 'backup_created':False,
        'process_started':False, 'process_stopped':False, 'network_configuration_changed':False}
    if old_hash == expected:
        return result  # Strict zero-write no-op, including while that exact Host is running.
    if canonical.exists() and not canonical.is_file():
        raise PromotionError('Canonical Host path is not a regular file.')
    _ensure_no_running_host(project, process_reader)
    old_version = version_reader(canonical) if old_hash is not None else None
    if old_version is not None and not re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?', old_version):
        raise PromotionError('Invalid old Host file version; canonical file was not changed.')
    previous = dist / 'previous'
    if previous.exists() and not _within(previous.resolve(strict=True), project):
        raise PromotionError('Host backup directory must stay inside this project.')
    backup = previous / ('VFHost-Local-' + old_version + '-' + old_hash + '.exe') if old_hash else None
    lock_path = dist / '.promote-host.lock'
    try:
        lock = lock_path.open('x', encoding='ascii')
    except FileExistsError as error:
        raise PromotionError('Another promotion or an unresolved promotion lock exists: ' + str(lock_path)) from error
    staged = None
    try:
        with lock:
            lock.write(str(os.getpid()) + '\n')
            lock.flush()
            descriptor, stage_name = tempfile.mkstemp(prefix='.VFHost-promote-', suffix='.tmp', dir=dist)
            staged = Path(stage_name)
            with os.fdopen(descriptor, 'wb') as target, source.open('rb') as candidate:
                shutil.copyfileobj(candidate, target, 1024 * 1024)
                target.flush()
                os.fsync(target.fileno())
            if staged.stat().st_size != size or sha256(staged) != expected:
                raise PromotionError('Staged candidate verification failed; canonical Host was not changed.')
            if backup is not None:
                previous.mkdir(exist_ok=True)
                result['backup_created'] = _copy_exclusive(canonical, backup, old_hash)
                result['backup_path'] = str(backup)
            _ensure_no_running_host(project, process_reader)
            current_hash = sha256(canonical) if canonical.is_file() else None
            if current_hash != old_hash or canonical.is_symlink():
                raise PromotionError('Canonical Host changed during promotion; refusing replacement.')
            replace_file(staged, canonical)
            staged = None
            if sha256(canonical) != expected:
                raise PromotionError('Canonical verification failed; preserved backup: ' + str(backup))
        return result
    finally:
        if staged is not None:
            staged.unlink(missing_ok=True)
        lock_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True,
        help='Explicit same-project audit/.../offline-validation.json containing one verified Host artifact')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    try:
        result = promote_host(args.manifest, project)
    except (PromotionError, OSError) as error:
        print(json.dumps({'status':'refused', 'error':str(error)}, ensure_ascii=False, indent=2))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
