# FreeCAD script: two-part printed case for the ESP32-C3 SuperMini + TFBS4711 gateway.
#
# Run inside FreeCAD (Macro > Execute, or paste into the Python console), or headless:
#   freecadcmd case/make_case.py
# It rebuilds case/irda_gateway_case.FCStd and exports STL/STEP for both parts.
#
# Coordinates (mm): X runs along the board, antenna edge at X=0, USB edge at X=PCB_L.
# Y is across the board, centred. Z is up, PCB underside at Z=0, components on top.
# The hot-glued transceiver hangs under the board and looks out of the -X end.

import os

import FreeCAD as App
import Part
from FreeCAD import Vector as V

# ------------------------------------------------------------------------------
# Measured / estimated hardware dimensions
# ------------------------------------------------------------------------------
PCB_L, PCB_W, PCB_T = 22.5, 18.0, 1.6

USB_W, USB_H = 8.94, 3.2        # USB-C receptacle shell
USB_DEPTH = 7.35
USB_OVERHANG = 1.2              # shell sticks out past the PCB edge by this much

BLOB_FRONT = 4.5                # hot-glue blob reaches this far past the antenna edge
BLOB_W, BLOB_BOTTOM = 10.0, 7.5 # blob width, and how far below the PCB it hangs
LENS_BELOW_PCB = 5.2            # TFBS4711 lens centre below the PCB underside

BUTTON_X = 12.3                 # BOOT / RST centres, from the antenna edge
BUTTON_DY = 2.35                # BOOT at +Y (GPIO side), RST at -Y (5V side)

# ------------------------------------------------------------------------------
# Case parameters
# ------------------------------------------------------------------------------
WALL, FLOOR, LID_T = 1.6, 1.2, 1.2
FIT = 0.15                      # lid lip to wall clearance, per side

CLR_Y = 0.6                     # PCB edge to side wall (room for solder on the pads)
CLR_X = 0.2                     # PCB end play
FRONT_GAP = 5.0                 # cavity ahead of the antenna edge for the blob
BELOW = 9.0                     # cavity under the PCB: blob plus a layer of foam
ABOVE = USB_H + 0.3             # cavity over the PCB, lid rests just above the USB shell

R_IN = 0.8                      # inner vertical corner radius
EDGE_CHAMFER = 0.6

LEDGE_W = 1.5                   # PCB rests on these at the antenna end
LEDGE_LEN = 7.0
STOP_LEN = 1.4                  # posts in front of the PCB take the USB plug push

IR_W, IR_H = 9.0, 5.0           # window at the inner face of the front wall
IR_FLARE = 1.0                  # it widens by this much per side towards the outside

USB_SLOT_W = USB_W + 0.7
USB_RECESS_W, USB_RECESS_H = 13.0, 7.0   # relief for the cable plug overmould
USB_RECESS_D = 0.8

LIP_H, LIP_T = 2.0, 1.2
TAB_T = 1.7                     # lid tabs that hold the PCB down on the ledges
SNAP_X0, SNAP_X1 = 8.0, 17.0
SNAP_DROP = 1.3                 # bead centre below the wall top
SNAP_R, SNAP_OUT = 0.5, 0.25    # bead radius, and how far it bites into the wall

BUTTON_HOLE_D = 2.5
PRY_W, PRY_D, PRY_H = 7.0, 0.8, 0.8

# ------------------------------------------------------------------------------
# Derived
# ------------------------------------------------------------------------------
XI0, XI1 = -FRONT_GAP, PCB_L + CLR_X         # inner cavity, X
YI = PCB_W / 2 + CLR_Y                       # inner cavity, half width
Z_FLOOR = -BELOW
Z_TOP = PCB_T + ABOVE                        # top of the walls, underside of the lid
XO0, XO1, YO = XI0 - WALL, XI1 + WALL, YI + WALL
R_OUT = R_IN + WALL
USB_ZC = PCB_T + USB_H / 2
IR_ZC = -LENS_BELOW_PCB


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def round_edges(shape, axis, r):
    """Fillet every straight edge of shape running along axis (0=X, 1=Y, 2=Z)."""
    edges = []
    for e in shape.Edges:
        if len(e.Vertexes) == 2:
            d = e.Vertexes[1].Point - e.Vertexes[0].Point
            if abs(abs(d[axis]) - d.Length) < 1e-6:
                edges.append(e)
    return shape.makeFillet(r, edges)


def edges_at_z(shape, z):
    return [e for e in shape.Edges if all(abs(v.Point.z - z) < 1e-6 for v in e.Vertexes)]


def ir_window():
    """Rectangular opening through the front wall, flared towards the outside."""
    def section(x):
        g = (XI0 - x) * IR_FLARE / WALL
        hy, z0, z1 = IR_W / 2 + g, IR_ZC - IR_H / 2 - g, IR_ZC + IR_H / 2 + g
        return Part.makePolygon([V(x, -hy, z0), V(x, hy, z0), V(x, hy, z1), V(x, -hy, z1), V(x, -hy, z0)])
    return Part.makeLoft([section(XI0 + 0.2), section(XO0 - 0.2)], True)


def usb_recess():
    """Shallow pocket around the USB opening so the cable plug can seat fully."""
    r = box(XO1 - USB_RECESS_D, XO1 + 1, -USB_RECESS_W / 2, USB_RECESS_W / 2,
            USB_ZC - USB_RECESS_H / 2, USB_ZC + USB_RECESS_H / 2)
    return round_edges(r, 0, 1.5)


def snap_cylinder(side, radius, x0, x1):
    c = V(x0, side * (YI + SNAP_OUT - SNAP_R), Z_TOP - SNAP_DROP)
    return Part.makeCylinder(radius, x1 - x0, c, V(1, 0, 0))


def make_bottom():
    outer = round_edges(box(XO0, XO1, -YO, YO, Z_FLOOR - FLOOR, Z_TOP), 2, R_OUT)
    outer = outer.makeChamfer(EDGE_CHAMFER, edges_at_z(outer, Z_FLOOR - FLOOR))
    cavity = round_edges(box(XI0, XI1, -YI, YI, Z_FLOOR, Z_TOP + 1), 2, R_IN)
    tub = outer.cut(cavity)

    for side in (1, -1):
        y0, y1 = sorted((side * (YI + 0.2), side * (YI - LEDGE_W)))
        tub = tub.fuse(box(-CLR_X, LEDGE_LEN, y0, y1, Z_FLOOR - 0.2, 0))
        tub = tub.fuse(box(-CLR_X - STOP_LEN, -CLR_X, y0, y1, Z_FLOOR - 0.2, PCB_T))

    tub = tub.cut(ir_window())
    # USB slot is open to the top so the board drops straight in; the lid closes it.
    tub = tub.cut(box(XI1 - 0.2, XO1 + 1, -USB_SLOT_W / 2, USB_SLOT_W / 2, PCB_T - 0.15, Z_TOP + 1))
    tub = tub.cut(usb_recess())
    for side in (1, -1):
        tub = tub.cut(snap_cylinder(side, SNAP_R + 0.1, SNAP_X0 - 0.5, SNAP_X1 + 0.5))
    tub = tub.cut(box(BUTTON_X - PRY_W / 2, BUTTON_X + PRY_W / 2, YO - PRY_D, YO + 1, Z_TOP - PRY_H, Z_TOP + 1))
    return tub.removeSplitter()


def make_lid():
    plate = round_edges(box(XO0, XO1, -YO, YO, Z_TOP, Z_TOP + LID_T), 2, R_OUT)
    plate = plate.makeChamfer(EDGE_CHAMFER, edges_at_z(plate, Z_TOP + LID_T))

    lip = round_edges(box(XI0 + FIT, XI1 - FIT, -YI + FIT, YI - FIT, Z_TOP - LIP_H, Z_TOP + 0.2), 2, R_IN - FIT)
    lip = lip.cut(box(XI0 + FIT + LIP_T, XI1 - FIT - LIP_T, -YI + FIT + LIP_T, YI - FIT - LIP_T,
                      Z_TOP - LIP_H - 1, Z_TOP + 1))
    # No lip where the USB connector sits.
    lip = lip.cut(box(XI1 - FIT - LIP_T - 1, XI1, -USB_SLOT_W / 2 - 0.5, USB_SLOT_W / 2 + 0.5,
                      Z_TOP - LIP_H - 1, Z_TOP))
    lid = plate.fuse(lip)

    for side in (1, -1):
        y0, y1 = sorted((side * (YI - FIT), side * (YI - FIT - TAB_T)))
        lid = lid.fuse(box(0.5, LEDGE_LEN, y0, y1, PCB_T + 0.2, Z_TOP + 0.2))
        lid = lid.fuse(snap_cylinder(side, SNAP_R, SNAP_X0, SNAP_X1))

    lid = lid.cut(usb_recess())
    for side in (1, -1):
        lid = lid.cut(Part.makeCylinder(BUTTON_HOLE_D / 2, LID_T + 2,
                                        V(BUTTON_X, side * BUTTON_DY, Z_TOP - 1), V(0, 0, 1)))
    return lid.removeSplitter()


def make_mockup():
    """Rough stand-ins for the electronics, for checking the fit only."""
    pcb = box(0, PCB_L, -PCB_W / 2, PCB_W / 2, 0, PCB_T)
    usb = box(PCB_L + USB_OVERHANG - USB_DEPTH, PCB_L + USB_OVERHANG, -USB_W / 2, USB_W / 2, PCB_T, PCB_T + USB_H)
    usb = round_edges(usb, 0, 1.2)
    blob = box(-BLOB_FRONT, 17.0, -BLOB_W / 2, BLOB_W / 2, -BLOB_BOTTOM, -1.5)
    blob = round_edges(blob, 0, 2.0)
    lens = box(-BLOB_FRONT - 0.05, -BLOB_FRONT + 3.0, -3.0, 3.0, IR_ZC - 0.95, IR_ZC + 0.95)
    return {"PCB": pcb, "USB_C": usb, "HotGlueBlob": blob, "TFBS4711": lens}


def export(shape, path_base):
    import MeshPart
    shape.exportStep(path_base + ".step")
    MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.02, AngularDeflection=0.2).write(path_base + ".stl")


def main():
    out_dir = os.path.dirname(os.path.abspath(__file__))
    name = "irda_gateway_case"
    if name in App.listDocuments():
        App.closeDocument(name)
    doc = App.newDocument(name)

    bottom, lid, mock = make_bottom(), make_lid(), make_mockup()
    objs = {}
    for label, shape in (("CaseBottom", bottom), ("CaseLid", lid)):
        objs[label] = doc.addObject("Part::Feature", label)
        objs[label].Shape = shape
    group = doc.addObject("App::DocumentObjectGroup", "ReferenceMockup")
    for label, shape in mock.items():
        objs[label] = doc.addObject("Part::Feature", label)
        objs[label].Shape = shape
        group.addObject(objs[label])
    doc.recompute()

    if App.GuiUp:
        import FreeCADGui as Gui
        look = {"CaseBottom": ((0.20, 0.45, 0.80), 0), "CaseLid": ((0.90, 0.60, 0.15), 60),
                "PCB": ((0.10, 0.10, 0.10), 0), "USB_C": ((0.75, 0.75, 0.75), 0),
                "HotGlueBlob": ((0.90, 0.85, 0.70), 40), "TFBS4711": ((0.05, 0.05, 0.05), 0)}
        for label, (color, transparency) in look.items():
            objs[label].ViewObject.ShapeColor = color
            objs[label].ViewObject.Transparency = transparency
        Gui.ActiveDocument.ActiveView.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")

    # Print orientation: both parts flat side down on the bed.
    bottom_print = bottom.copy()
    bottom_print.translate(V(0, 0, -bottom.BoundBox.ZMin))
    lid_print = lid.copy()
    lid_print.rotate(V(0, 0, 0), V(1, 0, 0), 180)
    lid_print.translate(V(0, 0, -lid_print.BoundBox.ZMin))
    export(bottom_print, os.path.join(out_dir, "case_bottom"))
    export(lid_print, os.path.join(out_dir, "case_lid"))
    doc.saveAs(os.path.join(out_dir, name + ".FCStd"))

    for label, shape in (("bottom", bottom), ("lid", lid)):
        bb = shape.BoundBox
        print("%s: valid=%s solids=%d volume=%.0f mm3 size=%.1f x %.1f x %.1f"
              % (label, shape.isValid(), len(shape.Solids), shape.Volume, bb.XLength, bb.YLength, bb.ZLength))
    print("bottom/lid overlap: %.3f mm3" % bottom.common(lid).Volume)
    for label, shape in mock.items():
        print("%s overlap: bottom %.3f, lid %.3f mm3"
              % (label, bottom.common(shape).Volume, lid.common(shape).Volume))


main()
