#!/usr/bin/env python3
"""
Render a top-down 2D map PNG from a BVH collision file.

Usage:
    python bvh_to_map.py <input.bvh> <output.png> [size] [--bounds xmin xmax zmin zmax]

Reads the triangle soup, projects each triangle onto the XZ plane,
and rasterizes edges + filled regions into a PNG.
"""

import sys, struct, math
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Requires Pillow: pip install Pillow")
    sys.exit(1)


def load_bvh_triangles(path: str):
    """Read just the triangle data from a BVH1 binary."""
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"BVH1":
            print(f"Bad magic: {magic}")
            return []
        num_nodes, num_tris = struct.unpack("<II", f.read(8))
        # Skip nodes (32 bytes each)
        f.seek(12 + num_nodes * 32)
        # Read triangles (36 bytes each: 3 vertices x 3 floats)
        tris = []
        for _ in range(num_tris):
            data = f.read(36)
            if len(data) < 36:
                break
            vals = struct.unpack("<9f", data)
            v0 = (vals[0], vals[1], vals[2])
            v1 = (vals[3], vals[4], vals[5])
            v2 = (vals[6], vals[7], vals[8])
            tris.append((v0, v1, v2))
    print(f"Loaded {len(tris)} triangles")
    return tris


def compute_bounds(tris):
    """Compute world XZ bounds from all triangle vertices."""
    xmin = zmin = float("inf")
    xmax = zmax = float("-inf")
    for v0, v1, v2 in tris:
        for v in (v0, v1, v2):
            xmin = min(xmin, v[0])
            xmax = max(xmax, v[0])
            zmin = min(zmin, v[2])
            zmax = max(zmax, v[2])
    return xmin, xmax, zmin, zmax


def render_map(tris, size=2048, bounds=None, bg=(15, 18, 25),
               wall_color=(90, 140, 210), floor_color=(35, 42, 55),
               y_filter=None):
    """Render triangles as a top-down XZ image.

    y_filter: (ymin, ymax) — only include triangles with at least one vertex
              in this Y range. Useful for filtering ground-level geometry.
    """
    if bounds:
        xmin, xmax, zmin, zmax = bounds
    else:
        xmin, xmax, zmin, zmax = compute_bounds(tris)

    # Add small margin
    margin = max(xmax - xmin, zmax - zmin) * 0.02
    xmin -= margin
    xmax += margin
    zmin -= margin
    zmax += margin

    world_w = xmax - xmin
    world_h = zmax - zmin

    # Square output
    scale = (size - 1) / max(world_w, world_h)

    img_w = int(world_w * scale) + 1
    img_h = int(world_h * scale) + 1

    img = Image.new("RGBA", (img_w, img_h), bg + (255,))
    draw = ImageDraw.Draw(img)

    def world_to_px(x, z):
        px = (x - xmin) * scale
        py = (zmax - z) * scale
        return (int(px), int(py))

    def in_bounds(v):
        return xmin <= v[0] <= xmax and zmin <= v[2] <= zmax

    def xz_edge_len_sq(a, b):
        dx = a[0] - b[0]; dz = a[2] - b[2]
        return dx*dx + dz*dz

    # Max XZ edge length — skip huge terrain/roof triangles
    max_edge = 20.0  # units; building walls are small
    max_edge_sq = max_edge * max_edge

    count = 0
    for v0, v1, v2 in tris:
        if not (in_bounds(v0) or in_bounds(v1) or in_bounds(v2)):
            continue

        if y_filter:
            ylo, yhi = y_filter
            if not (ylo <= v0[1] <= yhi or ylo <= v1[1] <= yhi or ylo <= v2[1] <= yhi):
                continue

        # Skip large triangles (terrain, roofs)
        if (xz_edge_len_sq(v0, v1) > max_edge_sq or
            xz_edge_len_sq(v1, v2) > max_edge_sq or
            xz_edge_len_sq(v2, v0) > max_edge_sq):
            continue

        p0 = world_to_px(v0[0], v0[2])
        p1 = world_to_px(v1[0], v1[2])
        p2 = world_to_px(v2[0], v2[2])

        if p0 == p1 == p2:
            continue

        # Compute face normal to skip floors
        e1 = (v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2])
        e2 = (v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2])
        ny = e1[2]*e2[0] - e1[0]*e2[2]
        nx = e1[1]*e2[2] - e1[2]*e2[1]
        nz = e1[0]*e2[1] - e1[1]*e2[0]
        nlen = math.sqrt(nx*nx + ny*ny + nz*nz)
        if nlen < 1e-8:
            continue
        if abs(ny / nlen) > 0.7:
            continue

        draw.line([p0, p1], fill=wall_color + (210,), width=1)
        draw.line([p1, p2], fill=wall_color + (210,), width=1)
        draw.line([p2, p0], fill=wall_color + (210,), width=1)
        count += 1

    print(f"Rendered {count} triangles at {img_w}x{img_h}")
    print(f"World bounds: X[{xmin:.0f}, {xmax:.0f}] Z[{zmin:.0f}, {zmax:.0f}]")
    return img


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    bvh_path = sys.argv[1]
    out_path = sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 2048

    bounds = None
    if "--bounds" in sys.argv:
        bi = sys.argv.index("--bounds")
        bounds = tuple(float(sys.argv[bi + 1 + i]) for i in range(4))

    y_filter = None
    if "--yfilter" in sys.argv:
        yi = sys.argv.index("--yfilter")
        y_filter = (float(sys.argv[yi + 1]), float(sys.argv[yi + 2]))

    tris = load_bvh_triangles(bvh_path)
    if not tris:
        print("No triangles to render")
        sys.exit(1)

    img = render_map(tris, size=size, bounds=bounds, y_filter=y_filter)
    img.save(out_path)
    print(f"Saved {out_path}")


if __name__ == "__main__":
    main()
