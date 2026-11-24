"""Run the shared local DXIL toolchain over verified shader bytes."""

from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


def prepare_container(data, output, transform, *, dxc, dxa, dxv):
    with TemporaryDirectory() as temporary:
        work = Path(temporary)
        source = work / "input.dxil"
        ir = work / "shader.ll"
        assembled = work / "shader.dxil"
        validated = work / "validated.dxil"
        # Disassemble the same bytes the caller matched to its build profile.
        source.write_bytes(data)
        text = subprocess.check_output([dxc, "-dumpbin", str(source)], text=True, timeout=30)
        ir.write_text(transform(text), encoding="utf-8")
        subprocess.run([dxa, f"-o={assembled}", str(ir)], timeout=30, check=True)
        subprocess.run([dxv, "-o", str(validated), str(assembled)], timeout=30, check=True)
        with output.open("xb") as target:
            target.write(validated.read_bytes())
