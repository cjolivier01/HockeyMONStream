"""Bounded, pinned filesystem operations for private workflow state."""

import contextlib
import fcntl
import hashlib
import json
import os
import stat
import tempfile
from pathlib import Path

from .schema import MAX_PLAN_BYTES, WorkflowError, canonical, require


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def private_directory(path, create=False):
    path = Path(path).absolute()
    require(
        path == path.resolve(), "Workflow state directories cannot contain symlinks"
    )
    if create:
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.lstat()
    require(
        stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid(),
        "Workflow directory needs the current owner",
    )
    require(
        not info.st_mode & 0o022,
        "Workflow directory must not be writable by other users",
    )
    return path


@contextlib.contextmanager
def _pinned_file(path):
    """Traverse with openat and retain every ancestor through validation."""
    path = Path(path).absolute()
    require(".." not in path.parts, "Parent traversal is forbidden")
    directories = []
    descriptor = None
    try:
        parent = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        directories.append((parent, None, None))
        for part in path.parts[1:-1]:
            child = os.open(
                part,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                dir_fd=parent,
            )
            directories.append((child, parent, part))
            parent = child
        descriptor = os.open(
            path.name,
            os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
            dir_fd=parent,
        )
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode), "Expected a regular workflow file")
        yield descriptor, before
        require(
            identity(before) == identity(os.fstat(descriptor)),
            "Workflow file changed while reading",
        )
        require(
            identity(before)
            == identity(os.stat(path.name, dir_fd=parent, follow_symlinks=False)),
            "Workflow file was replaced while reading",
        )
        for child, ancestor, name in directories[1:]:
            opened = os.fstat(child)
            current = os.stat(name, dir_fd=ancestor, follow_symlinks=False)
            require(
                stat.S_ISDIR(current.st_mode)
                and (opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino),
                "Workflow ancestor was replaced while reading",
            )
    finally:
        if descriptor is not None:
            os.close(descriptor)
        for descriptor, _, _ in reversed(directories):
            os.close(descriptor)


def read_bytes(path, maximum=MAX_PLAN_BYTES):
    with _pinned_file(path) as (descriptor, before):
        require(0 <= before.st_size <= maximum, "Workflow file exceeds its byte bound")
        chunks, size = [], 0
        while True:
            chunk = os.read(descriptor, min(65536, maximum + 1 - size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
            require(size <= maximum, "Workflow file grew beyond its byte bound")
        require(
            identity(before) == identity(os.fstat(descriptor)),
            "Workflow file changed while reading",
        )
        require(
            identity(before) == identity(os.stat(path, follow_symlinks=False)),
            "Workflow file was replaced while reading",
        )
        return b"".join(chunks)


def read_json(path, maximum=MAX_PLAN_BYTES):
    try:

        def unique(pairs):
            result = {}
            for key, child in pairs:
                require(key not in result, "Duplicate workflow JSON key")
                result[key] = child
            return result

        value = json.loads(
            read_bytes(path, maximum),
            object_pairs_hook=unique,
            parse_constant=lambda _: (_ for _ in ()).throw(
                ValueError("Non-finite JSON")
            ),
        )
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise WorkflowError("Invalid workflow JSON document") from exc
    require(isinstance(value, dict), "Expected workflow JSON object")
    return value


def read_tail(path, maximum):
    """Read a bounded tail of a stopped task's log with stable file identity."""
    with _pinned_file(path) as (descriptor, before):
        os.lseek(descriptor, max(0, before.st_size - maximum), os.SEEK_SET)
        chunks, size = [], 0
        while size < maximum:
            chunk = os.read(descriptor, maximum - size)
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
        return b"".join(chunks)


def sync_directory(path):
    descriptor = os.open(
        path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
    )
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_json(path, value, *, exclusive=False):
    path = Path(path)
    contents = canonical(value) + b"\n"
    private_directory(path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".workflow-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        if exclusive:
            os.link(temporary, path, follow_symlinks=False)
            os.unlink(temporary)
        else:
            require(not path.is_symlink(), "Cannot replace a symlinked state file")
            os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextlib.contextmanager
def locked(path, *, blocking=True):
    path = Path(path)
    private_directory(path.parent)
    descriptor = os.open(
        path,
        os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
        0o600,
    )
    try:
        info = os.fstat(descriptor)
        require(
            stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
            "Invalid workflow lock owner/type",
        )
        require(not info.st_mode & 0o077, "Workflow lock must be private")
        fcntl.flock(descriptor, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        require(
            identity(info) == identity(os.stat(path, follow_symlinks=False)),
            "Workflow lock was replaced",
        )
        yield descriptor
    finally:
        os.close(descriptor)


def file_digest(path, maximum):
    with _pinned_file(path) as (descriptor, before):
        require(0 <= before.st_size <= maximum, "Artifact exceeds its byte bound")
        result, size = hashlib.sha256(), 0
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            require(size <= maximum, "Artifact grew beyond its byte bound")
            result.update(chunk)
        require(
            identity(before) == identity(os.fstat(descriptor)),
            "Artifact changed while hashing",
        )
        require(
            identity(before) == identity(os.stat(path, follow_symlinks=False)),
            "Artifact path changed while hashing",
        )
        return {"size": size, "sha256": result.hexdigest()}


def contained(root, relative):
    root = Path(root).resolve(strict=True)
    relative = Path(relative)
    require(
        not relative.is_absolute() and relative.parts and ".." not in relative.parts,
        "Invalid artifact relative path",
    )
    path = root
    for part in relative.parts:
        path = path / part
        require(not path.is_symlink(), "Artifact symlinks are forbidden")
    require(path.resolve().is_relative_to(root), "Artifact escaped its private root")
    return path
