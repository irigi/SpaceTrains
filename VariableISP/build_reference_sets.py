#!/usr/bin/env python3
"""
Generate stride-based atlas-derived Python reference datasets for C++ verification.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import numpy as np

import rocketHamilton as rh


R0_SI = rh.AU
MU_SI = rh.MU_SI
M0_KG = 3000.0
M_DRY_KG = 1000.0
DELTA_INV_M = (1.0 / M_DRY_KG) - (1.0 / M0_KG)
KAPPA_SCALE_FACTOR = (R0_SI ** 2.5) / (MU_SI ** 1.5)


SAMPLE_COUNT = 129
STRIDE = 10
SHIFTED_OFFSET = STRIDE // 2
STATE_SOLVED = 2


def get_canonical_mission_config(rho: float, kappa: float):
    r_target = R0_SI * rho
    j_capacity = kappa / KAPPA_SCALE_FACTOR
    power = j_capacity / (2.0 * DELTA_INV_M)
    config = rh.TrajectoryConfig(
        mu=MU_SI,
        power=power,
        m_dry=M_DRY_KG,
        m0=M0_KG,
        r0=R0_SI,
        vr0=0.0,
        vtheta0=None,
    )
    return r_target, config


def integrate_seed(record: np.ndarray, rho: float, kappa: float, theta: float) -> dict:
    _, config = get_canonical_mission_config(rho, kappa)
    t_days = float(record[5])
    sample_times = np.linspace(0.0, t_days * rh.DAY, SAMPLE_COUNT)
    vtheta0 = np.sqrt(config.mu / config.r0) if config.vtheta0 is None else config.vtheta0
    y0 = [config.r0, 0.0, config.vr0, vtheta0, config.m0, record[0], record[1], record[2]]
    sol = rh.solve_ivp(
        rh.ode_system,
        (0.0, t_days * rh.DAY),
        y0,
        args=(config, record[3], record[4]),
        t_eval=sample_times,
        rtol=1e-8,
        atol=1e-9,
        max_step=0.5 * rh.DAY,
    )

    states = []
    for sample_index, sample_time in enumerate(sample_times):
        states.append({
            "time_s": float(sample_time),
            "r_m": float(sol.y[0, sample_index]),
            "theta_rad": float(sol.y[1, sample_index]),
            "vr_mps": float(sol.y[2, sample_index]),
            "vtheta_mps": float(sol.y[3, sample_index]),
            "mass_kg": float(sol.y[4, sample_index]),
        })

    return {
        "rho": float(rho),
        "kappa": float(kappa),
        "theta_rad": float(theta),
        "seed": [float(v) for v in record],
        "samples": states,
    }


def stride_indices(size: int, stride: int, offset: int) -> list[int]:
    if offset >= size:
        return []
    return list(range(offset, size, stride))


def build_dataset(name: str, data: np.ndarray, state: np.ndarray, rho_grid: np.ndarray, kappa_grid: np.ndarray, theta_grid: np.ndarray, stride: int, offset: int) -> dict:
    trajectories = []
    total_stride_hits = 0
    skipped_unsolved = 0

    rho_indices = stride_indices(state.shape[0], stride, offset)
    kappa_indices = stride_indices(state.shape[1], stride, offset)
    theta_indices = stride_indices(state.shape[2], stride, offset)

    for i in rho_indices:
        for j in kappa_indices:
            for k in theta_indices:
                total_stride_hits += 1
                if state[i, j, k] != STATE_SOLVED:
                    skipped_unsolved += 1
                    continue

                rho = rho_grid[i]
                kappa = kappa_grid[j]
                theta = theta_grid[k]
                record = np.asarray(data[i, j, k], dtype=np.float64)
                trajectories.append({
                    "source": "atlas_cell",
                    "indices": [int(i), int(j), int(k)],
                    **integrate_seed(record, float(rho), float(kappa), float(theta)),
                })
    return {
        "name": name,
        "sample_count": SAMPLE_COUNT,
        "stride": int(stride),
        "offset": int(offset),
        "total_stride_hits": int(total_stride_hits),
        "skipped_unsolved": int(skipped_unsolved),
        "included_solved": int(len(trajectories)),
        "trajectories": trajectories,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=pathlib.Path, default=pathlib.Path(__file__).with_name("trajectory_atlas.npz"))
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("tests/data/variable_isp"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    bundle = np.load(args.atlas)
    rho_grid = np.asarray(bundle["rho"], dtype=np.float64)
    kappa_grid = np.asarray(bundle["kappa"], dtype=np.float64)
    theta_grid = np.asarray(bundle["theta"], dtype=np.float64)
    data = np.asarray(bundle["data"], dtype=np.float64)
    state = np.asarray(bundle["state"], dtype=np.uint8)

    args.output_dir.mkdir(parents=True, exist_ok=True)

    train = build_dataset(
        "unit_stride",
        data, state, rho_grid, kappa_grid, theta_grid,
        stride=STRIDE,
        offset=0)
    validation = build_dataset(
        "validation_stride_shifted",
        data, state, rho_grid, kappa_grid, theta_grid,
        stride=STRIDE,
        offset=SHIFTED_OFFSET)

    (args.output_dir / "reference_unit.json").write_text(json.dumps(train))
    (args.output_dir / "reference_validation.json").write_text(json.dumps(validation))

    print("Wrote reference datasets to", args.output_dir)


if __name__ == "__main__":
    main()
