#!/usr/bin/env python3
"""
Convert the research NPZ atlas into a runtime-oriented binary file plus JSON metadata.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct

import numpy as np


MAGIC = 0x3154415053495654  # "TVISPAT1"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("trajectory_atlas.npz"),
        help="Input NPZ atlas path",
    )
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path("tests/data/variable_isp"),
        help="Directory for the exported runtime atlas",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    bundle = np.load(args.input)
    rho = np.asarray(bundle["rho"], dtype=np.float64)
    kappa = np.asarray(bundle["kappa"], dtype=np.float64)
    theta = np.asarray(bundle["theta"], dtype=np.float64)
    data = np.asarray(bundle["data"], dtype=np.float64)
    state = np.asarray(bundle["state"], dtype=np.uint8)

    solved_mask = (state == 2).astype(np.uint8)
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    meta = {
        "version": 1,
        "format": "VariableISPAtlasBinary",
        "record_layout": ["lambda_r0", "lambda_vr0", "lambda_vtheta0", "gauge_Cm_zero", "C_theta", "transfer_time_days"],
        "source_npz": str(args.input),
        "solved_state_value": 2,
        "record_width": 6,
        "shape": [int(rho.size), int(kappa.size), int(theta.size)],
        "solved_count": int(solved_mask.sum()),
        "rho": rho.tolist(),
        "kappa": kappa.tolist(),
        "theta": theta.tolist(),
    }
    (output_dir / "variable_isp_atlas.meta.json").write_text(json.dumps(meta, indent=2))

    bin_path = output_dir / "variable_isp_atlas.bin"
    with bin_path.open("wb") as f:
        f.write(struct.pack("<Q", MAGIC))
        f.write(struct.pack("<QQQ", rho.size, kappa.size, theta.size))
        f.write(rho.tobytes(order="C"))
        f.write(kappa.tobytes(order="C"))
        f.write(theta.tobytes(order="C"))
        f.write(solved_mask.reshape(-1).tobytes(order="C"))
        f.write(data.reshape(-1).tobytes(order="C"))

    print(f"Wrote {bin_path}")


if __name__ == "__main__":
    main()
