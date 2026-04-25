#!/usr/bin/env python3
"""
Load atlas seeds, reconstruct a trajectory with the Python reference solver, and emit samples.
"""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Iterable

import numpy as np

import rocketHamilton as rh


R0_SI = rh.AU
MU_SI = rh.MU_SI
M0_KG = 3000.0
M_DRY_KG = 1000.0
DELTA_INV_M = (1.0 / M_DRY_KG) - (1.0 / M0_KG)
KAPPA_SCALE_FACTOR = (R0_SI ** 2.5) / (MU_SI ** 1.5)


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=pathlib.Path, default=pathlib.Path(__file__).with_name("trajectory_atlas.npz"))
    parser.add_argument("--rho", type=float, help="Similarity rho coordinate")
    parser.add_argument("--kappa", type=float, help="Similarity kappa coordinate")
    parser.add_argument("--theta", type=float, help="Similarity theta coordinate [rad]")
    parser.add_argument("--index", nargs=3, type=int, metavar=("I", "J", "K"), help="Exact atlas indices")
    parser.add_argument("--samples", type=int, default=129)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args()


def lower_index(axis: np.ndarray, value: float) -> int:
    if value <= axis[0]:
        return 0
    if value >= axis[-1]:
        return len(axis) - 2
    upper = int(np.searchsorted(axis, value, side="left"))
    if axis[upper] == value:
        return min(upper, len(axis) - 2)
    return upper - 1


def nearest_solved(state: np.ndarray, rho_grid: np.ndarray, kappa_grid: np.ndarray, theta_grid: np.ndarray, rho: float, kappa: float, theta: float) -> tuple[int, int, int]:
    i0 = lower_index(rho_grid, rho)
    j0 = lower_index(kappa_grid, kappa)
    k0 = lower_index(theta_grid, theta)
    solved_indices = np.argwhere(state == 2)
    for radius in range(1, max(state.shape) + 1):
        best = None
        for i in range(max(0, i0 - radius), min(state.shape[0], i0 + radius + 2)):
            for j in range(max(0, j0 - radius), min(state.shape[1], j0 + radius + 2)):
                for k in range(max(0, k0 - radius), min(state.shape[2], k0 + radius + 2)):
                    if state[i, j, k] != 2:
                        continue
                    score = abs(np.log(rho_grid[i] / rho)) + abs(np.log(kappa_grid[j] / kappa)) + abs(((theta_grid[k] - theta + np.pi) % (2 * np.pi)) - np.pi)
                    if best is None or score < best[0]:
                        best = (score, i, j, k)
        if best is not None:
            return best[1], best[2], best[3]
    if solved_indices.size == 0:
        raise RuntimeError("Atlas contains no solved points")
    scores = (
        np.abs(np.log(rho_grid[solved_indices[:, 0]] / rho))
        + np.abs(np.log(kappa_grid[solved_indices[:, 1]] / kappa))
        + np.abs(((theta_grid[solved_indices[:, 2]] - theta + np.pi) % (2 * np.pi)) - np.pi)
    )
    best_idx = int(np.argmin(scores))
    return tuple(int(v) for v in solved_indices[best_idx])


def trilinear_record(data: np.ndarray, state: np.ndarray, rho_grid: np.ndarray, kappa_grid: np.ndarray, theta_grid: np.ndarray, rho: float, kappa: float, theta: float) -> np.ndarray:
    i0 = lower_index(rho_grid, rho)
    j0 = lower_index(kappa_grid, kappa)
    k0 = lower_index(theta_grid, theta)
    i1, j1, k1 = i0 + 1, j0 + 1, k0 + 1

    corners = [
        (i0, j0, k0), (i1, j0, k0), (i0, j1, k0), (i1, j1, k0),
        (i0, j0, k1), (i1, j0, k1), (i0, j1, k1), (i1, j1, k1),
    ]
    if any(state[i, j, k] != 2 for i, j, k in corners):
        ni, nj, nk = nearest_solved(state, rho_grid, kappa_grid, theta_grid, rho, kappa, theta)
        return np.asarray(data[ni, nj, nk], dtype=np.float64)

    tx = (rho - rho_grid[i0]) / (rho_grid[i1] - rho_grid[i0])
    ty = (kappa - kappa_grid[j0]) / (kappa_grid[j1] - kappa_grid[j0])
    tz = (theta - theta_grid[k0]) / (theta_grid[k1] - theta_grid[k0])
    cube = np.array([data[i, j, k] for (i, j, k) in corners], dtype=np.float64).reshape(2, 2, 2, -1)
    c00 = cube[0, 0, 0] * (1.0 - tx) + cube[1, 0, 0] * tx
    c10 = cube[0, 1, 0] * (1.0 - tx) + cube[1, 1, 0] * tx
    c01 = cube[0, 0, 1] * (1.0 - tx) + cube[1, 0, 1] * tx
    c11 = cube[0, 1, 1] * (1.0 - tx) + cube[1, 1, 1] * tx
    c0 = c00 * (1.0 - ty) + c10 * ty
    c1 = c01 * (1.0 - ty) + c11 * ty
    return c0 * (1.0 - tz) + c1 * tz


def build_payload(sol, sample_times_s: Iterable[float], rho: float, kappa: float, theta: float, record: np.ndarray) -> dict:
    states = []
    for time_s in sample_times_s:
        idx = int(np.searchsorted(sol.t, time_s, side="left"))
        idx = min(idx, sol.t.size - 1)
        states.append({
            "time_s": float(sol.t[idx]),
            "r_m": float(sol.y[0, idx]),
            "theta_rad": float(sol.y[1, idx]),
            "vr_mps": float(sol.y[2, idx]),
            "vtheta_mps": float(sol.y[3, idx]),
            "mass_kg": float(sol.y[4, idx]),
        })
    return {
        "rho": rho,
        "kappa": kappa,
        "theta_rad": theta,
        "seed": record.tolist(),
        "samples": states,
    }


def main() -> None:
    args = parse_args()
    bundle = np.load(args.atlas)
    rho_grid = np.asarray(bundle["rho"], dtype=np.float64)
    kappa_grid = np.asarray(bundle["kappa"], dtype=np.float64)
    theta_grid = np.asarray(bundle["theta"], dtype=np.float64)
    data = np.asarray(bundle["data"], dtype=np.float64)
    state = np.asarray(bundle["state"], dtype=np.uint8)

    if args.index is not None:
        i, j, k = args.index
        record = np.asarray(data[i, j, k], dtype=np.float64)
        rho = float(rho_grid[i])
        kappa = float(kappa_grid[j])
        theta = float(theta_grid[k])
    else:
        if args.rho is None or args.kappa is None or args.theta is None:
            raise SystemExit("Either --index or all of --rho/--kappa/--theta are required")
        rho = float(args.rho)
        kappa = float(args.kappa)
        theta = float(args.theta)
        record = trilinear_record(data, state, rho_grid, kappa_grid, theta_grid, rho, kappa, theta)

    _, config = get_canonical_mission_config(rho, kappa)
    params = record[:5]
    t_days = float(record[5])
    sample_times_s = np.linspace(0.0, t_days * rh.DAY, args.samples)
    vtheta0 = np.sqrt(config.mu / config.r0) if config.vtheta0 is None else config.vtheta0
    y0 = [config.r0, 0.0, config.vr0, vtheta0, config.m0, params[0], params[1], params[2]]
    sol = rh.solve_ivp(
        rh.ode_system,
        (0.0, t_days * rh.DAY),
        y0,
        args=(config, params[3], params[4]),
        t_eval=sample_times_s,
        rtol=1e-8,
        atol=1e-9,
        max_step=0.5 * rh.DAY,
    )
    payload = build_payload(sol, sample_times_s, rho, kappa, theta, record)
    args.output.write_text(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
