import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import uuid
import zipfile

MAX_UNCOMPRESSED_BYTES = 32 * 1024 * 1024
MAX_PACKAGE_BYTES = 1 * 1024 * 1024
MAX_APPINFO_BYTES = 1 * 1024 * 1024
CHUNK_BYTES = 1024 * 1024
APPINFO_NAME = "appinfo.json"


class FinalizeError(Exception):
    pass


def _reject_constant(value):
    raise ValueError


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError
        result[key] = value
    return result


def _canonical_uuid(value, label):
    if not isinstance(value, str) or not value.strip():
        raise FinalizeError(f"{label} is missing")
    try:
        return str(uuid.UUID(value))
    except (AttributeError, TypeError, ValueError):
        raise FinalizeError(f"{label} is not a valid UUID") from None


def _load_package(path):
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_PACKAGE_BYTES + 1)
    except OSError:
        raise FinalizeError("package file could not be read") from None
    if len(raw) > MAX_PACKAGE_BYTES:
        raise FinalizeError("package file exceeds the size limit")
    try:
        package = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, ValueError, TypeError, RecursionError):
        raise FinalizeError("package file is not valid JSON") from None
    if not isinstance(package, dict):
        raise FinalizeError("package file must contain a JSON object")
    pebble = package.get("pebble")
    if not isinstance(pebble, dict):
        raise FinalizeError("package pebble metadata is missing")
    repository_uuid = _canonical_uuid(pebble.get("uuid"), "package UUID")
    companion = pebble.get("companionApp")
    _validate_companion(companion)
    return package, repository_uuid, copy.deepcopy(companion)


def _validate_companion(companion):
    if not isinstance(companion, dict):
        raise FinalizeError("package companionApp metadata is missing")
    android = companion.get("android")
    if not isinstance(android, dict):
        raise FinalizeError("package Android companion metadata is missing")
    apps = android.get("apps")
    if not isinstance(apps, list) or not apps:
        raise FinalizeError("package Android companion apps must be nonempty")
    seen = set()
    for app in apps:
        if not isinstance(app, dict):
            raise FinalizeError("package Android companion apps must be objects")
        package_name = app.get("package")
        if not isinstance(package_name, str) or not package_name.strip():
            raise FinalizeError("package Android companion entry has no package")
        if package_name in seen:
            raise FinalizeError("package Android companion packages must be unique")
        seen.add(package_name)


def _archive_members(archive):
    try:
        members = archive.infolist()
    except (OSError, RuntimeError, zipfile.BadZipFile):
        raise FinalizeError("input PBW directory could not be read") from None
    if not members:
        raise FinalizeError("input PBW is empty")
    names = set()
    total_uncompressed = 0
    for member in members:
        name = member.filename
        if name in names:
            raise FinalizeError("input PBW contains duplicate archive members")
        names.add(name)
        if member.flag_bits & 0x1:
            raise FinalizeError("encrypted PBW members are not supported")
        if member.file_size < 0:
            raise FinalizeError("input PBW contains an invalid member size")
        total_uncompressed += member.file_size
        if total_uncompressed > MAX_UNCOMPRESSED_BYTES:
            raise FinalizeError("input PBW exceeds the uncompressed size limit")
    appinfo_members = [member for member in members if member.filename == APPINFO_NAME]
    if len(appinfo_members) != 1:
        raise FinalizeError("input PBW must contain exactly one root appinfo.json")
    if appinfo_members[0].is_dir():
        raise FinalizeError("root appinfo.json must be a file")
    if appinfo_members[0].file_size > MAX_APPINFO_BYTES:
        raise FinalizeError("root appinfo.json exceeds the size limit")
    return members, appinfo_members[0]


def _read_member(archive, member, limit):
    if member.file_size > limit:
        raise FinalizeError("a PBW member exceeds the read limit")
    try:
        with archive.open(member, "r") as stream:
            data = stream.read(limit + 1)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, EOFError):
        raise FinalizeError("a PBW member could not be read") from None
    if len(data) != member.file_size:
        raise FinalizeError("a PBW member has an invalid size")
    return data


def _parse_json_object(data, label):
    try:
        value = json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, ValueError, TypeError, RecursionError):
        raise FinalizeError(f"{label} is not valid JSON") from None
    if not isinstance(value, dict):
        raise FinalizeError(f"{label} must contain a JSON object")
    return value


def _member_digest(archive, member):
    digest = hashlib.sha256()
    if member.is_dir() and member.file_size == 0:
        return digest.hexdigest()
    try:
        with archive.open(member, "r") as stream:
            while True:
                chunk = stream.read(CHUNK_BYTES)
                if not chunk:
                    break
                digest.update(chunk)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, EOFError):
        raise FinalizeError("a PBW member could not be hashed") from None
    return digest.hexdigest()


def _hash_members(archive, members):
    return {
        member.filename: _member_digest(archive, member)
        for member in members
    }


def _copy_member(source, target, member, data):
    copied = copy.copy(member)
    try:
        if data is not None:
            target.writestr(copied, data, compress_type=member.compress_type)
        elif member.is_dir() and member.file_size == 0:
            target.writestr(copied, b"", compress_type=member.compress_type)
        else:
            with source.open(member, "r") as source_stream:
                with target.open(copied, "w", force_zip64=False) as target_stream:
                    shutil.copyfileobj(source_stream, target_stream, length=CHUNK_BYTES)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, EOFError, NotImplementedError):
        raise FinalizeError("output PBW could not be written") from None


def _write_output(source, output, members, appinfo_bytes):
    output.comment = source.comment
    for member in members:
        data = appinfo_bytes if member.filename == APPINFO_NAME else None
        _copy_member(source, output, member, data)


def _verify_output(
    path,
    expected_members,
    expected_hashes,
    repository_uuid,
    companion,
    expected_appinfo,
):
    try:
        with zipfile.ZipFile(path, "r") as result:
            result_members = result.infolist()
            result_names = [member.filename for member in result_members]
            expected_names = [member.filename for member in expected_members]
            if sum(member.file_size for member in result_members) > MAX_UNCOMPRESSED_BYTES:
                raise FinalizeError("output PBW exceeds the uncompressed size limit")
            if len(result_names) != len(set(result_names)):
                raise FinalizeError("output PBW contains duplicate archive members")
            if result_names != expected_names:
                raise FinalizeError("output PBW member set or order changed")
            appinfo_members = [member for member in result_members if member.filename == APPINFO_NAME]
            if len(appinfo_members) != 1 or appinfo_members[0].is_dir():
                raise FinalizeError("output PBW has no unique root appinfo.json")
            result_appinfo = _parse_json_object(
                _read_member(result, appinfo_members[0], MAX_APPINFO_BYTES),
                "root appinfo.json",
            )
            result_uuid = _canonical_uuid(result_appinfo.get("uuid"), "appinfo UUID")
            if result_uuid != repository_uuid:
                raise FinalizeError("output PBW UUID does not match package metadata")
            if result_appinfo.get("companionApp") != companion:
                raise FinalizeError("output PBW companion declaration is not exact")
            if {
                key: value
                for key, value in result_appinfo.items()
                if key != "companionApp"
            } != {
                key: value
                for key, value in expected_appinfo.items()
                if key != "companionApp"
            }:
                raise FinalizeError("output PBW changed another appinfo value")
            for member in result_members:
                if member.filename == APPINFO_NAME:
                    continue
                if _member_digest(result, member) != expected_hashes[member.filename]:
                    raise FinalizeError("a nonmetadata PBW member changed")

    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile):
        raise FinalizeError("output PBW could not be reopened") from None


def _remove_output(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    except OSError:
        pass


def finalize_pbw(package_path, input_path, output_path):
    package_path = Path(package_path)
    input_path = Path(input_path)
    output_path = Path(output_path)
    package_resolved = package_path.resolve()
    input_resolved = input_path.resolve()
    output_resolved = output_path.resolve()
    output_write_path = Path(os.path.abspath(output_path))
    if output_resolved == input_resolved or output_resolved == package_resolved:
        raise FinalizeError("output must be a separate path")
    if os.path.lexists(output_write_path):
        raise FinalizeError("output already exists")
    _, repository_uuid, companion = _load_package(package_resolved)
    if not input_resolved.is_file():
        raise FinalizeError("input PBW does not exist")
    output_created = False
    try:
        with zipfile.ZipFile(input_resolved, "r") as source:
            members, appinfo_member = _archive_members(source)
            original_hashes = _hash_members(source, members)
            original_appinfo_bytes = _read_member(
                source,
                appinfo_member,
                MAX_APPINFO_BYTES,
            )
            appinfo = _parse_json_object(original_appinfo_bytes, "root appinfo.json")
            appinfo_uuid = _canonical_uuid(appinfo.get("uuid"), "appinfo UUID")
            if appinfo_uuid != repository_uuid:
                raise FinalizeError("input PBW UUID does not match package metadata")
            already_present = appinfo.get("companionApp") == companion
            if already_present:
                output_appinfo_bytes = original_appinfo_bytes
            else:
                updated = copy.deepcopy(appinfo)
                updated["companionApp"] = copy.deepcopy(companion)
                try:
                    output_appinfo_bytes = (
                        json.dumps(
                            updated,
                            ensure_ascii=True,
                            allow_nan=False,
                            separators=(",", ":"),
                        ).encode("utf-8")
                        + b"\n"
                    )
                except (TypeError, ValueError, UnicodeError):
                    raise FinalizeError("appinfo.json could not be serialized") from None
            if len(output_appinfo_bytes) > MAX_APPINFO_BYTES:
                raise FinalizeError("updated appinfo.json exceeds the size limit")
            try:
                with output_write_path.open("xb") as output_stream:
                    output_created = True
                    with zipfile.ZipFile(
                        output_stream,
                        "w",
                        allowZip64=True,
                    ) as output:
                        _write_output(source, output, members, output_appinfo_bytes)
            except FileExistsError:
                raise FinalizeError("output already exists") from None
        _verify_output(
            output_write_path,
            members,
            original_hashes,
            repository_uuid,
            companion,
            appinfo,
        )
    except FinalizeError:
        if output_created:
            _remove_output(output_write_path)
        raise
    except Exception:
        if output_created:
            _remove_output(output_write_path)
        raise FinalizeError("PBW finalization failed") from None
    print(f"package={package_resolved}")
    print(f"uuid={repository_uuid}")
    print(f"output={output_resolved}")
    print("metadata already present" if already_present else "metadata restored")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Finalize a Hermes Pebble PBW")
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        finalize_pbw(args.package, args.input, args.output)
    except FinalizeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except Exception:
        print("error: PBW finalization failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
