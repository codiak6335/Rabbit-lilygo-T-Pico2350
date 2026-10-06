#!/usr/bin/env python3
"""Parametric printable prototype; dimensions in mm, no waterproof certification.

XY follows the carrier PCB drawing: x right, y down. Assembly Z is up.
STLs are independently placed on Z=0 in their recommended print orientation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from pathlib import Path

import manifold3d as md
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.path import Path as PlotPath
from matplotlib.patches import PathPatch, Rectangle
import numpy as np
import trimesh
from mpl_toolkits.mplot3d.art3d import Poly3DCollection


def rounded_section(width, height, radius, center):
    if min(width, height) <= 2 * radius:
        raise ValueError("Rounded rectangle is smaller than its corner radius")
    return md.CrossSection.square((width - 2 * radius, height - 2 * radius), True).offset(
        radius, circular_segments=96
    ).translate(center)


def box(size, origin):
    return md.Manifold.cube(size).translate(origin)


def cylinder(diameter, height, origin):
    return md.Manifold.cylinder(height, diameter / 2, circular_segments=96).translate(origin)


def mesh(solid):
    if solid.status() != md.Error.NoError:
        raise ValueError(f"Manifold construction failed: {solid.status()}")
    raw = solid.to_mesh()
    return trimesh.Trimesh(vertices=raw.vert_properties[:, :3], faces=raw.tri_verts,
                           process=True)


def on_bed(solid):
    lower = mesh(solid).bounds[0]
    return solid.translate((-lower[0], -lower[1], -lower[2]))


def load_board_reference(p, source_folder):
    """Check the released enclosure datums against the captured Rev D KiCad file."""
    source = source_folder / p["pcb_reference_file"]
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    assert digest == p["pcb_reference_sha256"], "PCB reference changed: review the mechanical datums"
    stack, tree = [], None
    for token in re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', source.read_text()):
        if token == "(":
            item = []
            if stack:
                stack[-1].append(item)
            stack.append(item)
        elif token == ")":
            item = stack.pop()
            if not stack:
                tree = item
        else:
            stack[-1].append(json.loads(token) if token.startswith('"') else token)
    assert not stack and tree[0] == "kicad_pcb"
    footprints, corners, mounts = {}, [], []
    for element in tree:
        if not isinstance(element, list):
            continue
        if element[0] == "gr_line" and ["layer", "Edge.Cuts"] in element:
            corners += [[float(v) for v in q[1:3]] for q in element
                        if isinstance(q, list) and q[0] in ("start", "end")]
        if element[0] == "footprint":
            ref = next(q[2] for q in element if isinstance(q, list) and
                       q[0] == "property" and q[1] == "Reference")
            pos = next([float(v) for v in q[1:3]] for q in element
                       if isinstance(q, list) and q[0] == "at")
            footprints[ref] = pos
            if ref in ("H1", "H2", "H3", "H4"):
                mounts.append(pos)
                pad = next(q for q in element if isinstance(q, list) and q[0] == "pad")
                drill = next(float(q[1]) for q in pad if isinstance(q, list) and q[0] == "drill")
                assert abs(drill - p["carrier_mount_drill"]) < 1e-6
    low, high = np.min(corners, axis=0), np.max(corners, axis=0)
    assert np.allclose(low, (0, 0)) and np.allclose(high, (p["carrier_width"], p["carrier_height"]))
    assert sorted(mounts) == sorted(p["carrier_mount_holes"]), "PCB mounting datums differ"
    lcd_center = np.array(footprints["J3"]) + p["lcd_socket_offset"]
    assert np.allclose(lcd_center, (p["lcd_center_x"], p["lcd_center_y"])), "LCD datum differs from J3"
    expected_rows = {"J3": [3.23, 41.11], "J4": [3.23, 58.89],
                     "J2": [3.23, 71.11], "J1": [3.23, 88.89]}
    assert all(np.allclose(footprints[ref], xy) for ref, xy in expected_rows.items())
    assert np.allclose(footprints["J6"], (12, 5)) and np.allclose(footprints["J5"], (25, 95))
    assert np.allclose(p["esp_body"], [footprints["J3"][0]-1.37, footprints["J3"][1]-4.11, 51, 26])
    assert np.allclose(p["rp_body"], [footprints["J2"][0]-1.37, footprints["J2"][1]-1.61, 51, 21])
    assert np.allclose(p["drok_body"][:2], footprints["MOD1"])
    general = next(q for q in tree if isinstance(q, list) and q[0] == "general")
    thickness = next(float(q[1]) for q in general if isinstance(q, list) and q[0] == "thickness")
    assert abs(thickness - p["carrier_thickness"]) < 1e-6
    return {"revision": p["pcb_revision"], "sha256": digest,
            "board_mm": high.tolist(), "mount_holes_mm": p["carrier_mount_holes"],
            "lcd_center_mm": lcd_center.tolist(), "socket_rows_mm": expected_rows,
            "led_pad_1_mm": footprints["J6"], "power_pad_1_mm": footprints["J5"]}


def make_parts(p):
    w, h = p["carrier_width"], p["carrier_height"]
    left, right = -p["cavity_left_clearance"], w + p["cavity_right_clearance"]
    front, back = -p["cavity_front_clearance"], h + p["cavity_back_clearance"]
    cx, cy = (left + right) / 2, (front + back) / 2
    inner_w, inner_h = right - left, back - front
    rim = p["rim_width"]
    support_z = p["floor_thickness"] + p["pcb_underside_clearance"]
    pcb_top = support_z + p["carrier_thickness"]
    body_h = pcb_top + p["above_pcb_clearance"]
    lid_t = p["lid_thickness"]
    cavity = rounded_section(inner_w, inner_h, 4, (cx, cy))
    outline = rounded_section(inner_w + 2 * rim, inner_h + 2 * rim, 4, (cx, cy))
    body = outline.extrude(body_h) - cavity.extrude(body_h + 1).translate((0, 0, p["floor_thickness"]))

    # Match Rev D's four actual NPTH holes, including its offset lower-right hole.
    for x, y in p["carrier_mount_holes"]:
        body += cylinder(p["carrier_mount_post_diameter"], support_z - p["floor_thickness"],
                         (x, y, p["floor_thickness"]))
        body -= cylinder(p["carrier_mount_pilot_diameter"], support_z - 0.8 + 0.01, (x, y, 0.8))

    # Cord sits entirely in the rim; the flat lid/rim contact is the hard stop.
    gc, gw = p["gasket_center_from_cavity"], p["gasket_groove_width"]
    groove = cavity.offset(gc + gw / 2) - cavity.offset(gc - gw / 2)
    body -= groove.extrude(p["gasket_groove_depth"] + 0.01).translate(
        (0, 0, body_h - p["gasket_groove_depth"]))
    gasket = (cavity.offset(gc + 1) - cavity.offset(gc - 1)).extrude(
        p["gasket_groove_depth"]).translate((0, 0, body_h - p["gasket_groove_depth"]))

    bolts = [(left - rim + 4, front - rim + 4), (right + rim - 4, front - rim + 4),
             (left - rim + 4, back + rim - 4), (right + rim - 4, back + rim - 4)]
    for x, y in bolts:
        body -= cylinder(2.8, 7.5, (x, y, body_h - 7.5))
        body -= cylinder(4.4, 4.2, (x, y, body_h - 4.2))

    # Screen facing the viewer: top is -Y / J6 LED, bottom is +Y / J5 power.
    for entry in p["cable_entries"]:
        top = entry["face"] == "top"
        bore = md.Manifold.cylinder(rim + 0.4, p["entry_hole_diameter"] / 2,
                                   circular_segments=96).rotate((90 if top else -90, 0, 0))
        body -= bore.translate((entry["x"], front + 0.2 if top else back - 0.2, entry["z"]))

    lcd = (p["lcd_center_x"], p["lcd_center_y"])
    view = rounded_section(p["view_width"], p["view_height"], p["view_radius"], lcd)
    fw = p["view_width"] + 2 * p["resin_flange_border"]
    fh = p["view_height"] + 2 * p["resin_flange_border"]
    pocket = rounded_section(fw, fh, 3, lcd)
    lid = outline.extrude(lid_t) - view.extrude(lid_t + 0.4).translate((0, 0, -0.2))
    lid -= pocket.extrude(p["resin_flange_thickness"] + 0.2).translate((0, 0, -0.2))
    resin = (view.extrude(lid_t) + pocket.extrude(p["resin_flange_thickness"]))
    retainer_outer = rounded_section(fw + 2 * p["retainer_border"],
                                    fh + 2 * p["retainer_border"], 2, lcd)
    retainer = (retainer_outer - view.offset(1)).extrude(p["retainer_thickness"])
    # This bead straddles the resin/PETG seam; mechanical capture carries the load.
    seal_w, seal_d = p["window_seal_groove_width"], p["window_seal_groove_depth"]
    window_seal = pocket.offset(seal_w / 2) - pocket.offset(-seal_w / 2)
    retainer -= window_seal.extrude(seal_d + 0.01).translate(
        (0, 0, p["retainer_thickness"] - seal_d))
    # Keep lower retainer screws left of the RP RF exclusion rectangle.
    retainer_bolts = [(lcd[0] + dx, lcd[1] + dy * (fh / 2 + p["retainer_border"] / 2))
                      for dx in p["retainer_screw_x_offsets"] for dy in (-1, 1)]
    for x, y in retainer_bolts:
        lid -= cylinder(1.7, lid_t - 0.8, (x, y, 0))
        retainer -= cylinder(2.2, p["retainer_thickness"] + 0.4, (x, y, -0.2))
    for x, y in bolts:
        lid -= cylinder(3.4, lid_t + 0.4, (x, y, -0.2))
        lid -= cylinder(6.4, 1.8 + 0.2, (x, y, lid_t - 1.8))

    # References are not print files: they are used to verify clearances/render.
    pcb = box((w, h, p["carrier_thickness"]), (0, 0, support_z))
    for x, y in p["carrier_mount_holes"]:
        pcb -= cylinder(p["carrier_mount_drill"], p["carrier_thickness"] + 0.2, (x, y, support_z - 0.1))
    ex, ey, ew, eh = p["esp_body"]
    rx, ry, rw, rh = p["rp_body"]
    ax, ay, aw, ah = p["rp_antenna_body"]
    dx, dy, dw, dh = p["drok_body"]
    esp = box((ew, eh, p["lcd_face_above_pcb"]), (ex, ey, pcb_top))
    rp = box((rw, rh, p["rp_module_height_above_pcb"]), (rx, ry, pcb_top))
    antenna = box((aw, ah, p["rp_module_height_above_pcb"]), (ax, ay, pcb_top))
    drok = box((dw, dh, p["drok_height_above_pcb"]), (dx, dy, pcb_top))
    lcd_glass = box((42.72, 22.70, 0.7),
                    (lcd[0] - 21.36, lcd[1] - 11.35, pcb_top + p["lcd_face_above_pcb"] - 0.7))
    solids = {"body_petg": on_bed(body),
              "lid_frame_petg": on_bed(lid.rotate((180, 0, 0))),
              "window_retainer_petg": on_bed(retainer)}

    # Same aperture and wall thickness, for checking fit and adhesive before a full print.
    coupon = box((32, 20, rim), (0, 0, 0))
    for x in (8, 24):
        coupon -= cylinder(p["entry_hole_diameter"], rim + 0.4, (x, 10, -0.2))
    solids["cable_entry_fit_coupon_petg"] = on_bed(coupon.rotate((90, 0, 0)))
    # Small duplicate of the lid's cast flange and retainer/seal construction.
    coupon_view = rounded_section(20, 14, 1, (0, 0))
    coupon_pocket = rounded_section(28, 22, 3, (0, 0))
    coupon_outer = rounded_section(34, 28, 2, (0, 0))
    cast_coupon = coupon_outer.extrude(lid_t) - coupon_view.extrude(lid_t)
    cast_coupon -= coupon_pocket.extrude(p["resin_flange_thickness"])
    coupon_retainer = (coupon_outer - coupon_view.offset(1)).extrude(p["retainer_thickness"])
    coupon_seal = coupon_pocket.offset(seal_w / 2) - coupon_pocket.offset(-seal_w / 2)
    coupon_retainer -= coupon_seal.extrude(seal_d + 0.01).translate(
        (0, 0, p["retainer_thickness"] - seal_d))
    for x in (-15, 15):
        for y in (-12, 12):
            cast_coupon -= cylinder(1.7, lid_t - 0.8, (x, y, 0))
            coupon_retainer -= cylinder(2.2, p["retainer_thickness"] + 0.2, (x, y, -0.1))
    solids["resin_cast_fit_coupon_petg"] = on_bed(cast_coupon.rotate((180, 0, 0)))
    solids["resin_coupon_retainer_petg"] = on_bed(coupon_retainer)

    def relief(wire, split=False, blank=False):
        neck_end = 2 + rim + p["relief_neck_axial_slack"]
        neck_r, flange_r = p["relief_neck_diameter"] / 2, p["relief_flange_diameter"] / 2
        ramp_top = neck_end + flange_r - neck_r
        tail_base = ramp_top + 1.2
        tail_end = tail_base + p["relief_tail_length"]
        profile = [(0, flange_r), (2, flange_r), (2, neck_r),
                   (neck_end, neck_r), (ramp_top, flange_r), (tail_base, flange_r)]
        if not blank:
            def tail_radius(z):
                return flange_r + (3.3 - flange_r) * (z - tail_base) / (tail_end - tail_base)
            for z in (tail_base + 4, tail_base + 8, tail_base + 12, tail_base + 16):
                profile += [(z - 0.6, tail_radius(z - 0.6)),
                            (z, tail_radius(z) - 0.45),
                            (z + 0.6, tail_radius(z + 0.6))]
            profile += [(tail_end, 3.3)]
        result = md.Manifold()
        for (z0, r0), (z1, r1) in zip(profile, profile[1:]):
            if z1 > z0:
                result += md.Manifold.cylinder(z1 - z0, r0, r1,
                                              circular_segments=96).translate((0, 0, z0))
        length = profile[-1][0]
        if not blank:
            result -= cylinder(wire + p["relief_bore_clearance"], length + 0.4, (0, 0, -0.2))
        if split:
            result -= box((flange_r + 1, p["wrap_seam_width"], length + 0.4),
                          (0, -p["wrap_seam_width"] / 2, -0.2))
        return result

    for wire in p["wire_diameters"]:
        tag = f"{wire:.1f}".replace(".", "_")
        for split in (False, True):
            solids[f"strain_relief_{tag}mm_{'split_wrap' if split else 'closed'}_tpu"] = on_bed(relief(wire, split))
    solids["unused_entry_plug_tpu"] = on_bed(relief(0, blank=True))
    reliefs = []
    for entry in p["cable_entries"]:
        top = entry["face"] == "top"
        reliefs.append(relief(3.5).rotate((90 if top else -90, 0, 0)).translate(
            (entry["x"], front + 2 if top else back - 2, entry["z"])))
    metal_envelopes = [cylinder(6.4, 1, (x, y, 0)) for x, y in bolts]
    metal_envelopes += [cylinder(4, 1, (x, y, 0)) for x, y in retainer_bolts]
    metal_envelopes += [cylinder(6, 1, (x, y, 0)) for x, y in p["carrier_mount_holes"]]
    meta = {"body_height": body_h, "closed_height": body_h + lid_t,
            "outer_width": inner_w + 2 * rim, "outer_height": inner_h + 2 * rim,
            "pcb_bottom_z": support_z, "pcb_top_z": pcb_top,
            "resin_volume_ml": resin.volume() / 1000,
            "gasket_cut_length_mm": 2 * (inner_w + 2 * gc + inner_h + 2 * gc - 4 * (4 + gc)) + 2 * math.pi * (4 + gc),
            "cable_glue_gap_radial_mm": (p["entry_hole_diameter"] - p["relief_neck_diameter"]) / 2}
    refs = dict(body=body, lid=lid.translate((0, 0, body_h)),
                resin=resin.translate((0, 0, body_h)),
                retainer=retainer.translate((0, 0, body_h - p["retainer_thickness"])),
                pcb=pcb, esp=esp, rp=rp, antenna=antenna, drok=drok, lcd=lcd_glass, gasket=gasket,
                metal_envelopes=metal_envelopes, reliefs=reliefs, outline=outline, cavity=cavity,
                view=view, pocket=pocket, bolts=bolts, retainer_bolts=retainer_bolts)
    return solids, refs, meta


def validate(p, solids, refs, meta):
    checks = {}
    for name, solid in solids.items():
        m = mesh(solid)
        components = len(m.split(only_watertight=False))
        assert m.is_watertight and m.is_winding_consistent and m.volume > 0, name
        assert components == 1, (name, components)
        assert abs(m.bounds[0, 2]) < 1e-5, (name, "not on print bed")
        checks[name] = {"watertight_mesh": True, "connected_bodies": components,
                        "dimensions_mm": np.round(m.extents, 3).tolist(),
                        "volume_ml": round(m.volume / 1000, 3), "triangles": len(m.faces)}
    pairs = [("body", "pcb"), ("body", "esp"), ("body", "rp"), ("body", "antenna"), ("body", "drok"),
             ("lid", "resin"), ("retainer", "resin"), ("body", "lid"),
             ("body", "retainer"), ("retainer", "esp"), ("lid", "esp"),
             ("retainer", "rp"), ("retainer", "antenna"), ("lid", "drok")]
    for a, b in pairs:
        overlap = (refs[a] ^ refs[b]).volume()
        assert overlap < 1e-6, (a, b, overlap)
    for item in refs["reliefs"]:
        for key in ("body", "pcb", "esp", "rp", "antenna", "drok", "retainer"):
            assert (item ^ refs[key]).volume() < 1e-6, ("relief collision", key)
    # Verify lid pocket and mounting bores remain inside the continuous gasket.
    inside = refs["cavity"].extrude(1)
    assert (refs["pocket"].extrude(1) - inside).volume() < 1e-6, "pocket crosses gasket"
    for x, y in refs["retainer_bolts"]:
        assert (cylinder(1.7, 1, (x, y, 0)) - inside).volume() < 1e-6
    assert p["gasket_groove_width"] > p["gasket_cord_diameter"]
    assert 0.20 <= 1 - p["gasket_groove_depth"] / p["gasket_cord_diameter"] <= 0.30
    assert meta["cable_glue_gap_radial_mm"] > 0
    assert {e["face"] for e in p["cable_entries"]} == {"top", "bottom"}
    assert next(e for e in p["cable_entries"] if e["role"] == "LED")["face"] == "top"
    assert next(e for e in p["cable_entries"] if e["role"] == "POWER")["face"] == "bottom"
    for entry in p["cable_entries"]:
        radius = p["relief_flange_diameter"] / 2
        assert entry["z"] - radius > p["floor_thickness"]
        assert entry["z"] + radius < meta["body_height"] - p["gasket_groove_depth"]
        assert entry["x"] - radius > 0 and entry["x"] + radius < p["carrier_width"]
    for region in p["rf_keepouts"]:
        x0, y0, x1, y1 = region["bounds"]
        zone = box((x1-x0, y1-y0, 1), (x0, y0, 0))
        for hardware in refs["metal_envelopes"]:
            assert (zone ^ hardware).volume() < 1e-6, ("metal in RF exclusion", region["name"])
    for x, y in refs["bolts"]:
        assert (cylinder(4.4, p["gasket_groove_depth"],
                         (x, y, meta["body_height"] - p["gasket_groove_depth"])) ^ refs["gasket"]).volume() < 1e-6
    aa = box((42.72, 22.70, 1), (p["lcd_center_x"] - 21.36, p["lcd_center_y"] - 11.35, 0))
    assert (aa - refs["view"].extrude(1)).volume() < 1e-6, "window obscures active LCD"
    return {"units": "mm", "parameters": p, "dimensions": meta,
            "mesh_checks": checks,
            "assembly_checks": "PASS: Rev D mounts/PCB/module/gland clearances; gasket/window isolated from fasteners; case metal clears both RF exclusions",
            "limitations": "Geometric checks only. Carrier and stack dimensions require physical confirmation; waterproof performance is untested."}


def draw(ax, solid, color, alpha=1):
    m = mesh(solid)
    collection = Poly3DCollection(m.triangles, facecolors=color, edgecolors="none", alpha=alpha)
    ax.add_collection3d(collection)


def draw_section(ax, section, color, edge="#365766"):
    paths = []
    for contour in section.to_polygons():
        points = np.vstack((contour, contour[0]))
        codes = [PlotPath.MOVETO] + [PlotPath.LINETO] * (len(contour) - 1) + [PlotPath.CLOSEPOLY]
        paths.append(PlotPath(points, codes))
    ax.add_patch(PathPatch(PlotPath.make_compound_path(*paths), facecolor=color,
                          edgecolor=edge, linewidth=0.65))


def preview(output, solids, refs, meta):
    fig = plt.figure(figsize=(15, 8))
    plan = fig.add_subplot(121)
    draw_section(plan, refs["body"].slice(meta["body_height"] - 0.5), "#9cc8da")
    draw_section(plan, refs["gasket"].slice(meta["body_height"] - 0.5), "#eac269")
    draw_section(plan, refs["pcb"].slice(meta["pcb_bottom_z"] + 0.8), "#579966")
    for key, color in [("esp", "#37444e"), ("rp", "#37444e"),
                       ("antenna", "#8fb69a"), ("drok", "#709cc5"), ("lcd", "#35d1e5")]:
        m = mesh(refs[key]); lo, hi = m.bounds
        plan.add_patch(Rectangle(lo[:2], *(hi-lo)[:2], facecolor=color, edgecolor="#333", linewidth=0.5))
    for part, entry in zip(refs["reliefs"], refs["parameters"]["cable_entries"]):
        draw_section(plan, part.slice(entry["z"]), "#393e45")
        bounds = mesh(part).bounds
        top = entry["face"] == "top"
        label_y = bounds[0,1] - 3 if top else bounds[1,1] + 5
        plan.text(entry["x"], label_y, entry["role"], ha="center", fontsize=9, fontweight="bold")
    for label, point in [("ESP32 + display", (30, 50)), ("RP2350", (27.36, 80)), ("DROK", (46, 18.25))]:
        plan.text(*point, label, ha="center", va="center", color="white", fontsize=8)
    for x in (12, 18, 24, 30):
        draw_section(plan, md.CrossSection.circle(1.4, 32).translate((x, 5)), "#eac269")
    for x in (25, 31):
        draw_section(plan, md.CrossSection.circle(1.4, 32).translate((x, 95)), "#eac269")
    for x, y in refs["parameters"]["carrier_mount_holes"]:
        draw_section(plan, md.CrossSection.circle(1.35, 32).translate((x, y)), "white")
    for region in refs["parameters"]["rf_keepouts"]:
        x0, y0, x1, y1 = region["bounds"]
        plan.add_patch(Rectangle((x0, y0), x1-x0, y1-y0, fill=False,
                                 edgecolor="#9d7dc1", hatch="///", linewidth=0.6))
    plan.set(xlim=(-14, 75), ylim=(147, -47), xlabel="Rev D carrier X (mm)", ylabel="Rev D carrier Y (mm)")
    plan.set_aspect("equal")
    plan.set_title("Rev D — top LED / bottom power / actual mounting holes")
    ax = fig.add_subplot(122, projection="3d")
    for name, color in [("body", "#458bb0"), ("pcb", "#388348"),
                        ("esp", "#37444e"), ("rp", "#37444e"),
                        ("antenna", "#8fb69a"), ("drok", "#709cc5"), ("lcd", "#20c9e8")]:
        draw(ax, refs[name], color)
    for item in refs["reliefs"]:
        draw(ax, item, "#393e45")
    draw(ax, refs["gasket"], "#eac269")
    draw(ax, refs["lid"].translate((0, 0, 28)), "#75adc6")
    draw(ax, refs["resin"].translate((0, 0, 28)), "#86e0ed", 0.42)
    draw(ax, refs["retainer"].translate((0, 0, 13)), "#aac1cc")
    ax.set(xlim=(-14, 75), ylim=(-42, 142), zlim=(0, 66), xlabel="X (mm)", ylabel="Y (mm)", zlabel="Z (mm)")
    ax.set_box_aspect((89, 184, 66))
    ax.view_init(elev=62, azim=-50)
    ax.set_title("Exploded lid, window and retainer", pad=16)
    fig.suptitle(f"Resin-window housing — {meta['outer_width']:g} × {meta['outer_height']:g} × {meta['closed_height']:g} mm closed", fontsize=17)
    fig.text(0.03, 0.05, "Blue: PETG shell/lid/retainer   Cyan: cast resin   Gold: silicone gasket   Purple hatch: RF exclusions\nPCB datums checked against the captured Rev D KiCad file. Module heights and waterproof performance need physical qualification.", fontsize=10)
    fig.subplots_adjust(bottom=0.17, top=0.87, wspace=0.08)
    fig.savefig(output / "assembly_preview.png", dpi=170, bbox_inches="tight")
    plt.close(fig)
    fig = plt.figure(figsize=(13, 6))
    for i, variant in enumerate(("closed", "split_wrap"), 1):
        ax = fig.add_subplot(1, 3, i)
        draw_section(ax, solids[f"strain_relief_4_0mm_{variant}_tpu"].slice(1), "#7fa7b2")
        ax.set(xlim=(-1, 13), ylim=(-1, 13), xlabel="mm", ylabel="mm")
        ax.set_aspect("equal")
        ax.set_title(variant.replace("_", " ") + " — end view")
        ax.text(6, -0.5, "Ø12 flange / Ø4.2 bore", ha="center", fontsize=9)
    ax = fig.add_subplot(133)
    draw_section(ax, solids["strain_relief_4_0mm_closed_tpu"].rotate((90, 0, 0)).slice(6), "#7fa7b2")
    ax.set(xlim=(-1, 13), ylim=(-35, 1), xlabel="mm", ylabel="mm")
    ax.set_aspect("equal")
    ax.set_title("Closed — axial section")
    fig.suptitle("TPU strain reliefs — 2.3, 3.0, 3.5 and 4.0 mm cable jackets\nØ7.4 mm neck; Ø8.6 mm housing bore; 0.6 mm radial adhesive space")
    fig.subplots_adjust(top=0.77, wspace=0.4)
    fig.savefig(output / "strain_relief_preview.png", dpi=170, bbox_inches="tight")
    plt.close(fig)


def drawing(output, p, meta):
    # Dimensioned plan is drawn as SVG so it stays crisp and editable.
    x0, y0 = -p["cavity_left_clearance"] - p["rim_width"], -p["cavity_front_clearance"] - p["rim_width"]
    vw, vh = meta["outer_width"], meta["outer_height"]
    tx, ty = 28 - x0, 40 - y0
    vx, vy = p["lcd_center_x"] + tx, p["lcd_center_y"] + ty
    fw = p["view_width"] + 2 * p["resin_flange_border"]
    fh = p["view_height"] + 2 * p["resin_flange_border"]
    ports = ''
    for entry in p["cable_entries"]:
        top = entry["face"] == "top"
        px = entry["x"] + tx
        py = 40 if top else 40 + vh
        end = py - 12 if top else py + 12
        label_y = end - 3 if top else end + 5
        ports += f'<path d="M{px} {py} V{end}" stroke="#444" stroke-width="1"/><text x="{px}" y="{label_y}" text-anchor="middle" font-size="3.5">{entry["role"]} Ø{p["entry_hole_diameter"]:g}</text>'
    mounts = ''.join(f'<circle cx="{x+tx}" cy="{y+ty}" r="1.35" fill="white" stroke="#555" stroke-width="0.3"/>' for x,y in p["carrier_mount_holes"])
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="200mm" height="260mm" viewBox="0 0 200 260">
<rect width="200" height="260" fill="white"/>
<g font-family="sans-serif" fill="#203643"><text x="14" y="12" font-size="5">Rev D resin-window housing — dimensions (mm)</text>
<rect x="28" y="40" width="{vw}" height="{vh}" rx="4" fill="#ebf4f8" stroke="#356c8b" stroke-width="0.5"/>
<rect x="{tx}" y="{ty}" width="{p['carrier_width']}" height="{p['carrier_height']}" fill="none" stroke="#398745" stroke-dasharray="2 1" stroke-width="0.35"/>
<rect x="{vx-p['view_width']/2}" y="{vy-p['view_height']/2}" width="{p['view_width']}" height="{p['view_height']}" rx="1" fill="#b8eafa" stroke="#356c8b" stroke-width="0.4"/>
<text x="{vx}" y="{vy+1}" text-anchor="middle" font-size="4">{p['view_width']:g} × {p['view_height']:g} view</text>{ports}{mounts}
<path d="M28 34 H{28+vw} M28 31 V37 M{28+vw} 31 V37" fill="none" stroke="#333" stroke-width="0.3"/>
<text x="{28+vw/2}" y="32" text-anchor="middle" font-size="4">{vw:g}</text>
<path d="M22 40 V{40+vh} M19 40 H25 M19 {40+vh} H25" fill="none" stroke="#333" stroke-width="0.3"/>
<text x="17" y="{40+vh/2}" text-anchor="middle" font-size="4" transform="rotate(-90 17 {40+vh/2})">{vh:g}</text>
<text x="28" y="201" font-size="4">Closed height: {meta['closed_height']:g}; floor: {p['floor_thickness']:g}; lid: {p['lid_thickness']:g}</text>
<text x="28" y="208" font-size="4">Cast flange: {fw:g} × {fh:g} × {p['resin_flange_thickness']:g}; clear centre: {p['lid_thickness']:g} thick</text>
<text x="28" y="215" font-size="4">Rev D PCB: {p['carrier_width']:g} × {p['carrier_height']:g}; {p['above_pcb_clearance']:g} above PCB / {p['above_pcb_clearance']-p['retainer_thickness']:g} below retainer</text>
<text x="28" y="222" font-size="4">LCD centre: PCB (30,50); LED entry X=21; power entry X=28</text>
<text x="28" y="229" font-size="4">Both cable axes Z=15; 4 × Ø2.7 PCB mounts, lower-right offset</text>
<text x="28" y="242" font-size="3.5">PCB datums checked against captured Rev D file. Verify assembled heights.</text></g></svg>'''
    (output / "dimensions.svg").write_text(svg)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--parameters", type=Path, default=Path(__file__).with_name("parameters.json"))
    parser.add_argument("--output", type=Path, default=Path(__file__).parent)
    args = parser.parse_args()
    p = json.loads(args.parameters.read_text())
    source_check = load_board_reference(p, Path(__file__).parent)
    solids, refs, meta = make_parts(p)
    refs["parameters"] = p
    report = validate(p, solids, refs, meta)
    report["pcb_reference_check"] = source_check
    args.output.mkdir(parents=True, exist_ok=True)
    stl = args.output / "stl"
    stl.mkdir(exist_ok=True)
    for name, solid in solids.items():
        target = stl / f"{name}.stl"
        mesh(solid).export(target)
        reopened = trimesh.load_mesh(target, process=True)
        assert reopened.is_watertight and reopened.is_winding_consistent
        assert len(reopened.split(only_watertight=False)) == 1
    (args.output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    preview(args.output, solids, refs, meta)
    drawing(args.output, p, meta)
    print(json.dumps({"STLs": len(solids), "dimensions_mm": [meta['outer_width'], meta['outer_height'], meta['closed_height']],
                      "resin_ml": meta['resin_volume_ml'], "checks": "passed"}))


if __name__ == "__main__":
    main()
