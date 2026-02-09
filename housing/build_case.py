import bpy
import bmesh
from math import radians
from mathutils import Vector

# Clean scene
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()


def create_triangular_housing():
    # Create triangle base as mesh
    bm = bmesh.new()

    # Triangle points
    A = Vector((0, 0, 0))
    B = Vector((5, 0, 0))
    C = Vector((2.5, 4.3, 0))  # Approx equilateral triangle

    vA = bm.verts.new(A)
    vB = bm.verts.new(B)
    vC = bm.verts.new(C)
    bm.faces.new([vA, vB, vC])

    # Extrude upward to give it thickness
    mesh = bpy.data.meshes.new("TriangleBase")
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("Housing", mesh)
    bpy.context.collection.objects.link(obj)

    # Extrude to prism
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0, 0, 2)})
    bpy.ops.object.mode_set(mode="OBJECT")

    # Bevel edges
    mod_bevel = obj.modifiers.new("Bevel", type="BEVEL")
    mod_bevel.width = 0.2
    mod_bevel.segments = 5
    mod_bevel.limit_method = "ANGLE"
    mod_bevel.angle_limit = radians(15)

    # Hollow it out
    mod_solidify = obj.modifiers.new("Solidify", type="SOLIDIFY")
    mod_solidify.thickness = -0.2  # inward wall

    # Apply modifiers
    bpy.ops.object.convert(target="MESH")

    return obj


def create_lens_hole(location):
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.3, depth=2.5, location=location)
    cyl = bpy.context.active_object
    cyl.rotation_euler[1] = radians(90)  # point forward
    return cyl


def boolean_subtract(target, cutter):
    mod = target.modifiers.new(name="Boolean", type="BOOLEAN")
    mod.object = cutter
    mod.operation = "DIFFERENCE"
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(cutter)


def create_lid(housing_obj):
    # Duplicate top face region
    bpy.ops.object.select_all(action="DESELECT")
    housing_obj.select_set(True)
    bpy.context.view_layer.objects.active = housing_obj
    bpy.ops.object.duplicate()
    lid = bpy.context.active_object
    lid.name = "Lid"

    # Shrinkwrap/offset upward slightly and scale
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0, 0, 0.1)})
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0, 0, 0.3)})
    bpy.ops.object.mode_set(mode="OBJECT")

    return lid


# --- Run creation ---
housing = create_triangular_housing()

# Create lens holes
lens1 = create_lens_hole(location=(1.2, -0.1, 1))
lens2 = create_lens_hole(location=(3.8, -0.1, 1))
boolean_subtract(housing, lens1)
boolean_subtract(housing, lens2)

# Create lid
lid = create_lid(housing)

# Optionally separate lid from base slightly
lid.location.z += 0.1
