"""Recoverable multi-file commit. Independent replacements are not atomic as a group.

All staging and backup files live beside their destinations. Successful commits
and recoverable failures remove staging; failed rollback retains it for recovery.
No process is stopped and unchanged destination bytes are never replaced.
"""
from pathlib import Path
import hashlib
import os
import shutil
import tempfile


class FileRollbackError(RuntimeError):
    def __init__(self, original, failures, backups):
        self.original = original
        self.failures = failures
        self.backups = backups
        super().__init__('File commit failed; rollback incomplete for ' +
                         ', '.join(str(p) for p, _ in failures) +
                         '; recovery backups: ' + ', '.join(map(str, backups)))


def _digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def commit_files(changes, *, after_commit=None, after_rollback=None,
                 copy_file=None, replace_file=None, callback_paths=()):
    """Commit ordered (target, source Path or bytes) entries, with rollback.

    Callbacks run inside the transaction boundary. A restored configuration is
    reloaded only after every modified file has been restored successfully.
    Exceptions retain the original cause; incomplete rollback exposes backups.
    """
    copy_file = copy_file or shutil.copy2
    replace_file = replace_file or os.replace
    entries, directories, started, watched = [], [], [], []
    keep_backups = False
    seen = set()
    try:
        # Complete and verify every candidate and old snapshot before mutation.
        for target, source in changes:
            target = Path(target).resolve()
            if target in seen:
                raise ValueError('duplicate transaction destination: ' + str(target))
            seen.add(target)
            data = source if isinstance(source, bytes) else Path(source).read_bytes()
            if target.is_file() and target.read_bytes() == data:
                continue
            if target.exists() and not target.is_file():
                raise ValueError('transaction destination is not a file: ' + str(target))
            target.parent.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='.vf-transaction-', dir=target.parent))
            directories.append(directory)
            stage, backup = directory / 'candidate', directory / 'previous'
            if isinstance(source, bytes):
                stage.write_bytes(data)
            else:
                copy_file(Path(source), stage)
            if _digest(stage) != hashlib.sha256(data).digest():
                raise OSError('staged file hash mismatch: ' + str(target))
            existed = target.is_file()
            if existed:
                copy_file(target, backup)
                if _digest(backup) != _digest(target):
                    raise OSError('backup file hash mismatch: ' + str(target))
            entries.append((target, stage, backup, existed))
        # Callback-owned metadata is snapshotted too, including absent state.
        for value in callback_paths:
            target = Path(value).resolve()
            if target in seen:
                raise ValueError('duplicate callback destination: ' + str(target))
            seen.add(target)
            target.parent.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='.vf-transaction-', dir=target.parent))
            directories.append(directory)
            backup = directory / 'previous'
            existed = target.is_file()
            if existed:
                copy_file(target, backup)
                if _digest(backup) != _digest(target):
                    raise OSError('callback backup hash mismatch: ' + str(target))
            watched.append((target, None, backup, existed))
        try:
            for entry in entries:
                # Include the attempted destination: injected/custom writes can
                # fail after modifying it, unlike a normal atomic os.replace.
                started.append(entry)
                replace_file(entry[1], entry[0])
            if after_commit:
                started.extend(watched)
                after_commit()
        except BaseException as original:
            failures = []
            for target, _, backup, existed in reversed(started):
                try:
                    if existed:
                        # A failed Windows replace can leave the locked Host
                        # wholly unchanged. Do not attempt a needless restore
                        # into that same lock or misreport an incomplete pair.
                        if target.is_file() and _digest(target) == _digest(backup):
                            continue
                        # Copy backup to another same-volume staging file so a
                        # failed replace never consumes the recoverable backup.
                        restore = backup.with_name('restore')
                        copy_file(backup, restore)
                        replace_file(restore, target)
                        if _digest(target) != _digest(backup):
                            raise OSError('restored file hash mismatch')
                    elif target.exists():
                        target.unlink()
                except BaseException as error:
                    failures.append((target, error))
            if not failures and after_rollback:
                try:
                    after_rollback()
                except BaseException as error:
                    failures.append((Path('<rollback callback>'), error))
            if failures:
                keep_backups = True
                raise FileRollbackError(original, failures, directories) from original
            raise
        return [str(entry[0]) for entry in entries]
    finally:
        if not keep_backups:
            for directory in directories:
                # Only exact mkdtemp-created directories are recursively removed.
                shutil.rmtree(directory)
