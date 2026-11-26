#!/usr/bin/env python3
"""The signed release manifest, prism_release.json (schema: manifest_schema.json, which says what each field is).

    manifest.py check <prism_release.json> [--sidecars <folder>]     schema + rules (+ each sidecar's ids and SHA)
    manifest.py sign <prism_release.json> --key-file <seed file>     writes prism_release.json.sig
    manifest.py verify <prism_release.json> [--public-key <64 hex>]  exit 0: signed by that key (default: the
                                                                     release key, port/assets/release-key.pub)

Signed as a release's SHA256SUMS is (tools/release_sign.py): the .sig is the Ed25519 signature (RFC 8032) of the
file's exact bytes, 128 lowercase hex digits and a newline. The file is written canonically (dumps): keys sorted,
two-space indent, ASCII, a final newline, so a rebuild of the same content is the same bytes.

The rules beyond the schema (section 2.6's pack gate and section 2.5's identities):
- a base_id appears once in the whole manifest; a classic_world_id once per map;
- every BSP has exactly one classic pack, whose world_id is world_id_classic(classic_world_id, bsp);
- a world with a sidecar names an enhanced world_id for every BSP and has exactly one enhanced pack per BSP with
  that world_id; a sidecar's name carries the first 8 hex digits of its file_sha;
- every pack's bake_version is one the manifest accepts;
- with the sidecar files at hand, each one's SHA-256, size and the world_ids the engine derives from its
  directory (identity.py, the twin of prism_identity.c) are the manifest's.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from geopatch import identity, prsx  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads((Path(__file__).resolve().parent / "manifest_schema.json").read_text())
RELEASE_KEY = ROOT / "port/assets/release-key.pub"


class ManifestError(Exception):
    pass


def dumps(manifest):
    return json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


# ---------- the schema (the subset of JSON Schema manifest_schema.json uses)

def _validate(value, schema, path, errors):
    if "$ref" in schema:
        name = schema["$ref"].split("/")[-1]
        return _validate(value, SCHEMA["$defs"][name], path, errors)
    kind = schema.get("type")
    types = {"object": dict, "array": list, "string": str, "integer": int}
    if kind and (not isinstance(value, types[kind]) or (kind == "integer" and isinstance(value, bool))):
        errors.append("%s: not a %s" % (path, kind))
        return
    if "const" in schema and value != schema["const"]:
        errors.append("%s: must be %r" % (path, schema["const"]))
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            errors.append("%s: too short" % path)
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append("%s: %r does not match %s" % (path, value, schema["pattern"]))
    if isinstance(value, int):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append("%s: below %d" % (path, schema["minimum"]))
        if "maximum" in schema and value > schema["maximum"]:
            errors.append("%s: above %d" % (path, schema["maximum"]))
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", 1 << 30):
            errors.append("%s: %d items" % (path, len(value)))
        for index, item in enumerate(value):
            if "items" in schema:
                _validate(item, schema["items"], "%s[%d]" % (path, index), errors)
    if isinstance(value, dict):
        for name in schema.get("required", []):
            if name not in value:
                errors.append("%s: lacks %s" % (path, name))
        properties = schema.get("properties", {})
        for name, item in value.items():
            if "propertyNames" in schema:
                _validate(name, schema["propertyNames"], "%s key %r" % (path, name), errors)
            if name in properties:
                _validate(item, properties[name], "%s.%s" % (path, name), errors)
            elif schema.get("additionalProperties") is False:
                errors.append("%s: unknown field %s" % (path, name))
            elif isinstance(schema.get("additionalProperties"), dict):
                _validate(item, schema["additionalProperties"], "%s.%s" % (path, name), errors)


def check(manifest, sidecar_folder=None):
    """[error text]: empty when the manifest follows its schema and rules"""
    errors = []
    _validate(manifest, SCHEMA, "$", errors)
    if errors:
        return errors
    accepted = set(manifest["bake_versions"])
    base_ids = {}
    for name, entry in manifest["maps"].items():
        seen_worlds = set()
        for w, world in enumerate(entry["worlds"]):
            where = "%s world %d" % (name, w)
            cwid = world["classic_world_id"]
            if cwid in seen_worlds:
                errors.append("%s: classic_world_id twice" % where)
            seen_worlds.add(cwid)
            for base in world["base_ids"]:
                if base["base_id"] in base_ids:
                    errors.append("%s: base_id %s already serves %s" % (where, base["base_id"][:12],
                                                                      base_ids[base["base_id"]]))
                base_ids[base["base_id"]] = where
            count = world["bsp_count"]
            cid = bytes.fromhex(cwid)
            errors += _packs(where, "classic", world["packs"]["classic"], count, accepted,
                             {b: identity.world_id_classic(cid, b) for b in range(count)})
            sidecar = world.get("sidecar")
            if sidecar is None:
                if world["packs"].get("enhanced"):
                    errors.append("%s: enhanced packs without a sidecar" % where)
                continue
            if not sidecar["name"].split(".")[1] == sidecar["file_sha"][:8]:
                errors.append("%s: the sidecar's name does not carry its file_sha" % where)
            if sidecar["prsx_format"] not in manifest["prsx_formats"]:
                errors.append("%s: sidecar format %d not accepted" % (where, sidecar["prsx_format"]))
            ids = {int(k): v for k, v in sidecar["world_ids"].items()}
            if sorted(ids) != list(range(count)):
                errors.append("%s: the sidecar's world_ids do not name every BSP" % where)
            errors += _packs(where, "enhanced", world["packs"].get("enhanced", []), count, accepted, ids)
            if sidecar_folder is not None:
                errors += _sidecar_file(where, Path(sidecar_folder) / sidecar["name"], sidecar, cid, ids)
    return errors


def _packs(where, variant, packs, count, accepted, expected):
    errors = []
    by_bsp = {}
    for pack in packs:
        if pack["bsp"] in by_bsp:
            errors.append("%s: two %s packs for BSP %d" % (where, variant, pack["bsp"]))
        by_bsp[pack["bsp"]] = pack
        if pack["bake_version"] not in accepted:
            errors.append("%s: %s pack BSP %d of BAKE_VERSION %d" % (where, variant, pack["bsp"],
                                                                     pack["bake_version"]))
    for bsp in range(count):
        pack = by_bsp.get(bsp)
        if pack is None:
            errors.append("%s: no %s pack for BSP %d" % (where, variant, bsp))
        elif pack["world_id"] != expected.get(bsp):
            errors.append("%s: the %s pack of BSP %d is for another world_id" % (where, variant, bsp))
    for bsp in by_bsp:
        if bsp >= count:
            errors.append("%s: a %s pack for BSP %d of %d" % (where, variant, bsp, count))
    return errors


def _sidecar_file(where, path, sidecar, cid, ids):
    if not path.exists():
        return ["%s: %s missing" % (where, path.name)]
    data = path.read_bytes()
    errors = []
    if hashlib.sha256(data).hexdigest() != sidecar["file_sha"] or len(data) != sidecar["size"]:
        errors.append("%s: %s is not the file the manifest names" % (where, path.name))
    try:
        opened = prsx.Sidecar(data)
    except prsx.PrsxError as error:
        return errors + ["%s: %s: %s" % (where, path.name, error)]
    if opened.classic_world_id != cid:
        errors.append("%s: %s is for another classic_world_id" % (where, path.name))
    for bsp, expected in ids.items():
        if identity.world_id_enhanced(opened, bsp) != expected:
            errors.append("%s: %s gives BSP %d another world_id" % (where, path.name, bsp))
    collvs = sorted({e.collv for e in opened.entries if e.collv})
    if collvs != sorted(sidecar["collvs"]):
        errors.append("%s: %s holds collision versions %s" % (where, path.name, collvs))
    return errors


# ---------- signature (release_sign.py's scheme, with the pure-Python Ed25519 of tools/ed25519_ref.py)

def _ed25519():
    sys.path.insert(0, str(ROOT / "tools"))
    import ed25519_ref
    return ed25519_ref


def sign(data, seed):
    """the .sig file's text for data, signed by seed (32 bytes)"""
    return _ed25519().sign(seed, data).hex() + "\n"


def verify(data, signature_text, public):
    text = signature_text.strip()
    if not re.fullmatch(r"[0-9a-f]{128}", text):
        return False
    return _ed25519().verify(public, data, bytes.fromhex(text))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check")
    c.add_argument("manifest")
    c.add_argument("--sidecars")
    s = sub.add_parser("sign")
    s.add_argument("manifest")
    s.add_argument("--key-file", required=True)
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    v.add_argument("--public-key")
    args = parser.parse_args(argv)
    path = Path(args.manifest)
    data = path.read_bytes()
    if args.command == "check":
        manifest = json.loads(data)
        errors = check(manifest, args.sidecars)
        if data.decode("ascii", "replace") != dumps(manifest):
            errors.append("not written canonically (manifest.dumps)")
        for error in errors:
            print(error)
        print("ok" if not errors else "%d errors" % len(errors))
        return 0 if not errors else 1
    if args.command == "sign":
        seed = bytes.fromhex(Path(args.key_file).read_text().strip())
        Path(str(path) + ".sig").write_text(sign(data, seed))
        return 0
    public = bytes.fromhex(args.public_key or RELEASE_KEY.read_text().split()[0])
    ok = verify(data, Path(str(path) + ".sig").read_text(), public)
    print("signed" if ok else "not signed by that key")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
