from pathlib import Path
import runpy
import unittest


lower = runpy.run_path(str(Path(__file__).parents[1] / "probe" / "dxil-legacy-bindings.py"))["lower_fixed_bindings"]
ASSEMBLY = """\
define void @main() {
  %1 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 7, i32 18, i32 2, i8 1 }, i32 9, i1 true)
  %2 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %1, %dx.types.ResourceProperties { i32 4106, i32 1029 })
  ret void
}
declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties)
!dx.version = !{!0}
!dx.valver = !{!0}
!dx.shaderModel = !{!1}
!dx.resources = !{!2}
!dx.entryPoints = !{!5}
!0 = !{i32 1, i32 6}
!1 = !{!"cs", i32 6, i32 6}
!2 = !{null, !3, null, null}
!3 = !{!4}
!4 = !{i32 5, %Buffer* undef, !"data", i32 2, i32 7, i32 12, i32 10}
!5 = !{void ()* @main, !"main", null, !2, !6}
!6 = !{i32 0, i64 32784, i32 4, !7}
!7 = !{i32 1, i32 1, i32 1}
"""


class DxilLegacyBindingsTest(unittest.TestCase):
    def test_uses_metadata_range_and_preserves_register_index_and_nonuniform_flag(self):
        output = lower(ASSEMBLY)
        call = "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 5, i32 9, i1 true)"
        self.assertIn("%1 = " + call, output)
        self.assertIn("%2 = " + call, output)
        self.assertNotIn("@dx.op.annotateHandle", output)
        self.assertIn('!1 = !{!"cs", i32 6, i32 4}', output)
        self.assertIn("!6 = !{i32 0, i64 16, i32 4, !7}", output)

    def test_rejects_a_range_that_does_not_match_declared_resource_metadata(self):
        with self.assertRaisesRegex(ValueError, "declared DXIL range"):
            lower(ASSEMBLY.replace("i32 18, i32 2", "i32 17, i32 2"))

    def test_rejects_mesh_and_heap_bindings(self):
        with self.assertRaisesRegex(ValueError, "Shader Model 6.6"):
            lower(ASSEMBLY.replace('!"cs"', '!"ms"'))
        with self.assertRaisesRegex(ValueError, "heap or library"):
            lower(ASSEMBLY.replace("createHandleFromBinding", "createHandleFromHeap"))
