#!/usr/bin/env python3
"""P17: drop the vop's reg-less 'endpoint' (the hdmi0 one) from a
decompiled rk3588-qemu DTB text.

Background: of_graph_get_endpoint_by_regs(port, 0, -1) returns the
FIRST endpoint child of the vop's port@0.  In the eDP-only DTB that
child is the hdmi endpoint, whose remote node (hdmi0) is
status="disabled"; of_graph_get_remote_node() then returns NULL and
rockchip's vop2_create_crtcs() skips vp0 entirely -> zero CRTCs ->
"Bogus possible_crtcs ... full crtc mask=0x0" and fbdev's
"Cannot find any crtc or sizes".  Deleting the hdmi endpoint from the
DERIVED dtb (never from the source dts) makes endpoint@3 the only
child, whose remote (edp0) is enabled.

Usage: edp-drop-hdmi-ep.py IN.dts OUT.dts
The dtc-decompiled text is indent-structured, so the reg-less
`endpoint { ... };` block is found by indentation inside the
vop@fdd90000 node extent.
"""
import sys


def indent(line: str) -> int:
    return len(line) - len(line.lstrip())


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        lines = f.read().splitlines(keepends=True)

    # locate the vop node
    vi = None
    for i, l in enumerate(lines):
        if l.strip().startswith("vop@fdd90000 {"):
            vi = i
            break
    if vi is None:
        print("vop@fdd90000 node not found", file=sys.stderr)
        return 1
    vop_ind = indent(lines[vi])

    # vop node extent: first line at <= vop indent that closes it
    vend = None
    for i in range(vi + 1, len(lines)):
        if indent(lines[i]) <= vop_ind and lines[i].strip() == "};":
            vend = i
            break
    if vend is None:
        print("vop node end not found", file=sys.stderr)
        return 1

    # the hdmi0 endpoint child (ROCKCHIP_VOP2_EP_HDMI0 = 2) inside the
    # vop's port@0 — dtc names it endpoint@2 (it carries reg = <2>)
    ei = None
    for i in range(vi + 1, vend):
        if lines[i].strip() in ("endpoint@2 {", "endpoint {"):
            ei = i
            break
    if ei is None:
        print("hdmi endpoint not found in vop", file=sys.stderr)
        return 1
    ep_ind = indent(lines[ei])
    ej = None
    for i in range(ei + 1, vend):
        if indent(lines[i]) == ep_ind and lines[i].strip() == "};":
            ej = i
            break
    if ej is None:
        print("endpoint block end not found", file=sys.stderr)
        return 1

    removed = "".join(lines[ei:ej + 1])
    del lines[ei:ej + 1]
    with open(dst, "w") as f:
        f.writelines(lines)
    print(f"removed endpoint block:\n{removed}", end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())