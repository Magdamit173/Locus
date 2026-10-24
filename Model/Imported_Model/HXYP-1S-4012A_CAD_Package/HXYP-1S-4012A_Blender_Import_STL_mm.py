import bpy

# Run this script in Blender's Scripting workspace after changing STL_PATH below.
# It imports a millimeter-coordinate STL and scales it into Blender's meter-based scene.
STL_PATH = r"C:\\path\\to\\HXYP-1S-4012A_Replica.stl"

bpy.context.scene.unit_settings.system = 'METRIC'
bpy.context.scene.unit_settings.length_unit = 'MILLIMETERS'
bpy.ops.wm.stl_import(filepath=STL_PATH)
obj = bpy.context.object
obj.scale = (0.001, 0.001, 0.001)
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
print("Model dimensions in mm:", tuple(round(v * 1000, 4) for v in obj.dimensions))
