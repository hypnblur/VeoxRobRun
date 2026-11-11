#!/usr/bin/env python3
"""
Convert an AssetRipper-exported Unity scene + mesh assets into a compact
BVH binary that the cheat's offline raycaster can load.

Usage:
    python convert_scene.py <scene.unity> <meshdir> <output.bvh>
"""

import sys, os, re, struct, math, time
from pathlib import Path
from collections import defaultdict

# ---------------------------------------------------------------------------
# 1.  GUID -> mesh file path mapping
# ---------------------------------------------------------------------------

def build_guid_map(mesh_dir: Path) -> dict[str, Path]:
    """Scan .meta files for guid -> .asset path."""
    guid_map = {}
    for meta in mesh_dir.glob("*.asset.meta"):
        asset = meta.with_suffix("")  # strip .meta
        if not asset.exists():
            continue
        with open(meta, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("guid:"):
                    guid = line.split(":", 1)[1].strip()
                    guid_map[guid] = asset
                    break
    return guid_map


# ---------------------------------------------------------------------------
# 2.  Stream-parse Unity YAML scene
# ---------------------------------------------------------------------------

_OBJ_HEADER = re.compile(r"^--- !u!(\d+) &(\d+)")
_FIELD_INT  = re.compile(r"^\s+(\w+):\s*(-?\d+)")
_FIELD_FLOAT = re.compile(r"^\s+(\w+):\s*(-?[\d.Ee+-]+)")
_VEC3 = re.compile(
    r"\{x:\s*(-?[\d.Ee+-]+),\s*y:\s*(-?[\d.Ee+-]+),\s*z:\s*(-?[\d.Ee+-]+)\}")
_VEC4 = re.compile(
    r"\{x:\s*(-?[\d.Ee+-]+),\s*y:\s*(-?[\d.Ee+-]+),"
    r"\s*z:\s*(-?[\d.Ee+-]+),\s*w:\s*(-?[\d.Ee+-]+)\}")
_FILEREF = re.compile(r"\{fileID:\s*(\d+)")
_MESHREF = re.compile(
    r"\{fileID:\s*\d+,\s*guid:\s*([0-9a-f]+),\s*type:\s*\d+\}")

class Transform:
    __slots__ = ("file_id", "go_id", "pos", "rot", "scale", "father_id")
    def __init__(self):
        self.file_id = 0
        self.go_id = 0
        self.pos = (0.0, 0.0, 0.0)
        self.rot = (0.0, 0.0, 0.0, 1.0)
        self.scale = (1.0, 1.0, 1.0)
        self.father_id = 0

class MeshCol:
    __slots__ = ("go_id", "guid", "enabled", "is_trigger")
    def __init__(self):
        self.go_id = 0
        self.guid = ""
        self.enabled = True
        self.is_trigger = False

class BoxCol:
    __slots__ = ("go_id", "size", "center", "enabled", "is_trigger")
    def __init__(self):
        self.go_id = 0
        self.size = (1.0, 1.0, 1.0)
        self.center = (0.0, 0.0, 0.0)
        self.enabled = True
        self.is_trigger = False


def parse_scene(scene_path: Path):
    """Stream-parse the YAML scene, yielding (type_id, object) tuples."""
    transforms = []
    mesh_cols = []
    box_cols = []

    current_type = 0
    current_id = 0
    obj_lines: list[str] = []

    def flush():
        nonlocal current_type, obj_lines
        if current_type == 4:   # Transform
            t = _parse_transform(current_id, obj_lines)
            if t:
                transforms.append(t)
        elif current_type == 64: # MeshCollider
            m = _parse_mesh_collider(obj_lines)
            if m:
                mesh_cols.append(m)
        elif current_type == 65: # BoxCollider
            b = _parse_box_collider(obj_lines)
            if b:
                box_cols.append(b)
        obj_lines = []

    count = 0
    with open(scene_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = _OBJ_HEADER.match(line)
            if m:
                flush()
                current_type = int(m.group(1))
                current_id = int(m.group(2))
                count += 1
                if count % 50000 == 0:
                    print(f"  ... parsed {count} objects", flush=True)
            elif current_type in (4, 64, 65):
                obj_lines.append(line)
    flush()

    print(f"  Scene totals: {len(transforms)} transforms, "
          f"{len(mesh_cols)} mesh colliders, {len(box_cols)} box colliders")
    return transforms, mesh_cols, box_cols


def _parse_transform(file_id: int, lines: list[str]) -> Transform | None:
    t = Transform()
    t.file_id = file_id
    for line in lines:
        if "m_GameObject:" in line:
            m = _FILEREF.search(line)
            if m:
                t.go_id = int(m.group(1))
        elif "m_LocalRotation:" in line:
            m = _VEC4.search(line)
            if m:
                t.rot = tuple(float(m.group(i)) for i in range(1, 5))
        elif "m_LocalPosition:" in line:
            m = _VEC3.search(line)
            if m:
                t.pos = tuple(float(m.group(i)) for i in range(1, 4))
        elif "m_LocalScale:" in line:
            m = _VEC3.search(line)
            if m:
                t.scale = tuple(float(m.group(i)) for i in range(1, 4))
        elif "m_Father:" in line:
            m = _FILEREF.search(line)
            if m:
                t.father_id = int(m.group(1))
    return t


def _parse_mesh_collider(lines: list[str]) -> MeshCol | None:
    mc = MeshCol()
    for line in lines:
        if "m_GameObject:" in line:
            m = _FILEREF.search(line)
            if m:
                mc.go_id = int(m.group(1))
        elif "m_Mesh:" in line:
            m = _MESHREF.search(line)
            if m:
                mc.guid = m.group(1)
        elif "m_Enabled:" in line:
            mc.enabled = "1" in line.split(":", 1)[1]
        elif "m_IsTrigger:" in line:
            mc.is_trigger = "1" in line.split(":", 1)[1]
    if not mc.guid or not mc.enabled or mc.is_trigger:
        return None
    return mc


def _parse_box_collider(lines: list[str]) -> BoxCol | None:
    bc = BoxCol()
    for line in lines:
        if "m_GameObject:" in line:
            m = _FILEREF.search(line)
            if m:
                bc.go_id = int(m.group(1))
        elif "m_Size:" in line:
            m = _VEC3.search(line)
            if m:
                bc.size = tuple(float(m.group(i)) for i in range(1, 4))
        elif "m_Center:" in line:
            m = _VEC3.search(line)
            if m:
                bc.center = tuple(float(m.group(i)) for i in range(1, 4))
        elif "m_Enabled:" in line:
            bc.enabled = "1" in line.split(":", 1)[1]
        elif "m_IsTrigger:" in line:
            bc.is_trigger = "1" in line.split(":", 1)[1]
    if not bc.enabled or bc.is_trigger:
        return None
    return bc


# ---------------------------------------------------------------------------
# 3.  Parse mesh .asset files  (vertex positions + triangle indices)
# ---------------------------------------------------------------------------

def parse_mesh_asset(path: Path) -> tuple[list[tuple], list[tuple]] | None:
    """Returns (vertices_xyz[], triangles_idx[]) or None on failure."""
    text = path.read_text(encoding="utf-8", errors="replace")

    # vertex count
    m = re.search(r"m_VertexCount:\s*(\d+)", text)
    if not m:
        return None
    vert_count = int(m.group(1))
    if vert_count == 0:
        return None

    # index format: 0=uint16, 1=uint32
    m = re.search(r"m_IndexFormat:\s*(\d+)", text)
    idx_fmt = int(m.group(1)) if m else 0

    # channels – compute stride and position offset
    channels_block = re.search(
        r"m_Channels:\s*\n((?:\s+-.*\n)+)", text)
    stride = 0
    pos_offset = 0
    if channels_block:
        ch_text = channels_block.group(1)
        entries = re.findall(
            r"stream:\s*(\d+)\s*\n\s*offset:\s*(\d+)\s*\n"
            r"\s*format:\s*(\d+)\s*\n\s*dimension:\s*(\d+)",
            ch_text)
        for i, (stream, offset, fmt, dim) in enumerate(entries):
            stream, offset, fmt, dim = int(stream), int(offset), int(fmt), int(dim)
            if dim == 0:
                continue
            fmt_size = {0: 4, 1: 2, 2: 1, 3: 2, 4: 1, 11: 2}
            byte_size = fmt_size.get(fmt, 4) * dim
            if stream == 0:
                stride = max(stride, offset + byte_size)
            if i == 0:
                pos_offset = offset

    if stride == 0:
        stride = 12  # fallback: just positions

    # vertex data
    m = re.search(r"_typelessdata:\s*([0-9a-fA-F]+)", text)
    if not m:
        return None
    vdata = bytes.fromhex(m.group(1))

    vertices = []
    for i in range(vert_count):
        off = i * stride + pos_offset
        if off + 12 > len(vdata):
            break
        x, y, z = struct.unpack_from("<fff", vdata, off)
        vertices.append((x, y, z))

    # index buffer
    m = re.search(r"m_IndexBuffer:\s*([0-9a-fA-F]+)", text)
    if not m:
        return None
    idata = bytes.fromhex(m.group(1))

    if idx_fmt == 1:
        idx_count = len(idata) // 4
        indices = list(struct.unpack_from(f"<{idx_count}I", idata))
    else:
        idx_count = len(idata) // 2
        indices = list(struct.unpack_from(f"<{idx_count}H", idata))

    # submesh index count (use first submesh if available)
    m = re.search(r"indexCount:\s*(\d+)", text)
    if m:
        sub_count = int(m.group(1))
        indices = indices[:sub_count]

    triangles = []
    for i in range(0, len(indices) - 2, 3):
        a, b, c = indices[i], indices[i+1], indices[i+2]
        if a < len(vertices) and b < len(vertices) and c < len(vertices):
            triangles.append((a, b, c))

    return vertices, triangles


# ---------------------------------------------------------------------------
# 4.  Math: quaternion rotation, matrix building, world transforms
# ---------------------------------------------------------------------------

def quat_mul(q1, q2):
    x1, y1, z1, w1 = q1
    x2, y2, z2, w2 = q2
    return (
        w1*x2 + x1*w2 + y1*z2 - z1*y2,
        w1*y2 - x1*z2 + y1*w2 + z1*x2,
        w1*z2 + x1*y2 - y1*x2 + z1*w2,
        w1*w2 - x1*x2 - y1*y2 - z1*z2,
    )

def quat_rotate(q, v):
    """Rotate vector v by quaternion q."""
    x, y, z, w = q
    vx, vy, vz = v
    # t = 2 * cross(q.xyz, v)
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    )

def transform_point(pos, rot, scale, point):
    """Apply TRS transform to a point: scale -> rotate -> translate."""
    sx = point[0] * scale[0]
    sy = point[1] * scale[1]
    sz = point[2] * scale[2]
    rx, ry, rz = quat_rotate(rot, (sx, sy, sz))
    return (rx + pos[0], ry + pos[1], rz + pos[2])


def compute_world_transforms(transforms: list[Transform]):
    """Compute world pos/rot/scale for every transform. Returns dict[file_id -> (pos, rot, scale)]."""
    by_id: dict[int, Transform] = {t.file_id: t for t in transforms}
    cache: dict[int, tuple] = {}

    def get_world(fid: int, depth=0):
        if fid in cache:
            return cache[fid]
        if fid == 0 or fid not in by_id or depth > 100:
            result = ((0,0,0), (0,0,0,1), (1,1,1))
            cache[fid] = result
            return result
        t = by_id[fid]
        parent_world = get_world(t.father_id, depth + 1)
        pp, pr, ps = parent_world

        # world_scale = parent_scale * local_scale
        ws = (ps[0]*t.scale[0], ps[1]*t.scale[1], ps[2]*t.scale[2])
        # world_rot = parent_rot * local_rot
        wr = quat_mul(pr, t.rot)
        # world_pos = parent_pos + parent_rot * (parent_scale * local_pos)
        scaled = (t.pos[0]*ps[0], t.pos[1]*ps[1], t.pos[2]*ps[2])
        rotated = quat_rotate(pr, scaled)
        wp = (pp[0]+rotated[0], pp[1]+rotated[1], pp[2]+rotated[2])

        cache[fid] = (wp, wr, ws)
        return cache[fid]

    # eagerly compute all
    for t in transforms:
        get_world(t.file_id)

    return cache


# ---------------------------------------------------------------------------
# 5.  Build world-space triangle soup
# ---------------------------------------------------------------------------

def build_triangle_soup(
    transforms, mesh_cols, box_cols,
    guid_map, mesh_cache,
    world_xforms
):
    # Map gameObject ID -> transform file ID
    go_to_xform: dict[int, int] = {}
    for t in transforms:
        go_to_xform[t.go_id] = t.file_id

    all_tris = []  # list of (v0, v1, v2) as float3 tuples in world space

    # -- MeshColliders --
    loaded = 0
    skipped = 0
    for mc in mesh_cols:
        xf_id = go_to_xform.get(mc.go_id, 0)
        if xf_id == 0:
            skipped += 1
            continue
        wp, wr, ws = world_xforms.get(xf_id, ((0,0,0),(0,0,0,1),(1,1,1)))
        mesh_path = guid_map.get(mc.guid)
        if not mesh_path:
            skipped += 1
            continue

        # Load mesh (cached)
        key = str(mesh_path)
        if key not in mesh_cache:
            result = parse_mesh_asset(mesh_path)
            mesh_cache[key] = result
        mesh_data = mesh_cache[key]
        if mesh_data is None:
            skipped += 1
            continue

        verts, tris = mesh_data
        # Transform vertices to world
        world_verts = [transform_point(wp, wr, ws, v) for v in verts]
        for a, b, c in tris:
            all_tris.append((world_verts[a], world_verts[b], world_verts[c]))
        loaded += 1

    print(f"  MeshColliders: {loaded} loaded, {skipped} skipped")

    # -- BoxColliders --
    loaded_b = 0
    for bc in box_cols:
        xf_id = go_to_xform.get(bc.go_id, 0)
        wp, wr, ws = world_xforms.get(xf_id, ((0,0,0),(0,0,0,1),(1,1,1)))
        cx, cy, cz = bc.center
        hx, hy, hz = bc.size[0]*0.5, bc.size[1]*0.5, bc.size[2]*0.5

        # 8 corners of the box in local space (center-offset)
        corners = [
            (cx-hx, cy-hy, cz-hz), (cx+hx, cy-hy, cz-hz),
            (cx+hx, cy+hy, cz-hz), (cx-hx, cy+hy, cz-hz),
            (cx-hx, cy-hy, cz+hz), (cx+hx, cy-hy, cz+hz),
            (cx+hx, cy+hy, cz+hz), (cx-hx, cy+hy, cz+hz),
        ]
        wc = [transform_point(wp, wr, ws, c) for c in corners]
        # 12 triangles (2 per face)
        faces = [
            (0,1,2),(0,2,3), (4,6,5),(4,7,6),
            (0,4,5),(0,5,1), (2,6,7),(2,7,3),
            (0,3,7),(0,7,4), (1,5,6),(1,6,2),
        ]
        for a, b, c in faces:
            all_tris.append((wc[a], wc[b], wc[c]))
        loaded_b += 1

    print(f"  BoxColliders: {loaded_b} loaded")
    print(f"  Total triangles: {len(all_tris)}")
    return all_tris


# ---------------------------------------------------------------------------
# 6.  BVH builder  (top-down, SAH-based)
# ---------------------------------------------------------------------------

class BVHNode:
    __slots__ = ("aabb_min", "aabb_max", "left", "right", "tri_start", "tri_count")
    def __init__(self):
        self.aabb_min = [1e30, 1e30, 1e30]
        self.aabb_max = [-1e30, -1e30, -1e30]
        self.left = -1
        self.right = -1
        self.tri_start = 0
        self.tri_count = 0


def tri_centroid(tri):
    return (
        (tri[0][0] + tri[1][0] + tri[2][0]) / 3.0,
        (tri[0][1] + tri[1][1] + tri[2][1]) / 3.0,
        (tri[0][2] + tri[1][2] + tri[2][2]) / 3.0,
    )

def tri_aabb(tri):
    mn = [min(tri[0][i], tri[1][i], tri[2][i]) for i in range(3)]
    mx = [max(tri[0][i], tri[1][i], tri[2][i]) for i in range(3)]
    return mn, mx

def aabb_surface_area(mn, mx):
    dx = mx[0] - mn[0]
    dy = mx[1] - mn[1]
    dz = mx[2] - mn[2]
    return 2.0 * (dx*dy + dy*dz + dz*dx)


def build_bvh(triangles: list):
    """Build BVH, returns (nodes[], reordered_triangles[])."""
    n = len(triangles)
    if n == 0:
        return [], []

    centroids = [tri_centroid(t) for t in triangles]
    indices = list(range(n))
    nodes: list[BVHNode] = []

    MAX_LEAF = 8

    def build(start, count) -> int:
        node = BVHNode()
        idx = len(nodes)
        nodes.append(node)

        # Compute AABB over all triangles in range
        for i in range(start, start + count):
            ti = indices[i]
            for v in triangles[ti]:
                for a in range(3):
                    node.aabb_min[a] = min(node.aabb_min[a], v[a])
                    node.aabb_max[a] = max(node.aabb_max[a], v[a])

        if count <= MAX_LEAF:
            node.tri_start = start
            node.tri_count = count
            return idx

        # Find best split axis + position using SAH (binned, 16 bins)
        best_cost = 1e30
        best_axis = 0
        best_pos = 0.0
        parent_area = aabb_surface_area(node.aabb_min, node.aabb_max)
        if parent_area < 1e-10:
            node.tri_start = start
            node.tri_count = count
            return idx

        BINS = 16
        for axis in range(3):
            # bounds of centroids on this axis
            cmin = min(centroids[indices[i]][axis] for i in range(start, start+count))
            cmax = max(centroids[indices[i]][axis] for i in range(start, start+count))
            if cmax - cmin < 1e-8:
                continue

            bin_size = (cmax - cmin) / BINS
            # accumulate per-bin
            bin_counts = [0] * BINS
            bin_aabb_min = [[1e30]*3 for _ in range(BINS)]
            bin_aabb_max = [[-1e30]*3 for _ in range(BINS)]

            for i in range(start, start + count):
                ti = indices[i]
                b = int((centroids[ti][axis] - cmin) / bin_size)
                b = min(b, BINS - 1)
                bin_counts[b] += 1
                tmn, tmx = tri_aabb(triangles[ti])
                for a in range(3):
                    bin_aabb_min[b][a] = min(bin_aabb_min[b][a], tmn[a])
                    bin_aabb_max[b][a] = max(bin_aabb_max[b][a], tmx[a])

            # sweep from left
            left_count = [0] * (BINS - 1)
            left_area = [0.0] * (BINS - 1)
            lmn = [1e30]*3
            lmx = [-1e30]*3
            lc = 0
            for i in range(BINS - 1):
                lc += bin_counts[i]
                left_count[i] = lc
                for a in range(3):
                    lmn[a] = min(lmn[a], bin_aabb_min[i][a])
                    lmx[a] = max(lmx[a], bin_aabb_max[i][a])
                left_area[i] = aabb_surface_area(lmn, lmx) if lc > 0 else 0.0

            # sweep from right
            rmn = [1e30]*3
            rmx = [-1e30]*3
            rc = 0
            for i in range(BINS - 1, 0, -1):
                rc += bin_counts[i]
                for a in range(3):
                    rmn[a] = min(rmn[a], bin_aabb_min[i][a])
                    rmx[a] = max(rmx[a], bin_aabb_max[i][a])
                ra = aabb_surface_area(rmn, rmx) if rc > 0 else 0.0
                cost = left_count[i-1] * left_area[i-1] + rc * ra
                if cost < best_cost and left_count[i-1] > 0 and rc > 0:
                    best_cost = cost
                    best_axis = axis
                    best_pos = cmin + i * bin_size

        # If SAH found no valid split, fall back to median on longest axis
        if best_cost >= 1e29:
            extents = [node.aabb_max[a] - node.aabb_min[a] for a in range(3)]
            best_axis = extents.index(max(extents))
            sub = indices[start:start+count]
            sub.sort(key=lambda ti: centroids[ti][best_axis])
            indices[start:start+count] = sub
            mid = start + count // 2
        else:
            # Partition by SAH split
            mid = start
            end_i = start + count
            i = mid
            while i < end_i:
                if centroids[indices[i]][best_axis] < best_pos:
                    indices[mid], indices[i] = indices[i], indices[mid]
                    mid += 1
                i += 1

            left_count_final = mid - start
            if left_count_final == 0 or left_count_final == count:
                sub = indices[start:start+count]
                sub.sort(key=lambda ti: centroids[ti][best_axis])
                indices[start:start+count] = sub
                mid = start + count // 2

        left_n = mid - start
        right_n = count - left_n
        if left_n == 0 or right_n == 0:
            node.tri_start = start
            node.tri_count = count
            return idx

        node.left = build(start, left_n)
        node.right = build(mid, right_n)
        return idx

    print("  Building BVH...", flush=True)
    build(0, n)

    # Reorder triangles by indices
    reordered = [triangles[indices[i]] for i in range(n)]
    print(f"  BVH: {len(nodes)} nodes")
    return nodes, reordered


# ---------------------------------------------------------------------------
# 7.  Save binary
# ---------------------------------------------------------------------------

def save_bvh_binary(path: str, nodes: list[BVHNode], triangles: list):
    """
    Binary format:
      Header:  "BVH1" (4 bytes) + numNodes(u32) + numTris(u32) = 12 bytes
      Nodes[]:  aabbMin(3f) + aabbMax(3f) + left_or_triStart(u32) + right_or_triCount(u32) = 32 bytes each
        - if tri_count > 0 -> leaf node; left_or_triStart = tri_start, right_or_triCount = tri_count
        - else              -> interior;  left_or_triStart = left child idx, right_or_triCount = right child idx
      Tris[]:   v0(3f) + v1(3f) + v2(3f) = 36 bytes each
    """
    with open(path, "wb") as f:
        f.write(b"BVH1")
        f.write(struct.pack("<II", len(nodes), len(triangles)))

        for node in nodes:
            is_leaf = node.tri_count > 0
            f.write(struct.pack("<fff", *node.aabb_min))
            f.write(struct.pack("<fff", *node.aabb_max))
            if is_leaf:
                f.write(struct.pack("<II", node.tri_start, node.tri_count | 0x80000000))
            else:
                f.write(struct.pack("<II", node.left, node.right))

    # Write triangles in a second pass (keeps memory cleaner for huge files)
    with open(path, "ab") as f:
        for v0, v1, v2 in triangles:
            f.write(struct.pack("<fffffffff", *v0, *v1, *v2))

    file_size = os.path.getsize(path)
    print(f"  Saved {path} ({file_size / 1024 / 1024:.1f} MB)")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    scene_path = Path(sys.argv[1])
    mesh_dir = Path(sys.argv[2])
    output_path = sys.argv[3]

    t0 = time.time()

    print("[1/6] Building GUID -> mesh path map...")
    guid_map = build_guid_map(mesh_dir)
    print(f"  Mapped {len(guid_map)} GUIDs")

    print("[2/6] Parsing scene...")
    transforms, mesh_cols, box_cols = parse_scene(scene_path)

    print("[3/6] Computing world transforms...")
    sys.setrecursionlimit(10000)
    world_xforms = compute_world_transforms(transforms)
    print(f"  Computed {len(world_xforms)} world transforms")

    print("[4/6] Loading meshes + building triangle soup...")
    mesh_cache: dict[str, any] = {}
    all_tris = build_triangle_soup(
        transforms, mesh_cols, box_cols,
        guid_map, mesh_cache, world_xforms
    )

    if not all_tris:
        print("ERROR: No triangles extracted!")
        sys.exit(1)

    # Filter degenerate triangles (NaN, inf, or coords beyond world bounds)
    WORLD_LIMIT = 5000.0
    clean = []
    for tri in all_tris:
        ok = True
        for v in tri:
            for c in v:
                if not math.isfinite(c) or abs(c) > WORLD_LIMIT:
                    ok = False
                    break
            if not ok:
                break
        if ok:
            clean.append(tri)
    if len(clean) < len(all_tris):
        print(f"  Filtered {len(all_tris) - len(clean)} degenerate/out-of-bounds triangles")
    all_tris = clean

    print("[5/6] Building BVH...")
    nodes, reordered = build_bvh(all_tris)

    print("[6/6] Saving binary...")
    save_bvh_binary(output_path, nodes, reordered)

    elapsed = time.time() - t0
    print(f"\nDone in {elapsed:.1f}s")


if __name__ == "__main__":
    main()
