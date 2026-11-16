#!/usr/bin/env python3
"""
Convert AssetRipper-exported character mesh assets into a compact binary
for the overlay's mesh-based chams renderer.

Usage:
    python convert_character.py [body.asset] [head.asset] [-o output.bin]

Defaults to m_body01_lod4.asset + m_head01_lod3.asset → character_mesh.bin.
"""

import sys, struct, re, math, os
from pathlib import Path

# Unity vertex channel format byte sizes
FMT_SIZE = {
    0: 4,   # Float32
    1: 2,   # Float16
    2: 1,   # UNorm8
    3: 1,   # SNorm8
    4: 2,   # UNorm16
    5: 2,   # SNorm16
    6: 1,   # UInt8
    7: 1,   # SInt8
    8: 2,   # UInt16
    9: 2,   # SInt16
    10: 4,  # UInt32
    11: 4,  # SInt32
}


def parse_mesh_asset(path: str):
    """Parse a Unity skinned mesh .asset exported by AssetRipper."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")

    # vertex count
    m = re.search(r"m_VertexCount:\s*(\d+)", text)
    if not m:
        raise ValueError(f"No m_VertexCount in {path}")
    vert_count = int(m.group(1))

    # index format (0 = uint16, 1 = uint32)
    m = re.search(r"m_IndexFormat:\s*(\d+)", text)
    idx_fmt = int(m.group(1)) if m else 0

    # --- channels ---
    channels = []
    block = re.search(r"m_Channels:\s*\n((?:\s+-\s.*\n|\s+\w+:.*\n)+)", text)
    if block:
        entries = re.findall(
            r"stream:\s*(\d+)\s*\n\s*offset:\s*(\d+)\s*\n"
            r"\s*format:\s*(\d+)\s*\n\s*dimension:\s*(\d+)",
            block.group(1),
        )
        channels = [(int(s), int(o), int(f), int(d)) for s, o, f, d in entries]

    # per-stream strides
    strides = {}
    for stream, offset, fmt, dim in channels:
        if dim == 0:
            continue
        end = offset + FMT_SIZE.get(fmt, 4) * dim
        strides[stream] = max(strides.get(stream, 0), end)

    # vertex data blob
    m = re.search(r"_typelessdata:\s*([0-9a-fA-F]+)", text)
    if not m:
        raise ValueError(f"No vertex data in {path}")
    vdata = bytes.fromhex(m.group(1))

    # stream start offsets (sequential in buffer)
    stream_starts = {}
    off = 0
    for sid in sorted(strides.keys()):
        stream_starts[sid] = off
        off += strides[sid] * vert_count

    # channel roles by index:
    #  0=Position  1=Normal  2=Tangent  3=Color
    #  4=UV0  5=UV1  6=UV2  7=UV3
    #  8-11=unused  12=BlendWeight  13=BlendIndices
    pos_ch = channels[0] if len(channels) > 0 and channels[0][3] > 0 else None
    bw_ch = channels[12] if len(channels) > 12 and channels[12][3] > 0 else None
    bi_ch = channels[13] if len(channels) > 13 and channels[13][3] > 0 else None

    # --- positions ---
    positions = []
    if pos_ch:
        s, ch_off, fmt, dim = pos_ch
        base = stream_starts.get(s, 0)
        stride = strides.get(s, 12)
        for i in range(vert_count):
            p = base + i * stride + ch_off
            x, y, z = struct.unpack_from("<fff", vdata, p)
            positions.append((x, y, z))

    # --- bone weights (channel 12 = BlendWeight) ---
    bone_weights = []
    if bw_ch:
        s, ch_off, fmt, dim = bw_ch
        base = stream_starts.get(s, 0)
        stride = strides.get(s, 32)
        for i in range(vert_count):
            p = base + i * stride + ch_off
            if fmt == 0:  # float32
                wts = struct.unpack_from("<ffff", vdata, p)
            elif fmt == 2:  # UNorm8
                raw = struct.unpack_from("<BBBB", vdata, p)
                wts = tuple(w / 255.0 for w in raw)
            elif fmt == 4:  # UNorm16
                raw = struct.unpack_from("<HHHH", vdata, p)
                wts = tuple(w / 65535.0 for w in raw)
            else:
                wts = (1.0, 0.0, 0.0, 0.0)
            bone_weights.append(wts)

    # --- bone indices (channel 13 = BlendIndices) ---
    bone_indices = []
    if bi_ch:
        s, ch_off, fmt, dim = bi_ch
        base = stream_starts.get(s, 0)
        stride = strides.get(s, 32)
        for i in range(vert_count):
            p = base + i * stride + ch_off
            if fmt == 10:  # UInt32
                idxs = struct.unpack_from("<IIII", vdata, p)
            elif fmt == 0:  # Float32 (cast)
                raw = struct.unpack_from("<ffff", vdata, p)
                idxs = tuple(int(r) for r in raw)
            elif fmt == 6:  # UInt8
                idxs = struct.unpack_from("<BBBB", vdata, p)
            elif fmt == 8:  # UInt16
                idxs = struct.unpack_from("<HHHH", vdata, p)
            else:
                idxs = (0, 0, 0, 0)
            bone_indices.append(idxs)

    # --- triangles ---
    m = re.search(r"m_IndexBuffer:\s*([0-9a-fA-F]+)", text)
    if not m:
        raise ValueError(f"No index buffer in {path}")
    idata = bytes.fromhex(m.group(1))

    if idx_fmt == 1:
        cnt = len(idata) // 4
        indices = list(struct.unpack_from(f"<{cnt}I", idata))
    else:
        cnt = len(idata) // 2
        indices = list(struct.unpack_from(f"<{cnt}H", idata))

    m = re.search(r"indexCount:\s*(\d+)", text)
    if m:
        indices = indices[: int(m.group(1))]

    triangles = []
    for i in range(0, len(indices) - 2, 3):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a < vert_count and b < vert_count and c < vert_count:
            triangles.append((a, b, c))

    # --- bind poses ---
    bind_poses = []
    # Match each "- e00: ..." block within m_BindPose
    bp_start = text.find("m_BindPose:")
    if bp_start >= 0:
        bp_text = text[bp_start:]
        # Stop at next top-level field
        bp_end = re.search(r"\n  m_\w+:", bp_text[12:])
        if bp_end:
            bp_text = bp_text[: 12 + bp_end.start()]

        mat_starts = [m.start() for m in re.finditer(r"- e00:", bp_text)]
        for ms in mat_starts:
            block = bp_text[ms : ms + 800]
            vals = []
            for field in [
                "e00", "e01", "e02", "e03",
                "e10", "e11", "e12", "e13",
                "e20", "e21", "e22", "e23",
                "e30", "e31", "e32", "e33",
            ]:
                fm = re.search(rf"{field}:\s*([-\dEe.+]+)", block)
                vals.append(float(fm.group(1)) if fm else 0.0)
            bind_poses.append(vals)

    # --- bone name hashes ---
    bone_hashes = []
    m = re.search(r"m_BoneNameHashes:\s*([0-9a-fA-F]+)", text)
    if m:
        hex_str = m.group(1)
        for i in range(0, len(hex_str), 8):
            chunk = hex_str[i : i + 8]
            if len(chunk) == 8:
                raw = bytes.fromhex(chunk)
                bone_hashes.append(struct.unpack("<I", raw)[0])

    print(
        f"  {Path(path).name}: {vert_count} verts, {len(triangles)} tris, "
        f"{len(bind_poses)} bones, channels {len(channels)}"
    )

    return {
        "positions": positions,
        "bone_indices": bone_indices,
        "bone_weights": bone_weights,
        "triangles": triangles,
        "bind_poses": bind_poses,
        "bone_hashes": bone_hashes,
    }


def validate(mesh, label=""):
    """Sanity-check bone weights and indices."""
    bad_wt = 0
    max_bone = 0
    for i, (wts, idxs) in enumerate(
        zip(mesh["bone_weights"], mesh["bone_indices"])
    ):
        s = sum(wts)
        if abs(s - 1.0) > 0.1 and s > 0.001:
            bad_wt += 1
        for idx in idxs:
            if idx > max_bone:
                max_bone = idx

    n = len(mesh["bone_weights"])
    if bad_wt:
        print(f"  {label}WARNING: {bad_wt}/{n} verts have weight sum far from 1.0")
    else:
        print(f"  {label}Weights OK ({n} verts, max bone idx {max_bone})")


def row_to_col_major(vals):
    """Convert 16-float row-major matrix to column-major."""
    out = [0.0] * 16
    for r in range(4):
        for c in range(4):
            out[c * 4 + r] = vals[r * 4 + c]
    return out


def merge_meshes(body, head):
    """Merge body and head into one mesh."""
    nb = len(body["bind_poses"])
    nh = len(head["bind_poses"])
    assert nb == nh, f"Bone count mismatch: body={nb}, head={nh}"

    bv = len(body["positions"])

    combined = {
        "positions": body["positions"] + head["positions"],
        "bone_indices": body["bone_indices"] + head["bone_indices"],
        "bone_weights": body["bone_weights"] + head["bone_weights"],
        "triangles": body["triangles"]
        + [(a + bv, b + bv, c + bv) for a, b, c in head["triangles"]],
        "bind_poses": body["bind_poses"],
        "bone_hashes": body["bone_hashes"],
    }
    print(
        f"  Merged: {len(combined['positions'])} verts, "
        f"{len(combined['triangles'])} tris"
    )
    return combined


def save_binary(mesh, path):
    """Save as character_mesh.bin.

    Format (all little-endian):
        char[4]   magic "CHR1"
        uint32    numBones
        uint32    numVerts
        uint32    numTris

        uint32    boneNameHashes[numBones]
        float[16] bindPoses[numBones]       (column-major 4x4, matching Matrix4x4)

        struct Vertex {
            float  x, y, z;     // bind-pose position
            uint8  bones[4];    // bone indices into this file's bone list
            float  weights[4];  // blend weights
        } vertices[numVerts];   // 32 bytes each

        uint16 indices[numTris * 3]
    """
    num_bones = len(mesh["bind_poses"])
    num_verts = len(mesh["positions"])
    num_tris = len(mesh["triangles"])

    with open(path, "wb") as f:
        f.write(b"CHR1")
        f.write(struct.pack("<III", num_bones, num_verts, num_tris))

        # bone hashes
        for h in mesh["bone_hashes"]:
            f.write(struct.pack("<I", h))

        # bind poses (column-major)
        for bp in mesh["bind_poses"]:
            cm = row_to_col_major(bp)
            f.write(struct.pack("<16f", *cm))

        # vertices
        for i in range(num_verts):
            x, y, z = mesh["positions"][i]
            f.write(struct.pack("<fff", x, y, z))

            if mesh["bone_indices"]:
                bi = mesh["bone_indices"][i]
                f.write(
                    struct.pack(
                        "<BBBB",
                        min(int(bi[0]), 255),
                        min(int(bi[1]), 255),
                        min(int(bi[2]), 255),
                        min(int(bi[3]), 255),
                    )
                )
            else:
                f.write(b"\x00\x00\x00\x00")

            if mesh["bone_weights"]:
                bw = mesh["bone_weights"][i]
                f.write(struct.pack("<ffff", *bw))
            else:
                f.write(struct.pack("<ffff", 1.0, 0.0, 0.0, 0.0))

        # triangle indices
        for a, b, c in mesh["triangles"]:
            f.write(struct.pack("<HHH", a, b, c))

    sz = os.path.getsize(path)
    print(f"  Saved {path} ({sz / 1024:.1f} KB)")


def main():
    export_base = Path(
        r"C:\Users\machine2\Documents\Export\ExportedProject\Assets\Mesh"
    )

    body_path = (
        sys.argv[1]
        if len(sys.argv) > 1
        else str(export_base / "m_body01_lod4.asset")
    )
    head_path = (
        sys.argv[2]
        if len(sys.argv) > 2
        else str(export_base / "m_head01_lod3.asset")
    )
    out_path = "character_mesh.bin"
    for i, arg in enumerate(sys.argv):
        if arg == "-o" and i + 1 < len(sys.argv):
            out_path = sys.argv[i + 1]

    print(f"[1/4] Parsing body: {Path(body_path).name}")
    body = parse_mesh_asset(body_path)
    validate(body, "body: ")

    print(f"[2/4] Parsing head: {Path(head_path).name}")
    head = parse_mesh_asset(head_path)
    validate(head, "head: ")

    print("[3/4] Merging...")
    combined = merge_meshes(body, head)

    print("[4/4] Saving binary...")
    save_binary(combined, out_path)

    print(
        f"\nDone: {len(combined['bind_poses'])} bones, "
        f"{len(combined['positions'])} verts, "
        f"{len(combined['triangles'])} tris"
    )


if __name__ == "__main__":
    main()
