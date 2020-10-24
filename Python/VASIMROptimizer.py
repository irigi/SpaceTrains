import numpy as np
import matplotlib.pyplot as plt
from scipy.optimize import minimize, basinhopping
from scipy.integrate import solve_ivp
import time
import numba

GM = 39.4876393  # in AU^3 / year^2
KMPS = 0.210945021  # kilometer per second in au / year
GW = 1404213.84  # gigawatt in ton * AU^2 / year^3
payload = 300  # tons
power = 2 * GW  # gigawatts
veff_max = 100 * KMPS


class FourierParametrizer:
    def __init__(self, x0, x1, v0, v1, t, fourier_len):
        self.x0 = x0
        self.x1 = x1
        self.v0 = v0
        self.v1 = v1

        par = (t - t[0]) / (t[-1] - t[0])
        cosini, sini = [], []
        ddcosini, ddsini = [], []
        for i in range(fourier_len // 2):
            cosini.append(np.cos(np.pi * par * (i + 1)))
            sini.append(np.sin(np.pi * par * (i + 1)))
            ddcosini.append(-(i + 1) ** 2 * np.pi ** 2 * np.cos(np.pi * par * (i + 1)))
            ddsini.append(-(i + 1) ** 2 * np.pi ** 2 * np.sin(np.pi * par * (i + 1)))

        self.cosini = np.array(cosini)
        self.sini = np.array(sini)
        self.ddcosini = np.array(ddcosini)
        self.ddsini = np.array(ddsini)
        self.par = par

    def get_x(self, x_fourier):
        aa = x_fourier[:len(x_fourier) // 2]
        bb = x_fourier[len(x_fourier) // 2:]

        xx = (self.x1 - self.x0) * self.par + self.x0
        xx += aa @ self.cosini + bb @ self.sini

        return xx

    def get_d2x(self, x_fourier, t):
        aa = x_fourier[:len(x_fourier) // 2]
        bb = x_fourier[len(x_fourier) // 2:]

        xx = (aa @ self.ddcosini + bb @ self.ddsini) / (t[-1] - t[0]) ** 2

        return xx

    def force_v0(self, x_fourier, t0, t1):
        aa = x_fourier[:len(x_fourier) // 2]
        bb = x_fourier[len(x_fourier) // 2:]
        kk = (self.x1 - self.x0) / (t1 - t0)
        bb[0] = -np.sum((np.array(range(len(bb)))[2::2] + 1) * bb[2::2]) - (t0 - t1) * (self.v0 - self.v1) / 2 / np.pi
        bb[1] = -np.sum((np.array(range(len(bb[3::2]))) + 2) * bb[3::2]) + (t0 - t1) * (
                    2 * kk - self.v0 - self.v1) / 4 / np.pi
        aa[0] = -np.sum(aa[2::2])
        aa[1] = -np.sum(aa[3::2])

        return np.concatenate([aa, bb])


@numba.jit("float64[:](float64[:], float64, float64, float64[:], float64[:])", nopython=True, nogil=True)
def calc_mass(accmiddle_, veff_max_, power_, tt_, mass_):
    for i in range(len(accmiddle_), 0, -1):
        if accmiddle_[i - 1] <= 0 or mass_[i] > 10*mass_[-1]:
            mass_[i - 1] = mass_[i]+1
        else:
            veff_theor_ = 2 * power_ / accmiddle_[i - 1] / mass_[i]
            veff = np.minimum(veff_max_, veff_theor_)
            power_real = power_ * veff / veff_theor_
            mass_[i - 1] = mass_[i] + 2 * power_real / veff ** 2 * (tt_[1] - tt_[0])
    return mass_


def multiscore(x, args):
    tt, four_x, four_y = args

    tmax, x = np.maximum(0.001, x[0]), x[1:]
    tt = np.linspace(0, tmax, len(tt))
    xx, yy = four_x.force_v0(x[:len(x)//2], tt[0], tt[-1]), four_y.force_v0(x[len(x) // 2:], tt[0], tt[-1])

    # calculate forces applied by the ship
    xxx, yyy = four_x.get_x(xx), four_y.get_x(yy)
    rrr = np.reshape(np.concatenate([xxx, yyy]), (2, -1))
    rnorm = np.maximum(0.0046547454, np.linalg.norm(rrr, axis=0))**3
    fmxs, fmys = four_x.get_d2x(xx, tt), four_y.get_d2x(yy, tt)
    fmx, fmy = fmxs + GM * xxx / rnorm, fmys + GM * yyy / rnorm

    acc = np.sqrt(fmx**2+fmy**2)
    accmiddle = (acc[:-1] + acc[1:]) / 2

    mass = acc * 0 + payload
    mass = calc_mass(accmiddle, veff_max, power, tt, mass)

    return (np.abs(mass[0]-3*payload)+(mass[0]-3*payload))/200 + tmax, mass[0], tmax


def score(x, args):
    sc, _, _ = multiscore(x, args)
    return sc


def one_iter(phi, dist, initial=None):
    nn = 100
    t = np.linspace(0, 6.5, 3600)

    if initial is None:
        initial = [6.5]+[0 for _ in range(2*nn)]

    if len(initial) < 2*nn+1:
        n = (len(initial)-1) // 2
        ii = initial[1:][:n//2], initial[1:][n//2:n], initial[1:][n:n+n//2], initial[1:][n+n//2:]
        initial = ([initial[0]] + list(ii[0]) + [0 for _ in range((nn-n)//2)]
                   + list(ii[1]) + [0 for _ in range((nn - n) // 2)]
                   + list(ii[2]) + [0 for _ in range((nn - n) // 2)]
                   + list(ii[3]) + [0 for _ in range((nn - n) // 2)]
                   )
    else:
        nn = (len(initial)-1) // 2

    fourx = FourierParametrizer(1, dist*np.cos(phi), 0, -np.sqrt(GM/dist)*np.sin(phi), t, nn)
    foury = FourierParametrizer(0, dist*np.sin(phi), np.sqrt(GM), np.sqrt(GM/dist)*np.cos(phi), t, nn)

    time_start = time.time()
    args = [t, fourx, foury]
    res = minimize(score, x0=initial, args=args)
    _, m, tmax = multiscore(res["x"], args)
    print(f'mass_end: {m:.2f}, flight duration {tmax:.2f}, execution time: {time.time() - time_start:.2f}')

    return m, tmax, res["x"]


def plot_results():
    for dist in [1.5, 2, 3, 4, 5, 10, 15, 20, 25, 30, 40, 50]:
        for dname in ['out']:
            angles = []
            for i in range(20):
                try:
                    with np.load(f'{dname}/data_a{i:02d}_d{dist:.2f}.npz') as dat:
                        __tmax1, __m1 = dat['tmax'].item(), dat['m'].item()
                except:
                    __tmax1 = 10
                angles.append(__tmax1)
            plt.plot([2 * np.pi * i / 20 for i in range(20)], angles, label=f'{dist:.2f} AU')
    plt.ylabel('time [years]')
    plt.xlabel('angle')
    plt.legend()
    plt.show()


def main(dist):
    # plot results
    plot_results()

    while True:
        # take initial condition from random neighbor
        shift = (np.random.randint(0, 5)-2+20) % 20
        for i in range(20):
            try:
                with np.load(f'out/data_a{(i+shift)%20:02d}_d{dist:.2f}.npz') as dat:
                    tmax0, m0, init = dat['tmax'].item(), dat['m'].item(), dat['result']
                with np.load(f'out/data_a{i:02d}_d{dist:.2f}.npz') as dat:
                    tmax1, m1 = dat['tmax'].item(), dat['m'].item()
            except:
                init = None
            m, tmax, res = one_iter(2*np.pi*i/20, dist, init)
            if init is None or (m1 > 9*payload and __m < 9*payload) or (tmax < tmax1 and __m < 9*payload):
                print('found better solution')
                np.savez_compressed(f'out/data_a{i:02d}_d{dist:.2f}.npz', m=__m, tmax=__tmax, result=__res,
                                    distance=dist, angle=2*np.pi*i/20)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description='VASIMR rocket optimizer')
    parser.add_argument('--dist', type=float, help='target destination distance [AU]')
    ags = parser.parse_args()
    main(ags.dist)

